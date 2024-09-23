// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <index/disktxpos.h>
#include <index/txospenderindex.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <uint256.h>
#include <validation.h>

// LeveLDB key prefix. We only have one key for now but it will make it easier to add others if needed.
constexpr uint8_t DB_TXOSPENDERINDEX{'s'};

std::unique_ptr<TxoSpenderIndex> g_txospenderindex;

TxoSpenderIndex::TxoSpenderIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "txospenderindex")
{
    fs::path path{gArgs.GetDataDirNet() / "indexes" / "txospenderindex"};
    fs::create_directories(path);

    m_db = std::make_unique<TxoSpenderIndex::DB>(path / "db", n_cache_size, f_memory, f_wipe);
}

TxoSpenderIndex::~TxoSpenderIndex() = default;

static uint64_t CreateIndex(const COutPoint& vout)
{
    // create a 64 bits index from a COutPoint: convert the first 8 bytes of the tx hash to a 64 bits integer and the output index
    return vout.hash.ToUint256().GetUint64(0) + vout.n;
}

bool TxoSpenderIndex::WriteSpenderInfos(const std::vector<std::pair<COutPoint, CDiskTxPos>>& items)
{
    CDBBatch batch(*m_db);
    for (const auto& [outpoint, pos] : items) {
        std::vector<CDiskTxPos> positions;
        std::pair<uint8_t, uint64_t> key{DB_TXOSPENDERINDEX, CreateIndex(outpoint)};
        if (m_db->Exists(key)) {
            if (!m_db->Read(key, positions)) {
                LogError("%s: Cannot read current state; tx spender index may be corrupted\n", __func__);
            }
        }
        if (std::find(positions.begin(), positions.end(), pos) == positions.end()) {
            positions.push_back(pos);
            batch.Write(key, positions);
        }
    }
    return m_db->WriteBatch(batch);
}


bool TxoSpenderIndex::EraseSpenderInfos(const std::vector<COutPoint>& items)
{
    CDBBatch batch(*m_db);
    for (const auto& outpoint : items) {
        std::vector<CDiskTxPos> positions;
        std::pair<uint8_t, uint64_t> key{DB_TXOSPENDERINDEX, CreateIndex(outpoint)};
        m_db->Read(key, positions);
        if (positions.size() > 1) {
            // there are collisions: find the position of the tx that spends the outpoint we want to erase
            size_t index = std::numeric_limits<size_t>::max();;
            for (size_t i = 0; i < positions.size(); i++) {
                CTransactionRef tx;
                if (!ReadTransaction(positions[i], tx)) continue;
                 for (const auto& input : tx->vin) {
                    if (input.prevout == outpoint) {
                        index = i;
                        break;
                    }
                }
            }
            if (index != std::numeric_limits<size_t>::max()) {
                // remove it from the list
                positions.erase(positions.begin() + index);
                batch.Write(key, positions);
            }
        } else {
            batch.Erase(key);
        }
    }
    return m_db->WriteBatch(batch);
}

bool TxoSpenderIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    std::vector<std::pair<COutPoint, CDiskTxPos>> items;
    items.reserve(block.data->vtx.size());

    CDiskTxPos pos({block.file_number, block.data_pos}, GetSizeOfCompactSize(block.data->vtx.size()));
    for (const auto& tx : block.data->vtx) {
        if (!tx->IsCoinBase()) {
            for (const auto& input : tx->vin) {
                items.emplace_back(input.prevout, pos);
            }
        }
        pos.nTxOffset += ::GetSerializeSize(TX_WITH_WITNESS(*tx));
    }

    return WriteSpenderInfos(items);
}

bool TxoSpenderIndex::CustomRewind(const interfaces::BlockRef& current_tip, const interfaces::BlockRef& new_tip)
{
    LOCK(cs_main);
    const CBlockIndex* iter_tip{m_chainstate->m_blockman.LookupBlockIndex(current_tip.hash)};
    const CBlockIndex* new_tip_index{m_chainstate->m_blockman.LookupBlockIndex(new_tip.hash)};

    do {
        CBlock block;
        if (!m_chainstate->m_blockman.ReadBlockFromDisk(block, *iter_tip)) {
            LogError("Failed to read block %s from disk\n", iter_tip->GetBlockHash().ToString());
            return false;
        }
        std::vector<COutPoint> items;
        items.reserve(block.vtx.size());
        for (const auto& tx : block.vtx) {
            if (tx->IsCoinBase()) {
                continue;
            }
            for (const auto& input : tx->vin) {
                items.emplace_back(input.prevout);
            }
        }
        if (!EraseSpenderInfos(items)) {
            LogError("Failed to erase indexed data for disconnected block %s from disk\n", iter_tip->GetBlockHash().ToString());
            return false;
        }

        iter_tip = iter_tip->GetAncestor(iter_tip->nHeight - 1);
    } while (new_tip_index != iter_tip);

    return true;
}

bool TxoSpenderIndex::ReadTransaction(const CDiskTxPos& postx, CTransactionRef& tx) const
{
    AutoFile file{m_chainstate->m_blockman.OpenBlockFile(postx, true)};
    if (file.IsNull()) {
        return false;
    }
    CBlockHeader header;
    try {
        file >> header;
        file.seek(postx.nTxOffset, SEEK_CUR);
        file >> TX_WITH_WITNESS(tx);
        return true;
    } catch (const std::exception& e) {
        LogError("%s: Deserialize or I/O error - %s\n", __func__, e.what());
        return false;
    }
}

std::optional<Txid> TxoSpenderIndex::FindSpender(const COutPoint& txo) const
{
    std::vector<CDiskTxPos> positions;
    // read all tx position candidates from the db. there may be index collisions, in which case the db will return more than one tx position
    if (!m_db->Read(std::pair{DB_TXOSPENDERINDEX, CreateIndex(txo)}, positions)) {
        return std::nullopt;
    }
    // loop until we find a tx that spends our outpoint
    for (const auto& postx : positions) {
        CTransactionRef tx;
        if (ReadTransaction(postx, tx)) {
            for (const auto& input : tx->vin) {
                if (input.prevout == txo) {
                    return tx->GetHash();
                }
            }
        }
    }
    return std::nullopt;
}

BaseIndex::DB& TxoSpenderIndex::GetDB() const { return *m_db; }

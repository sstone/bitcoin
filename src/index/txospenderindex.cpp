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
constexpr uint8_t DB_TXOSPENDERINDEX_SPENDING_TX{'s'};
constexpr uint8_t DB_TXOSPENDERINDEX_TX{'t'};

struct SpenderKey {
    CDiskTxPos pos;
    uint32_t n;

    explicit SpenderKey(const CDiskTxPos& pos_in, uint32_t n_in) : pos(pos_in), n(n_in) {}

    SERIALIZE_METHODS(SpenderKey, obj)
    {
        READWRITE(obj.pos);
        READWRITE(obj.n);
    }
};

std::unique_ptr<TxoSpenderIndex> g_txospenderindex;

TxoSpenderIndex::TxoSpenderIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "txospenderindex")
{
    fs::path path{gArgs.GetDataDirNet() / "indexes" / "txospenderindex"};
    fs::create_directories(path);

    m_db = std::make_unique<TxoSpenderIndex::DB>(path / "db", n_cache_size, f_memory, f_wipe);
}

TxoSpenderIndex::~TxoSpenderIndex() = default;

bool TxoSpenderIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    CDBBatch batch(*m_db);
    CDiskTxPos pos({block.file_number, block.data_pos}, GetSizeOfCompactSize(block.data->vtx.size()));
    std::map<Txid, CDiskTxPos> positions;

    for (const auto& tx : block.data->vtx) {
        LogDebug(BCLog::ALL, "%s saving tx %s\n", __func__, tx->GetHash().ToString());
        batch.Write(std::make_pair(DB_TXOSPENDERINDEX_TX, tx->GetHash()), pos);
        positions[tx->GetHash()] = pos;
        if (!tx->IsCoinBase()) {
            for (const auto& input : tx->vin) {
                CDiskTxPos spentPos;

                // spent tx was either seen earlier in this block or has already been indexed in previous blocks
                const auto it{positions.find(input.prevout.hash)};
                if (it != positions.end()) {
                    spentPos = it->second;
                } else if (!m_db->Read(std::make_pair(DB_TXOSPENDERINDEX_TX, input.prevout.hash), spentPos)) {
                    LogError("%s: Cannot find spent output data for %s %d spent by %s, index may be corrupted\n", __func__, input.prevout.hash.ToString(), input.prevout.n, tx->GetHash().ToString());
                }
                LogDebug(BCLog::ALL, "%s saving outpoint %s %d spent by %s\n", __func__, input.prevout.hash.ToString(), input.prevout.n, tx->GetHash().ToString());
                batch.Write(std::make_pair(DB_TXOSPENDERINDEX_SPENDING_TX, SpenderKey(spentPos, input.prevout.n)), pos);
            }
        }
        pos.nTxOffset += ::GetSerializeSize(TX_WITH_WITNESS(*tx));
    }
    return m_db->WriteBatch(batch);
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
        CDBBatch batch(*m_db);
        for (const auto& tx : block.vtx) {
            if (tx->IsCoinBase()) {
                continue;
            }
            for (const auto& input : tx->vin) {
                CDiskTxPos spentPos;
                m_db->Read(std::make_pair(DB_TXOSPENDERINDEX_TX, input.prevout.hash), spentPos);
                batch.Erase(std::make_pair(DB_TXOSPENDERINDEX_SPENDING_TX, SpenderKey(spentPos, input.prevout.n)));
            }
        }
        if (!m_db->WriteBatch(batch)) {
            LogError("Failed to erase indexed data for disconnected block %s from disk\n", iter_tip->GetBlockHash().ToString());
            return false;
        }
        iter_tip = iter_tip->GetAncestor(iter_tip->nHeight - 1);
    } while (new_tip_index != iter_tip);

    return true;
}

std::optional<Txid> TxoSpenderIndex::FindSpender(const COutPoint& txo) const
{
    CDiskTxPos spentPos;
    if (!m_db->Read(std::make_pair(DB_TXOSPENDERINDEX_TX, txo.hash), spentPos)) {
        return std::nullopt;
    }
    CDiskTxPos pos;
    if (!m_db->Read(std::make_pair(DB_TXOSPENDERINDEX_SPENDING_TX, SpenderKey(spentPos, txo.n)), pos)) {
        return std::nullopt;
    }
    AutoFile file{m_chainstate->m_blockman.OpenBlockFile(pos, true)};
    if (file.IsNull()) {
        return std::nullopt;
    }
    CBlockHeader header;
    CTransactionRef tx;
    try {
        file >> header;
        file.seek(pos.nTxOffset, SEEK_CUR);
        file >> TX_WITH_WITNESS(tx);
        return tx->GetHash();
    } catch (const std::exception& e) {
        LogError("%s: Deserialize or I/O error - %s\n", __func__, e.what());
        return std::nullopt;
    }
}

BaseIndex::DB& TxoSpenderIndex::GetDB() const { return *m_db; }

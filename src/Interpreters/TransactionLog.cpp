#include <Interpreters/TransactionLog.h>
#include <Interpreters/TransactionVersionMetadata.h>
#include <Common/Exception.h>
#include <Core/ServerUUID.h>
#include <base/logger_useful.h>

namespace DB
{

namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
}

TransactionLog & TransactionLog::instance()
{
    static TransactionLog inst;
    return inst;
}

TransactionLog::TransactionLog()
    : log(&Poco::Logger::get("TransactionLog"))
{
    latest_snapshot = Tx::MaxReservedCSN;
    csn_counter = Tx::MaxReservedCSN;
    local_tid_counter = Tx::MaxReservedLocalTID;
}

Snapshot TransactionLog::getLatestSnapshot() const
{
    return latest_snapshot.load();
}

MergeTreeTransactionPtr TransactionLog::beginTransaction()
{
    MergeTreeTransactionPtr txn;
    {
        std::lock_guard lock{running_list_mutex};
        Snapshot snapshot = latest_snapshot.load();
        LocalTID ltid = 1 + local_tid_counter.fetch_add(1);
        txn = std::make_shared<MergeTreeTransaction>(snapshot, ltid, ServerUUID::get());
        bool inserted = running_list.try_emplace(txn->tid.getHash(), txn).second;
        if (!inserted)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "I's a bug: TID {} {} exists", txn->tid.getHash(), txn->tid);
        txn->snapshot_in_use_it = snapshots_in_use.insert(snapshots_in_use.end(), snapshot);
    }
    LOG_TRACE(log, "Beginning transaction {} ({})", txn->tid, txn->tid.getHash());
    return txn;
}

CSN TransactionLog::commitTransaction(const MergeTreeTransactionPtr & txn)
{
    txn->beforeCommit();

    CSN new_csn;
    /// TODO Transactions: reset local_tid_counter
    if (txn->isReadOnly())
    {
        LOG_TRACE(log, "Closing readonly transaction {}", txn->tid);
        new_csn = txn->snapshot;
    }
    else
    {
        LOG_TRACE(log, "Committing transaction {}{}", txn->tid, txn->dumpDescription());
        std::lock_guard lock{commit_mutex};
        new_csn = 1 + csn_counter.fetch_add(1);
        bool inserted = tid_to_csn.try_emplace(txn->tid.getHash(), new_csn).second;     /// Commit point
        if (!inserted)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "I's a bug: TID {} {} exists", txn->tid.getHash(), txn->tid);
        latest_snapshot.store(new_csn, std::memory_order_relaxed);
    }

    LOG_INFO(log, "Transaction {} committed with CSN={}", txn->tid, new_csn);

    txn->afterCommit(new_csn);

    {
        std::lock_guard lock{running_list_mutex};
        bool removed = running_list.erase(txn->tid.getHash());
        if (!removed)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "I's a bug: TID {} {} doesn't exist", txn->tid.getHash(), txn->tid);
        snapshots_in_use.erase(txn->snapshot_in_use_it);
    }
    return new_csn;
}

void TransactionLog::rollbackTransaction(const MergeTreeTransactionPtr & txn) noexcept
{
    LOG_TRACE(log, "Rolling back transaction {}", txn->tid);
    if (txn->rollback())
    {
        std::lock_guard lock{running_list_mutex};
        bool removed = running_list.erase(txn->tid.getHash());
        if (!removed)
            abort();
        snapshots_in_use.erase(txn->snapshot_in_use_it);
    }
}

MergeTreeTransactionPtr TransactionLog::tryGetRunningTransaction(const TIDHash & tid)
{
    std::lock_guard lock{running_list_mutex};
    auto it = running_list.find(tid);
    if (it == running_list.end())
        return nullptr;
    return it->second;
}

CSN TransactionLog::getCSN(const TransactionID & tid) const
{
    return getCSN(tid.getHash());
}

CSN TransactionLog::getCSN(const TIDHash & tid) const
{
    assert(tid);
    assert(tid != Tx::EmptyTID.getHash());
    if (tid == Tx::PrehistoricTID.getHash())
        return Tx::PrehistoricCSN;

    std::lock_guard lock{commit_mutex};
    auto it = tid_to_csn.find(tid);
    if (it == tid_to_csn.end())
        return Tx::UnknownCSN;
    return it->second;
}

Snapshot TransactionLog::getOldestSnapshot() const
{
    std::lock_guard lock{running_list_mutex};
    if (snapshots_in_use.empty())
        return getLatestSnapshot();
    return snapshots_in_use.front();
}

}

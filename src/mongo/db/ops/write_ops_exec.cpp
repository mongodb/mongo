/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/checked_cast.h"
#include "mongo/base/transaction_error.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/introspect.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

// Convention in this file: generic helpers go in the anonymous namespace. Helpers that are for a
// single type of operation are static functions defined above their caller.
namespace {

MONGO_FAIL_POINT_DEFINE(failAllInserts);
MONGO_FAIL_POINT_DEFINE(failAllUpdates);
MONGO_FAIL_POINT_DEFINE(failAllRemoves);
MONGO_FAIL_POINT_DEFINE(hangBeforeChildRemoveOpFinishes);
MONGO_FAIL_POINT_DEFINE(hangBeforeChildRemoveOpIsPopped);
MONGO_FAIL_POINT_DEFINE(hangAfterAllChildRemoveOpsArePopped);
MONGO_FAIL_POINT_DEFINE(hangDuringBatchInsert);
MONGO_FAIL_POINT_DEFINE(hangDuringBatchUpdate);
MONGO_FAIL_POINT_DEFINE(hangDuringBatchRemove);
MONGO_FAIL_POINT_DEFINE(hangAndFailAfterDocumentInsertsReserveOpTimes);
// The withLock fail points are for testing interruptability of these operations, so they will not
// themselves check for interrupt.
MONGO_FAIL_POINT_DEFINE(hangWithLockDuringBatchInsert);
MONGO_FAIL_POINT_DEFINE(hangWithLockDuringBatchUpdate);
MONGO_FAIL_POINT_DEFINE(hangWithLockDuringBatchRemove);

void updateRetryStats(OperationContext* opCtx, bool containsRetry) {
    if (containsRetry) {
        RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
    }
}

void finishCurOp(OperationContext* opCtx, CurOp* curOp) {
    try {
        curOp->done();
        long long executionTimeMicros =
            durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses());
        curOp->debug().executionTimeMicros = executionTimeMicros;

        recordCurOpMetrics(opCtx);
        Top::get(opCtx->getServiceContext())
            .record(opCtx,
                    curOp->getNS(),
                    curOp->getLogicalOp(),
                    Top::LockType::WriteLocked,
                    durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                    curOp->isCommand(),
                    curOp->getReadWriteType());

        if (!curOp->debug().errInfo.isOK()) {
            LOG(3) << "Caught Assertion in " << redact(logicalOpToString(curOp->getLogicalOp()))
                   << ": " << curOp->debug().errInfo.toString();
        }

        // Mark the op as complete, and log it if appropriate. Returns a boolean indicating whether
        // this op should be sampled for profiling.
        const bool shouldSample =
            curOp->completeAndLogOperation(opCtx, MONGO_LOG_DEFAULT_COMPONENT);

        if (curOp->shouldDBProfile(shouldSample)) {
            // Stash the current transaction so that writes to the profile collection are not
            // done as part of the transaction.
            TransactionParticipant::SideTransactionBlock sideTxn(opCtx);
            profile(opCtx, CurOp::get(opCtx)->getNetworkOp());
        }
    } catch (const DBException& ex) {
        // We need to ignore all errors here. We don't want a successful op to fail because of a
        // failure to record stats. We also don't want to replace the error reported for an op that
        // is failing.
        log() << "Ignoring error from finishCurOp: " << redact(ex);
    }
}

/**
 * Sets the Client's LastOp to the system OpTime if needed. This is especially helpful for
 * adjusting the client opTime for cases when batched write performed multiple writes, but
 * when the last write was a no-op (which will not advance the client opTime).
 */
class LastOpFixer {
public:
    LastOpFixer(OperationContext* opCtx, const NamespaceString& ns)
        : _opCtx(opCtx), _isOnLocalDb(ns.isLocal()) {}

    ~LastOpFixer() {
        if (_needToFixLastOp && !_isOnLocalDb) {
            // If this operation has already generated a new lastOp, don't bother setting it
            // here. No-op updates will not generate a new lastOp, so we still need the
            // guard to fire in that case. Operations on the local DB aren't replicated, so they
            // don't need to bump the lastOp.
            replClientInfo().setLastOpToSystemLastOpTime(_opCtx);
        }
    }

    void startingOp() {
        _needToFixLastOp = true;
        _opTimeAtLastOpStart = replClientInfo().getLastOp();
    }

    void finishedOpSuccessfully() {
        // If the op was succesful and bumped LastOp, we don't need to do it again. However, we
        // still need to for no-ops and all failing ops.
        _needToFixLastOp = (replClientInfo().getLastOp() == _opTimeAtLastOpStart);
    }

private:
    repl::ReplClientInfo& replClientInfo() {
        return repl::ReplClientInfo::forClient(_opCtx->getClient());
    }

    OperationContext* const _opCtx;
    bool _needToFixLastOp = true;
    const bool _isOnLocalDb;
    repl::OpTime _opTimeAtLastOpStart;
};

void assertCanWrite_inlock(OperationContext* opCtx, const NamespaceString& ns) {
    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Not primary while writing to " << ns.ns(),
            repl::ReplicationCoordinator::get(opCtx->getServiceContext())
                ->canAcceptWritesFor(opCtx, ns));
    CollectionShardingState::get(opCtx, ns)->checkShardVersionOrThrow(opCtx);
}

void makeCollection(OperationContext* opCtx, const NamespaceString& ns) {
    auto inTransaction = opCtx->inMultiDocumentTransaction();
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot create namespace " << ns.ns()
                          << " in multi-document transaction.",
            !inTransaction);

    writeConflictRetry(opCtx, "implicit collection creation", ns.ns(), [&opCtx, &ns] {
        AutoGetOrCreateDb db(opCtx, ns.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, ns, MODE_X);

        assertCanWrite_inlock(opCtx, ns);
        if (!db.getDb()->getCollection(opCtx, ns)) {  // someone else may have beat us to it.
            uassertStatusOK(userAllowedCreateNS(ns.db(), ns.coll()));
            WriteUnitOfWork wuow(opCtx);
            CollectionOptions collectionOptions;
            uassertStatusOK(
                collectionOptions.parse(BSONObj(), CollectionOptions::ParseKind::parseForCommand));
            uassertStatusOK(db.getDb()->userCreateNS(opCtx, ns, collectionOptions));
            wuow.commit();
        }
    });
}

/**
 * Returns true if the batch can continue, false to stop the batch, or throws to fail the command.
 */
bool handleError(OperationContext* opCtx,
                 const DBException& ex,
                 const NamespaceString& nss,
                 const write_ops::WriteCommandBase& wholeOp,
                 WriteResult* out) {
    LastError::get(opCtx->getClient()).setLastError(ex.code(), ex.reason());
    auto& curOp = *CurOp::get(opCtx);
    curOp.debug().errInfo = ex.toStatus();

    if (ErrorCodes::isInterruption(ex.code())) {
        throw;  // These have always failed the whole batch.
    }

    if (ErrorCodes::WouldChangeOwningShard == ex.code()) {
        throw;  // Fail this write so mongos can retry
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (txnParticipant && opCtx->inMultiDocumentTransaction()) {
        if (isTransientTransactionError(
                ex.code(), false /* hasWriteConcernError */, false /* isCommitTransaction */)) {
            // Tell the client to try the whole txn again, by returning ok: 0 with errorLabels.
            throw;
        }
        // If we are in a transaction, we must fail the whole batch.
        out->results.emplace_back(ex.toStatus());
        txnParticipant.abortTransaction(opCtx);
        return false;
    }

    if (ex.extraInfo<StaleConfigInfo>()) {
        if (!opCtx->getClient()->isInDirectClient()) {
            auto& oss = OperationShardingState::get(opCtx);
            oss.setShardingOperationFailedStatus(ex.toStatus());
        }

        // Don't try doing more ops since they will fail with the same error.
        // Command reply serializer will handle repeating this error if needed.
        out->results.emplace_back(ex.toStatus());
        return false;
    } else if (ex.extraInfo<CannotImplicitlyCreateCollectionInfo>()) {
        auto& oss = OperationShardingState::get(opCtx);
        oss.setShardingOperationFailedStatus(ex.toStatus());

        // Don't try doing more ops since they will fail with the same error.
        // Command reply serializer will handle repeating this error if needed.
        out->results.emplace_back(ex.toStatus());
        return false;
    }

    out->results.emplace_back(ex.toStatus());
    return !wholeOp.getOrdered();
}

void insertDocuments(OperationContext* opCtx,
                     Collection* collection,
                     std::vector<InsertStatement>::iterator begin,
                     std::vector<InsertStatement>::iterator end,
                     bool fromMigrate) {
    // Intentionally not using writeConflictRetry. That is handled by the caller so it can react to
    // oversized batches.
    WriteUnitOfWork wuow(opCtx);

    // Acquire optimes and fill them in for each item in the batch.
    // This must only be done for doc-locking storage engines, which are allowed to insert oplog
    // documents out-of-timestamp-order.  For other storage engines, the oplog entries must be
    // physically written in timestamp order, so we defer optime assignment until the oplog is about
    // to be written. Multidocument transactions should not generate opTimes because they are
    // generated at the time of commit.
    auto batchSize = std::distance(begin, end);
    if (supportsDocLocking()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        auto inTransaction = opCtx->inMultiDocumentTransaction();

        if (!inTransaction && !replCoord->isOplogDisabledFor(opCtx, collection->ns())) {
            // Populate 'slots' with new optimes for each insert.
            // This also notifies the storage engine of each new timestamp.
            auto oplogSlots = repl::getNextOpTimes(opCtx, batchSize);
            auto slot = oplogSlots.begin();
            for (auto it = begin; it != end; it++) {
                it->oplogSlot = *slot++;
            }
        }
    }

    MONGO_FAIL_POINT_BLOCK(hangAndFailAfterDocumentInsertsReserveOpTimes, nssData) {
        const BSONObj& data = nssData.getData();
        const auto collElem = data["collectionNS"];
        if (!collElem || collection->ns().ns() == collElem.str()) {
            MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
                opCtx, hangAndFailAfterDocumentInsertsReserveOpTimes);
            uasserted(51269, "hangAndFailAfterDocumentInsertsReserveOpTimes fail point enabled");
        }
    }

    uassertStatusOK(
        collection->insertDocuments(opCtx, begin, end, &CurOp::get(opCtx)->debug(), fromMigrate));
    wuow.commit();
}

/**
 * Returns a OperationNotSupportedInTransaction error Status if we are in a transaction and
 * operating on a capped collection.
 *
 * The behavior of an operation against a capped collection may differ across replica set members,
 * where it can succeed on one member and fail on another, crashing the failing member. Prepared
 * transactions are not allowed to fail, so capped collections will not be allowed on shards.
 * Even in the unsharded case, capped collections are still problematic with transactions because
 * they only allow one operation at a time because they enforce insertion order with a MODE_X
 * collection lock, which we cannot hold in transactions.
 */
Status checkIfTransactionOnCappedColl(OperationContext* opCtx, Collection* collection) {
    if (opCtx->inMultiDocumentTransaction() && collection->isCapped()) {
        return {ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Collection '" << collection->ns()
                              << "' is a capped collection. Writes in transactions are not allowed "
                                 "on capped collections."};
    }
    return Status::OK();
}

/**
 * Returns true if caller should try to insert more documents. Does nothing else if batch is empty.
 */
bool insertBatchAndHandleErrors(OperationContext* opCtx,
                                const write_ops::Insert& wholeOp,
                                std::vector<InsertStatement>& batch,
                                LastOpFixer* lastOpFixer,
                                WriteResult* out,
                                bool fromMigrate) {
    if (batch.empty())
        return true;

    auto& curOp = *CurOp::get(opCtx);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchInsert,
        opCtx,
        "hangDuringBatchInsert",
        [&wholeOp]() {
            log() << "batch insert - hangDuringBatchInsert fail point enabled for namespace "
                  << wholeOp.getNamespace()
                  << ". Blocking "
                     "until fail point is disabled.";
        },
        true,  // Check for interrupt periodically.
        wholeOp.getNamespace());

    if (MONGO_FAIL_POINT(failAllInserts)) {
        uasserted(ErrorCodes::InternalError, "failAllInserts failpoint active!");
    }

    boost::optional<AutoGetCollection> collection;
    auto acquireCollection = [&] {
        while (true) {
            collection.emplace(
                opCtx,
                wholeOp.getNamespace(),
                fixLockModeForSystemDotViewsChanges(wholeOp.getNamespace(), MODE_IX));
            if (collection->getCollection())
                break;

            collection.reset();  // unlock.
            makeCollection(opCtx, wholeOp.getNamespace());
        }

        curOp.raiseDbProfileLevel(collection->getDb()->getProfilingLevel());
        assertCanWrite_inlock(opCtx, wholeOp.getNamespace());

        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangWithLockDuringBatchInsert, opCtx, "hangWithLockDuringBatchInsert");
    };

    try {
        acquireCollection();
        auto txnParticipant = TransactionParticipant::get(opCtx);
        auto inTxn = txnParticipant && opCtx->inMultiDocumentTransaction();
        if (!collection->getCollection()->isCapped() && !inTxn && batch.size() > 1) {
            // First try doing it all together. If all goes well, this is all we need to do.
            // See Collection::_insertDocuments for why we do all capped inserts one-at-a-time.
            lastOpFixer->startingOp();
            insertDocuments(
                opCtx, collection->getCollection(), batch.begin(), batch.end(), fromMigrate);
            lastOpFixer->finishedOpSuccessfully();
            globalOpCounters.gotInserts(batch.size());
            ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInserts(
                opCtx->getWriteConcern(), batch.size());
            SingleWriteResult result;
            result.setN(1);

            std::fill_n(std::back_inserter(out->results), batch.size(), std::move(result));
            curOp.debug().additiveMetrics.incrementNinserted(batch.size());
            return true;
        }
    } catch (const DBException&) {
        // Ignore this failure and behave as if we never tried to do the combined batch
        // insert. The loop below will handle reporting any non-transient errors.
        collection.reset();
    }

    // Try to insert the batch one-at-a-time. This path is executed for singular batches,
    // multi-statement transactions, capped collections, and if we failed all-at-once inserting.
    for (auto it = batch.begin(); it != batch.end(); ++it) {
        globalOpCounters.gotInsert();
        ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
            opCtx->getWriteConcern());
        try {
            writeConflictRetry(opCtx, "insert", wholeOp.getNamespace().ns(), [&] {
                try {
                    if (!collection)
                        acquireCollection();
                    // Transactions are not allowed to operate on capped collections.
                    uassertStatusOK(
                        checkIfTransactionOnCappedColl(opCtx, collection->getCollection()));
                    lastOpFixer->startingOp();
                    insertDocuments(opCtx, collection->getCollection(), it, it + 1, fromMigrate);
                    lastOpFixer->finishedOpSuccessfully();
                    SingleWriteResult result;
                    result.setN(1);
                    out->results.emplace_back(std::move(result));
                    curOp.debug().additiveMetrics.incrementNinserted(1);
                } catch (...) {
                    // Release the lock following any error if we are not in multi-statement
                    // transaction. Among other things, this ensures that we don't sleep in the WCE
                    // retry loop with the lock held.
                    // If we are in multi-statement transaction and under a WUOW, we will
                    // not actually release the lock.
                    collection.reset();
                    throw;
                }
            });
        } catch (const DBException& ex) {
            bool canContinue =
                handleError(opCtx, ex, wholeOp.getNamespace(), wholeOp.getWriteCommandBase(), out);

            if (!canContinue) {
                // Failed in ordered batch, or in a transaction, or from some unrecoverable error.
                return false;
            }
        }
    }

    return true;
}

template <typename T>
StmtId getStmtIdForWriteOp(OperationContext* opCtx, const T& wholeOp, size_t opIndex) {
    return opCtx->getTxnNumber() ? write_ops::getStmtIdForWriteAt(wholeOp, opIndex)
                                 : kUninitializedStmtId;
}

SingleWriteResult makeWriteResultForInsertOrDeleteRetry() {
    SingleWriteResult res;
    res.setN(1);
    res.setNModified(0);
    return res;
}

}  // namespace

WriteResult performInserts(OperationContext* opCtx,
                           const write_ops::Insert& wholeOp,
                           bool fromMigrate) {
    // Insert performs its own retries, so we should only be within a WriteUnitOfWork when run in a
    // transaction.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(!opCtx->lockState()->inAWriteUnitOfWork() ||
              (txnParticipant && opCtx->inMultiDocumentTransaction()));
    auto& curOp = *CurOp::get(opCtx);
    ON_BLOCK_EXIT([&] {
        // This is the only part of finishCurOp we need to do for inserts because they reuse the
        // top-level curOp. The rest is handled by the top-level entrypoint.
        curOp.done();
        Top::get(opCtx->getServiceContext())
            .record(opCtx,
                    wholeOp.getNamespace().ns(),
                    LogicalOp::opInsert,
                    Top::LockType::WriteLocked,
                    durationCount<Microseconds>(curOp.elapsedTimeExcludingPauses()),
                    curOp.isCommand(),
                    curOp.getReadWriteType());
    });

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS_inlock(wholeOp.getNamespace().ns());
        curOp.setLogicalOp_inlock(LogicalOp::opInsert);
        curOp.ensureStarted();
        curOp.debug().additiveMetrics.ninserted = 0;
    }

    uassertStatusOK(userAllowedWriteNS(wholeOp.getNamespace()));

    DisableDocumentValidationIfTrue docValidationDisabler(
        opCtx, wholeOp.getWriteCommandBase().getBypassDocumentValidation());
    LastOpFixer lastOpFixer(opCtx, wholeOp.getNamespace());

    WriteResult out;
    out.results.reserve(wholeOp.getDocuments().size());

    bool containsRetry = false;
    ON_BLOCK_EXIT([&] { updateRetryStats(opCtx, containsRetry); });

    size_t stmtIdIndex = 0;
    size_t bytesInBatch = 0;
    std::vector<InsertStatement> batch;
    const size_t maxBatchSize = internalInsertMaxBatchSize.load();
    const size_t maxBatchBytes = write_ops::insertVectorMaxBytes;
    batch.reserve(std::min(wholeOp.getDocuments().size(), maxBatchSize));

    for (auto&& doc : wholeOp.getDocuments()) {
        const bool isLastDoc = (&doc == &wholeOp.getDocuments().back());
        auto fixedDoc = fixDocumentForInsert(opCtx->getServiceContext(), doc);
        if (!fixedDoc.isOK()) {
            // Handled after we insert anything in the batch to be sure we report errors in the
            // correct order. In an ordered insert, if one of the docs ahead of us fails, we should
            // behave as-if we never got to this document.
        } else {
            const auto stmtId = getStmtIdForWriteOp(opCtx, wholeOp, stmtIdIndex++);
            if (opCtx->getTxnNumber()) {
                if (!opCtx->inMultiDocumentTransaction() &&
                    txnParticipant.checkStatementExecutedNoOplogEntryFetch(stmtId)) {
                    containsRetry = true;
                    RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                    out.results.emplace_back(makeWriteResultForInsertOrDeleteRetry());
                    continue;
                }
            }

            BSONObj toInsert = fixedDoc.getValue().isEmpty() ? doc : std::move(fixedDoc.getValue());
            batch.emplace_back(stmtId, toInsert);
            bytesInBatch += batch.back().doc.objsize();
            if (!isLastDoc && batch.size() < maxBatchSize && bytesInBatch < maxBatchBytes)
                continue;  // Add more to batch before inserting.
        }

        bool canContinue =
            insertBatchAndHandleErrors(opCtx, wholeOp, batch, &lastOpFixer, &out, fromMigrate);
        batch.clear();  // We won't need the current batch any more.
        bytesInBatch = 0;

        if (canContinue && !fixedDoc.isOK()) {
            globalOpCounters.gotInsert();
            ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
                opCtx->getWriteConcern());
            try {
                uassertStatusOK(fixedDoc.getStatus());
                MONGO_UNREACHABLE;
            } catch (const DBException& ex) {
                canContinue = handleError(
                    opCtx, ex, wholeOp.getNamespace(), wholeOp.getWriteCommandBase(), &out);
            }
        }

        if (!canContinue)
            break;
    }

    return out;
}

static SingleWriteResult performSingleUpdateOp(OperationContext* opCtx,
                                               const NamespaceString& ns,
                                               StmtId stmtId,
                                               const UpdateRequest& updateRequest) {
    const ExtensionsCallbackReal extensionsCallback(opCtx, &updateRequest.getNamespaceString());
    ParsedUpdate parsedUpdate(opCtx, &updateRequest, extensionsCallback);
    uassertStatusOK(parsedUpdate.parseRequest());

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchUpdate,
        opCtx,
        "hangDuringBatchUpdate",
        [&ns]() {
            log() << "batch update - hangDuringBatchUpdate fail point enabled for nss " << ns
                  << ". Blocking until "
                     "fail point is disabled.";
        },
        false /*checkForInterrupt*/,
        ns);

    if (MONGO_FAIL_POINT(failAllUpdates)) {
        uasserted(ErrorCodes::InternalError, "failAllUpdates failpoint active!");
    }

    boost::optional<AutoGetCollection> collection;
    while (true) {
        collection.emplace(opCtx, ns, MODE_IX, fixLockModeForSystemDotViewsChanges(ns, MODE_IX));

        // If this is an upsert, which is an insert, we must have a collection.
        // An update on a non-existant collection is okay and handled later.
        if (collection->getCollection() || !updateRequest.isUpsert())
            break;

        collection.reset();  // unlock.
        makeCollection(opCtx, ns);
    }

    if (auto coll = collection->getCollection()) {
        // Transactions are not allowed to operate on capped collections.
        uassertStatusOK(checkIfTransactionOnCappedColl(opCtx, coll));
    }

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWithLockDuringBatchUpdate, opCtx, "hangWithLockDuringBatchUpdate");

    auto& curOp = *CurOp::get(opCtx);

    if (collection->getDb()) {
        curOp.raiseDbProfileLevel(collection->getDb()->getProfilingLevel());
    }

    assertCanWrite_inlock(opCtx, ns);

    auto exec = uassertStatusOK(
        getExecutorUpdate(opCtx, &curOp.debug(), collection->getCollection(), &parsedUpdate));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
    }

    uassertStatusOK(exec->executePlan());

    PlanSummaryStats summary;
    Explain::getSummaryStats(*exec, &summary);
    if (collection->getCollection()) {
        collection->getCollection()->infoCache()->notifyOfQuery(opCtx, summary.indexesUsed);
    }

    if (curOp.shouldDBProfile()) {
        BSONObjBuilder execStatsBob;
        Explain::getWinningPlanStats(exec.get(), &execStatsBob);
        curOp.debug().execStats = execStatsBob.obj();
    }

    const UpdateStats* updateStats = UpdateStage::getUpdateStats(exec.get());
    UpdateStage::recordUpdateStatsInOpDebug(updateStats, &curOp.debug());
    curOp.debug().setPlanSummaryMetrics(summary);
    UpdateResult res = UpdateStage::makeUpdateResult(updateStats);

    const bool didInsert = !res.upserted.isEmpty();
    const long long nMatchedOrInserted = didInsert ? 1 : res.numMatched;
    LastError::get(opCtx->getClient()).recordUpdate(res.existing, nMatchedOrInserted, res.upserted);

    SingleWriteResult result;
    result.setN(nMatchedOrInserted);
    result.setNModified(res.numDocsModified);
    result.setUpsertedId(res.upserted);

    return result;
}

/**
 * Performs a single update, retrying failure due to DuplicateKeyError when eligible.
 */
static SingleWriteResult performSingleUpdateOpWithDupKeyRetry(OperationContext* opCtx,
                                                              const NamespaceString& ns,
                                                              StmtId stmtId,
                                                              const write_ops::UpdateOpEntry& op,
                                                              RuntimeConstants runtimeConstants) {
    globalOpCounters.gotUpdate();
    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForUpdate(opCtx->getWriteConcern());
    auto& curOp = *CurOp::get(opCtx);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS_inlock(ns.ns());
        curOp.setNetworkOp_inlock(dbUpdate);
        curOp.setLogicalOp_inlock(LogicalOp::opUpdate);
        curOp.setOpDescription_inlock(op.toBSON());
        curOp.ensureStarted();
    }

    uassert(ErrorCodes::InvalidOptions,
            "Cannot use (or request) retryable writes with multi=true",
            opCtx->inMultiDocumentTransaction() || !opCtx->getTxnNumber() || !op.getMulti());

    UpdateRequest request(ns);
    request.setQuery(op.getQ());
    request.setUpdateModification(op.getU());
    request.setUpdateConstants(op.getC());
    request.setRuntimeConstants(std::move(runtimeConstants));
    request.setCollation(write_ops::collationOf(op));
    request.setStmtId(stmtId);
    request.setArrayFilters(write_ops::arrayFiltersOf(op));
    request.setMulti(op.getMulti());
    request.setUpsert(op.getUpsert());
    request.setUpsertSuppliedDocument(op.getUpsertSupplied());
    request.setHint(op.getHint());

    request.setYieldPolicy(opCtx->inMultiDocumentTransaction() ? PlanExecutor::INTERRUPT_ONLY
                                                               : PlanExecutor::YIELD_AUTO);

    size_t numAttempts = 0;

    // Upper bound on the number of retries allowed. This prevents the server from getting stuck
    // in an infinite loop when trying to upsert invalid BSON (see SERVER-60897 for details).
    constexpr size_t kMaxNumAttempts = 1000;
    while (true) {
        ++numAttempts;

        try {
            return performSingleUpdateOp(opCtx, ns, stmtId, request);
        } catch (ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
            const ExtensionsCallbackReal extensionsCallback(opCtx, &request.getNamespaceString());
            ParsedUpdate parsedUpdate(opCtx, &request, extensionsCallback);
            uassertStatusOK(parsedUpdate.parseRequest());

            if (!parsedUpdate.hasParsedQuery()) {
                uassertStatusOK(parsedUpdate.parseQueryToCQ());
            }

            if (numAttempts >= kMaxNumAttempts) {
                throw;
            }

            if (!UpdateStage::shouldRetryDuplicateKeyException(
                    parsedUpdate, *ex.extraInfo<DuplicateKeyErrorInfo>())) {
                throw;
            }

            logAndBackoff(::mongo::logger::LogComponent::kWrite,
                          logger::LogSeverity::Debug(1),
                          numAttempts,
                          str::stream()
                              << "Caught DuplicateKey exception during upsert for namespace "
                              << ns.ns());
        }
    }

    MONGO_UNREACHABLE;
}

WriteResult performUpdates(OperationContext* opCtx, const write_ops::Update& wholeOp) {
    // Update performs its own retries, so we should not be in a WriteUnitOfWork unless run in a
    // transaction.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(!opCtx->lockState()->inAWriteUnitOfWork() ||
              (txnParticipant && opCtx->inMultiDocumentTransaction()));
    uassertStatusOK(userAllowedWriteNS(wholeOp.getNamespace()));

    DisableDocumentValidationIfTrue docValidationDisabler(
        opCtx, wholeOp.getWriteCommandBase().getBypassDocumentValidation());
    LastOpFixer lastOpFixer(opCtx, wholeOp.getNamespace());

    bool containsRetry = false;
    ON_BLOCK_EXIT([&] { updateRetryStats(opCtx, containsRetry); });

    size_t stmtIdIndex = 0;
    WriteResult out;
    out.results.reserve(wholeOp.getUpdates().size());

    // If the update command specified runtime constants, we adopt them. Otherwise, we set them to
    // the current local and cluster time. These constants are applied to each update in the batch.
    const auto& runtimeConstants =
        wholeOp.getRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx));

    for (auto&& singleOp : wholeOp.getUpdates()) {
        const auto stmtId = getStmtIdForWriteOp(opCtx, wholeOp, stmtIdIndex++);
        if (opCtx->getTxnNumber()) {
            if (!opCtx->inMultiDocumentTransaction()) {
                if (auto entry = txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
                    containsRetry = true;
                    RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                    out.results.emplace_back(parseOplogEntryForUpdate(*entry));
                    continue;
                }
            }
        }

        // TODO: don't create nested CurOp for legacy writes.
        // Add Command pointer to the nested CurOp.
        auto& parentCurOp = *CurOp::get(opCtx);
        const Command* cmd = parentCurOp.getCommand();
        CurOp curOp(opCtx);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp.setCommand_inlock(cmd);
        }
        ON_BLOCK_EXIT([&] { finishCurOp(opCtx, &curOp); });
        try {
            lastOpFixer.startingOp();
            out.results.emplace_back(performSingleUpdateOpWithDupKeyRetry(
                opCtx, wholeOp.getNamespace(), stmtId, singleOp, runtimeConstants));
            lastOpFixer.finishedOpSuccessfully();
        } catch (const DBException& ex) {
            const bool canContinue =
                handleError(opCtx, ex, wholeOp.getNamespace(), wholeOp.getWriteCommandBase(), &out);
            if (!canContinue)
                break;
        }
    }

    return out;
}

static SingleWriteResult performSingleDeleteOp(OperationContext* opCtx,
                                               const NamespaceString& ns,
                                               StmtId stmtId,
                                               const write_ops::DeleteOpEntry& op) {
    uassert(ErrorCodes::InvalidOptions,
            "Cannot use (or request) retryable writes with limit=0",
            opCtx->inMultiDocumentTransaction() || !opCtx->getTxnNumber() || !op.getMulti());

    globalOpCounters.gotDelete();
    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForDelete(opCtx->getWriteConcern());
    auto& curOp = *CurOp::get(opCtx);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS_inlock(ns.ns());
        curOp.setNetworkOp_inlock(dbDelete);
        curOp.setLogicalOp_inlock(LogicalOp::opDelete);
        curOp.setOpDescription_inlock(op.toBSON());
        curOp.ensureStarted();
    }

    DeleteRequest request(ns);
    request.setQuery(op.getQ());
    request.setCollation(write_ops::collationOf(op));
    request.setMulti(op.getMulti());
    request.setYieldPolicy(opCtx->inMultiDocumentTransaction() ? PlanExecutor::INTERRUPT_ONLY
                                                               : PlanExecutor::YIELD_AUTO);
    request.setStmtId(stmtId);

    ParsedDelete parsedDelete(opCtx, &request);
    uassertStatusOK(parsedDelete.parseRequest());

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchRemove,
        opCtx,
        "hangDuringBatchRemove",
        []() {
            log() << "batch remove - hangDuringBatchRemove fail point enabled. Blocking "
                     "until fail point is disabled.";
        },
        true  // Check for interrupt periodically.
    );
    if (MONGO_FAIL_POINT(failAllRemoves)) {
        uasserted(ErrorCodes::InternalError, "failAllRemoves failpoint active!");
    }

    AutoGetCollection collection(
        opCtx, ns, MODE_IX, fixLockModeForSystemDotViewsChanges(ns, MODE_IX));

    if (collection.getDb()) {
        curOp.raiseDbProfileLevel(collection.getDb()->getProfilingLevel());
    }

    assertCanWrite_inlock(opCtx, ns);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWithLockDuringBatchRemove, opCtx, "hangWithLockDuringBatchRemove");

    auto exec = uassertStatusOK(
        getExecutorDelete(opCtx, &curOp.debug(), collection.getCollection(), &parsedDelete));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
    }

    uassertStatusOK(exec->executePlan());
    long long n = DeleteStage::getNumDeleted(*exec);
    curOp.debug().additiveMetrics.ndeleted = n;

    PlanSummaryStats summary;
    Explain::getSummaryStats(*exec, &summary);
    if (collection.getCollection()) {
        collection.getCollection()->infoCache()->notifyOfQuery(opCtx, summary.indexesUsed);
    }
    curOp.debug().setPlanSummaryMetrics(summary);

    if (curOp.shouldDBProfile()) {
        BSONObjBuilder execStatsBob;
        Explain::getWinningPlanStats(exec.get(), &execStatsBob);
        curOp.debug().execStats = execStatsBob.obj();
    }

    LastError::get(opCtx->getClient()).recordDelete(n);

    SingleWriteResult result;
    result.setN(n);
    return result;
}

WriteResult performDeletes(OperationContext* opCtx, const write_ops::Delete& wholeOp) {
    // Delete performs its own retries, so we should not be in a WriteUnitOfWork unless we are in a
    // transaction.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(!opCtx->lockState()->inAWriteUnitOfWork() ||
              (txnParticipant && opCtx->inMultiDocumentTransaction()));
    uassertStatusOK(userAllowedWriteNS(wholeOp.getNamespace()));

    DisableDocumentValidationIfTrue docValidationDisabler(
        opCtx, wholeOp.getWriteCommandBase().getBypassDocumentValidation());
    LastOpFixer lastOpFixer(opCtx, wholeOp.getNamespace());

    bool containsRetry = false;
    ON_BLOCK_EXIT([&] { updateRetryStats(opCtx, containsRetry); });

    size_t stmtIdIndex = 0;
    WriteResult out;
    out.results.reserve(wholeOp.getDeletes().size());

    for (auto&& singleOp : wholeOp.getDeletes()) {
        const auto stmtId = getStmtIdForWriteOp(opCtx, wholeOp, stmtIdIndex++);
        if (opCtx->getTxnNumber()) {
            if (!opCtx->inMultiDocumentTransaction() &&
                txnParticipant.checkStatementExecutedNoOplogEntryFetch(stmtId)) {
                containsRetry = true;
                RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                out.results.emplace_back(makeWriteResultForInsertOrDeleteRetry());
                continue;
            }
        }

        // TODO: don't create nested CurOp for legacy writes.
        // Add Command pointer to the nested CurOp.
        auto& parentCurOp = *CurOp::get(opCtx);
        const Command* cmd = parentCurOp.getCommand();
        CurOp curOp(opCtx);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp.setCommand_inlock(cmd);
        }
        ON_BLOCK_EXIT([&] {
            if (MONGO_FAIL_POINT(hangBeforeChildRemoveOpFinishes)) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeChildRemoveOpFinishes, opCtx, "hangBeforeChildRemoveOpFinishes");
            }
            finishCurOp(opCtx, &curOp);
            if (MONGO_FAIL_POINT(hangBeforeChildRemoveOpIsPopped)) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeChildRemoveOpIsPopped, opCtx, "hangBeforeChildRemoveOpIsPopped");
            }
        });
        try {
            lastOpFixer.startingOp();
            out.results.emplace_back(
                performSingleDeleteOp(opCtx, wholeOp.getNamespace(), stmtId, singleOp));
            lastOpFixer.finishedOpSuccessfully();
        } catch (const DBException& ex) {
            const bool canContinue =
                handleError(opCtx, ex, wholeOp.getNamespace(), wholeOp.getWriteCommandBase(), &out);
            if (!canContinue)
                break;
        }
    }

    if (MONGO_FAIL_POINT(hangAfterAllChildRemoveOpsArePopped)) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangAfterAllChildRemoveOpsArePopped, opCtx, "hangAfterAllChildRemoveOpsArePopped");
    }

    return out;
}

}  // namespace mongo

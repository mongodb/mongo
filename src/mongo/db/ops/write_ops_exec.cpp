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


#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/checked_cast.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/introspect.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo::write_ops_exec {

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
MONGO_FAIL_POINT_DEFINE(hangAfterBatchUpdate);
MONGO_FAIL_POINT_DEFINE(hangDuringBatchRemove);
MONGO_FAIL_POINT_DEFINE(hangAndFailAfterDocumentInsertsReserveOpTimes);
// The withLock fail points are for testing interruptability of these operations, so they will not
// themselves check for interrupt.
MONGO_FAIL_POINT_DEFINE(hangWithLockDuringBatchInsert);
MONGO_FAIL_POINT_DEFINE(hangWithLockDuringBatchUpdate);
MONGO_FAIL_POINT_DEFINE(hangWithLockDuringBatchRemove);
MONGO_FAIL_POINT_DEFINE(failAtomicTimeseriesWrites);

void updateRetryStats(OperationContext* opCtx, bool containsRetry) {
    if (containsRetry) {
        RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
    }
}

void finishCurOp(OperationContext* opCtx, CurOp* curOp) {
    try {
        curOp->done();
        auto executionTimeMicros = duration_cast<Microseconds>(curOp->elapsedTimeExcludingPauses());
        curOp->debug().executionTime = executionTimeMicros;

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
            LOGV2_DEBUG(20886,
                        3,
                        "Caught Assertion in finishCurOp. Op: {operation}, error: {error}",
                        "Caught Assertion in finishCurOp",
                        "operation"_attr = redact(logicalOpToString(curOp->getLogicalOp())),
                        "error"_attr = curOp->debug().errInfo.toString());
        }

        // Mark the op as complete, and log it if appropriate. Returns a boolean indicating whether
        // this op should be sampled for profiling.
        const bool shouldProfile =
            curOp->completeAndLogOperation(opCtx, MONGO_LOGV2_DEFAULT_COMPONENT);

        if (shouldProfile) {
            // Stash the current transaction so that writes to the profile collection are not
            // done as part of the transaction.
            TransactionParticipant::SideTransactionBlock sideTxn(opCtx);
            profile(opCtx, CurOp::get(opCtx)->getNetworkOp());
        }
    } catch (const DBException& ex) {
        // We need to ignore all errors here. We don't want a successful op to fail because of a
        // failure to record stats. We also don't want to replace the error reported for an op that
        // is failing.
        LOGV2(20887,
              "Ignoring error from finishCurOp: {error}",
              "Ignoring error from finishCurOp",
              "error"_attr = redact(ex));
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
        // We don't need to do this if we are in a multi-document transaction as read-only/noop
        // transactions will always write another noop entry at transaction commit time which we can
        // use to wait for writeConcern.
        if (!_opCtx->inMultiDocumentTransaction() && _needToFixLastOp && !_isOnLocalDb) {
            // If this operation has already generated a new lastOp, don't bother setting it
            // here. No-op updates will not generate a new lastOp, so we still need the
            // guard to fire in that case. Operations on the local DB aren't replicated, so they
            // don't need to bump the lastOp.
            replClientInfo().setLastOpToSystemLastOpTimeIgnoringInterrupt(_opCtx);
            LOGV2_DEBUG(20888,
                        5,
                        "Set last op to system time: {timestamp}",
                        "Set last op to system time",
                        "timestamp"_attr = replClientInfo().getLastOp().getTimestamp());
        }
    }

    void startingOp() {
        _needToFixLastOp = true;
        _opTimeAtLastOpStart = replClientInfo().getLastOp();
    }

    void finishedOpSuccessfully() {
        // If the op was successful and bumped LastOp, we don't need to do it again. However, we
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
    writeConflictRetry(opCtx, "implicit collection creation", ns.ns(), [&opCtx, &ns] {
        AutoGetDb autoDb(opCtx, ns.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, ns, MODE_IX);

        assertCanWrite_inlock(opCtx, ns);
        if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
                opCtx, ns)) {  // someone else may have beat us to it.
            uassertStatusOK(userAllowedCreateNS(opCtx, ns));
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx);
            WriteUnitOfWork wuow(opCtx);
            CollectionOptions defaultCollectionOptions;
            if (auto fp = globalFailPointRegistry().find("clusterAllCollectionsByDefault"); fp &&
                fp->shouldFail() &&
                feature_flags::gClusteredIndexes.isEnabled(
                    serverGlobalParams.featureCompatibility) &&
                !clustered_util::requiresLegacyFormat(ns)) {
                defaultCollectionOptions.clusteredIndex =
                    clustered_util::makeDefaultClusteredIdIndex();
            }
            auto db = autoDb.ensureDbExists(opCtx);
            uassertStatusOK(db->userCreateNS(opCtx, ns, defaultCollectionOptions));
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
                 const write_ops::WriteCommandRequestBase& wholeOp,
                 bool isMultiUpdate,
                 WriteResult* out) {
    NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
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
                ex.code(), false /* hasWriteConcernError */, false /* isCommitOrAbort */)) {
            // Tell the client to try the whole txn again, by returning ok: 0 with errorLabels.
            throw;
        }
        // If we are in a transaction, we must fail the whole batch.
        out->results.emplace_back(ex.toStatus());
        txnParticipant.abortTransaction(opCtx);
        return false;
    }

    if (ex.code() == ErrorCodes::StaleDbVersion || ErrorCodes::isStaleShardVersionError(ex)) {
        if (!opCtx->getClient()->isInDirectClient()) {
            auto& oss = OperationShardingState::get(opCtx);
            oss.setShardingOperationFailedStatus(ex.toStatus());
        }

        // Since this is a routing error, it is guaranteed that all subsequent operations will fail
        // with the same cause, so don't try doing any more operations. The command reply serializer
        // will handle repeating this error for unordered writes.
        out->results.emplace_back(ex.toStatus());
        return false;
    }

    if (ErrorCodes::isTenantMigrationError(ex)) {
        // Multiple not-idempotent updates are not safe to retry at the cloud level. We treat these
        // the same as an interruption due to a repl state change and fail the whole batch.
        if (isMultiUpdate) {
            if (ex.code() != ErrorCodes::TenantMigrationConflict) {
                uassertStatusOK(kNonRetryableTenantMigrationStatus);
            }

            // If the migration is active, we throw a different code that will be caught higher up
            // and replaced with a non-retryable code after the migration finishes to avoid wasted
            // retries.
            auto migrationConflictInfo = ex.toStatus().extraInfo<TenantMigrationConflictInfo>();
            uassertStatusOK(
                Status(NonRetryableTenantMigrationConflictInfo(
                           migrationConflictInfo->getTenantId(),
                           migrationConflictInfo->getTenantMigrationAccessBlocker()),
                       "Multi update must block until this tenant migration commits or aborts"));
        }

        // If an op fails due to a TenantMigrationError then subsequent ops will also fail due to a
        // migration blocking, committing, or aborting.
        out->results.emplace_back(ex.toStatus());
        return false;
    }

    if (ex.code() == ErrorCodes::ShardCannotRefreshDueToLocksHeld) {
        throw;
    }

    out->results.emplace_back(ex.toStatus());
    return !wholeOp.getOrdered();
}

void insertDocuments(OperationContext* opCtx,
                     const CollectionPtr& collection,
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

    hangAndFailAfterDocumentInsertsReserveOpTimes.executeIf(
        [&](const BSONObj& data) {
            hangAndFailAfterDocumentInsertsReserveOpTimes.pauseWhileSet(opCtx);
            const auto skipFail = data["skipFail"];
            if (!skipFail || !skipFail.boolean()) {
                uasserted(51269,
                          "hangAndFailAfterDocumentInsertsReserveOpTimes fail point enabled");
            }
        },
        [&](const BSONObj& data) {
            // Check if the failpoint specifies no collection or matches the existing one.
            const auto collElem = data["collectionNS"];
            return !collElem || collection->ns().ns() == collElem.str();
        });

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
Status checkIfTransactionOnCappedColl(OperationContext* opCtx, const CollectionPtr& collection) {
    if (opCtx->inMultiDocumentTransaction() && collection->isCapped()) {
        return {ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Collection '" << collection->ns()
                              << "' is a capped collection. Writes in transactions are not allowed "
                                 "on capped collections."};
    }
    return Status::OK();
}

void assertTimeseriesBucketsCollectionNotFound(const NamespaceString& ns) {
    uasserted(ErrorCodes::NamespaceNotFound,
              str::stream() << "Buckets collection not found for time-series collection "
                            << ns.getTimeseriesViewNamespace());
}

/**
 * Returns true if caller should try to insert more documents. Does nothing else if batch is empty.
 */
bool insertBatchAndHandleErrors(OperationContext* opCtx,
                                const write_ops::InsertCommandRequest& wholeOp,
                                std::vector<InsertStatement>& batch,
                                LastOpFixer* lastOpFixer,
                                WriteResult* out,
                                OperationSource source) {
    if (batch.empty())
        return true;

    auto& curOp = *CurOp::get(opCtx);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchInsert,
        opCtx,
        "hangDuringBatchInsert",
        [&wholeOp]() {
            LOGV2(20889,
                  "Batch insert - hangDuringBatchInsert fail point enabled for namespace "
                  "{namespace}. Blocking until fail point is disabled",
                  "Batch insert - hangDuringBatchInsert fail point enabled for a namespace. "
                  "Blocking until fail point is disabled",
                  "namespace"_attr = wholeOp.getNamespace());
        },
        wholeOp.getNamespace());

    if (MONGO_unlikely(failAllInserts.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllInserts failpoint active!");
    }

    boost::optional<AutoGetCollection> collection;
    auto acquireCollection = [&] {
        while (true) {
            collection.emplace(
                opCtx,
                wholeOp.getNamespace(),
                fixLockModeForSystemDotViewsChanges(wholeOp.getNamespace(), MODE_IX));
            checkCollectionUUIDMismatch(opCtx,
                                        wholeOp.getNamespace(),
                                        collection->getCollection(),
                                        wholeOp.getCollectionUUID());
            if (*collection) {
                break;
            }

            if (source == OperationSource::kTimeseriesInsert) {
                assertTimeseriesBucketsCollectionNotFound(wholeOp.getNamespace());
            }

            collection.reset();  // unlock.
            makeCollection(opCtx, wholeOp.getNamespace());
        }

        curOp.raiseDbProfileLevel(CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(
            wholeOp.getNamespace().dbName()));
        assertCanWrite_inlock(opCtx, wholeOp.getNamespace());

        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangWithLockDuringBatchInsert, opCtx, "hangWithLockDuringBatchInsert");
    };

    auto txnParticipant = TransactionParticipant::get(opCtx);
    auto inTxn = txnParticipant && opCtx->inMultiDocumentTransaction();
    bool shouldProceedWithBatchInsert = true;

    try {
        acquireCollection();
    } catch (const DBException& ex) {
        collection.reset();
        if (inTxn) {
            // It is not safe to ignore errors from collection creation while inside a
            // multi-document transaction.
            auto canContinue = handleError(opCtx,
                                           ex,
                                           wholeOp.getNamespace(),
                                           wholeOp.getWriteCommandRequestBase(),
                                           false /* multiUpdate */,
                                           out);
            invariant(!canContinue);
            return false;
        }
        // Otherwise, proceed as though the batch insert block failed, since the batch insert block
        // assumes `acquireCollection` is successful.
        shouldProceedWithBatchInsert = false;
    }

    if (shouldProceedWithBatchInsert) {
        try {
            if (!collection->getCollection()->isCapped() && !inTxn && batch.size() > 1) {
                // First try doing it all together. If all goes well, this is all we need to do.
                // See Collection::_insertDocuments for why we do all capped inserts one-at-a-time.
                lastOpFixer->startingOp();
                insertDocuments(opCtx,
                                collection->getCollection(),
                                batch.begin(),
                                batch.end(),
                                source == OperationSource::kFromMigrate);
                lastOpFixer->finishedOpSuccessfully();
                globalOpCounters.gotInserts(batch.size());
                ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInserts(
                    opCtx->getWriteConcern(), batch.size());
                SingleWriteResult result;
                result.setN(1);

                std::fill_n(std::back_inserter(out->results), batch.size(), std::move(result));
                if (source != OperationSource::kTimeseriesInsert) {
                    curOp.debug().additiveMetrics.incrementNinserted(batch.size());
                }
                return true;
            }
        } catch (const DBException&) {
            // Ignore this failure and behave as if we never tried to do the combined batch
            // insert. The loop below will handle reporting any non-transient errors.
            collection.reset();
        }
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
                    insertDocuments(opCtx,
                                    collection->getCollection(),
                                    it,
                                    it + 1,
                                    source == OperationSource::kFromMigrate);
                    lastOpFixer->finishedOpSuccessfully();
                    SingleWriteResult result;
                    result.setN(1);
                    out->results.emplace_back(std::move(result));
                    if (source != OperationSource::kTimeseriesInsert) {
                        curOp.debug().additiveMetrics.incrementNinserted(1);
                    }
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
            bool canContinue = handleError(opCtx,
                                           ex,
                                           wholeOp.getNamespace(),
                                           wholeOp.getWriteCommandRequestBase(),
                                           false /* multiUpdate */,
                                           out);

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
    return opCtx->isRetryableWrite() ? write_ops::getStmtIdForWriteAt(wholeOp, opIndex)
                                     : kUninitializedStmtId;
}

SingleWriteResult makeWriteResultForInsertOrDeleteRetry() {
    SingleWriteResult res;
    res.setN(1);
    res.setNModified(0);
    return res;
}


// Returns the flags that determine the type of document validation we want to
// perform. First item in the tuple determines whether to bypass document validation altogether,
// second item determines if _safeContent_ array can be modified in an encrypted collection.
std::tuple<bool, bool> getDocumentValidationFlags(OperationContext* opCtx,
                                                  const write_ops::WriteCommandRequestBase& req) {
    auto& encryptionInfo = req.getEncryptionInformation();
    const bool fleCrudProcessed = getFleCrudProcessed(opCtx, encryptionInfo);
    return std::make_tuple(req.getBypassDocumentValidation(), fleCrudProcessed);
}
}  // namespace

bool getFleCrudProcessed(OperationContext* opCtx,
                         const boost::optional<EncryptionInformation>& encryptionInfo) {
    if (encryptionInfo && encryptionInfo->getCrudProcessed().value_or(false)) {
        uassert(6666201,
                "External users cannot have crudProcessed enabled",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                       ActionType::internal));

        return true;
    }
    return false;
}

WriteResult performInserts(OperationContext* opCtx,
                           const write_ops::InsertCommandRequest& wholeOp,
                           OperationSource source) {

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

    if (source != OperationSource::kTimeseriesInsert) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS_inlock(wholeOp.getNamespace().ns());
        curOp.setLogicalOp_inlock(LogicalOp::opInsert);
        curOp.ensureStarted();
        curOp.debug().additiveMetrics.ninserted = 0;
    }

    // If we are performing inserts from tenant migrations, skip checking if the user is allowed to
    // write to the namespace.
    if (!repl::tenantMigrationRecipientInfo(opCtx)) {
        uassertStatusOK(userAllowedWriteNS(opCtx, wholeOp.getNamespace()));
    }

    const auto [disableDocumentValidation, fleCrudProcessed] =
        getDocumentValidationFlags(opCtx, wholeOp.getWriteCommandRequestBase());

    DisableDocumentSchemaValidationIfTrue docSchemaValidationDisabler(opCtx,
                                                                      disableDocumentValidation);

    DisableSafeContentValidationIfTrue safeContentValidationDisabler(
        opCtx, disableDocumentValidation, fleCrudProcessed);

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
        bool containsDotsAndDollarsField = false;
        auto fixedDoc = fixDocumentForInsert(opCtx, doc, &containsDotsAndDollarsField);
        const StmtId stmtId = getStmtIdForWriteOp(opCtx, wholeOp, stmtIdIndex++);
        const bool wasAlreadyExecuted = opCtx->isRetryableWrite() &&
            txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx, stmtId);

        if (!fixedDoc.isOK()) {
            // Handled after we insert anything in the batch to be sure we report errors in the
            // correct order. In an ordered insert, if one of the docs ahead of us fails, we should
            // behave as-if we never got to this document.
        } else if (wasAlreadyExecuted) {
            // Similarly, if the insert was already executed as part of a retryable write, flush the
            // current batch to preserve the error results order.
        } else {
            BSONObj toInsert = fixedDoc.getValue().isEmpty() ? doc : std::move(fixedDoc.getValue());

            if (containsDotsAndDollarsField)
                dotsAndDollarsFieldsCounters.inserts.increment();

            // A time-series insert can combine multiple writes into a single operation, and thus
            // can have multiple statement ids associated with it if it is retryable.
            batch.emplace_back(source == OperationSource::kTimeseriesInsert && wholeOp.getStmtIds()
                                   ? *wholeOp.getStmtIds()
                                   : std::vector<StmtId>{stmtId},
                               toInsert);

            bytesInBatch += batch.back().doc.objsize();

            if (!isLastDoc && batch.size() < maxBatchSize && bytesInBatch < maxBatchBytes)
                continue;  // Add more to batch before inserting.
        }

        out.canContinue =
            insertBatchAndHandleErrors(opCtx, wholeOp, batch, &lastOpFixer, &out, source);
        batch.clear();  // We won't need the current batch any more.
        bytesInBatch = 0;

        // If the batch had an error and decides to not continue, do not process a current doc that
        // was unsuccessfully "fixed" or an already executed retryable write.
        if (!out.canContinue) {
            break;
        }

        // Revisit any conditions that may have caused the batch to be flushed. In those cases,
        // append the appropriate result to the output.
        if (!fixedDoc.isOK()) {
            globalOpCounters.gotInsert();
            ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
                opCtx->getWriteConcern());
            try {
                uassertStatusOK(fixedDoc.getStatus());
                MONGO_UNREACHABLE;
            } catch (const DBException& ex) {
                out.canContinue = handleError(opCtx,
                                              ex,
                                              wholeOp.getNamespace(),
                                              wholeOp.getWriteCommandRequestBase(),
                                              false /* multiUpdate */,
                                              &out);
            }

            if (!out.canContinue) {
                break;
            }
        } else if (wasAlreadyExecuted) {
            containsRetry = true;
            RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
            out.retriedStmtIds.push_back(stmtId);
            out.results.emplace_back(makeWriteResultForInsertOrDeleteRetry());
        }
    }
    invariant(batch.empty());

    return out;
}

static SingleWriteResult performSingleUpdateOp(OperationContext* opCtx,
                                               const NamespaceString& ns,
                                               const boost::optional<mongo::UUID>& opCollectionUUID,
                                               UpdateRequest* updateRequest,
                                               OperationSource source,
                                               bool* containsDotsAndDollarsField,
                                               bool forgoOpCounterIncrements) {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchUpdate,
        opCtx,
        "hangDuringBatchUpdate",
        [&ns]() {
            LOGV2(20890,
                  "Batch update - hangDuringBatchUpdate fail point enabled for namespace "
                  "{namespace}. Blocking until fail point is disabled",
                  "Batch update - hangDuringBatchUpdate fail point enabled for a namespace. "
                  "Blocking until fail point is disabled",
                  "namespace"_attr = ns);
        },
        ns);

    if (MONGO_unlikely(failAllUpdates.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllUpdates failpoint active!");
    }

    boost::optional<AutoGetCollection> collection;
    while (true) {
        collection.emplace(opCtx, ns, fixLockModeForSystemDotViewsChanges(ns, MODE_IX));
        checkCollectionUUIDMismatch(opCtx, ns, collection->getCollection(), opCollectionUUID);
        if (*collection) {
            break;
        }

        if (source == OperationSource::kTimeseriesInsert ||
            source == OperationSource::kTimeseriesUpdate) {
            assertTimeseriesBucketsCollectionNotFound(ns);
        }

        // If this is an upsert, which is an insert, we must have a collection.
        // An update on a non-existent collection is okay and handled later.
        if (!updateRequest->isUpsert())
            break;

        collection.reset();  // unlock.
        makeCollection(opCtx, ns);
    }

    UpdateStageParams::DocumentCounter documentCounter = nullptr;

    if (source == OperationSource::kTimeseriesUpdate) {
        uassert(ErrorCodes::NamespaceNotFound,
                "Could not find time-series buckets collection for update",
                collection);

        auto timeseriesOptions = collection->getCollection()->getTimeseriesOptions();
        uassert(ErrorCodes::InvalidOptions,
                "Time-series buckets collection is missing time-series options",
                timeseriesOptions);

        auto metaField = timeseriesOptions->getMetaField();
        uassert(
            ErrorCodes::InvalidOptions,
            "Cannot perform an update on a time-series collection that does not have a metaField",
            metaField);

        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform a non-multi update on a time-series collection",
                updateRequest->isMulti());

        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform an upsert on a time-series collection",
                !updateRequest->isUpsert());

        // Only translate the hint if it is specified with an index key.
        if (timeseries::isHintIndexKey(updateRequest->getHint())) {
            updateRequest->setHint(
                uassertStatusOK(timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
                    *timeseriesOptions, updateRequest->getHint())));
        }

        updateRequest->setQuery(timeseries::translateQuery(updateRequest->getQuery(), *metaField));
        updateRequest->setUpdateModification(
            timeseries::translateUpdate(updateRequest->getUpdateModification(), *metaField));

        documentCounter =
            timeseries::numMeasurementsForBucketCounter(timeseriesOptions->getTimeField());
    }

    if (const auto& coll = collection->getCollection()) {
        // Transactions are not allowed to operate on capped collections.
        uassertStatusOK(checkIfTransactionOnCappedColl(opCtx, coll));
    }

    const ExtensionsCallbackReal extensionsCallback(opCtx, &updateRequest->getNamespaceString());
    ParsedUpdate parsedUpdate(opCtx, updateRequest, extensionsCallback, forgoOpCounterIncrements);
    uassertStatusOK(parsedUpdate.parseRequest());

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWithLockDuringBatchUpdate, opCtx, "hangWithLockDuringBatchUpdate");

    auto& curOp = *CurOp::get(opCtx);

    if (collection->getDb()) {
        curOp.raiseDbProfileLevel(
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));
    }

    assertCanWrite_inlock(opCtx, ns);

    auto exec = uassertStatusOK(
        getExecutorUpdate(&curOp.debug(),
                          collection ? &collection->getCollection() : &CollectionPtr::null,
                          &parsedUpdate,
                          boost::none /* verbosity */,
                          std::move(documentCounter)));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    auto updateResult = exec->executeUpdate();

    PlanSummaryStats summary;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summary);
    if (const auto& coll = collection->getCollection()) {
        CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, summary);
    }

    if (curOp.shouldDBProfile(opCtx)) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp.debug().execStats = std::move(stats);
    }

    if (source != OperationSource::kTimeseriesInsert &&
        source != OperationSource::kTimeseriesUpdate) {
        recordUpdateResultInOpDebug(updateResult, &curOp.debug());
    }
    curOp.debug().setPlanSummaryMetrics(summary);

    const bool didInsert = !updateResult.upsertedId.isEmpty();
    const long long nMatchedOrInserted = didInsert ? 1 : updateResult.numMatched;
    SingleWriteResult result;
    result.setN(nMatchedOrInserted);
    result.setNModified(updateResult.numDocsModified);
    result.setUpsertedId(updateResult.upsertedId);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangAfterBatchUpdate, opCtx, "hangAfterBatchUpdate");

    if (containsDotsAndDollarsField && updateResult.containsDotsAndDollarsField) {
        *containsDotsAndDollarsField = true;
    }

    return result;
}

/**
 * Performs a single update, retrying failure due to DuplicateKeyError when eligible.
 */
static SingleWriteResult performSingleUpdateOpWithDupKeyRetry(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const boost::optional<mongo::UUID>& opCollectionUUID,
    const std::vector<StmtId>& stmtIds,
    const write_ops::UpdateOpEntry& op,
    LegacyRuntimeConstants runtimeConstants,
    const boost::optional<BSONObj>& letParams,
    OperationSource source,
    bool forgoOpCounterIncrements) {
    globalOpCounters.gotUpdate();
    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForUpdate(opCtx->getWriteConcern());
    auto& curOp = *CurOp::get(opCtx);
    if (source != OperationSource::kTimeseriesInsert) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS_inlock(source == OperationSource::kTimeseriesUpdate
                               ? ns.getTimeseriesViewNamespace().ns()
                               : ns.ns());
        curOp.setNetworkOp_inlock(dbUpdate);
        curOp.setLogicalOp_inlock(LogicalOp::opUpdate);
        curOp.setOpDescription_inlock(op.toBSON());
        curOp.ensureStarted();
    }

    uassert(ErrorCodes::InvalidOptions,
            "Cannot use (or request) retryable writes with multi=true",
            !opCtx->isRetryableWrite() || !op.getMulti() ||
                // If the first stmtId is uninitialized, we assume all are.
                (stmtIds.empty() || stmtIds.front() == kUninitializedStmtId));

    UpdateRequest request(op);
    request.setNamespaceString(ns);
    request.setLegacyRuntimeConstants(std::move(runtimeConstants));
    if (letParams) {
        request.setLetParameters(std::move(letParams));
    }
    request.setStmtIds(stmtIds);
    request.setYieldPolicy(opCtx->inMultiDocumentTransaction()
                               ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                               : PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    request.setSource(source);

    size_t numAttempts = 0;
    while (true) {
        ++numAttempts;

        try {
            bool containsDotsAndDollarsField = false;
            const auto ret = performSingleUpdateOp(opCtx,
                                                   ns,
                                                   opCollectionUUID,
                                                   &request,
                                                   source,
                                                   &containsDotsAndDollarsField,
                                                   forgoOpCounterIncrements);

            if (containsDotsAndDollarsField) {
                // If it's an upsert, increment 'inserts' metric, otherwise increment 'updates'.
                dotsAndDollarsFieldsCounters.incrementForUpsert(!ret.getUpsertedId().isEmpty());
            }

            return ret;
        } catch (ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
            const ExtensionsCallbackReal extensionsCallback(opCtx, &request.getNamespaceString());
            ParsedUpdate parsedUpdate(opCtx, &request, extensionsCallback);
            uassertStatusOK(parsedUpdate.parseRequest());

            if (!parsedUpdate.hasParsedQuery()) {
                uassertStatusOK(parsedUpdate.parseQueryToCQ());
            }

            if (!shouldRetryDuplicateKeyException(parsedUpdate,
                                                  *ex.extraInfo<DuplicateKeyErrorInfo>())) {
                throw;
            }

            logAndBackoff(4640402,
                          ::mongo::logv2::LogComponent::kWrite,
                          logv2::LogSeverity::Debug(1),
                          numAttempts,
                          "Caught DuplicateKey exception during upsert",
                          "namespace"_attr = ns.ns());
        }
    }

    MONGO_UNREACHABLE;
}

WriteResult performUpdates(OperationContext* opCtx,
                           const write_ops::UpdateCommandRequest& wholeOp,
                           OperationSource source) {
    auto ns = wholeOp.getNamespace();
    if (source == OperationSource::kTimeseriesUpdate && !ns.isTimeseriesBucketsCollection()) {
        ns = ns.makeTimeseriesBucketsNamespace();
    }

    // Update performs its own retries, so we should not be in a WriteUnitOfWork unless run in a
    // transaction.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(!opCtx->lockState()->inAWriteUnitOfWork() ||
              (txnParticipant && opCtx->inMultiDocumentTransaction()));
    uassertStatusOK(userAllowedWriteNS(opCtx, ns));

    const auto [disableDocumentValidation, fleCrudProcessed] =
        getDocumentValidationFlags(opCtx, wholeOp.getWriteCommandRequestBase());

    DisableDocumentSchemaValidationIfTrue docSchemaValidationDisabler(opCtx,
                                                                      disableDocumentValidation);

    DisableSafeContentValidationIfTrue safeContentValidationDisabler(
        opCtx, disableDocumentValidation, fleCrudProcessed);

    LastOpFixer lastOpFixer(opCtx, ns);

    bool containsRetry = false;
    ON_BLOCK_EXIT([&] { updateRetryStats(opCtx, containsRetry); });

    size_t stmtIdIndex = 0;
    WriteResult out;
    out.results.reserve(wholeOp.getUpdates().size());

    // If the update command specified runtime constants, we adopt them. Otherwise, we set them to
    // the current local and cluster time. These constants are applied to each update in the batch.
    const auto& runtimeConstants =
        wholeOp.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx));

    // Increment operator counters only during the fisrt single update operation in a batch of
    // updates.
    bool forgoOpCounterIncrements = false;
    for (auto&& singleOp : wholeOp.getUpdates()) {
        const auto stmtId = getStmtIdForWriteOp(opCtx, wholeOp, stmtIdIndex++);
        if (opCtx->isRetryableWrite()) {
            if (auto entry = txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
                containsRetry = true;
                RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                out.results.emplace_back(parseOplogEntryForUpdate(*entry));
                out.retriedStmtIds.push_back(stmtId);
                continue;
            }
        }

        // TODO: don't create nested CurOp for legacy writes.
        // Add Command pointer to the nested CurOp.
        auto& parentCurOp = *CurOp::get(opCtx);
        const Command* cmd = parentCurOp.getCommand();
        boost::optional<CurOp> curOp;
        if (source != OperationSource::kTimeseriesInsert) {
            curOp.emplace(opCtx);

            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setCommand_inlock(cmd);
        }
        ON_BLOCK_EXIT([&] {
            if (curOp) {
                finishCurOp(opCtx, &*curOp);
            }
        });
        try {
            lastOpFixer.startingOp();

            // A time-series insert can combine multiple writes into a single operation, and thus
            // can have multiple statement ids associated with it if it is retryable.
            auto stmtIds = source == OperationSource::kTimeseriesInsert && wholeOp.getStmtIds()
                ? *wholeOp.getStmtIds()
                : std::vector<StmtId>{stmtId};

            out.results.emplace_back(
                performSingleUpdateOpWithDupKeyRetry(opCtx,
                                                     ns,
                                                     wholeOp.getCollectionUUID(),
                                                     stmtIds,
                                                     singleOp,
                                                     runtimeConstants,
                                                     wholeOp.getLet(),
                                                     source,
                                                     forgoOpCounterIncrements));
            forgoOpCounterIncrements = true;
            lastOpFixer.finishedOpSuccessfully();
        } catch (const DBException& ex) {
            out.canContinue = handleError(
                opCtx, ex, ns, wholeOp.getWriteCommandRequestBase(), singleOp.getMulti(), &out);
            if (!out.canContinue) {
                break;
            }
        }
    }

    return out;
}

static SingleWriteResult performSingleDeleteOp(OperationContext* opCtx,
                                               const NamespaceString& ns,
                                               const boost::optional<mongo::UUID>& opCollectionUUID,
                                               StmtId stmtId,
                                               const write_ops::DeleteOpEntry& op,
                                               const LegacyRuntimeConstants& runtimeConstants,
                                               const boost::optional<BSONObj>& letParams,
                                               OperationSource source) {
    uassert(ErrorCodes::InvalidOptions,
            "Cannot use (or request) retryable writes with limit=0",
            !opCtx->isRetryableWrite() || !op.getMulti() || stmtId == kUninitializedStmtId);

    globalOpCounters.gotDelete();
    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForDelete(opCtx->getWriteConcern());
    auto& curOp = *CurOp::get(opCtx);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS_inlock(source == OperationSource::kTimeseriesDelete
                               ? ns.getTimeseriesViewNamespace().ns()
                               : ns.ns());
        curOp.setNetworkOp_inlock(dbDelete);
        curOp.setLogicalOp_inlock(LogicalOp::opDelete);
        curOp.setOpDescription_inlock(op.toBSON());
        curOp.ensureStarted();
    }

    auto request = DeleteRequest{};
    request.setNsString(ns);
    request.setLegacyRuntimeConstants(runtimeConstants);
    if (letParams)
        request.setLet(letParams);
    request.setQuery(op.getQ());
    request.setCollation(write_ops::collationOf(op));
    request.setMulti(op.getMulti());
    request.setYieldPolicy(opCtx->inMultiDocumentTransaction()
                               ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                               : PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    request.setStmtId(stmtId);
    request.setHint(op.getHint());

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchRemove, opCtx, "hangDuringBatchRemove", []() {
            LOGV2(20891,
                  "Batch remove - hangDuringBatchRemove fail point enabled. Blocking until fail "
                  "point is disabled");
        });
    if (MONGO_unlikely(failAllRemoves.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllRemoves failpoint active!");
    }

    AutoGetCollection collection(opCtx, ns, fixLockModeForSystemDotViewsChanges(ns, MODE_IX));

    DeleteStageParams::DocumentCounter documentCounter = nullptr;

    if (source == OperationSource::kTimeseriesDelete) {
        uassert(ErrorCodes::NamespaceNotFound,
                "Could not find time-series buckets collection for write",
                *collection);
        auto timeseriesOptions = collection->getTimeseriesOptions();
        uassert(ErrorCodes::InvalidOptions,
                "Time-series buckets collection is missing time-series options",
                timeseriesOptions);

        // Only translate the hint if it is specified by index key.
        if (timeseries::isHintIndexKey(request.getHint())) {
            request.setHint(
                uassertStatusOK(timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
                    *timeseriesOptions, request.getHint())));
        }

        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform a delete with a non-empty query on a time-series collection that "
                "does not have a metaField",
                timeseriesOptions->getMetaField() || request.getQuery().isEmpty());

        uassert(ErrorCodes::IllegalOperation,
                "Cannot perform a non-multi delete on a time-series collection",
                request.getMulti());
        if (auto metaField = timeseriesOptions->getMetaField()) {
            request.setQuery(timeseries::translateQuery(request.getQuery(), *metaField));
        }

        documentCounter =
            timeseries::numMeasurementsForBucketCounter(timeseriesOptions->getTimeField());
    }

    checkCollectionUUIDMismatch(opCtx, ns, collection.getCollection(), opCollectionUUID);

    ParsedDelete parsedDelete(opCtx, &request);
    uassertStatusOK(parsedDelete.parseRequest());

    if (collection.getDb()) {
        curOp.raiseDbProfileLevel(
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));
    }

    assertCanWrite_inlock(opCtx, ns);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWithLockDuringBatchRemove, opCtx, "hangWithLockDuringBatchRemove");

    auto exec = uassertStatusOK(getExecutorDelete(&curOp.debug(),
                                                  &collection.getCollection(),
                                                  &parsedDelete,
                                                  boost::none /* verbosity */,
                                                  std::move(documentCounter)));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    auto nDeleted = exec->executeDelete();
    curOp.debug().additiveMetrics.ndeleted = nDeleted;

    PlanSummaryStats summary;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summary);
    if (const auto& coll = collection.getCollection()) {
        CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, summary);
    }
    curOp.debug().setPlanSummaryMetrics(summary);

    if (curOp.shouldDBProfile(opCtx)) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp.debug().execStats = std::move(stats);
    }

    SingleWriteResult result;
    result.setN(nDeleted);
    return result;
}

WriteResult performDeletes(OperationContext* opCtx,
                           const write_ops::DeleteCommandRequest& wholeOp,
                           OperationSource source) {
    auto ns = wholeOp.getNamespace();
    if (source == OperationSource::kTimeseriesDelete && !ns.isTimeseriesBucketsCollection()) {
        ns = ns.makeTimeseriesBucketsNamespace();
    }

    // Delete performs its own retries, so we should not be in a WriteUnitOfWork unless we are in a
    // transaction.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(!opCtx->lockState()->inAWriteUnitOfWork() ||
              (txnParticipant && opCtx->inMultiDocumentTransaction()));
    uassertStatusOK(userAllowedWriteNS(opCtx, ns));

    const auto [disableDocumentValidation, fleCrudProcessed] =
        getDocumentValidationFlags(opCtx, wholeOp.getWriteCommandRequestBase());

    DisableDocumentSchemaValidationIfTrue docSchemaValidationDisabler(opCtx,
                                                                      disableDocumentValidation);

    DisableSafeContentValidationIfTrue safeContentValidationDisabler(
        opCtx, disableDocumentValidation, fleCrudProcessed);

    LastOpFixer lastOpFixer(opCtx, ns);

    bool containsRetry = false;
    ON_BLOCK_EXIT([&] { updateRetryStats(opCtx, containsRetry); });

    size_t stmtIdIndex = 0;
    WriteResult out;
    out.results.reserve(wholeOp.getDeletes().size());

    // If the delete command specified runtime constants, we adopt them. Otherwise, we set them to
    // the current local and cluster time. These constants are applied to each delete in the batch.
    const auto& runtimeConstants =
        wholeOp.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx));

    for (auto&& singleOp : wholeOp.getDeletes()) {
        const auto stmtId = getStmtIdForWriteOp(opCtx, wholeOp, stmtIdIndex++);
        if (opCtx->isRetryableWrite() &&
            txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx, stmtId)) {
            containsRetry = true;
            RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
            out.results.emplace_back(makeWriteResultForInsertOrDeleteRetry());
            out.retriedStmtIds.push_back(stmtId);
            continue;
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
            if (MONGO_unlikely(hangBeforeChildRemoveOpFinishes.shouldFail())) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeChildRemoveOpFinishes, opCtx, "hangBeforeChildRemoveOpFinishes");
            }
            finishCurOp(opCtx, &curOp);
            if (MONGO_unlikely(hangBeforeChildRemoveOpIsPopped.shouldFail())) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeChildRemoveOpIsPopped, opCtx, "hangBeforeChildRemoveOpIsPopped");
            }
        });
        try {
            lastOpFixer.startingOp();
            out.results.push_back(performSingleDeleteOp(opCtx,
                                                        ns,
                                                        wholeOp.getCollectionUUID(),
                                                        stmtId,
                                                        singleOp,
                                                        runtimeConstants,
                                                        wholeOp.getLet(),
                                                        source));
            lastOpFixer.finishedOpSuccessfully();
        } catch (const DBException& ex) {
            out.canContinue = handleError(
                opCtx, ex, ns, wholeOp.getWriteCommandRequestBase(), false /* multiUpdate */, &out);
            if (!out.canContinue)
                break;
        }
    }

    if (MONGO_unlikely(hangAfterAllChildRemoveOpsArePopped.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangAfterAllChildRemoveOpsArePopped, opCtx, "hangAfterAllChildRemoveOpsArePopped");
    }

    return out;
}

Status performAtomicTimeseriesWrites(
    OperationContext* opCtx,
    const std::vector<write_ops::InsertCommandRequest>& insertOps,
    const std::vector<write_ops::UpdateCommandRequest>& updateOps) try {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    invariant(!opCtx->inMultiDocumentTransaction());
    invariant(!insertOps.empty() || !updateOps.empty());

    auto ns =
        !insertOps.empty() ? insertOps.front().getNamespace() : updateOps.front().getNamespace();

    DisableDocumentValidation disableDocumentValidation{opCtx};

    LastOpFixer lastOpFixer{opCtx, ns};
    lastOpFixer.startingOp();

    AutoGetCollection coll{opCtx, ns, MODE_IX};
    if (!coll) {
        assertTimeseriesBucketsCollectionNotFound(ns);
    }

    auto curOp = CurOp::get(opCtx);
    curOp->raiseDbProfileLevel(CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));

    assertCanWrite_inlock(opCtx, ns);

    WriteUnitOfWork wuow{opCtx};

    std::vector<repl::OpTime> oplogSlots;
    boost::optional<std::vector<repl::OpTime>::iterator> slot;
    if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, ns)) {
        oplogSlots = repl::getNextOpTimes(opCtx, insertOps.size() + updateOps.size());
        slot = oplogSlots.begin();
    }

    auto participant = TransactionParticipant::get(opCtx);
    // Since we are manually updating the "lastWriteOpTime" before committing, we'll also need to
    // manually reset if the storage transaction is aborted.
    if (slot && participant) {
        opCtx->recoveryUnit()->onRollback([opCtx] {
            TransactionParticipant::get(opCtx).setLastWriteOpTime(opCtx, repl::OpTime());
        });
    }

    std::vector<InsertStatement> inserts;
    inserts.reserve(insertOps.size());

    for (auto& op : insertOps) {
        invariant(op.getDocuments().size() == 1);

        inserts.emplace_back(op.getStmtIds() ? *op.getStmtIds()
                                             : std::vector<StmtId>{kUninitializedStmtId},
                             op.getDocuments().front(),
                             slot ? *(*slot)++ : OplogSlot{});
    }

    if (!insertOps.empty()) {
        auto status = coll->insertDocuments(opCtx, inserts.begin(), inserts.end(), &curOp->debug());
        if (!status.isOK()) {
            return status;
        }
        if (slot && participant) {
            // Manually sets the timestamp so that the "prevOpTime" field in the oplog entry is
            // correctly chained to the previous operations.
            participant.setLastWriteOpTime(opCtx, *(std::prev(*slot)));
        }
    }

    for (auto& op : updateOps) {
        invariant(op.getUpdates().size() == 1);
        auto& update = op.getUpdates().front();

        invariant(coll->isClustered());
        auto recordId = record_id_helpers::keyForOID(update.getQ()["_id"].OID());

        auto original = coll->docFor(opCtx, recordId);
        auto [updated, indexesAffected] =
            doc_diff::applyDiff(original.value(),
                                update.getU().getDiff(),
                                &CollectionQueryInfo::get(*coll).getIndexKeys(opCtx),
                                static_cast<bool>(repl::tenantMigrationRecipientInfo(opCtx)));

        CollectionUpdateArgs args;
        if (const auto& stmtIds = op.getStmtIds()) {
            args.stmtIds = *stmtIds;
        }
        args.preImageDoc = original.value();
        args.update = update_oplog_entry::makeDeltaOplogEntry(update.getU().getDiff());
        args.criteria = update.getQ();
        args.source = OperationSource::kTimeseriesInsert;
        if (slot) {
            args.oplogSlots = {**slot};
            fassert(5481600,
                    opCtx->recoveryUnit()->setTimestamp(args.oplogSlots[0].getTimestamp()));
        }

        coll->updateDocument(
            opCtx, recordId, original, updated, indexesAffected, &curOp->debug(), &args);
        if (slot) {
            if (participant) {
                // Manually sets the timestamp so that the "prevOpTime" field in the oplog entry is
                // correctly chained to the previous operations.
                participant.setLastWriteOpTime(opCtx, **slot);
            }
            ++(*slot);
        }
    }

    if (MONGO_unlikely(failAtomicTimeseriesWrites.shouldFail())) {
        return {ErrorCodes::FailPointEnabled,
                "Failing time-series writes due to failAtomicTimeseriesWrites fail point"};
    }

    wuow.commit();

    lastOpFixer.finishedOpSuccessfully();

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

void recordUpdateResultInOpDebug(const UpdateResult& updateResult, OpDebug* opDebug) {
    invariant(opDebug);
    opDebug->additiveMetrics.nMatched = updateResult.numMatched;
    opDebug->additiveMetrics.nModified = updateResult.numDocsModified;
    opDebug->additiveMetrics.nUpserted = static_cast<long long>(!updateResult.upsertedId.isEmpty());
}

namespace {
/**
 * Returns whether a given MatchExpression contains is a MatchType::EQ or a MatchType::AND node with
 * only MatchType::EQ children.
 */
bool matchContainsOnlyAndedEqualityNodes(const MatchExpression& root) {
    if (root.matchType() == MatchExpression::EQ) {
        return true;
    }

    if (root.matchType() == MatchExpression::AND) {
        for (size_t i = 0; i < root.numChildren(); ++i) {
            if (root.getChild(i)->matchType() != MatchExpression::EQ) {
                return false;
            }
        }

        return true;
    }

    return false;
}
}  // namespace

bool shouldRetryDuplicateKeyException(const ParsedUpdate& parsedUpdate,
                                      const DuplicateKeyErrorInfo& errorInfo) {
    invariant(parsedUpdate.hasParsedQuery());

    const auto updateRequest = parsedUpdate.getRequest();

    // In order to be retryable, the update must be an upsert with multi:false.
    if (!updateRequest->isUpsert() || updateRequest->isMulti()) {
        return false;
    }

    auto matchExpr = parsedUpdate.getParsedQuery()->root();
    invariant(matchExpr);

    // In order to be retryable, the update query must contain no expressions other than AND and EQ.
    if (!matchContainsOnlyAndedEqualityNodes(*matchExpr)) {
        return false;
    }

    // In order to be retryable, the update equality field paths must be identical to the unique
    // index key field paths. Also, the values that triggered the DuplicateKey error must match the
    // values used in the upsert query predicate.
    pathsupport::EqualityMatches equalities;
    auto status = pathsupport::extractEqualityMatches(*matchExpr, &equalities);
    if (!status.isOK()) {
        return false;
    }

    auto keyPattern = errorInfo.getKeyPattern();
    if (equalities.size() != static_cast<size_t>(keyPattern.nFields())) {
        return false;
    }

    auto keyValue = errorInfo.getDuplicatedKeyValue();

    BSONObjIterator keyPatternIter(keyPattern);
    BSONObjIterator keyValueIter(keyValue);
    while (keyPatternIter.more() && keyValueIter.more()) {
        auto keyPatternElem = keyPatternIter.next();
        auto keyValueElem = keyValueIter.next();

        auto keyName = keyPatternElem.fieldNameStringData();
        if (!equalities.count(keyName)) {
            return false;
        }

        // Comparison which obeys field ordering but ignores field name.
        BSONElementComparator cmp{BSONElementComparator::FieldNamesMode::kIgnore, nullptr};
        if (cmp.evaluate(equalities[keyName]->getData() != keyValueElem)) {
            return false;
        }
    }
    invariant(!keyPatternIter.more());
    invariant(!keyValueIter.more());

    return true;
}

}  // namespace mongo::write_ops_exec

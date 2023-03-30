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

#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/collection_yield_restore.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/dbdirectclient.h"
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
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_stats.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

namespace mongo::write_ops_exec {
class Atomic64Metric;
}  // namespace mongo::write_ops_exec

namespace mongo {
template <>
struct BSONObjAppendFormat<write_ops_exec::Atomic64Metric> : FormatKind<NumberLong> {};
}  // namespace mongo


namespace mongo::write_ops_exec {

/**
 * Atomic wrapper for long long type for Metrics.
 */
class Atomic64Metric {
public:
    /** Set _value to the max of the current or newMax. */
    void setIfMax(long long newMax) {
        /*  Note: compareAndSwap will load into val most recent value. */
        for (long long val = _value.load(); val < newMax && !_value.compareAndSwap(&val, newMax);) {
        }
    }

    /** store val into value. */
    void set(long long val) {
        _value.store(val);
    }

    /** Return the current value. */
    long long get() const {
        return _value.load();
    }

    /** TODO: SERVER-73806 Avoid implicit conversion to long long */
    operator long long() const {
        return get();
    }

private:
    mongo::AtomicWord<long long> _value;
};

// Convention in this file: generic helpers go in the anonymous namespace. Helpers that are for a
// single type of operation are static functions defined above their caller.
namespace {

MONGO_FAIL_POINT_DEFINE(failAllInserts);
MONGO_FAIL_POINT_DEFINE(failAllUpdates);
MONGO_FAIL_POINT_DEFINE(failAllRemoves);
MONGO_FAIL_POINT_DEFINE(hangBeforeChildRemoveOpFinishes);
MONGO_FAIL_POINT_DEFINE(hangBeforeChildRemoveOpIsPopped);
MONGO_FAIL_POINT_DEFINE(hangAfterAllChildRemoveOpsArePopped);
MONGO_FAIL_POINT_DEFINE(hangWriteBeforeWaitingForMigrationDecision);
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
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeCommit);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeReopeningQuery);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeWrite);
MONGO_FAIL_POINT_DEFINE(failUnorderedTimeseriesInsert);


/**
 * Metrics group for the `updateMany` and `deleteMany` operations. For each
 * operation, the `duration` and `numDocs` will contribute to aggregated total
 * and max metrics.
 */
class MultiUpdateDeleteMetrics {
public:
    void operator()(Microseconds duration, size_t numDocs) {
        _durationTotalMicroseconds.increment(durationCount<Microseconds>(duration));
        _durationTotalMs.set(
            durationCount<Milliseconds>(Microseconds{_durationTotalMicroseconds.get()}));
        _durationMaxMs.setIfMax(durationCount<Milliseconds>(duration));

        _numDocsTotal.increment(numDocs);
        _numDocsMax.setIfMax(numDocs);
    }

private:
    /**
     * To avoid rapid accumulation of roundoff error in the duration total, it
     * is maintained precisely, and we arrange for the corresponding
     * Millisecond metric to hold an exported low-res image of it.
     */
    Counter64 _durationTotalMicroseconds;

    Atomic64Metric& _durationTotalMs =
        makeServerStatusMetric<Atomic64Metric>("query.updateDeleteManyDurationTotalMs");
    Atomic64Metric& _durationMaxMs =
        makeServerStatusMetric<Atomic64Metric>("query.updateDeleteManyDurationMaxMs");

    CounterMetric _numDocsTotal{"query.updateDeleteManyDocumentsTotalCount"};
    Atomic64Metric& _numDocsMax =
        makeServerStatusMetric<Atomic64Metric>("query.updateDeleteManyDocumentsMaxCount");
};

MultiUpdateDeleteMetrics collectMultiUpdateDeleteMetrics;

void updateRetryStats(OperationContext* opCtx, bool containsRetry) {
    if (containsRetry) {
        RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
    }
}

void finishCurOp(OperationContext* opCtx, CurOp* curOp) {
    try {
        curOp->done();
        auto executionTimeMicros = duration_cast<Microseconds>(curOp->elapsedTimeExcludingPauses());
        curOp->debug().additiveMetrics.executionTime = executionTimeMicros;

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
        const bool shouldProfile = curOp->completeAndLogOperation(
            MONGO_LOGV2_DEFAULT_COMPONENT,
            CollectionCatalog::get(opCtx)
                ->getDatabaseProfileSettings(curOp->getNSS().dbName())
                .filter);

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
            if (auto fp = globalFailPointRegistry().find("clusterAllCollectionsByDefault");
                fp && fp->shouldFail() && !clustered_util::requiresLegacyFormat(ns)) {
                defaultCollectionOptions.clusteredIndex =
                    clustered_util::makeDefaultClusteredIdIndex();
            }
            auto db = autoDb.ensureDbExists(opCtx);
            uassertStatusOK(db->userCreateNS(opCtx, ns, defaultCollectionOptions));
            wuow.commit();
        }
    });
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

    uassertStatusOK(collection_internal::insertDocuments(
        opCtx, collection, begin, end, &CurOp::get(opCtx)->debug(), fromMigrate));
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

bool handleError(OperationContext* opCtx,
                 const DBException& ex,
                 const NamespaceString& nss,
                 const bool ordered,
                 bool isMultiUpdate,
                 const boost::optional<UUID> sampleId,
                 WriteResult* out) {
    NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
    auto& curOp = *CurOp::get(opCtx);
    curOp.debug().errInfo = ex.toStatus();

    if (ErrorCodes::isInterruption(ex.code())) {
        throw;  // These have always failed the whole batch.
    }

    if (ErrorCodes::WouldChangeOwningShard == ex.code()) {
        if (analyze_shard_key::supportsPersistingSampledQueries(opCtx) && sampleId) {
            // Sample the diff before rethrowing the error since mongos will handle this update by
            // by performing a delete on the shard owning the pre-image doc and an insert on the
            // shard owning the post-image doc. As a result, this update will not show up in the
            // OpObserver as an update.
            auto wouldChangeOwningShardInfo = ex.extraInfo<WouldChangeOwningShardInfo>();
            invariant(wouldChangeOwningShardInfo);

            analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                ->addDiff(*sampleId,
                          nss,
                          *wouldChangeOwningShardInfo->getUuid(),
                          wouldChangeOwningShardInfo->getPreImage(),
                          wouldChangeOwningShardInfo->getPostImage())
                .getAsync([](auto) {});
        }
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
                           migrationConflictInfo->getMigrationId(),
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
    return !ordered;
}

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

/**
 * Returns true if caller should try to insert more documents. Does nothing else if batch is empty.
 */
bool insertBatchAndHandleErrors(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const boost::optional<mongo::UUID>& collectionUUID,
                                bool ordered,
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
        [&nss]() {
            LOGV2(20889,
                  "Batch insert - hangDuringBatchInsert fail point enabled for namespace "
                  "{namespace}. Blocking until fail point is disabled",
                  "Batch insert - hangDuringBatchInsert fail point enabled for a namespace. "
                  "Blocking until fail point is disabled",
                  logAttrs(nss));
        },
        nss);

    if (MONGO_unlikely(failAllInserts.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllInserts failpoint active!");
    }

    boost::optional<AutoGetCollection> collection;
    auto acquireCollection = [&] {
        while (true) {
            collection.emplace(opCtx,
                               nss,
                               fixLockModeForSystemDotViewsChanges(nss, MODE_IX),
                               AutoGetCollection::Options{}.expectedUUID(collectionUUID));
            if (*collection) {
                break;
            }

            if (source == OperationSource::kTimeseriesInsert) {
                assertTimeseriesBucketsCollectionNotFound(nss);
            }

            collection.reset();  // unlock.
            makeCollection(opCtx, nss);
        }

        curOp.raiseDbProfileLevel(
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nss.dbName()));
        assertCanWrite_inlock(opCtx, nss);

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
            auto canContinue = handleError(
                opCtx, ex, nss, ordered, false /* multiUpdate */, boost::none /* sampleId */, out);
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
            writeConflictRetry(opCtx, "insert", nss.ns(), [&] {
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
            bool canContinue = handleError(
                opCtx, ex, nss, ordered, false /* multiUpdate */, boost::none /* sampleId */, out);

            if (!canContinue) {
                // Failed in ordered batch, or in a transaction, or from some unrecoverable error.
                return false;
            }
        }
    }

    return true;
}

boost::optional<BSONObj> advanceExecutor(OperationContext* opCtx,
                                         PlanExecutor* exec,
                                         bool isRemove) {
    BSONObj value;
    PlanExecutor::ExecState state;
    try {
        state = exec->getNext(&value, nullptr);
    } catch (DBException& exception) {
        auto&& explainer = exec->getPlanExplainer();
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        LOGV2_WARNING(7267501,
                      "Plan executor error during findAndModify: {error}, stats: {stats}",
                      "Plan executor error during findAndModify",
                      "error"_attr = exception.toStatus(),
                      "stats"_attr = redact(stats));

        exception.addContext("Plan executor error during findAndModify");
        throw;
    }

    if (PlanExecutor::ADVANCED == state) {
        return {std::move(value)};
    }

    invariant(state == PlanExecutor::IS_EOF);
    return boost::none;
}

UpdateResult writeConflictRetryUpsert(OperationContext* opCtx,
                                      const NamespaceString& nsString,
                                      CurOp* curOp,
                                      OpDebug* opDebug,
                                      bool inTransaction,
                                      bool remove,
                                      bool upsert,
                                      boost::optional<BSONObj>& docFound,
                                      ParsedUpdate* parsedUpdate) {
    AutoGetCollection autoColl(opCtx, nsString, MODE_IX);
    Database* db = autoColl.ensureDbExists(opCtx);

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->enter_inlock(
            nsString, CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nsString.dbName()));
    }

    assertCanWrite_inlock(opCtx, nsString);

    CollectionPtr createdCollection;
    const CollectionPtr* collectionPtr = &autoColl.getCollection();

    // TODO SERVER-50983: Create abstraction for creating collection when using
    // AutoGetCollection Create the collection if it does not exist when performing an upsert
    // because the update stage does not create its own collection
    if (!*collectionPtr && upsert) {
        assertCanWrite_inlock(opCtx, nsString);

        createdCollection = CollectionPtr(
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nsString));

        // If someone else beat us to creating the collection, do nothing
        if (!createdCollection) {
            uassertStatusOK(userAllowedCreateNS(opCtx, nsString));
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx);
            WriteUnitOfWork wuow(opCtx);
            CollectionOptions defaultCollectionOptions;
            uassertStatusOK(db->userCreateNS(opCtx, nsString, defaultCollectionOptions));
            wuow.commit();

            createdCollection = CollectionPtr(
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nsString));
        }

        invariant(createdCollection);
        createdCollection.makeYieldable(opCtx,
                                        LockedCollectionYieldRestore(opCtx, createdCollection));
        collectionPtr = &createdCollection;
    }
    const auto& collection = *collectionPtr;

    if (collection && collection->isCapped()) {
        uassert(
            ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Collection '" << collection->ns()
                          << "' is a capped collection. Writes in transactions are not allowed on "
                             "capped collections.",
            !inTransaction);
    }

    const auto exec = uassertStatusOK(
        getExecutorUpdate(opDebug, &collection, parsedUpdate, boost::none /* verbosity */));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    docFound = advanceExecutor(opCtx, exec.get(), remove);
    // Nothing after advancing the plan executor should throw a WriteConflictException,
    // so the following bookkeeping with execution stats won't end up being done
    // multiple times.

    PlanSummaryStats summaryStats;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summaryStats);
    if (collection) {
        CollectionQueryInfo::get(collection).notifyOfQuery(opCtx, collection, summaryStats);
    }
    auto updateResult = exec->getUpdateResult();
    write_ops_exec::recordUpdateResultInOpDebug(updateResult, opDebug);
    opDebug->setPlanSummaryMetrics(summaryStats);

    if (updateResult.containsDotsAndDollarsField) {
        // If it's an upsert, increment 'inserts' metric, otherwise increment 'updates'.
        dotsAndDollarsFieldsCounters.incrementForUpsert(!updateResult.upsertedId.isEmpty());
    }

    if (curOp->shouldDBProfile()) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp->debug().execStats = std::move(stats);
    }

    if (docFound) {
        ResourceConsumption::DocumentUnitCounter docUnitsReturned;
        docUnitsReturned.observeOne(docFound->objsize());

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
        metricsCollector.incrementDocUnitsReturned(curOp->getNS(), docUnitsReturned);
    }

    return updateResult;
}

long long writeConflictRetryRemove(OperationContext* opCtx,
                                   const NamespaceString& nsString,
                                   DeleteRequest* deleteRequest,
                                   CurOp* curOp,
                                   OpDebug* opDebug,
                                   bool inTransaction,
                                   boost::optional<BSONObj>& docFound) {

    invariant(deleteRequest);

    ParsedDelete parsedDelete(opCtx, deleteRequest);
    uassertStatusOK(parsedDelete.parseRequest());

    AutoGetCollection collection(opCtx, nsString, MODE_IX);

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->enter_inlock(
            nsString, CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nsString.dbName()));
    }

    assertCanWrite_inlock(opCtx, nsString);

    if (collection && collection->isCapped()) {
        uassert(
            ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Collection '" << collection->ns()
                          << "' is a capped collection. Writes in transactions are not allowed on "
                             "capped collections.",
            !inTransaction);
    }

    const auto exec = uassertStatusOK(getExecutorDelete(
        opDebug, &collection.getCollection(), &parsedDelete, boost::none /* verbosity */));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    docFound = advanceExecutor(opCtx, exec.get(), true);
    // Nothing after advancing the plan executor should throw a WriteConflictException,
    // so the following bookkeeping with execution stats won't end up being done
    // multiple times.

    PlanSummaryStats summaryStats;
    exec->getPlanExplainer().getSummaryStats(&summaryStats);
    if (const auto& coll = collection.getCollection()) {
        CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, summaryStats);
    }
    opDebug->setPlanSummaryMetrics(summaryStats);

    // Fill out OpDebug with the number of deleted docs.
    auto nDeleted = exec->executeDelete();
    opDebug->additiveMetrics.ndeleted = nDeleted;

    if (curOp->shouldDBProfile()) {
        auto&& explainer = exec->getPlanExplainer();
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp->debug().execStats = std::move(stats);
    }

    if (docFound) {
        ResourceConsumption::DocumentUnitCounter docUnitsReturned;
        docUnitsReturned.observeOne(docFound->objsize());

        auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
        metricsCollector.incrementDocUnitsReturned(curOp->getNS(), docUnitsReturned);
    }

    return nDeleted;
}

boost::optional<write_ops::WriteError> generateError(OperationContext* opCtx,
                                                     const Status& status,
                                                     int index,
                                                     size_t numErrors) {
    if (status.isOK()) {
        return boost::none;
    }

    boost::optional<Status> overwrittenStatus;

    if (status == ErrorCodes::TenantMigrationConflict) {
        hangWriteBeforeWaitingForMigrationDecision.pauseWhileSet(opCtx);

        overwrittenStatus.emplace(
            tenant_migration_access_blocker::handleTenantMigrationConflict(opCtx, status));

        // Interruption errors encountered during batch execution fail the entire batch, so throw on
        // such errors here for consistency.
        if (ErrorCodes::isInterruption(*overwrittenStatus)) {
            uassertStatusOK(*overwrittenStatus);
        }

        // Tenant migration errors, similarly to migration errors consume too much space in the
        // ordered:false responses and get truncated. Since the call to
        // 'handleTenantMigrationConflict' above replaces the original status, we need to manually
        // truncate the new reason if the original 'status' was also truncated.
        if (status.reason().empty()) {
            overwrittenStatus = overwrittenStatus->withReason("");
        }
    }

    constexpr size_t kMaxErrorReasonsToReport = 1;
    constexpr size_t kMaxErrorSizeToReportAfterMaxReasonsReached = 1024 * 1024;

    if (numErrors > kMaxErrorReasonsToReport) {
        size_t errorSize =
            overwrittenStatus ? overwrittenStatus->reason().size() : status.reason().size();
        if (errorSize > kMaxErrorSizeToReportAfterMaxReasonsReached)
            overwrittenStatus =
                overwrittenStatus ? overwrittenStatus->withReason("") : status.withReason("");
    }

    if (overwrittenStatus)
        return write_ops::WriteError(index, std::move(*overwrittenStatus));
    else
        return write_ops::WriteError(index, status);
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
        curOp.setNS_inlock(wholeOp.getNamespace());
        curOp.setLogicalOp_inlock(LogicalOp::opInsert);
        curOp.ensureStarted();
        curOp.debug().additiveMetrics.ninserted = 0;
    }

    // If we are performing inserts from tenant migrations, skip checking if the user is allowed to
    // write to the namespace.
    if (!repl::tenantMigrationInfo(opCtx)) {
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

    size_t nextOpIndex = 0;
    size_t bytesInBatch = 0;
    std::vector<InsertStatement> batch;
    const size_t maxBatchSize = internalInsertMaxBatchSize.load();
    const size_t maxBatchBytes = write_ops::insertVectorMaxBytes;
    batch.reserve(std::min(wholeOp.getDocuments().size(), maxBatchSize));

    for (auto&& doc : wholeOp.getDocuments()) {
        const auto currentOpIndex = nextOpIndex++;
        const bool isLastDoc = (&doc == &wholeOp.getDocuments().back());
        bool containsDotsAndDollarsField = false;
        auto fixedDoc = fixDocumentForInsert(opCtx, doc, &containsDotsAndDollarsField);
        const StmtId stmtId = getStmtIdForWriteOp(opCtx, wholeOp, currentOpIndex);
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

        out.canContinue = insertBatchAndHandleErrors(opCtx,
                                                     wholeOp.getNamespace(),
                                                     wholeOp.getCollectionUUID(),
                                                     wholeOp.getOrdered(),
                                                     batch,
                                                     &lastOpFixer,
                                                     &out,
                                                     source);
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
                                              wholeOp.getOrdered(),
                                              false /* multiUpdate */,
                                              boost::none /* sampleId */,
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
                  logAttrs(ns));
        },
        ns);

    if (MONGO_unlikely(failAllUpdates.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllUpdates failpoint active!");
    }

    boost::optional<AutoGetCollection> collection;
    while (true) {
        collection.emplace(opCtx,
                           ns,
                           fixLockModeForSystemDotViewsChanges(ns, MODE_IX),
                           AutoGetCollection::Options{}.expectedUUID(opCollectionUUID));
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

    if (curOp.shouldDBProfile()) {
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
    const boost::optional<UUID>& sampleId,
    OperationSource source,
    bool forgoOpCounterIncrements) {
    globalOpCounters.gotUpdate();
    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForUpdate(opCtx->getWriteConcern());
    auto& curOp = *CurOp::get(opCtx);
    if (source != OperationSource::kTimeseriesInsert) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS_inlock(
            source == OperationSource::kTimeseriesUpdate ? ns.getTimeseriesViewNamespace() : ns);
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
    if (sampleId) {
        request.setSampleId(sampleId);
    }

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
                          logAttrs(ns));
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

    size_t nextOpIndex = 0;
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
        const auto currentOpIndex = nextOpIndex++;
        const auto stmtId = getStmtIdForWriteOp(opCtx, wholeOp, currentOpIndex);
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
            curOp.emplace(cmd);
            curOp->push(opCtx);
        }
        ON_BLOCK_EXIT([&] {
            if (curOp) {
                finishCurOp(opCtx, &*curOp);
            }
        });

        auto sampleId = analyze_shard_key::getOrGenerateSampleId(
            opCtx, ns, analyze_shard_key::SampledCommandNameEnum::kUpdate, singleOp);
        if (sampleId) {
            analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                ->addUpdateQuery(*sampleId, wholeOp, currentOpIndex)
                .getAsync([](auto) {});
        }

        try {
            lastOpFixer.startingOp();

            // A time-series insert can combine multiple writes into a single operation, and thus
            // can have multiple statement ids associated with it if it is retryable.
            auto stmtIds = source == OperationSource::kTimeseriesInsert && wholeOp.getStmtIds()
                ? *wholeOp.getStmtIds()
                : std::vector<StmtId>{stmtId};

            boost::optional<Timer> timer;
            if (singleOp.getMulti()) {
                timer.emplace();
            }

            const SingleWriteResult&& reply =
                performSingleUpdateOpWithDupKeyRetry(opCtx,
                                                     ns,
                                                     wholeOp.getCollectionUUID(),
                                                     stmtIds,
                                                     singleOp,
                                                     runtimeConstants,
                                                     wholeOp.getLet(),
                                                     sampleId,
                                                     source,
                                                     forgoOpCounterIncrements);
            out.results.emplace_back(reply);
            forgoOpCounterIncrements = true;
            lastOpFixer.finishedOpSuccessfully();

            if (singleOp.getMulti()) {
                updateManyCount.increment(1);
                collectMultiUpdateDeleteMetrics(timer->elapsed(), reply.getNModified());
            }
        } catch (const DBException& ex) {
            out.canContinue = handleError(opCtx,
                                          ex,
                                          ns,
                                          wholeOp.getOrdered(),
                                          singleOp.getMulti(),
                                          singleOp.getSampleId(),
                                          &out);
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
        curOp.setNS_inlock(
            source == OperationSource::kTimeseriesDelete ? ns.getTimeseriesViewNamespace() : ns);
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

    AutoGetCollection collection(opCtx,
                                 ns,
                                 fixLockModeForSystemDotViewsChanges(ns, MODE_IX),
                                 AutoGetCollection::Options{}.expectedUUID(opCollectionUUID));

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

        if (!feature_flags::gTimeseriesDeletesSupport.isEnabled(
                serverGlobalParams.featureCompatibility)) {
            uassert(
                ErrorCodes::InvalidOptions,
                "Cannot perform a delete with a non-empty query on a time-series collection that "
                "does not have a metaField",
                timeseriesOptions->getMetaField() || request.getQuery().isEmpty());

            uassert(ErrorCodes::IllegalOperation,
                    "Cannot perform a non-multi delete on a time-series collection",
                    request.getMulti());
            if (auto metaField = timeseriesOptions->getMetaField()) {
                request.setQuery(timeseries::translateQuery(request.getQuery(), *metaField));
            }
        }

        documentCounter =
            timeseries::numMeasurementsForBucketCounter(timeseriesOptions->getTimeField());
    }

    ParsedDelete parsedDelete(opCtx,
                              &request,
                              source == OperationSource::kTimeseriesDelete && collection
                                  ? collection->getTimeseriesOptions()
                                  : boost::none);
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

    if (curOp.shouldDBProfile()) {
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

    size_t nextOpIndex = 0;
    WriteResult out;
    out.results.reserve(wholeOp.getDeletes().size());

    // If the delete command specified runtime constants, we adopt them. Otherwise, we set them to
    // the current local and cluster time. These constants are applied to each delete in the batch.
    const auto& runtimeConstants =
        wholeOp.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx));

    for (auto&& singleOp : wholeOp.getDeletes()) {
        const auto currentOpIndex = nextOpIndex++;
        const auto stmtId = getStmtIdForWriteOp(opCtx, wholeOp, currentOpIndex);
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
        CurOp curOp(cmd);
        curOp.push(opCtx);
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

        if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                opCtx, ns, analyze_shard_key::SampledCommandNameEnum::kDelete, singleOp)) {
            analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                ->addDeleteQuery(*sampleId, wholeOp, currentOpIndex)
                .getAsync([](auto) {});
        }

        try {
            lastOpFixer.startingOp();

            boost::optional<Timer> timer;
            if (singleOp.getMulti()) {
                timer.emplace();
            }

            const SingleWriteResult&& reply = performSingleDeleteOp(opCtx,
                                                                    ns,
                                                                    wholeOp.getCollectionUUID(),
                                                                    stmtId,
                                                                    singleOp,
                                                                    runtimeConstants,
                                                                    wholeOp.getLet(),
                                                                    source);
            out.results.push_back(reply);
            lastOpFixer.finishedOpSuccessfully();

            if (singleOp.getMulti()) {
                deleteManyCount.increment(1);
                collectMultiUpdateDeleteMetrics(timer->elapsed(), reply.getN());
            }
        } catch (const DBException& ex) {
            out.canContinue = handleError(opCtx,
                                          ex,
                                          ns,
                                          wholeOp.getOrdered(),
                                          false /* multiUpdate */,
                                          singleOp.getSampleId(),
                                          &out);
            if (!out.canContinue)
                break;
        }
    }

    if (MONGO_unlikely(hangAfterAllChildRemoveOpsArePopped.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangAfterAllChildRemoveOpsArePopped, opCtx, "hangAfterAllChildRemoveOpsArePopped");
    }

    return out;
}  // namespace mongo::write_ops_exec

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
        opCtx->recoveryUnit()->onRollback([](OperationContext* opCtx) {
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
        auto status = collection_internal::insertDocuments(
            opCtx, *coll, inserts.begin(), inserts.end(), &curOp->debug());
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

        CollectionUpdateArgs args{original.value()};
        args.criteria = update.getQ();
        if (const auto& stmtIds = op.getStmtIds()) {
            args.stmtIds = *stmtIds;
        }
        args.source = OperationSource::kTimeseriesInsert;

        BSONObj updated;
        BSONObj diffFromUpdate;
        const BSONObj* diffOnIndexes =
            collection_internal::kUpdateAllIndexes;  // Assume all indexes are affected.
        if (update.getU().type() == write_ops::UpdateModification::Type::kDelta) {
            diffFromUpdate = update.getU().getDiff();
            auto result = doc_diff::applyDiff(original.value(),
                                              diffFromUpdate,
                                              &CollectionQueryInfo::get(*coll).getIndexKeys(opCtx),
                                              static_cast<bool>(repl::tenantMigrationInfo(opCtx)));
            updated = result.postImage;
            diffOnIndexes =
                result.indexesAffected ? &diffFromUpdate : collection_internal::kUpdateNoIndexes;
            args.update = update_oplog_entry::makeDeltaOplogEntry(diffFromUpdate);
        } else if (update.getU().type() == write_ops::UpdateModification::Type::kTransform) {
            const auto& transform = update.getU().getTransform();
            auto transformed = transform(original.value());
            tassert(7050400,
                    "Could not apply transformation to time series bucket document",
                    transformed.has_value());
            updated = std::move(transformed.value());
            args.update = update_oplog_entry::makeReplacementOplogEntry(updated);
        } else {
            invariant(false, "Unexpected update type");
        }

        if (slot) {
            args.oplogSlots = {**slot};
            fassert(5481600,
                    opCtx->recoveryUnit()->setTimestamp(args.oplogSlots[0].getTimestamp()));
        }

        collection_internal::updateDocument(
            opCtx, *coll, recordId, original, updated, diffOnIndexes, &curOp->debug(), &args);
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

namespace {

using TimeseriesBatches =
    std::vector<std::pair<std::shared_ptr<timeseries::bucket_catalog::WriteBatch>, size_t>>;
using TimeseriesStmtIds = stdx::unordered_map<OID, std::vector<StmtId>, OID::Hasher>;
struct TimeseriesSingleWriteResult {
    StatusWith<SingleWriteResult> result;
    bool canContinue = true;
};

enum struct TimeseriesAtomicWriteResult {
    kSuccess,
    kContinuableError,
    kNonContinuableError,
};
/**
 * Returns true if the time-series write is retryable.
 */
bool isTimeseriesWriteRetryable(OperationContext* opCtx) {
    if (!opCtx->getTxnNumber()) {
        return false;
    }

    if (opCtx->inMultiDocumentTransaction()) {
        return false;
    }

    return true;
}

void getOpTimeAndElectionId(OperationContext* opCtx,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId) {
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
    const auto replMode = replCoord->getReplicationMode();

    *opTime = replMode != repl::ReplicationCoordinator::modeNone
        ? boost::make_optional(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp())
        : boost::none;
    *electionId = replMode == repl::ReplicationCoordinator::modeReplSet
        ? boost::make_optional(replCoord->getElectionId())
        : boost::none;
}

NamespaceString ns(const write_ops::InsertCommandRequest& request) {
    return request.getNamespace();
}

NamespaceString makeTimeseriesBucketsNamespace(const NamespaceString& nss) {
    return nss.isTimeseriesBucketsCollection() ? nss : nss.makeTimeseriesBucketsNamespace();
}

/**
 * Transforms a single time-series insert to an update request on an existing bucket.
 */
write_ops::UpdateOpEntry makeTimeseriesUpdateOpEntry(
    OperationContext* opCtx,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata) {
    BSONObjBuilder updateBuilder;
    {
        if (!batch->min.isEmpty() || !batch->max.isEmpty()) {
            BSONObjBuilder controlBuilder(updateBuilder.subobjStart(
                str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "control"));
            if (!batch->min.isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "min", batch->min);
            }
            if (!batch->max.isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "max", batch->max);
            }
        }
    }
    {  // doc_diff::kSubDiffSectionFieldPrefix + <field name> => {<index_0>: ..., <index_1>:}
        StringDataMap<BSONObjBuilder> dataFieldBuilders;
        auto metadataElem = metadata.firstElement();
        DecimalCounter<uint32_t> count(batch->numPreviouslyCommittedMeasurements);
        for (const auto& doc : batch->measurements) {
            for (const auto& elem : doc) {
                auto key = elem.fieldNameStringData();
                if (metadataElem && key == metadataElem.fieldNameStringData()) {
                    continue;
                }
                auto& builder = dataFieldBuilders[key];
                builder.appendAs(elem, count);
            }
            ++count;
        }

        // doc_diff::kSubDiffSectionFieldPrefix + <field name>
        BSONObjBuilder dataBuilder(updateBuilder.subobjStart("sdata"));
        BSONObjBuilder newDataFieldsBuilder;
        for (auto& pair : dataFieldBuilders) {
            // Existing 'data' fields with measurements require different treatment from fields
            // not observed before (missing from control.min and control.max).
            if (batch->newFieldNamesToBeInserted.count(pair.first)) {
                newDataFieldsBuilder.append(pair.first, pair.second.obj());
            }
        }
        auto newDataFields = newDataFieldsBuilder.obj();
        if (!newDataFields.isEmpty()) {
            dataBuilder.append(doc_diff::kInsertSectionFieldName, newDataFields);
        }
        for (auto& pair : dataFieldBuilders) {
            // Existing 'data' fields with measurements require different treatment from fields
            // not observed before (missing from control.min and control.max).
            if (!batch->newFieldNamesToBeInserted.count(pair.first)) {
                dataBuilder.append(doc_diff::kSubDiffSectionFieldPrefix + pair.first.toString(),
                                   BSON(doc_diff::kInsertSectionFieldName << pair.second.obj()));
            }
        }
    }
    write_ops::UpdateModification::DiffOptions options;
    options.mustCheckExistenceForInsertOperations =
        static_cast<bool>(repl::tenantMigrationInfo(opCtx));
    write_ops::UpdateModification u(
        updateBuilder.obj(), write_ops::UpdateModification::DeltaTag{}, options);
    auto oid = batch->bucketHandle.bucketId.oid;
    write_ops::UpdateOpEntry update(BSON("_id" << oid), std::move(u));
    invariant(!update.getMulti(), oid.toString());
    invariant(!update.getUpsert(), oid.toString());
    return update;
}

/**
 * Transforms a single time-series insert to an update request on an existing bucket.
 */
write_ops::UpdateOpEntry makeTimeseriesTransformationOpEntry(
    OperationContext* opCtx,
    const OID& bucketId,
    write_ops::UpdateModification::TransformFunc transformationFunc) {
    write_ops::UpdateModification u(std::move(transformationFunc));
    write_ops::UpdateOpEntry update(BSON("_id" << bucketId), std::move(u));
    invariant(!update.getMulti(), bucketId.toString());
    invariant(!update.getUpsert(), bucketId.toString());
    return update;
}

boost::optional<std::pair<Status, bool>> checkFailUnorderedTimeseriesInsertFailPoint(
    const BSONObj& metadata) {
    bool canContinue = true;
    if (MONGO_unlikely(failUnorderedTimeseriesInsert.shouldFail(
            [&metadata, &canContinue](const BSONObj& data) {
                BSONElementComparator comp(BSONElementComparator::FieldNamesMode::kIgnore, nullptr);
                if (auto continueElem = data["canContinue"]) {
                    canContinue = data["canContinue"].trueValue();
                }
                return comp.compare(data["metadata"], metadata.firstElement()) == 0;
            }))) {
        return std::make_pair(Status(ErrorCodes::FailPointEnabled,
                                     "Failed unordered time-series insert due to "
                                     "failUnorderedTimeseriesInsert fail point"),
                              canContinue);
    }
    return boost::none;
}

timeseries::bucket_catalog::CombineWithInsertsFromOtherClients
canCombineTimeseriesInsertWithOtherClients(OperationContext* opCtx,
                                           const write_ops::InsertCommandRequest& request) {
    return isTimeseriesWriteRetryable(opCtx) || request.getOrdered()
        ? timeseries::bucket_catalog::CombineWithInsertsFromOtherClients::kDisallow
        : timeseries::bucket_catalog::CombineWithInsertsFromOtherClients::kAllow;
}

TimeseriesSingleWriteResult getTimeseriesSingleWriteResult(
    write_ops_exec::WriteResult&& reply, const write_ops::InsertCommandRequest& request) {
    invariant(reply.results.size() == 1,
              str::stream() << "Unexpected number of results (" << reply.results.size()
                            << ") for insert on time-series collection " << ns(request));

    return {std::move(reply.results[0]), reply.canContinue};
}

write_ops::WriteCommandRequestBase makeTimeseriesWriteOpBase(std::vector<StmtId>&& stmtIds) {
    write_ops::WriteCommandRequestBase base;

    // The schema validation configured in the bucket collection is intended for direct
    // operations by end users and is not applicable here.
    base.setBypassDocumentValidation(true);

    if (!stmtIds.empty()) {
        base.setStmtIds(std::move(stmtIds));
    }

    return base;
}

write_ops::InsertCommandRequest makeTimeseriesInsertOp(
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds,
    const write_ops::InsertCommandRequest& request) {
    write_ops::InsertCommandRequest op{makeTimeseriesBucketsNamespace(ns(request)),
                                       {timeseries::makeNewDocumentForWrite(batch, metadata)}};
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(stmtIds)));
    return op;
}

write_ops::UpdateCommandRequest makeTimeseriesUpdateOp(
    OperationContext* opCtx,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds,
    const write_ops::InsertCommandRequest& request) {
    write_ops::UpdateCommandRequest op(makeTimeseriesBucketsNamespace(ns(request)),
                                       {makeTimeseriesUpdateOpEntry(opCtx, batch, metadata)});
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(stmtIds)));
    return op;
}

write_ops::UpdateCommandRequest makeTimeseriesTransformationOp(
    OperationContext* opCtx,
    const OID& bucketId,
    write_ops::UpdateModification::TransformFunc transformationFunc,
    const write_ops::InsertCommandRequest& request) {
    write_ops::UpdateCommandRequest op(
        makeTimeseriesBucketsNamespace(ns(request)),
        {makeTimeseriesTransformationOpEntry(opCtx, bucketId, std::move(transformationFunc))});

    write_ops::WriteCommandRequestBase base;
    // The schema validation configured in the bucket collection is intended for direct
    // operations by end users and is not applicable here.
    base.setBypassDocumentValidation(true);

    // Timeseries compression operation is not a user operation and should not use a
    // statement id from any user op. Set to Uninitialized to bypass.
    base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});

    op.setWriteCommandRequestBase(std::move(base));
    return op;
}

/**
 * Returns the status and whether the request can continue.
 */
TimeseriesSingleWriteResult performTimeseriesInsert(
    OperationContext* opCtx,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds,
    const write_ops::InsertCommandRequest& request) {
    if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
        return {status->first, status->second};
    }
    return getTimeseriesSingleWriteResult(
        write_ops_exec::performInserts(
            opCtx,
            makeTimeseriesInsertOp(batch, metadata, std::move(stmtIds), request),
            OperationSource::kTimeseriesInsert),
        request);
}

/**
 * Returns the status and whether the request can continue.
 */
TimeseriesSingleWriteResult performTimeseriesUpdate(
    OperationContext* opCtx,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata,
    const write_ops::UpdateCommandRequest& op,
    const write_ops::InsertCommandRequest& request) {
    if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
        return {status->first, status->second};
    }
    return getTimeseriesSingleWriteResult(
        write_ops_exec::performUpdates(opCtx, op, OperationSource::kTimeseriesInsert), request);
}

TimeseriesSingleWriteResult performTimeseriesBucketCompression(
    OperationContext* opCtx,
    const timeseries::bucket_catalog::ClosedBucket& closedBucket,
    const write_ops::InsertCommandRequest& request) {
    // Buckets with just a single measurement is not worth compressing.
    if (closedBucket.numMeasurements.has_value() && closedBucket.numMeasurements.value() <= 1) {
        return {SingleWriteResult(), true};
    }

    bool validateCompression = gValidateTimeseriesCompression.load();

    boost::optional<int> beforeSize;
    TimeseriesStats::CompressedBucketInfo compressionStats;

    auto bucketCompressionFunc = [&](const BSONObj& bucketDoc) -> boost::optional<BSONObj> {
        beforeSize = bucketDoc.objsize();
        // Reset every time we run to ensure we never use a stale value
        compressionStats = {};
        auto compressed = timeseries::compressBucket(
            bucketDoc, closedBucket.timeField, ns(request), validateCompression);
        if (compressed.compressedBucket) {
            // If compressed object size is larger than uncompressed, skip compression
            // update.
            if (compressed.compressedBucket->objsize() >= *beforeSize) {
                LOGV2_DEBUG(5857802,
                            1,
                            "Skipping time-series bucket compression, compressed object is "
                            "larger than original",
                            "originalSize"_attr = bucketDoc.objsize(),
                            "compressedSize"_attr = compressed.compressedBucket->objsize());
                return boost::none;
            }

            compressionStats.size = compressed.compressedBucket->objsize();
            compressionStats.numInterleaveRestarts = compressed.numInterleavedRestarts;
        } else if (compressed.decompressionFailed) {
            compressionStats.decompressionFailed = true;
        }

        return compressed.compressedBucket;
    };

    auto compressionOp = makeTimeseriesTransformationOp(
        opCtx, closedBucket.bucketId.oid, bucketCompressionFunc, request);
    auto result = getTimeseriesSingleWriteResult(
        write_ops_exec::performUpdates(opCtx, compressionOp, OperationSource::kStandard), request);

    // Report stats, if we fail before running the transform function then just skip
    // reporting.
    if (beforeSize) {
        compressionStats.result = result.result.getStatus();

        // Report stats for the bucket collection
        // Hold reference to the catalog for collection lookup without locks to be safe.
        auto catalog = CollectionCatalog::get(opCtx);
        auto coll = catalog->lookupCollectionByNamespace(opCtx, compressionOp.getNamespace());
        if (coll) {
            const auto& stats = TimeseriesStats::get(coll);
            stats.onBucketClosed(*beforeSize, compressionStats);
        }
    }

    return result;
}

write_ops::UpdateCommandRequest makeTimeseriesDecompressAndUpdateOp(
    OperationContext* opCtx,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds,
    const write_ops::InsertCommandRequest& request) {
    // Generate the diff and apply it against the previously decrompressed bucket document.
    const bool mustCheckExistenceForInsertOperations =
        static_cast<bool>(repl::tenantMigrationInfo(opCtx));
    auto diff = makeTimeseriesUpdateOpEntry(opCtx, batch, metadata).getU().getDiff();
    auto after =
        doc_diff::applyDiff(
            batch->decompressed.value().after, diff, nullptr, mustCheckExistenceForInsertOperations)
            .postImage;

    auto bucketDecompressionFunc =
        [before = std::move(batch->decompressed.value().before),
         after = std::move(after)](const BSONObj& bucketDoc) -> boost::optional<BSONObj> {
        // Make sure the document hasn't changed since we read it into the BucketCatalog.
        // This should not happen, but since we can double-check it here, we can guard
        // against the missed update that would result from simply replacing with 'after'.
        if (!bucketDoc.binaryEqual(before)) {
            throwWriteConflictException("Bucket document changed between initial read and update");
        }
        return after;
    };

    write_ops::UpdateCommandRequest op(
        makeTimeseriesBucketsNamespace(ns(request)),
        {makeTimeseriesTransformationOpEntry(
            opCtx, batch->bucketHandle.bucketId.oid, std::move(bucketDecompressionFunc))});
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(stmtIds)));
    return op;
}

/**
 * Returns whether the request can continue.
 */
bool commitTimeseriesBucket(OperationContext* opCtx,
                            std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
                            size_t start,
                            size_t index,
                            std::vector<StmtId>&& stmtIds,
                            std::vector<write_ops::WriteError>* errors,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId,
                            std::vector<size_t>* docsToRetry,
                            const write_ops::InsertCommandRequest& request) try {
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);

    auto metadata = getMetadata(bucketCatalog, batch->bucketHandle);
    auto status = prepareCommit(bucketCatalog, batch);
    if (!status.isOK()) {
        invariant(timeseries::bucket_catalog::isWriteBatchFinished(*batch));
        docsToRetry->push_back(index);
        return true;
    }

    hangTimeseriesInsertBeforeWrite.pauseWhileSet();

    const auto docId = batch->bucketHandle.bucketId.oid;
    const bool performInsert = batch->numPreviouslyCommittedMeasurements == 0;
    if (performInsert) {
        const auto output =
            performTimeseriesInsert(opCtx, batch, metadata, std::move(stmtIds), request);
        if (auto error = write_ops_exec::generateError(
                opCtx, output.result.getStatus(), start + index, errors->size())) {
            errors->emplace_back(std::move(*error));
            abort(bucketCatalog, batch, output.result.getStatus());
            return output.canContinue;
        }

        invariant(output.result.getValue().getN() == 1,
                  str::stream() << "Expected 1 insertion of document with _id '" << docId
                                << "', but found " << output.result.getValue().getN() << ".");
    } else {
        auto op = batch->decompressed.has_value()
            ? makeTimeseriesDecompressAndUpdateOp(
                  opCtx, batch, metadata, std::move(stmtIds), request)
            : makeTimeseriesUpdateOp(opCtx, batch, metadata, std::move(stmtIds), request);
        auto const output = performTimeseriesUpdate(opCtx, batch, metadata, op, request);

        if ((output.result.isOK() && output.result.getValue().getNModified() != 1) ||
            output.result.getStatus().code() == ErrorCodes::WriteConflict) {
            abort(bucketCatalog,
                  batch,
                  output.result.isOK()
                      ? Status{ErrorCodes::WriteConflict, "Could not update non-existent bucket"}
                      : output.result.getStatus());
            docsToRetry->push_back(index);
            opCtx->recoveryUnit()->abandonSnapshot();
            return true;
        } else if (auto error = write_ops_exec::generateError(
                       opCtx, output.result.getStatus(), start + index, errors->size())) {
            errors->emplace_back(std::move(*error));
            abort(bucketCatalog, batch, output.result.getStatus());
            return output.canContinue;
        }
    }

    getOpTimeAndElectionId(opCtx, opTime, electionId);

    auto closedBucket =
        finish(bucketCatalog, batch, timeseries::bucket_catalog::CommitInfo{*opTime, *electionId});

    if (closedBucket) {
        // If this write closed a bucket, compress the bucket
        auto output = performTimeseriesBucketCompression(opCtx, *closedBucket, request);
        if (auto error = write_ops_exec::generateError(
                opCtx, output.result.getStatus(), start + index, errors->size())) {
            errors->emplace_back(std::move(*error));
            return output.canContinue;
        }
    }
    return true;
} catch (const DBException& ex) {
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
    abort(bucketCatalog, batch, ex.toStatus());
    throw;
}

TimeseriesAtomicWriteResult commitTimeseriesBucketsAtomically(
    OperationContext* opCtx,
    TimeseriesBatches* batches,
    TimeseriesStmtIds&& stmtIds,
    std::vector<write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    const write_ops::InsertCommandRequest& request) {
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);

    std::vector<std::reference_wrapper<std::shared_ptr<timeseries::bucket_catalog::WriteBatch>>>
        batchesToCommit;

    for (auto& [batch, _] : *batches) {
        if (timeseries::bucket_catalog::claimWriteBatchCommitRights(*batch)) {
            batchesToCommit.push_back(batch);
        }
    }

    if (batchesToCommit.empty()) {
        return TimeseriesAtomicWriteResult::kSuccess;
    }

    // Sort by bucket so that preparing the commit for each batch cannot deadlock.
    std::sort(batchesToCommit.begin(), batchesToCommit.end(), [](auto left, auto right) {
        return left.get()->bucketHandle.bucketId.oid < right.get()->bucketHandle.bucketId.oid;
    });

    Status abortStatus = Status::OK();
    ScopeGuard batchGuard{[&] {
        for (auto batch : batchesToCommit) {
            if (batch.get()) {
                abort(bucketCatalog, batch, abortStatus);
            }
        }
    }};

    try {
        std::vector<write_ops::InsertCommandRequest> insertOps;
        std::vector<write_ops::UpdateCommandRequest> updateOps;

        for (auto batch : batchesToCommit) {
            auto metadata = getMetadata(bucketCatalog, batch.get()->bucketHandle);
            auto prepareCommitStatus = prepareCommit(bucketCatalog, batch);
            if (!prepareCommitStatus.isOK()) {
                abortStatus = prepareCommitStatus;
                return TimeseriesAtomicWriteResult::kContinuableError;
            }

            if (batch.get()->numPreviouslyCommittedMeasurements == 0) {
                insertOps.push_back(makeTimeseriesInsertOp(
                    batch,
                    metadata,
                    std::move(stmtIds[batch.get()->bucketHandle.bucketId.oid]),
                    request));
            } else {
                if (batch.get()->decompressed.has_value()) {
                    updateOps.push_back(makeTimeseriesDecompressAndUpdateOp(
                        opCtx,
                        batch,
                        metadata,
                        std::move(stmtIds[batch.get()->bucketHandle.bucketId.oid]),
                        request));
                } else {
                    updateOps.push_back(makeTimeseriesUpdateOp(
                        opCtx,
                        batch,
                        metadata,
                        std::move(stmtIds[batch.get()->bucketHandle.bucketId.oid]),
                        request));
                }
            }
        }

        hangTimeseriesInsertBeforeWrite.pauseWhileSet();

        auto result = write_ops_exec::performAtomicTimeseriesWrites(opCtx, insertOps, updateOps);
        if (!result.isOK()) {
            abortStatus = result;
            return TimeseriesAtomicWriteResult::kContinuableError;
        }

        getOpTimeAndElectionId(opCtx, opTime, electionId);

        bool compressClosedBuckets = true;
        for (auto batch : batchesToCommit) {
            auto closedBucket = finish(
                bucketCatalog, batch, timeseries::bucket_catalog::CommitInfo{*opTime, *electionId});
            batch.get().reset();

            if (!closedBucket || !compressClosedBuckets) {
                continue;
            }

            // If this write closed a bucket, compress the bucket
            auto ret = performTimeseriesBucketCompression(opCtx, *closedBucket, request);
            if (!ret.result.isOK()) {
                // Don't try to compress any other buckets if we fail. We're not allowed to
                // do more write operations.
                compressClosedBuckets = false;
            }
            if (!ret.canContinue) {
                abortStatus = ret.result.getStatus();
                return TimeseriesAtomicWriteResult::kNonContinuableError;
            }
        }
    } catch (const DBException& ex) {
        abortStatus = ex.toStatus();
        throw;
    }

    batchGuard.dismiss();
    return TimeseriesAtomicWriteResult::kSuccess;
}

// For sharded time-series collections, we need to use the granularity from the config
// server (through shard filtering information) as the source of truth for the current
// granularity value, due to the possible inconsistency in the process of granularity
// updates.
void rebuildOptionsWithGranularityFromConfigServer(OperationContext* opCtx,
                                                   TimeseriesOptions& timeSeriesOptions,
                                                   const NamespaceString& bucketsNs) {
    AutoGetCollectionForRead coll(opCtx, bucketsNs);
    auto collDesc = CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, bucketsNs)
                        ->getCollectionDescription(opCtx);
    if (collDesc.isSharded()) {
        tassert(6102801,
                "Sharded time-series buckets collection is missing time-series fields",
                collDesc.getTimeseriesFields());
        auto granularity = collDesc.getTimeseriesFields()->getGranularity();
        auto bucketMaxSpanSeconds = collDesc.getTimeseriesFields()->getBucketMaxSpanSeconds();

        if (granularity) {
            timeSeriesOptions.setGranularity(granularity.get());
            timeSeriesOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(*granularity));

            if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility)) {
                timeSeriesOptions.setBucketRoundingSeconds(
                    timeseries::getBucketRoundingSecondsFromGranularity(*granularity));
            }
        } else if (!bucketMaxSpanSeconds) {
            timeSeriesOptions.setGranularity(BucketGranularityEnum::Seconds);
            timeSeriesOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(*timeSeriesOptions.getGranularity()));
            if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility)) {
                timeSeriesOptions.setBucketRoundingSeconds(
                    timeseries::getBucketRoundingSecondsFromGranularity(
                        *timeSeriesOptions.getGranularity()));
            }
        } else {
            invariant(feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                          serverGlobalParams.featureCompatibility) &&
                      bucketMaxSpanSeconds);
            timeSeriesOptions.setBucketMaxSpanSeconds(bucketMaxSpanSeconds);

            auto bucketRoundingSeconds = collDesc.getTimeseriesFields()->getBucketRoundingSeconds();
            invariant(bucketRoundingSeconds);
            timeSeriesOptions.setBucketRoundingSeconds(bucketRoundingSeconds);
        }
    }
}

std::tuple<TimeseriesBatches, TimeseriesStmtIds, size_t /* numInserted */, bool /* canContinue */>
insertIntoBucketCatalog(OperationContext* opCtx,
                        size_t start,
                        size_t numDocs,
                        const std::vector<size_t>& indices,
                        std::vector<write_ops::WriteError>* errors,
                        bool* containsRetry,
                        const write_ops::InsertCommandRequest& request) {
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);

    auto bucketsNs = makeTimeseriesBucketsNamespace(ns(request));
    // Holding this shared pointer to the collection guarantees that the collator is not
    // invalidated.
    auto catalog = CollectionCatalog::get(opCtx);
    auto bucketsColl = catalog->lookupCollectionByNamespace(opCtx, bucketsNs);
    uassert(ErrorCodes::NamespaceNotFound,
            "Could not find time-series buckets collection for write",
            bucketsColl);
    uassert(ErrorCodes::InvalidOptions,
            "Time-series buckets collection is missing time-series options",
            bucketsColl->getTimeseriesOptions());

    auto timeSeriesOptions = *bucketsColl->getTimeseriesOptions();

    boost::optional<Status> rebuildOptionsError;
    try {
        rebuildOptionsWithGranularityFromConfigServer(opCtx, timeSeriesOptions, bucketsNs);
    } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>& ex) {
        // This could occur when the shard version attached to the request is for the time
        // series namespace (unsharded), which is compared to the shard version of the
        // bucket namespace. Consequently, every single entry fails but the whole operation
        // succeeds.

        rebuildOptionsError = ex.toStatus();

        auto& oss{OperationShardingState::get(opCtx)};
        oss.setShardingOperationFailedStatus(ex.toStatus());
    }

    TimeseriesBatches batches;
    TimeseriesStmtIds stmtIds;
    bool canContinue = true;

    auto insert = [&](size_t index) {
        invariant(start + index < request.getDocuments().size());

        if (rebuildOptionsError) {
            const auto error{write_ops_exec::generateError(
                opCtx, *rebuildOptionsError, start + index, errors->size())};
            errors->emplace_back(std::move(*error));
            return false;
        }

        auto stmtId = request.getStmtIds() ? request.getStmtIds()->at(start + index)
                                           : request.getStmtId().value_or(0) + start + index;

        if (isTimeseriesWriteRetryable(opCtx) &&
            TransactionParticipant::get(opCtx).checkStatementExecutedNoOplogEntryFetch(opCtx,
                                                                                       stmtId)) {
            RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
            *containsRetry = true;
            return true;
        }

        auto viewNs = ns(request).isTimeseriesBucketsCollection()
            ? ns(request).getTimeseriesViewNamespace()
            : ns(request);
        auto& measurementDoc = request.getDocuments()[start + index];

        StatusWith<timeseries::bucket_catalog::InsertResult> swResult =
            Status{ErrorCodes::BadValue, "Uninitialized InsertResult"};
        do {
            if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility)) {
                swResult = timeseries::bucket_catalog::tryInsert(
                    opCtx,
                    bucketCatalog,
                    viewNs,
                    bucketsColl->getDefaultCollator(),
                    timeSeriesOptions,
                    measurementDoc,
                    canCombineTimeseriesInsertWithOtherClients(opCtx, request));

                if (swResult.isOK()) {
                    const auto& insertResult = swResult.getValue();

                    // If the InsertResult doesn't contain a batch, we failed to insert the
                    // measurement into an open bucket and need to create/reopen a bucket.
                    if (!insertResult.batch) {
                        timeseries::bucket_catalog::BucketFindResult bucketFindResult;
                        BSONObj suitableBucket;

                        if (auto* bucketId = stdx::get_if<OID>(&insertResult.candidate)) {
                            DBDirectClient client{opCtx};
                            hangTimeseriesInsertBeforeReopeningQuery.pauseWhileSet();
                            suitableBucket =
                                client.findOne(bucketsColl->ns(), BSON("_id" << *bucketId));
                            bucketFindResult.fetchedBucket = true;
                        } else if (auto* pipeline = stdx::get_if<std::vector<BSONObj>>(
                                       &insertResult.candidate)) {
                            // Resort to Query-Based reopening approach.
                            DBDirectClient client{opCtx};

                            // Ensure we have a index on meta and time for the time-series
                            // collection before performing the query. Without the index we
                            // will perform a full collection scan which could cause us to
                            // take a performance hit.
                            if (timeseries::collectionHasIndexSupportingReopeningQuery(
                                    opCtx, bucketsColl->getIndexCatalog(), timeSeriesOptions)) {
                                hangTimeseriesInsertBeforeReopeningQuery.pauseWhileSet();

                                // Run an aggregation to find a suitable bucket to reopen.
                                AggregateCommandRequest aggRequest(bucketsColl->ns(), *pipeline);

                                auto cursor = uassertStatusOK(
                                    DBClientCursor::fromAggregationRequest(&client,
                                                                           aggRequest,
                                                                           false /* secondaryOk
                                                                           */, false /*
                                                                           useExhaust*/));

                                if (cursor->more()) {
                                    suitableBucket = cursor->next();
                                }
                                bucketFindResult.queriedBucket = true;
                            }
                        }

                        boost::optional<timeseries::bucket_catalog::BucketToReopen> bucketToReopen =
                            boost::none;
                        if (!suitableBucket.isEmpty()) {
                            auto validator = [&](OperationContext * opCtx,
                                                 const BSONObj& bucketDoc) -> auto {
                                return bucketsColl->checkValidation(opCtx, bucketDoc);
                            };
                            auto bucketToReopen = timeseries::bucket_catalog::BucketToReopen{
                                suitableBucket, validator, insertResult.catalogEra};
                            bucketFindResult.bucketToReopen = std::move(bucketToReopen);
                        }

                        swResult = timeseries::bucket_catalog::insert(
                            opCtx,
                            bucketCatalog,
                            viewNs,
                            bucketsColl->getDefaultCollator(),
                            timeSeriesOptions,
                            measurementDoc,
                            canCombineTimeseriesInsertWithOtherClients(opCtx, request),
                            std::move(bucketFindResult));
                    }
                }
            } else {
                timeseries::bucket_catalog::BucketFindResult bucketFindResult;
                swResult = timeseries::bucket_catalog::insert(
                    opCtx,
                    bucketCatalog,
                    viewNs,
                    bucketsColl->getDefaultCollator(),
                    timeSeriesOptions,
                    measurementDoc,
                    canCombineTimeseriesInsertWithOtherClients(opCtx, request),
                    bucketFindResult);
            }

            // If there is an era offset (between the bucket we want to reopen and the
            // catalog's current era), we could hit a WriteConflict error indicating we will
            // need to refetch a bucket document as it is potentially stale.
        } while (!swResult.isOK() && (swResult.getStatus().code() == ErrorCodes::WriteConflict));

        if (auto error = write_ops_exec::generateError(
                opCtx, swResult.getStatus(), start + index, errors->size())) {
            invariant(swResult.getStatus().code() != ErrorCodes::WriteConflict);
            errors->emplace_back(std::move(*error));
            return false;
        }

        auto& insertResult = swResult.getValue();
        batches.emplace_back(std::move(insertResult.batch), index);
        const auto& batch = batches.back().first;
        if (isTimeseriesWriteRetryable(opCtx)) {
            stmtIds[batch->bucketHandle.bucketId.oid].push_back(stmtId);
        }

        // If this insert closed buckets, rewrite to be a compressed column. If we cannot
        // perform write operations at this point the bucket will be left uncompressed.
        for (const auto& closedBucket : insertResult.closedBuckets) {
            if (!canContinue) {
                break;
            }

            // If this write closed a bucket, compress the bucket
            auto ret = performTimeseriesBucketCompression(opCtx, closedBucket, request);
            if (auto error = write_ops_exec::generateError(
                    opCtx, ret.result.getStatus(), start + index, errors->size())) {
                // Bucket compression only fail when we may not try to perform any other
                // write operation. When handleError() inside write_ops_exec.cpp return
                // false.
                errors->emplace_back(std::move(*error));
                canContinue = false;
                return false;
            }
            canContinue = ret.canContinue;
        }

        return true;
    };

    if (!indices.empty()) {
        std::for_each(indices.begin(), indices.end(), insert);
    } else {
        for (size_t i = 0; i < numDocs; i++) {
            if (!insert(i) && request.getOrdered()) {
                return {std::move(batches), std::move(stmtIds), i, canContinue};
            }
        }
    }

    return {std::move(batches), std::move(stmtIds), request.getDocuments().size(), canContinue};
}

void getTimeseriesBatchResults(OperationContext* opCtx,
                               const TimeseriesBatches& batches,
                               size_t start,
                               size_t indexOfLastProcessedBatch,
                               bool canContinue,
                               std::vector<write_ops::WriteError>* errors,
                               boost::optional<repl::OpTime>* opTime,
                               boost::optional<OID>* electionId,
                               std::vector<size_t>* docsToRetry = nullptr) {
    boost::optional<write_ops::WriteError> lastError;
    if (!errors->empty()) {
        lastError = errors->back();
    }

    for (size_t itr = 0; itr < batches.size(); ++itr) {
        const auto& [batch, index] = batches[itr];
        if (!batch) {
            continue;
        }

        // If there are any unprocessed batches, we mark them as error with the last known
        // error.
        if (itr > indexOfLastProcessedBatch &&
            timeseries::bucket_catalog::claimWriteBatchCommitRights(*batch)) {
            auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
            abort(bucketCatalog, batch, lastError->getStatus());
            errors->emplace_back(start + index, lastError->getStatus());
            continue;
        }

        auto swCommitInfo = timeseries::bucket_catalog::getWriteBatchResult(*batch);
        if (swCommitInfo.getStatus() == ErrorCodes::TimeseriesBucketCleared) {
            tassert(6023102, "the 'docsToRetry' cannot be null", docsToRetry);
            docsToRetry->push_back(index);
            continue;
        }
        if (swCommitInfo.getStatus() == ErrorCodes::WriteConflict) {
            docsToRetry->push_back(index);
            opCtx->recoveryUnit()->abandonSnapshot();
            continue;
        }
        if (auto error = write_ops_exec::generateError(
                opCtx, swCommitInfo.getStatus(), start + index, errors->size())) {
            errors->emplace_back(std::move(*error));
            continue;
        }

        const auto& commitInfo = swCommitInfo.getValue();
        if (commitInfo.opTime) {
            *opTime = std::max(opTime->value_or(repl::OpTime()), *commitInfo.opTime);
        }
        if (commitInfo.electionId) {
            *electionId = std::max(electionId->value_or(OID()), *commitInfo.electionId);
        }
    }

    // If we cannot continue the request, we should convert all the 'docsToRetry' into an
    // error.
    if (!canContinue && docsToRetry) {
        for (auto&& index : *docsToRetry) {
            errors->emplace_back(start + index, lastError->getStatus());
        }
        docsToRetry->clear();
    }
}

TimeseriesAtomicWriteResult performOrderedTimeseriesWritesAtomically(
    OperationContext* opCtx,
    std::vector<write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    bool* containsRetry,
    const write_ops::InsertCommandRequest& request) {
    auto [batches, stmtIds, numInserted, canContinue] = insertIntoBucketCatalog(
        opCtx, 0, request.getDocuments().size(), {}, errors, containsRetry, request);
    if (!canContinue) {
        return TimeseriesAtomicWriteResult::kNonContinuableError;
    }

    hangTimeseriesInsertBeforeCommit.pauseWhileSet();

    auto result = commitTimeseriesBucketsAtomically(
        opCtx, &batches, std::move(stmtIds), errors, opTime, electionId, request);
    if (result != TimeseriesAtomicWriteResult::kSuccess) {
        return result;
    }

    getTimeseriesBatchResults(opCtx, batches, 0, batches.size(), true, errors, opTime, electionId);

    return TimeseriesAtomicWriteResult::kSuccess;
}

/**
 * Writes to the underlying system.buckets collection. Returns the indices, of the batch
 * which were attempted in an update operation, but found no bucket to update. These indices
 * can be passed as the 'indices' parameter in a subsequent call to this function, in order
 * to to be retried.
 */
std::vector<size_t> performUnorderedTimeseriesWrites(
    OperationContext* opCtx,
    size_t start,
    size_t numDocs,
    const std::vector<size_t>& indices,
    std::vector<write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    bool* containsRetry,
    const write_ops::InsertCommandRequest& request) {
    auto [batches, bucketStmtIds, _, canContinue] =
        insertIntoBucketCatalog(opCtx, start, numDocs, indices, errors, containsRetry, request);

    hangTimeseriesInsertBeforeCommit.pauseWhileSet();

    std::vector<size_t> docsToRetry;

    if (!canContinue) {
        return docsToRetry;
    }

    size_t itr = 0;
    for (; itr < batches.size(); ++itr) {
        auto& [batch, index] = batches[itr];
        if (timeseries::bucket_catalog::claimWriteBatchCommitRights(*batch)) {
            auto stmtIds = isTimeseriesWriteRetryable(opCtx)
                ? std::move(bucketStmtIds[batch->bucketHandle.bucketId.oid])
                : std::vector<StmtId>{};

            canContinue = commitTimeseriesBucket(opCtx,
                                                 batch,
                                                 start,
                                                 index,
                                                 std::move(stmtIds),
                                                 errors,
                                                 opTime,
                                                 electionId,
                                                 &docsToRetry,
                                                 request);
            batch.reset();
            if (!canContinue) {
                break;
            }
        }
    }

    getTimeseriesBatchResults(
        opCtx, batches, 0, itr, canContinue, errors, opTime, electionId, &docsToRetry);
    tassert(6023101,
            "the 'docsToRetry' cannot exist when the request cannot be continued",
            canContinue || docsToRetry.empty());
    return docsToRetry;
}

void performUnorderedTimeseriesWritesWithRetries(OperationContext* opCtx,
                                                 size_t start,
                                                 size_t numDocs,
                                                 std::vector<write_ops::WriteError>* errors,
                                                 boost::optional<repl::OpTime>* opTime,
                                                 boost::optional<OID>* electionId,
                                                 bool* containsRetry,
                                                 const write_ops::InsertCommandRequest& request) {
    std::vector<size_t> docsToRetry;
    do {
        docsToRetry = performUnorderedTimeseriesWrites(
            opCtx, start, numDocs, docsToRetry, errors, opTime, electionId, containsRetry, request);
    } while (!docsToRetry.empty());
}

/**
 * Returns the number of documents that were inserted.
 */
size_t performOrderedTimeseriesWrites(OperationContext* opCtx,
                                      std::vector<write_ops::WriteError>* errors,
                                      boost::optional<repl::OpTime>* opTime,
                                      boost::optional<OID>* electionId,
                                      bool* containsRetry,
                                      const write_ops::InsertCommandRequest& request) {
    auto result = performOrderedTimeseriesWritesAtomically(
        opCtx, errors, opTime, electionId, containsRetry, request);
    switch (result) {
        case TimeseriesAtomicWriteResult::kSuccess:
            return request.getDocuments().size();
        case TimeseriesAtomicWriteResult::kNonContinuableError:
            // If we can't continue, we know that 0 were inserted since this function should
            // guarantee that the inserts are atomic.
            return 0;
        case TimeseriesAtomicWriteResult::kContinuableError:
            break;
        default:
            MONGO_UNREACHABLE;
    }

    for (size_t i = 0; i < request.getDocuments().size(); ++i) {
        performUnorderedTimeseriesWritesWithRetries(
            opCtx, i, 1, errors, opTime, electionId, containsRetry, request);
        if (!errors->empty()) {
            return i;
        }
    }

    return request.getDocuments().size();
}

}  // namespace

write_ops::InsertCommandReply performTimeseriesWrites(
    OperationContext* opCtx, const write_ops::InsertCommandRequest& request) {
    auto& curOp = *CurOp::get(opCtx);
    ON_BLOCK_EXIT([&] {
        // This is the only part of finishCurOp we need to do for inserts because they reuse
        // the top-level curOp. The rest is handled by the top-level entrypoint.
        curOp.done();
        Top::get(opCtx->getServiceContext())
            .record(opCtx,
                    ns(request).ns(),
                    LogicalOp::opInsert,
                    Top::LockType::WriteLocked,
                    durationCount<Microseconds>(curOp.elapsedTimeExcludingPauses()),
                    curOp.isCommand(),
                    curOp.getReadWriteType());
    });

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot insert into a time-series collection in a multi-document "
                             "transaction: "
                          << ns(request),
            !opCtx->inMultiDocumentTransaction());

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS_inlock(ns(request));
        curOp.setLogicalOp_inlock(LogicalOp::opInsert);
        curOp.ensureStarted();
        curOp.debug().additiveMetrics.ninserted = 0;
    }

    std::vector<write_ops::WriteError> errors;
    boost::optional<repl::OpTime> opTime;
    boost::optional<OID> electionId;
    bool containsRetry = false;

    write_ops::InsertCommandReply insertReply;
    auto& baseReply = insertReply.getWriteCommandReplyBase();

    if (request.getOrdered()) {
        baseReply.setN(performOrderedTimeseriesWrites(
            opCtx, &errors, &opTime, &electionId, &containsRetry, request));
    } else {
        performUnorderedTimeseriesWritesWithRetries(opCtx,
                                                    0,
                                                    request.getDocuments().size(),
                                                    &errors,
                                                    &opTime,
                                                    &electionId,
                                                    &containsRetry,
                                                    request);
        baseReply.setN(request.getDocuments().size() - errors.size());
    }

    if (!errors.empty()) {
        baseReply.setWriteErrors(std::move(errors));
    }
    if (opTime) {
        baseReply.setOpTime(*opTime);
    }
    if (electionId) {
        baseReply.setElectionId(*electionId);
    }
    if (containsRetry) {
        RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
    }

    curOp.debug().additiveMetrics.ninserted = baseReply.getN();
    globalOpCounters.gotInserts(baseReply.getN());

    return insertReply;
}

}  // namespace mongo::write_ops_exec

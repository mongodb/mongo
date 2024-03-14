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

#include "mongo/base/error_codes.h"
#include <absl/container/flat_hash_map.h>
#include <absl/hash/hash.h>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "mongo/base/counter.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/introspect.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/parsed_writes_common.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/closed_bucket.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/message.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

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
        _durationMaxMs.setToMax(durationCount<Milliseconds>(duration));

        _numDocsTotal.increment(numDocs);
        _numDocsMax.setToMax(numDocs);
    }

private:
    /**
     * To avoid rapid accumulation of roundoff error in the duration total, it
     * is maintained precisely, and we arrange for the corresponding
     * Millisecond metric to hold an exported low-res image of it.
     */
    Counter64 _durationTotalMicroseconds;

    Atomic64Metric& _durationTotalMs =
        *MetricBuilder<Atomic64Metric>("query.updateDeleteManyDurationTotalMs");
    Atomic64Metric& _durationMaxMs =
        *MetricBuilder<Atomic64Metric>("query.updateDeleteManyDurationMaxMs");
    Counter64& _numDocsTotal =
        *MetricBuilder<Counter64>{"query.updateDeleteManyDocumentsTotalCount"};
    Atomic64Metric& _numDocsMax =
        *MetricBuilder<Atomic64Metric>("query.updateDeleteManyDocumentsMaxCount");
};

MultiUpdateDeleteMetrics collectMultiUpdateDeleteMetrics;

void finishCurOp(OperationContext* opCtx, CurOp* curOp) {
    try {
        curOp->done();
        auto executionTimeMicros = duration_cast<Microseconds>(curOp->elapsedTimeExcludingPauses());
        curOp->debug().additiveMetrics.executionTime = executionTimeMicros;

        recordCurOpMetrics(opCtx);
        Top::get(opCtx->getServiceContext())
            .record(opCtx,
                    curOp->getNSS(),
                    curOp->getLogicalOp(),
                    Top::LockType::WriteLocked,
                    durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                    curOp->isCommand(),
                    curOp->getReadWriteType());

        if (!curOp->debug().errInfo.isOK()) {
            LOGV2_DEBUG(20886,
                        3,
                        "Caught Assertion in finishCurOp",
                        "operation"_attr = redact(logicalOpToString(curOp->getLogicalOp())),
                        "error"_attr = curOp->debug().errInfo.toString());
        }

        // Mark the op as complete, log it and profile if the op should be sampled for profiling.
        logOperationAndProfileIfNeeded(opCtx, curOp);

    } catch (const DBException& ex) {
        // We need to ignore all errors here. We don't want a successful op to fail because of a
        // failure to record stats. We also don't want to replace the error reported for an op that
        // is failing.
        LOGV2(20887, "Ignoring error from finishCurOp", "error"_attr = redact(ex));
    }
}

void makeCollection(OperationContext* opCtx, const NamespaceString& ns) {
    writeConflictRetry(opCtx, "implicit collection creation", ns, [&opCtx, &ns] {
        AutoGetDb autoDb(opCtx, ns.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, ns, MODE_IX);

        assertCanWrite_inlock(opCtx, ns);
        if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
                opCtx, ns)) {  // someone else may have beat us to it.
            uassertStatusOK(userAllowedCreateNS(opCtx, ns));
            // TODO (SERVER-77915): Remove once 8.0 becomes last LTS.
            // TODO (SERVER-82066): Update handling for direct connections.
            // TODO (SERVER-81937): Update handling for transactions.
            // TODO (SERVER-85366): Update handling for retryable writes.
            boost::optional<OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE>
                allowCollectionCreation;
            const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
            if (!fcvSnapshot.isVersionInitialized() ||
                !feature_flags::gTrackUnshardedCollectionsUponCreation.isEnabled(fcvSnapshot) ||
                !OperationShardingState::get(opCtx).isComingFromRouter(opCtx) ||
                (opCtx->inMultiDocumentTransaction() || opCtx->isRetryableWrite())) {
                allowCollectionCreation.emplace(opCtx);
            }
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

namespace {
// Acquire optimes and fill them in for each item in the batch.  This must only be done for
// doc-locking storage engines, which are allowed to insert oplog documents
// out-of-timestamp-order.  For other storage engines, the oplog entries must be physically
// written in timestamp order, so we defer optime assignment until the oplog is about to be
// written. Multidocument transactions should not generate opTimes because they are
// generated at the time of commit.
void acquireOplogSlotsForInserts(OperationContext* opCtx,
                                 const CollectionAcquisition& collection,
                                 std::vector<InsertStatement>::iterator begin,
                                 std::vector<InsertStatement>::iterator end) {
    dassert(!opCtx->inMultiDocumentTransaction());
    dassert(!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collection.nss()));

    // Populate 'slots' with new optimes for each insert.
    // This also notifies the storage engine of each new timestamp.
    auto batchSize = std::distance(begin, end);
    auto oplogSlots = repl::getNextOpTimes(opCtx, batchSize);
    auto slot = oplogSlots.begin();
    for (auto it = begin; it != end; it++) {
        it->oplogSlot = *slot++;
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
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "collectionNS");
            return fpNss.isEmpty() || collection.nss() == fpNss;
        });
}
}  // namespace

void insertDocumentsAtomically(OperationContext* opCtx,
                               const CollectionAcquisition& collection,
                               std::vector<InsertStatement>::iterator begin,
                               std::vector<InsertStatement>::iterator end,
                               bool fromMigrate) {
    auto batchSize = std::distance(begin, end);
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool inTransaction = opCtx->inMultiDocumentTransaction();
    const bool oplogDisabled = replCoord->isOplogDisabledFor(opCtx, collection.nss());
    WriteUnitOfWork::OplogEntryGroupType oplogEntryGroupType = WriteUnitOfWork::kDontGroup;

    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const bool replicateVectoredInsertsTransactionally = fcvSnapshot.isVersionInitialized() &&
        repl::feature_flags::gReplicateVectoredInsertsTransactionally.isEnabled(fcvSnapshot);
    // For multiple inserts not part of a multi-document transaction, the inserts will be
    // batched into a single applyOps oplog entry.
    if (replicateVectoredInsertsTransactionally && !inTransaction && batchSize > 1 &&
        !oplogDisabled) {
        oplogEntryGroupType = WriteUnitOfWork::kGroupForPossiblyRetryableOperations;
    }

    // Intentionally not using writeConflictRetry. That is handled by the caller so it can react to
    // oversized batches.
    WriteUnitOfWork wuow(opCtx, oplogEntryGroupType);

    // Capped collections may implicitly generate a delete in the same unit of work as an
    // insert. In order to avoid that delete generating a second timestamp in a WUOW which
    // has un-timestamped writes (which is a violation of multi-timestamp constraints),
    // we must reserve the timestamp for the insert in advance.
    if (oplogEntryGroupType != WriteUnitOfWork::kGroupForPossiblyRetryableOperations &&
        !inTransaction && !oplogDisabled &&
        (!replicateVectoredInsertsTransactionally || collection.getCollectionPtr()->isCapped())) {
        acquireOplogSlotsForInserts(opCtx, collection, begin, end);
    }

    uassertStatusOK(collection_internal::insertDocuments(opCtx,
                                                         collection.getCollectionPtr(),
                                                         begin,
                                                         end,
                                                         &CurOp::get(opCtx)->debug(),
                                                         fromMigrate));
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
                str::stream() << "Collection '" << collection->ns().toStringForErrorMsg()
                              << "' is a capped collection. Writes in transactions are not allowed "
                                 "on capped collections."};
    }
    return Status::OK();
}

void assertTimeseriesBucketsCollectionNotFound(const NamespaceString& ns) {
    uasserted(ErrorCodes::NamespaceNotFound,
              str::stream() << "Buckets collection not found for time-series collection "
                            << ns.getTimeseriesViewNamespace().toStringForErrorMsg());
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
                                                  const write_ops::WriteCommandRequestBase& req,
                                                  const boost::optional<TenantId>& tenantId) {
    auto& encryptionInfo = req.getEncryptionInformation();
    const bool fleCrudProcessed = getFleCrudProcessed(opCtx, encryptionInfo, tenantId);
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

    if (ex.code() == ErrorCodes::StaleDbVersion || ErrorCodes::isStaleShardVersionError(ex) ||
        ex.code() == ErrorCodes::ShardCannotRefreshDueToLocksHeld ||
        ex.code() == ErrorCodes::CannotImplicitlyCreateCollection) {
        if (!opCtx->getClient()->isInDirectClient() &&
            ex.code() != ErrorCodes::CannotImplicitlyCreateCollection) {
            auto& oss = OperationShardingState::get(opCtx);
            oss.setShardingOperationFailedStatus(ex.toStatus());
        }

        // For routing errors, it is guaranteed that all subsequent operations will fail
        // with the same cause, so don't try doing any more operations. The command reply serializer
        // will handle repeating this error for unordered writes.
        // (On the other hand, ShardCannotRefreshDueToLocksHeld is caused by a temporary inability
        // to access a stable version of the cache during the execution of the batch; the error is
        // returned back to the router to leverage its capability of selectively retrying
        // operations).
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

    out->results.emplace_back(ex.toStatus());
    return !ordered;
}

bool getFleCrudProcessed(OperationContext* opCtx,
                         const boost::optional<EncryptionInformation>& encryptionInfo,
                         const boost::optional<TenantId>& tenantId) {
    if (encryptionInfo && encryptionInfo->getCrudProcessed().value_or(false)) {
        uassert(6666201,
                "External users cannot have crudProcessed enabled",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(tenantId), ActionType::internal));

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
                                OperationSource source,
                                LastOpFixer* lastOpFixer,
                                WriteResult* out) {
    if (batch.empty())
        return true;

    auto& curOp = *CurOp::get(opCtx);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchInsert,
        opCtx,
        "hangDuringBatchInsert",
        [&nss]() {
            LOGV2(20889,
                  "Batch insert - hangDuringBatchInsert fail point enabled for a namespace. "
                  "Blocking until fail point is disabled",
                  logAttrs(nss));
        },
        nss);

    if (MONGO_unlikely(failAllInserts.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllInserts failpoint active!");
    }

    boost::optional<CollectionAcquisition> collection;
    auto acquireCollection = [&] {
        while (true) {
            collection.emplace(mongo::acquireCollection(
                opCtx,
                CollectionAcquisitionRequest::fromOpCtx(
                    opCtx, nss, AcquisitionPrerequisites::kWrite, collectionUUID),
                fixLockModeForSystemDotViewsChanges(nss, MODE_IX)));
            if (collection->exists()) {
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
        // We can get an unauthorized error in the case of direct shard operations. This command
        // will not succeed upon the next collection acquisition either.
        if (ex.code() == ErrorCodes::Unauthorized) {
            throw;
        }
    }

    if (shouldProceedWithBatchInsert) {
        try {
            if (!collection->getCollectionPtr()->isCapped() && !inTxn && batch.size() > 1) {
                // First try doing it all together. If all goes well, this is all we need to do.
                // See Collection::_insertDocuments for why we do all capped inserts one-at-a-time.
                lastOpFixer->startingOp(nss);
                insertDocumentsAtomically(opCtx,
                                          *collection,
                                          batch.begin(),
                                          batch.end(),
                                          source == OperationSource::kFromMigrate);
                lastOpFixer->finishedOpSuccessfully();
                globalOpCounters.gotInserts(batch.size());
                SingleWriteResult result;
                result.setN(1);

                std::fill_n(std::back_inserter(out->results), batch.size(), std::move(result));
                if (source != OperationSource::kTimeseriesInsert) {
                    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInserts(
                        opCtx->getWriteConcern(), batch.size());
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
        if (source != OperationSource::kTimeseriesInsert) {
            ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
                opCtx->getWriteConcern());
        }
        try {
            writeConflictRetry(opCtx, "insert", nss, [&] {
                try {
                    if (!collection)
                        acquireCollection();
                    // Transactions are not allowed to operate on capped collections.
                    uassertStatusOK(
                        checkIfTransactionOnCappedColl(opCtx, collection->getCollectionPtr()));
                    lastOpFixer->startingOp(nss);
                    insertDocumentsAtomically(
                        opCtx, *collection, it, it + 1, source == OperationSource::kFromMigrate);
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
                                         bool isRemove,
                                         StringData operationName) {
    BSONObj value;
    PlanExecutor::ExecState state;
    try {
        state = exec->getNext(&value, nullptr);
    } catch (const StorageUnavailableException&) {
        throw;
    } catch (DBException& exception) {
        auto&& explainer = exec->getPlanExplainer();
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        LOGV2_WARNING(7267501,
                      "Plan executor error",
                      "operation"_attr = operationName,
                      "error"_attr = exception.toStatus(),
                      "stats"_attr = redact(stats));

        exception.addContext("Plan executor error during " + operationName);
        throw;
    }

    if (PlanExecutor::ADVANCED == state) {
        return {std::move(value)};
    }

    invariant(state == PlanExecutor::IS_EOF);
    return boost::none;
}

UpdateResult performUpdate(OperationContext* opCtx,
                           const NamespaceString& nss,
                           CurOp* curOp,
                           bool inTransaction,
                           bool remove,
                           bool upsert,
                           const boost::optional<mongo::UUID>& collectionUUID,
                           boost::optional<BSONObj>& docFound,
                           UpdateRequest* updateRequest) {
    auto [isTimeseriesViewUpdate, nsString] =
        timeseries::isTimeseriesViewRequest(opCtx, *updateRequest);

    if (isTimeseriesViewUpdate) {
        checkCollectionUUIDMismatch(
            opCtx, nsString.getTimeseriesViewNamespace(), nullptr, collectionUUID);
    }

    // TODO SERVER-76583: Remove this check.
    uassert(7314600,
            "Retryable findAndModify on a timeseries is not supported",
            !isTimeseriesViewUpdate || !updateRequest->shouldReturnAnyDocs() ||
                !opCtx->isRetryableWrite());

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchUpdate,
        opCtx,
        "hangDuringBatchUpdate",
        [&nss]() {
            LOGV2(7280400,
                  "Batch update - hangDuringBatchUpdate fail point enabled for a namespace. "
                  "Blocking until fail point is disabled",
                  logAttrs(nss));
        },
        nss);

    if (MONGO_unlikely(failAllUpdates.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllUpdates failpoint active!");
    }

    auto collection =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx, nsString, AcquisitionPrerequisites::kWrite, collectionUUID),
                          MODE_IX);
    auto dbName = nsString.dbName();
    Database* db = [&]() {
        AutoGetDb autoDb(opCtx, dbName, MODE_IX);
        return autoDb.ensureDbExists(opCtx);
    }();

    invariant(DatabaseHolder::get(opCtx)->getDb(opCtx, dbName));
    curOp->raiseDbProfileLevel(CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(dbName));

    assertCanWrite_inlock(opCtx, nsString);

    // TODO SERVER-50983: Create abstraction for creating collection when using
    // AutoGetCollection Create the collection if it does not exist when performing an upsert
    // because the update stage does not create its own collection
    if (!collection.exists() && upsert) {
        CollectionWriter collectionWriter(opCtx, &collection);
        uassertStatusOK(userAllowedCreateNS(opCtx, nsString));
        // TODO (SERVER-77915): Remove once 8.0 becomes last LTS.
        // TODO (SERVER-82066): Update handling for direct connections.
        // TODO (SERVER-81937): Update handling for transactions.
        // TODO (SERVER-85366): Update handling for retryable writes.
        boost::optional<OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE>
            allowCollectionCreation;
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        if (!fcvSnapshot.isVersionInitialized() ||
            !feature_flags::gTrackUnshardedCollectionsUponCreation.isEnabled(fcvSnapshot) ||
            !OperationShardingState::get(opCtx).isComingFromRouter(opCtx) ||
            (opCtx->inMultiDocumentTransaction() || opCtx->isRetryableWrite())) {
            allowCollectionCreation.emplace(opCtx);
        }
        WriteUnitOfWork wuow(opCtx);
        ScopedLocalCatalogWriteFence scopedLocalCatalogWriteFence(opCtx, &collection);
        CollectionOptions defaultCollectionOptions;
        uassertStatusOK(db->userCreateNS(opCtx, nsString, defaultCollectionOptions));
        wuow.commit();
    }

    if (collection.exists() && collection.getCollectionPtr()->isCapped()) {
        uassert(
            ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Collection '" << collection.nss().toStringForErrorMsg()
                          << "' is a capped collection. Writes in transactions are not allowed on "
                             "capped collections.",
            !inTransaction);
    }

    if (isTimeseriesViewUpdate) {
        timeseries::timeseriesRequestChecks<UpdateRequest>(
            collection.getCollectionPtr(), updateRequest, timeseries::updateRequestCheckFunction);
        timeseries::timeseriesHintTranslation<UpdateRequest>(collection.getCollectionPtr(),
                                                             updateRequest);
    }

    ParsedUpdate parsedUpdate(opCtx,
                              updateRequest,
                              collection.getCollectionPtr(),
                              false /*forgoOpCounterIncrements*/,
                              isTimeseriesViewUpdate);
    uassertStatusOK(parsedUpdate.parseRequest());

    const auto exec = uassertStatusOK(
        getExecutorUpdate(&curOp->debug(), collection, &parsedUpdate, boost::none /* verbosity
        */));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    docFound = advanceExecutor(opCtx,
                               exec.get(),
                               remove,
                               updateRequest->shouldReturnAnyDocs() ? "findAndModify" : "update");
    // Nothing after advancing the plan executor should throw a WriteConflictException,
    // so the following bookkeeping with execution stats won't end up being done
    // multiple times.

    PlanSummaryStats summaryStats;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summaryStats);
    if (collection.exists()) {
        CollectionQueryInfo::get(collection.getCollectionPtr())
            .notifyOfQuery(opCtx, collection.getCollectionPtr(), summaryStats);
    }
    auto updateResult = exec->getUpdateResult();

    write_ops_exec::recordUpdateResultInOpDebug(updateResult, &curOp->debug());

    curOp->debug().setPlanSummaryMetrics(summaryStats);

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

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangAfterBatchUpdate, opCtx, "hangAfterBatchUpdate");

    return updateResult;
}

long long performDelete(OperationContext* opCtx,
                        const NamespaceString& nss,
                        DeleteRequest* deleteRequest,
                        CurOp* curOp,
                        bool inTransaction,
                        const boost::optional<mongo::UUID>& collectionUUID,
                        boost::optional<BSONObj>& docFound) {
    auto [isTimeseriesViewDelete, nsString] =
        timeseries::isTimeseriesViewRequest(opCtx, *deleteRequest);

    if (isTimeseriesViewDelete) {
        checkCollectionUUIDMismatch(
            opCtx, nsString.getTimeseriesViewNamespace(), nullptr, collectionUUID);
    }

    // TODO SERVER-76583: Remove this check.
    uassert(7308305,
            "Retryable findAndModify on a timeseries is not supported",
            !isTimeseriesViewDelete || !deleteRequest->getReturnDeleted() ||
                !opCtx->isRetryableWrite());

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchRemove, opCtx, "hangDuringBatchRemove", []() {
            LOGV2(7280401,
                  "Batch remove - hangDuringBatchRemove fail point enabled. Blocking until fail "
                  "point is disabled");
        });
    if (MONGO_unlikely(failAllRemoves.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllRemoves failpoint active!");
    }

    const auto collection =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx, nsString, AcquisitionPrerequisites::kWrite, collectionUUID),
                          MODE_IX);
    const auto& collectionPtr = collection.getCollectionPtr();

    if (const auto& coll = collection.getCollectionPtr()) {
        // Transactions are not allowed to operate on capped collections.
        uassertStatusOK(checkIfTransactionOnCappedColl(opCtx, coll));
    }

    if (isTimeseriesViewDelete) {
        timeseries::timeseriesRequestChecks<DeleteRequest>(
            collection.getCollectionPtr(), deleteRequest, timeseries::deleteRequestCheckFunction);
        timeseries::timeseriesHintTranslation<DeleteRequest>(collection.getCollectionPtr(),
                                                             deleteRequest);
    }

    ParsedDelete parsedDelete(opCtx, deleteRequest, collectionPtr, isTimeseriesViewDelete);
    uassertStatusOK(parsedDelete.parseRequest());

    auto dbName = nsString.dbName();
    if (DatabaseHolder::get(opCtx)->getDb(opCtx, dbName)) {
        curOp->raiseDbProfileLevel(CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(dbName));
    }

    assertCanWrite_inlock(opCtx, nsString);

    const auto exec = uassertStatusOK(
        getExecutorDelete(&curOp->debug(), collection, &parsedDelete, boost::none /* verbosity
        */));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    docFound = advanceExecutor(
        opCtx, exec.get(), true, deleteRequest->getReturnDeleted() ? "findAndModify" : "delete");
    // Nothing after advancing the plan executor should throw a WriteConflictException,
    // so the following bookkeeping with execution stats won't end up being done
    // multiple times.

    PlanSummaryStats summaryStats;
    exec->getPlanExplainer().getSummaryStats(&summaryStats);
    if (const auto& coll = collectionPtr) {
        CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, summaryStats);
    }
    curOp->debug().setPlanSummaryMetrics(summaryStats);

    // Fill out OpDebug with the number of deleted docs.
    auto nDeleted = exec->getDeleteResult();
    curOp->debug().additiveMetrics.ndeleted = nDeleted;

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

    if (status == ErrorCodes::TenantMigrationConflict) {
        hangWriteBeforeWaitingForMigrationDecision.pauseWhileSet(opCtx);

        Status overwrittenStatus =
            tenant_migration_access_blocker::handleTenantMigrationConflict(opCtx, status);

        // Interruption errors encountered during batch execution fail the entire batch, so throw on
        // such errors here for consistency.
        if (ErrorCodes::isInterruption(overwrittenStatus)) {
            uassertStatusOK(overwrittenStatus);
        }

        // Tenant migration errors, similarly to migration errors consume too much space in the
        // ordered:false responses and get truncated. Since the call to
        // 'handleTenantMigrationConflict' above replaces the original status, we need to manually
        // truncate the new reason if the original 'status' was also truncated.
        if (status.reason().empty()) {
            overwrittenStatus = overwrittenStatus.withReason("");
        }

        return generateErrorNoTenantMigration(opCtx, overwrittenStatus, index, numErrors);
    }

    return generateErrorNoTenantMigration(opCtx, status, index, numErrors);
}

boost::optional<write_ops::WriteError> generateErrorNoTenantMigration(OperationContext* opCtx,
                                                                      const Status& status,
                                                                      int index,
                                                                      size_t numErrors) noexcept {
    if (status.isOK()) {
        return boost::none;
    }

    constexpr size_t kMaxErrorReasonsToReport = 1;
    constexpr size_t kMaxErrorSizeToReportAfterMaxReasonsReached = 1024 * 1024;

    if (numErrors > kMaxErrorReasonsToReport) {
        size_t errorSize = status.reason().size();
        if (errorSize > kMaxErrorSizeToReportAfterMaxReasonsReached) {
            return write_ops::WriteError(index, status.withReason(""));
        }
    }

    return write_ops::WriteError(index, status);
}

void updateRetryStats(OperationContext* opCtx, bool containsRetry) {
    if (containsRetry) {
        RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
    }
}

void logOperationAndProfileIfNeeded(OperationContext* opCtx, CurOp* curOp) {
    const bool shouldProfile = curOp->completeAndLogOperation(
        {MONGO_LOGV2_DEFAULT_COMPONENT},
        CollectionCatalog::get(opCtx)->getDatabaseProfileSettings(curOp->getNSS().dbName()).filter);

    if (shouldProfile) {
        // Stash the current transaction so that writes to the profile collection are not
        // done as part of the transaction.
        TransactionParticipant::SideTransactionBlock sideTxn(opCtx);
        profile(opCtx, CurOp::get(opCtx)->getNetworkOp());
    }
}

WriteResult performInserts(OperationContext* opCtx,
                           const write_ops::InsertCommandRequest& wholeOp,
                           OperationSource source) {

    // Insert performs its own retries, so we should only be within a WriteUnitOfWork when run in a
    // transaction.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork() ||
              (txnParticipant && opCtx->inMultiDocumentTransaction()));

    auto& curOp = *CurOp::get(opCtx);
    ON_BLOCK_EXIT([&] {
        // Timeseries inserts already did as part of performTimeseriesWrites.
        if (source != OperationSource::kTimeseriesInsert) {
            // This is the only part of finishCurOp we need to do for inserts because they
            // reuse the top-level curOp. The rest is handled by the top-level entrypoint.
            curOp.done();
            Top::get(opCtx->getServiceContext())
                .record(opCtx,
                        wholeOp.getNamespace(),
                        LogicalOp::opInsert,
                        Top::LockType::WriteLocked,
                        durationCount<Microseconds>(curOp.elapsedTimeExcludingPauses()),
                        curOp.isCommand(),
                        curOp.getReadWriteType());
        }
    });

    // Timeseries inserts already did as part of performTimeseriesWrites.
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

    const auto [disableDocumentValidation, fleCrudProcessed] = getDocumentValidationFlags(
        opCtx, wholeOp.getWriteCommandRequestBase(), wholeOp.getDbName().tenantId());

    DisableDocumentSchemaValidationIfTrue docSchemaValidationDisabler(opCtx,
                                                                      disableDocumentValidation);

    DisableSafeContentValidationIfTrue safeContentValidationDisabler(
        opCtx, disableDocumentValidation, fleCrudProcessed);

    LastOpFixer lastOpFixer(opCtx);

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
                                                     source,
                                                     &lastOpFixer,
                                                     &out);
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

/**
 * Performs a single update without retries.
 */
static SingleWriteResult performSingleUpdateOpNoRetry(OperationContext* opCtx,
                                                      OperationSource source,
                                                      CurOp& curOp,
                                                      CollectionAcquisition collection,
                                                      ParsedUpdate& parsedUpdate,
                                                      bool* containsDotsAndDollarsField) {
    auto exec = uassertStatusOK(
        getExecutorUpdate(&curOp.debug(), collection, &parsedUpdate, boost::none /* verbosity */));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    auto updateResult = exec->executeUpdate();

    PlanSummaryStats summary;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summary);
    if (const auto& coll = collection.getCollectionPtr()) {
        CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, summary);
    }

    if (curOp.shouldDBProfile()) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp.debug().execStats = std::move(stats);
    }

    if (source != OperationSource::kTimeseriesInsert) {
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
 * Performs a single update, sometimes retrying failure due to WriteConflictException.
 */
static SingleWriteResult performSingleUpdateOp(OperationContext* opCtx,
                                               const NamespaceString& ns,
                                               const boost::optional<mongo::UUID>& opCollectionUUID,
                                               UpdateRequest* updateRequest,
                                               OperationSource source,
                                               bool forgoOpCounterIncrements,
                                               bool* containsDotsAndDollarsField) {
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchUpdate,
        opCtx,
        "hangDuringBatchUpdate",
        [&ns]() {
            LOGV2(20890,
                  "Batch update - hangDuringBatchUpdate fail point enabled for a namespace. "
                  "Blocking until fail point is disabled",
                  logAttrs(ns));
        },
        ns);

    if (MONGO_unlikely(failAllUpdates.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "failAllUpdates failpoint active!");
    }

    const CollectionAcquisition collection = [&]() {
        const auto acquisitionRequest = CollectionAcquisitionRequest::fromOpCtx(
            opCtx, ns, AcquisitionPrerequisites::kWrite, opCollectionUUID);
        while (true) {
            {
                auto acquisition = acquireCollection(
                    opCtx, acquisitionRequest, fixLockModeForSystemDotViewsChanges(ns, MODE_IX));
                if (acquisition.exists()) {
                    return acquisition;
                }

                if (source == OperationSource::kTimeseriesInsert ||
                    source == OperationSource::kTimeseriesUpdate) {
                    assertTimeseriesBucketsCollectionNotFound(ns);
                }

                // If this is an upsert, which is an insert, we must have a collection.
                // An update on a non-existent collection is okay and handled later.
                if (!updateRequest->isUpsert()) {
                    // Inexistent collection.
                    return acquisition;
                }
            }
            makeCollection(opCtx, ns);
        }
    }();

    if (source == OperationSource::kTimeseriesUpdate) {
        timeseries::timeseriesRequestChecks<UpdateRequest>(
            collection.getCollectionPtr(), updateRequest, timeseries::updateRequestCheckFunction);
        timeseries::timeseriesHintTranslation<UpdateRequest>(collection.getCollectionPtr(),
                                                             updateRequest);
    }

    if (source == OperationSource::kTimeseriesInsert) {
        // Disable auto yielding in the plan executor in order to prevent retrying on
        // WriteConflictExceptions internally. A WriteConflictException can be thrown in order to
        // abort a WriteBatch in an attempt to retry the operation, so we need it to escape the plan
        // executor layer.
        updateRequest->setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
    }

    if (const auto& coll = collection.getCollectionPtr()) {
        // Transactions are not allowed to operate on capped collections.
        uassertStatusOK(checkIfTransactionOnCappedColl(opCtx, coll));
    }

    ParsedUpdate parsedUpdate(opCtx,
                              updateRequest,
                              collection.getCollectionPtr(),
                              forgoOpCounterIncrements,
                              updateRequest->source() == OperationSource::kTimeseriesUpdate);
    uassertStatusOK(parsedUpdate.parseRequest());

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWithLockDuringBatchUpdate, opCtx, "hangWithLockDuringBatchUpdate");

    auto& curOp = *CurOp::get(opCtx);

    if (DatabaseHolder::get(opCtx)->getDb(opCtx, ns.dbName())) {
        curOp.raiseDbProfileLevel(
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));
    }

    assertCanWrite_inlock(opCtx, ns);

    if (updateRequest->getSort().isEmpty()) {
        return performSingleUpdateOpNoRetry(
            opCtx, source, curOp, collection, parsedUpdate, containsDotsAndDollarsField);
    } else {
        return writeConflictRetry(opCtx, "update", ns, [&]() -> SingleWriteResult {
            return performSingleUpdateOpNoRetry(
                opCtx, source, curOp, collection, parsedUpdate, containsDotsAndDollarsField);
        });
    }
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
    auto& curOp = *CurOp::get(opCtx);
    if (source != OperationSource::kTimeseriesInsert) {
        ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForUpdate(
            opCtx->getWriteConcern());
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
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    request.setSource(source);
    if (sampleId) {
        request.setSampleId(sampleId);
    }

    size_t numAttempts = 0;
    while (true) {
        ++numAttempts;

        try {
            bool containsDotsAndDollarsField = false;
            auto ret = performSingleUpdateOp(opCtx,
                                             ns,
                                             opCollectionUUID,
                                             &request,
                                             source,
                                             forgoOpCounterIncrements,
                                             &containsDotsAndDollarsField);

            if (containsDotsAndDollarsField) {
                // If it's an upsert, increment 'inserts' metric, otherwise increment 'updates'.
                dotsAndDollarsFieldsCounters.incrementForUpsert(!ret.getUpsertedId().isEmpty());
            }

            return ret;
        } catch (ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
            auto cq = uassertStatusOK(parseWriteQueryToCQ(opCtx, nullptr /* expCtx */, request));

            if (!write_ops_exec::shouldRetryDuplicateKeyException(
                    request, *cq, *ex.extraInfo<DuplicateKeyErrorInfo>())) {
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

void runTimeseriesRetryableUpdates(OperationContext* opCtx,
                                   const NamespaceString& bucketNs,
                                   const write_ops::UpdateCommandRequest& wholeOp,
                                   std::shared_ptr<executor::TaskExecutor> executor,
                                   write_ops_exec::WriteResult* reply) {
    checkCollectionUUIDMismatch(
        opCtx, bucketNs.getTimeseriesViewNamespace(), nullptr, wholeOp.getCollectionUUID());

    size_t nextOpIndex = 0;
    for (auto&& singleOp : wholeOp.getUpdates()) {
        auto singleUpdateOp = timeseries::buildSingleUpdateOp(wholeOp, nextOpIndex);
        const auto stmtId = write_ops::getStmtIdForWriteAt(wholeOp, nextOpIndex++);

        auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
        txn_api::SyncTransactionWithRetries txn(
            opCtx, executor, TransactionParticipantResourceYielder::make("update"), inlineExecutor);

        auto swResult = txn.runNoThrow(
            opCtx,
            [&singleUpdateOp, stmtId, &reply](const txn_api::TransactionClient& txnClient,
                                              ExecutorPtr txnExec) {
                auto updateResponse = txnClient.runCRUDOpSync(singleUpdateOp, {stmtId});
                // Propagates the write results from executing the statement to the current
                // command's results.
                SingleWriteResult singleReply;
                singleReply.setN(updateResponse.getN());
                singleReply.setNModified(updateResponse.getNModified());
                if (updateResponse.isUpsertDetailsSet()) {
                    invariant(updateResponse.sizeUpsertDetails() == 1);
                    singleReply.setUpsertedId(
                        updateResponse.getUpsertDetailsAt(0)->getUpsertedID());
                }
                if (updateResponse.areRetriedStmtIdsSet()) {
                    invariant(updateResponse.getRetriedStmtIds().size() == 1);
                    reply->retriedStmtIds.push_back(updateResponse.getRetriedStmtIds()[0]);
                }
                reply->results.push_back(singleReply);
                return SemiFuture<void>::makeReady();
            });
        try {
            // Rethrows the error from the command or the internal transaction api to handle them
            // accordingly.
            uassertStatusOK(swResult);
            uassertStatusOK(swResult.getValue().getEffectiveStatus());
        } catch (const DBException& ex) {
            reply->canContinue = handleError(opCtx,
                                             ex,
                                             bucketNs,
                                             wholeOp.getOrdered(),
                                             singleOp.getMulti(),
                                             singleOp.getSampleId(),
                                             reply);
            if (!reply->canContinue) {
                break;
            }
        }
    }
}

WriteResult performUpdates(OperationContext* opCtx,
                           const write_ops::UpdateCommandRequest& wholeOp,
                           OperationSource source) {
    auto ns = wholeOp.getNamespace();
    NamespaceString originalNs;
    if (source == OperationSource::kTimeseriesUpdate) {
        originalNs = ns;
        if (!ns.isTimeseriesBucketsCollection()) {
            ns = ns.makeTimeseriesBucketsNamespace();
        }

        checkCollectionUUIDMismatch(
            opCtx, ns.getTimeseriesViewNamespace(), nullptr, wholeOp.getCollectionUUID());
    }

    // Update performs its own retries, so we should not be in a WriteUnitOfWork unless run in a
    // transaction.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork() ||
              (txnParticipant && opCtx->inMultiDocumentTransaction()));
    uassertStatusOK(userAllowedWriteNS(opCtx, ns));

    const auto [disableDocumentValidation, fleCrudProcessed] = getDocumentValidationFlags(
        opCtx, wholeOp.getWriteCommandRequestBase(), wholeOp.getDbName().tenantId());

    DisableDocumentSchemaValidationIfTrue docSchemaValidationDisabler(opCtx,
                                                                      disableDocumentValidation);

    DisableSafeContentValidationIfTrue safeContentValidationDisabler(
        opCtx, disableDocumentValidation, fleCrudProcessed);

    LastOpFixer lastOpFixer(opCtx);

    bool containsRetry = false;
    ON_BLOCK_EXIT([&] { updateRetryStats(opCtx, containsRetry); });

    size_t nextOpIndex = 0;
    WriteResult out;
    out.results.reserve(wholeOp.getUpdates().size());

    // If the update command specified runtime constants, we adopt them. Otherwise, we set them to
    // the current local and cluster time. These constants are applied to each update in the batch.
    const auto& runtimeConstants =
        wholeOp.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx));

    // Increment operator counters only during the first single update operation in a batch of
    // updates.
    bool forgoOpCounterIncrements = false;
    for (auto&& singleOp : wholeOp.getUpdates()) {
        if (source == OperationSource::kTimeseriesUpdate) {
            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    fmt::format(
                        "Cannot perform a multi update inside of a multi-document transaction on a "
                        "time-series collection: {}",
                        ns.toStringForErrorMsg()),
                    !opCtx->inMultiDocumentTransaction() || !singleOp.getMulti() ||
                        opCtx->isRetryableWrite());
        }

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify sort with multi=true",
                !singleOp.getSort() || !singleOp.getMulti());

        const auto currentOpIndex = nextOpIndex++;
        const auto stmtId = getStmtIdForWriteOp(opCtx, wholeOp, currentOpIndex);
        if (opCtx->isRetryableWrite()) {
            if (auto entry = txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
                // For non-sharded user time-series updates, handles the metrics of the command at
                // the caller since each statement will run as a command through the internal
                // transaction API.
                containsRetry = source != OperationSource::kTimeseriesUpdate ||
                    originalNs.isTimeseriesBucketsCollection();
                RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                // Returns the '_id' of the user measurement for time-series upserts.
                boost::optional<BSONElement> upsertedId;
                if (entry->getOpType() == repl::OpTypeEnum::kInsert &&
                    source == OperationSource::kTimeseriesUpdate) {
                    upsertedId = entry->getObject()["control"]["min"]["_id"];
                }
                out.results.emplace_back(parseOplogEntryForUpdate(*entry, upsertedId));
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
                ->addUpdateQuery(opCtx, *sampleId, wholeOp, currentOpIndex)
                .getAsync([](auto) {});
        }

        try {
            lastOpFixer.startingOp(ns);

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
            // Do not handle errors for time-series bucket compressions. They need to be transparent
            // to users to not interfere with any decisions around operation retry. It is OK to
            // leave bucket uncompressed in these edge cases. We just record the status to the
            // result vector so we can keep track of statistics for failed bucket compressions.
            if (source == OperationSource::kTimeseriesBucketCompression) {
                out.results.emplace_back(ex.toStatus());
                break;
            }

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
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
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

    auto acquisitionRequest = CollectionAcquisitionRequest::fromOpCtx(
        opCtx, ns, AcquisitionPrerequisites::kWrite, opCollectionUUID);
    const auto collection = acquireCollection(
        opCtx, acquisitionRequest, fixLockModeForSystemDotViewsChanges(ns, MODE_IX));

    if (source == OperationSource::kTimeseriesDelete) {
        timeseries::timeseriesRequestChecks<DeleteRequest>(
            collection.getCollectionPtr(), &request, timeseries::deleteRequestCheckFunction);
        timeseries::timeseriesHintTranslation<DeleteRequest>(collection.getCollectionPtr(),
                                                             &request);
    }

    ParsedDelete parsedDelete(opCtx,
                              &request,
                              collection.getCollectionPtr(),
                              source == OperationSource::kTimeseriesDelete);
    uassertStatusOK(parsedDelete.parseRequest());

    if (DatabaseHolder::get(opCtx)->getDb(opCtx, ns.dbName())) {
        curOp.raiseDbProfileLevel(
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));
    }

    assertCanWrite_inlock(opCtx, ns);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWithLockDuringBatchRemove, opCtx, "hangWithLockDuringBatchRemove");

    auto exec = uassertStatusOK(
        getExecutorDelete(&curOp.debug(), collection, &parsedDelete, boost::none /* verbosity */));

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
    }

    auto nDeleted = exec->executeDelete();
    curOp.debug().additiveMetrics.ndeleted = nDeleted;

    PlanSummaryStats summary;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summary);
    if (const auto& coll = collection.getCollectionPtr()) {
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
    if (source == OperationSource::kTimeseriesDelete) {
        if (!ns.isTimeseriesBucketsCollection()) {
            ns = ns.makeTimeseriesBucketsNamespace();
        }

        checkCollectionUUIDMismatch(
            opCtx, ns.getTimeseriesViewNamespace(), nullptr, wholeOp.getCollectionUUID());
    }

    // Delete performs its own retries, so we should not be in a WriteUnitOfWork unless we are in a
    // transaction.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork() ||
              (txnParticipant && opCtx->inMultiDocumentTransaction()));
    uassertStatusOK(userAllowedWriteNS(opCtx, ns));

    const auto [disableDocumentValidation, fleCrudProcessed] = getDocumentValidationFlags(
        opCtx, wholeOp.getWriteCommandRequestBase(), wholeOp.getDbName().tenantId());

    DisableDocumentSchemaValidationIfTrue docSchemaValidationDisabler(opCtx,
                                                                      disableDocumentValidation);

    DisableSafeContentValidationIfTrue safeContentValidationDisabler(
        opCtx, disableDocumentValidation, fleCrudProcessed);

    LastOpFixer lastOpFixer(opCtx);

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
        if (source == OperationSource::kTimeseriesDelete) {
            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    str::stream() << "Cannot perform a multi delete inside of a multi-document "
                                     "transaction on a time-series collection: "
                                  << ns.toStringForErrorMsg(),
                    !opCtx->inMultiDocumentTransaction() || !singleOp.getMulti());
        }

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
                ->addDeleteQuery(opCtx, *sampleId, wholeOp, currentOpIndex)
                .getAsync([](auto) {});
        }

        try {
            lastOpFixer.startingOp(ns);

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
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    invariant(!opCtx->inMultiDocumentTransaction());
    invariant(!insertOps.empty() || !updateOps.empty());

    auto ns =
        !insertOps.empty() ? insertOps.front().getNamespace() : updateOps.front().getNamespace();

    DisableDocumentValidation disableDocumentValidation{opCtx};

    LastOpFixer lastOpFixer(opCtx);
    lastOpFixer.startingOp(ns);

    const auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, ns, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    if (!coll.exists()) {
        assertTimeseriesBucketsCollectionNotFound(ns);
    }

    auto curOp = CurOp::get(opCtx);
    curOp->raiseDbProfileLevel(CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));

    assertCanWrite_inlock(opCtx, ns);

    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const bool replicateVectoredInsertsTransactionally = fcvSnapshot.isVersionInitialized() &&
        repl::feature_flags::gReplicateVectoredInsertsTransactionally.isEnabled(fcvSnapshot);

    WriteUnitOfWork::OplogEntryGroupType oplogEntryGroupType = WriteUnitOfWork::kDontGroup;
    if (replicateVectoredInsertsTransactionally && insertOps.size() > 1 && updateOps.empty() &&
        !repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, ns)) {
        oplogEntryGroupType = WriteUnitOfWork::kGroupForPossiblyRetryableOperations;
    }
    WriteUnitOfWork wuow{opCtx, oplogEntryGroupType};

    std::vector<repl::OpTime> oplogSlots;
    boost::optional<std::vector<repl::OpTime>::iterator> slot;
    if (!replicateVectoredInsertsTransactionally || !updateOps.empty()) {
        if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, ns)) {
            oplogSlots = repl::getNextOpTimes(opCtx, insertOps.size() + updateOps.size());
            slot = oplogSlots.begin();
        }
    }

    auto participant = TransactionParticipant::get(opCtx);
    // Since we are manually updating the "lastWriteOpTime" before committing, we'll also need to
    // manually reset if the storage transaction is aborted.
    if (slot && participant) {
        shard_role_details::getRecoveryUnit(opCtx)->onRollback([](OperationContext* opCtx) {
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
            opCtx, coll.getCollectionPtr(), inserts.begin(), inserts.end(), &curOp->debug());
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

        invariant(coll.getCollectionPtr()->isClustered());
        auto recordId = record_id_helpers::keyForOID(update.getQ()["_id"].OID());

        auto original = coll.getCollectionPtr()->docFor(opCtx, recordId);

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
            updated = doc_diff::applyDiff(original.value(),
                                          diffFromUpdate,
                                          update.getU().mustCheckExistenceForInsertOperations());
            diffOnIndexes = &diffFromUpdate;
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
                    shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(
                        args.oplogSlots[0].getTimestamp()));
        }

        collection_internal::updateDocument(opCtx,
                                            coll.getCollectionPtr(),
                                            recordId,
                                            original,
                                            updated,
                                            diffOnIndexes,
                                            nullptr /*indexesAffected*/,
                                            &curOp->debug(),
                                            &args);
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

bool shouldRetryDuplicateKeyException(const UpdateRequest& updateRequest,
                                      const CanonicalQuery& cq,
                                      const DuplicateKeyErrorInfo& errorInfo) {
    // In order to be retryable, the update must be an upsert with multi:false.
    if (!updateRequest.isUpsert() || updateRequest.isMulti()) {
        return false;
    }

    auto matchExpr = cq.getPrimaryMatchExpression();
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

NamespaceString ns(const write_ops::InsertCommandRequest& request) {
    return request.getNamespace();
}

NamespaceString makeTimeseriesBucketsNamespace(const NamespaceString& nss) {
    return nss.isTimeseriesBucketsCollection() ? nss : nss.makeTimeseriesBucketsNamespace();
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
                            << ") for insert on time-series collection "
                            << ns(request).toStringForErrorMsg());

    return {std::move(reply.results[0]), reply.canContinue};
}

write_ops::UpdateCommandRequest makeTimeseriesTransformationOp(
    OperationContext* opCtx,
    const OID& bucketId,
    write_ops::UpdateModification::TransformFunc transformationFunc,
    const write_ops::InsertCommandRequest& request) {
    write_ops::UpdateCommandRequest op(makeTimeseriesBucketsNamespace(ns(request)),
                                       {timeseries::makeTimeseriesTransformationOpEntry(
                                           opCtx, bucketId, std::move(transformationFunc))});

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
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds,
    const write_ops::InsertCommandRequest& request,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch) {
    if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
        return {status->first, status->second};
    }
    return getTimeseriesSingleWriteResult(
        write_ops_exec::performInserts(
            opCtx,
            timeseries::makeTimeseriesInsertOp(
                batch, makeTimeseriesBucketsNamespace(ns(request)), metadata, std::move(stmtIds)),
            OperationSource::kTimeseriesInsert),
        request);
}

/**
 * Returns the status and whether the request can continue.
 */
TimeseriesSingleWriteResult performTimeseriesUpdate(
    OperationContext* opCtx,
    const BSONObj& metadata,
    const write_ops::UpdateCommandRequest& op,
    const write_ops::InsertCommandRequest& request) {
    if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
        return {status->first, status->second};
    }
    return getTimeseriesSingleWriteResult(
        write_ops_exec::performUpdates(opCtx, op, OperationSource::kTimeseriesInsert), request);
}

/**
 * Attempts to perform bucket compression on time-series bucket. It will surpress any error caused
 * by the write and silently leave the bucket uncompressed when any type of error is encountered.
 */
void tryPerformTimeseriesBucketCompression(OperationContext* opCtx,
                                           const write_ops::InsertCommandRequest& request,
                                           timeseries::bucket_catalog::ClosedBucket& closedBucket) {
    // Buckets with just a single measurement is not worth compressing.
    if (closedBucket.numMeasurements.has_value() && closedBucket.numMeasurements.value() <= 1) {
        return;
    }

    bool validateCompression = gValidateTimeseriesCompression.load();

    struct {
        int uncompressedSize = 0;
        int compressedSize = 0;
        int numInterleavedRestarts = 0;
        bool decompressionFailed = false;
    } compressionStats;

    auto bucketCompressionFunc = [&](const BSONObj& bucketDoc) -> boost::optional<BSONObj> {
        // Skip compression if the bucket is already compressed.
        if (timeseries::isCompressedBucket(bucketDoc)) {
            return boost::none;
        }

        // Reset every time we run to ensure we never use a stale value
        compressionStats = {};
        compressionStats.uncompressedSize = bucketDoc.objsize();

        auto compressed = timeseries::compressBucket(
            bucketDoc, closedBucket.timeField, ns(request), validateCompression);
        if (compressed.compressedBucket) {
            // If compressed object size is larger than uncompressed, skip compression update.
            if (compressed.compressedBucket->objsize() >= compressionStats.uncompressedSize) {
                LOGV2_DEBUG(5857802,
                            1,
                            "Skipping time-series bucket compression, compressed object is "
                            "larger than original",
                            "originalSize"_attr = compressionStats.uncompressedSize,
                            "compressedSize"_attr = compressed.compressedBucket->objsize());
                return boost::none;
            }

            compressionStats.compressedSize = compressed.compressedBucket->objsize();
            compressionStats.numInterleavedRestarts = compressed.numInterleavedRestarts;
        }

        compressionStats.decompressionFailed = compressed.decompressionFailed;
        return compressed.compressedBucket;
    };

    auto compressionOp = makeTimeseriesTransformationOp(
        opCtx, closedBucket.bucketId.oid, bucketCompressionFunc, request);
    auto result = getTimeseriesSingleWriteResult(
        write_ops_exec::performUpdates(
            opCtx, compressionOp, OperationSource::kTimeseriesBucketCompression),
        request);

    // TODO(SERVER-70605): Remove deprecated metrics.
    closedBucket.stats.incNumBytesUncompressed(compressionStats.uncompressedSize);
    if (result.result.isOK() && compressionStats.compressedSize > 0) {
        closedBucket.stats.incNumBytesCompressed(compressionStats.compressedSize);
        closedBucket.stats.incNumSubObjCompressionRestart(compressionStats.numInterleavedRestarts);
        closedBucket.stats.incNumCompressedBuckets();
    } else {
        closedBucket.stats.incNumBytesCompressed(compressionStats.uncompressedSize);
        closedBucket.stats.incNumUncompressedBuckets();

        if (compressionStats.decompressionFailed) {
            closedBucket.stats.incNumFailedDecompressBuckets();
        }
    }
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
                            absl::flat_hash_map<int, int>& retryAttemptsForDup,
                            const write_ops::InsertCommandRequest& request) try {
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);

    auto metadata = getMetadata(bucketCatalog, batch->bucketHandle);
    auto status = prepareCommit(bucketCatalog, request.getNamespace(), batch);
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
            performTimeseriesInsert(opCtx, metadata, std::move(stmtIds), request, batch);
        if (auto error = write_ops_exec::generateError(
                opCtx, output.result.getStatus(), start + index, errors->size())) {
            bool canContinue = output.canContinue;
            // Automatically attempts to retry on DuplicateKey error.
            if (error->getStatus().code() == ErrorCodes::DuplicateKey &&
                retryAttemptsForDup[index]++ < gTimeseriesInsertMaxRetriesOnDuplicates.load()) {
                docsToRetry->push_back(index);
                canContinue = true;
            } else {
                errors->emplace_back(std::move(*error));
            }
            abort(bucketCatalog, batch, output.result.getStatus());
            return canContinue;
        }

        invariant(output.result.getValue().getN() == 1,
                  str::stream() << "Expected 1 insertion of document with _id '" << docId
                                << "', but found " << output.result.getValue().getN() << ".");
    } else {
        auto op = batch->generateCompressedDiff
            ? timeseries::makeTimeseriesCompressedDiffUpdateOp(
                  opCtx, batch, makeTimeseriesBucketsNamespace(ns(request)), std::move(stmtIds))
            : timeseries::makeTimeseriesUpdateOp(opCtx,
                                                 batch,
                                                 makeTimeseriesBucketsNamespace(ns(request)),
                                                 metadata,
                                                 std::move(stmtIds));
        auto const output = performTimeseriesUpdate(opCtx, metadata, op, request);

        if ((output.result.isOK() && output.result.getValue().getNModified() != 1) ||
            output.result.getStatus().code() == ErrorCodes::WriteConflict ||
            output.result.getStatus().code() == ErrorCodes::TemporarilyUnavailable) {
            abort(bucketCatalog,
                  batch,
                  output.result.isOK()
                      ? Status{ErrorCodes::WriteConflict, "Could not update non-existent bucket"}
                      : output.result.getStatus());
            docsToRetry->push_back(index);
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            return true;
        } else if (auto error = write_ops_exec::generateError(
                       opCtx, output.result.getStatus(), start + index, errors->size())) {
            errors->emplace_back(std::move(*error));
            abort(bucketCatalog, batch, output.result.getStatus());
            return output.canContinue;
        }
    }

    timeseries::getOpTimeAndElectionId(opCtx, opTime, electionId);

    auto closedBucket = finish(opCtx,
                               bucketCatalog,
                               request.getNamespace().makeTimeseriesBucketsNamespace(),
                               batch,
                               timeseries::bucket_catalog::CommitInfo{*opTime, *electionId});

    if (closedBucket) {
        // If this write closed a bucket, compress the bucket
        tryPerformTimeseriesBucketCompression(opCtx, request, *closedBucket);
    }
    return true;
} catch (const DBException& ex) {
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
    abort(bucketCatalog, batch, ex.toStatus());
    throw;
}

std::shared_ptr<timeseries::bucket_catalog::WriteBatch>& extractFromPair(
    std::pair<std::shared_ptr<timeseries::bucket_catalog::WriteBatch>, size_t>& pair) {
    return pair.first;
}

bool commitTimeseriesBucketsAtomically(OperationContext* opCtx,
                                       const write_ops::InsertCommandRequest& request,
                                       TimeseriesBatches& batches,
                                       TimeseriesStmtIds&& stmtIds,
                                       boost::optional<repl::OpTime>* opTime,
                                       boost::optional<OID>* electionId) {
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);

    auto batchesToCommit = timeseries::determineBatchesToCommit(batches, extractFromPair);
    if (batchesToCommit.empty()) {
        return true;
    }

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
            auto prepareCommitStatus = prepareCommit(bucketCatalog, request.getNamespace(), batch);
            if (!prepareCommitStatus.isOK()) {
                abortStatus = prepareCommitStatus;
                return false;
            }

            timeseries::makeWriteRequest(opCtx,
                                         batch,
                                         metadata,
                                         stmtIds,
                                         makeTimeseriesBucketsNamespace(ns(request)),
                                         &insertOps,
                                         &updateOps);
        }

        hangTimeseriesInsertBeforeWrite.pauseWhileSet();

        auto result = write_ops_exec::performAtomicTimeseriesWrites(opCtx, insertOps, updateOps);
        if (!result.isOK()) {
            if (result.code() == ErrorCodes::DuplicateKey) {
                timeseries::bucket_catalog::resetBucketOIDCounter();
            }
            abortStatus = result;
            return false;
        }

        timeseries::getOpTimeAndElectionId(opCtx, opTime, electionId);

        for (auto batch : batchesToCommit) {
            auto closedBucket =
                finish(opCtx,
                       bucketCatalog,
                       request.getNamespace().makeTimeseriesBucketsNamespace(),
                       batch,
                       timeseries::bucket_catalog::CommitInfo{*opTime, *electionId});
            batch.get().reset();

            if (!closedBucket) {
                continue;
            }

            tryPerformTimeseriesBucketCompression(opCtx, request, *closedBucket);
        }
    } catch (const ExceptionFor<ErrorCodes::TimeseriesBucketCompressionFailed>& ex) {
        abortStatus = ex.toStatus();
        return false;
    } catch (const DBException& ex) {
        abortStatus = ex.toStatus();
        throw;
    }

    batchGuard.dismiss();
    return true;
}

// For sharded time-series collections, we need to use the granularity from the config
// server (through shard filtering information) as the source of truth for the current
// granularity value, due to the possible inconsistency in the process of granularity
// updates.
void rebuildOptionsWithGranularityFromConfigServer(OperationContext* opCtx,
                                                   const NamespaceString& bucketsNs,
                                                   TimeseriesOptions& timeSeriesOptions) {
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
            timeSeriesOptions.setBucketRoundingSeconds(
                timeseries::getBucketRoundingSecondsFromGranularity(*granularity));
        } else if (!bucketMaxSpanSeconds) {
            timeSeriesOptions.setGranularity(BucketGranularityEnum::Seconds);
            timeSeriesOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(*timeSeriesOptions.getGranularity()));
            timeSeriesOptions.setBucketRoundingSeconds(
                timeseries::getBucketRoundingSecondsFromGranularity(
                    *timeSeriesOptions.getGranularity()));
        } else {
            invariant(bucketMaxSpanSeconds);
            timeSeriesOptions.setBucketMaxSpanSeconds(bucketMaxSpanSeconds);

            auto bucketRoundingSeconds = collDesc.getTimeseriesFields()->getBucketRoundingSeconds();
            invariant(bucketRoundingSeconds);
            timeSeriesOptions.setBucketRoundingSeconds(bucketRoundingSeconds);
        }
    }
}


// Waits for all batches to either commit or abort. This function will attempt to acquire commit
// rights to any batch to mark it as aborted. If another thread alreay have commit rights we will
// instead wait for the promise when the commit is either successful or failed. Marked as 'noexcept'
// as we need to safely be able to call this function during exception handling.
template <typename ErrorGenerator>
void getTimeseriesBatchResultsBase(OperationContext* opCtx,
                                   const TimeseriesBatches& batches,
                                   int64_t start,
                                   int64_t indexOfLastProcessedBatch,
                                   bool canContinue,
                                   std::vector<write_ops::WriteError>* errors,
                                   boost::optional<repl::OpTime>* opTime,
                                   boost::optional<OID>* electionId,
                                   std::vector<size_t>* docsToRetry) {
    boost::optional<write_ops::WriteError> lastError;
    if (!errors->empty()) {
        lastError = errors->back();
    }
    invariant(indexOfLastProcessedBatch == (int64_t)batches.size() || lastError);

    for (int64_t itr = 0, size = batches.size(); itr < size; ++itr) {
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
            invariant(docsToRetry, "the 'docsToRetry' cannot be null");
            docsToRetry->push_back(index);
            continue;
        }
        if (swCommitInfo.getStatus() == ErrorCodes::WriteConflict ||
            swCommitInfo.getStatus() == ErrorCodes::TemporarilyUnavailable) {
            docsToRetry->push_back(index);
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            continue;
        }
        if (auto error =
                ErrorGenerator{}(opCtx, swCommitInfo.getStatus(), start + index, errors->size())) {
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

void getTimeseriesBatchResults(OperationContext* opCtx,
                               const TimeseriesBatches& batches,
                               int64_t start,
                               int64_t indexOfLastProcessedBatch,
                               bool canContinue,
                               std::vector<write_ops::WriteError>* errors,
                               boost::optional<repl::OpTime>* opTime,
                               boost::optional<OID>* electionId,
                               std::vector<size_t>* docsToRetry = nullptr) {
    auto errorGenerator =
        [](OperationContext* opCtx, const Status& status, int index, size_t numErrors) {
            return write_ops_exec::generateError(opCtx, status, index, numErrors);
        };
    getTimeseriesBatchResultsBase<decltype(errorGenerator)>(opCtx,
                                                            batches,
                                                            start,
                                                            indexOfLastProcessedBatch,
                                                            canContinue,
                                                            errors,
                                                            opTime,
                                                            electionId,
                                                            docsToRetry);
}

void getTimeseriesBatchResultsNoTenantMigration(
    OperationContext* opCtx,
    const TimeseriesBatches& batches,
    int64_t start,
    int64_t indexOfLastProcessedBatch,
    bool canContinue,
    std::vector<write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    std::vector<size_t>* docsToRetry = nullptr) noexcept {
    auto errorGenerator =
        [](OperationContext* opCtx, const Status& status, int index, size_t numErrors) {
            return write_ops_exec::generateErrorNoTenantMigration(opCtx, status, index, numErrors);
        };
    getTimeseriesBatchResultsBase<decltype(errorGenerator)>(opCtx,
                                                            batches,
                                                            start,
                                                            indexOfLastProcessedBatch,
                                                            canContinue,
                                                            errors,
                                                            opTime,
                                                            electionId,
                                                            docsToRetry);
}

std::tuple<UUID, TimeseriesBatches, TimeseriesStmtIds, size_t /* numInserted */>
insertIntoBucketCatalog(OperationContext* opCtx,
                        const write_ops::InsertCommandRequest& request,
                        size_t start,
                        size_t numDocs,
                        const std::vector<size_t>& indices,
                        std::vector<write_ops::WriteError>* errors,
                        bool* containsRetry) {
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);

    auto bucketsNs = makeTimeseriesBucketsNamespace(ns(request));
    // Holding this shared pointer to the collection guarantees that the collator is not
    // invalidated.
    auto catalog = CollectionCatalog::get(opCtx);
    auto bucketsColl = catalog->lookupCollectionByNamespace(opCtx, bucketsNs);
    timeseries::assertTimeseriesBucketsCollection(bucketsColl);

    auto timeSeriesOptions = *bucketsColl->getTimeseriesOptions();

    boost::optional<Status> rebuildOptionsError;
    try {
        rebuildOptionsWithGranularityFromConfigServer(opCtx, bucketsNs, timeSeriesOptions);
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

        auto swResult = timeseries::attemptInsertIntoBucket(
            opCtx,
            bucketCatalog,
            bucketsColl,
            timeSeriesOptions,
            measurementDoc,
            timeseries::BucketReopeningPermittance::kAllowed,
            canCombineTimeseriesInsertWithOtherClients(opCtx, request));

        if (auto error = write_ops_exec::generateError(
                opCtx, swResult.getStatus(), start + index, errors->size())) {
            invariant(swResult.getStatus().code() != ErrorCodes::WriteConflict);
            errors->emplace_back(std::move(*error));
            return false;
        }

        auto& result = swResult.getValue();
        auto* insertResult = get_if<timeseries::bucket_catalog::SuccessfulInsertion>(&result);
        invariant(insertResult);
        batches.emplace_back(std::move(insertResult->batch), index);
        const auto& batch = batches.back().first;
        if (isTimeseriesWriteRetryable(opCtx)) {
            stmtIds[batch->bucketHandle.bucketId.oid].push_back(stmtId);
        }

        // If this insert closed buckets, rewrite to be a compressed column. If we cannot
        // perform write operations at this point the bucket will be left uncompressed.
        for (auto& closedBucket : insertResult->closedBuckets) {
            tryPerformTimeseriesBucketCompression(opCtx, request, closedBucket);
        }

        return true;
    };

    try {
        if (!indices.empty()) {
            std::for_each(indices.begin(), indices.end(), insert);
        } else {
            for (size_t i = 0; i < numDocs; i++) {
                if (!insert(i) && request.getOrdered()) {
                    return {bucketsColl->uuid(), std::move(batches), std::move(stmtIds), i};
                }
            }
        }
    } catch (const DBException& ex) {
        // Exception insert into bucket catalog, append error and wait for all batches that we've
        // already managed to write into to commit or abort. We need to wait here as pointers to
        // memory owned by this command is stored in the WriteBatch(es). This ensures that no other
        // thread may try to access this memory after this command has been torn down due to the
        // exception.

        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
        std::vector<size_t> docsToRetry;
        errors->emplace_back(*write_ops_exec::generateErrorNoTenantMigration(
            opCtx, ex.toStatus(), 0, errors->size()));

        getTimeseriesBatchResultsNoTenantMigration(
            opCtx, batches, 0, -1, false, errors, &opTime, &electionId, &docsToRetry);
        throw;
    }

    return {
        bucketsColl->uuid(), std::move(batches), std::move(stmtIds), request.getDocuments().size()};
}

bool performOrderedTimeseriesWritesAtomically(OperationContext* opCtx,
                                              const write_ops::InsertCommandRequest& request,
                                              std::vector<write_ops::WriteError>* errors,
                                              boost::optional<repl::OpTime>* opTime,
                                              boost::optional<OID>* electionId,
                                              bool* containsRetry) {
    auto [_, batches, stmtIds, numInserted] = insertIntoBucketCatalog(
        opCtx, request, 0, request.getDocuments().size(), {}, errors, containsRetry);

    hangTimeseriesInsertBeforeCommit.pauseWhileSet();

    if (!commitTimeseriesBucketsAtomically(
            opCtx, request, batches, std::move(stmtIds), opTime, electionId)) {
        return false;
    }

    getTimeseriesBatchResults(opCtx, batches, 0, batches.size(), true, errors, opTime, electionId);

    return true;
}

/**
 * Writes to the underlying system.buckets collection. Returns the indices, of the batch
 * which were attempted in an update operation, but found no bucket to update. These indices
 * can be passed as the 'indices' parameter in a subsequent call to this function, in order
 * to to be retried.
 * In rare cases due to collision from OID generation, we will also retry inserting those bucket
 * documents for a limited number of times.
 */
std::vector<size_t> performUnorderedTimeseriesWrites(
    OperationContext* opCtx,
    const write_ops::InsertCommandRequest& request,
    size_t start,
    size_t numDocs,
    const std::vector<size_t>& indices,
    std::vector<write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    bool* containsRetry,
    absl::flat_hash_map<int, int>& retryAttemptsForDup) {
    auto [uuid, batches, bucketStmtIds, _] =
        insertIntoBucketCatalog(opCtx, request, start, numDocs, indices, errors, containsRetry);
    UUID collectionUUID = uuid;

    hangTimeseriesInsertBeforeCommit.pauseWhileSet();

    bool canContinue = true;
    std::vector<size_t> docsToRetry;

    stdx::unordered_set<timeseries::bucket_catalog::WriteBatch*> handledHere;
    int64_t handledElsewhere = 0;
    auto reportMeasurementsGuard =
        ScopeGuard([&collectionUUID, &handledElsewhere, &request, opCtx]() {
            if (handledElsewhere > 0) {
                auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
                timeseries::bucket_catalog::reportMeasurementsGroupCommitted(
                    bucketCatalog, collectionUUID, handledElsewhere);
            }
        });

    size_t itr = 0;
    for (; itr < batches.size(); ++itr) {
        auto& [batch, index] = batches[itr];
        if (timeseries::bucket_catalog::claimWriteBatchCommitRights(*batch)) {
            handledHere.insert(batch.get());
            auto stmtIds = isTimeseriesWriteRetryable(opCtx)
                ? std::move(bucketStmtIds[batch->bucketHandle.bucketId.oid])
                : std::vector<StmtId>{};
            try {
                canContinue = commitTimeseriesBucket(opCtx,
                                                     batch,
                                                     start,
                                                     index,
                                                     std::move(stmtIds),
                                                     errors,
                                                     opTime,
                                                     electionId,
                                                     &docsToRetry,
                                                     retryAttemptsForDup,
                                                     request);
            } catch (const ExceptionFor<ErrorCodes::TimeseriesBucketCompressionFailed>& ex) {
                auto bucketId = ex.extraInfo<timeseries::BucketCompressionFailure>()->bucketId();

                timeseries::bucket_catalog::freeze(
                    timeseries::bucket_catalog::BucketCatalog::get(opCtx),
                    collectionUUID,
                    bucketId);

                LOGV2_WARNING(
                    8607200,
                    "Failed to compress bucket for time-series insert, please retry your write",
                    "bucketId"_attr = bucketId);

                errors->emplace_back(*write_ops_exec::generateErrorNoTenantMigration(
                    opCtx, ex.toStatus(), start + index, errors->size()));
            } catch (const DBException& ex) {
                // Exception during commit, append error and wait for all our batches to commit or
                // abort. We need to wait here as pointers to memory owned by this command is stored
                // in the WriteBatch(es). This ensures that no other thread may try to access this
                // memory after this command has been torn down due to the exception.
                errors->emplace_back(*write_ops_exec::generateErrorNoTenantMigration(
                    opCtx, ex.toStatus(), start + index, errors->size()));
                getTimeseriesBatchResultsNoTenantMigration(
                    opCtx, batches, 0, itr, canContinue, errors, opTime, electionId, &docsToRetry);
                throw;
            }

            batch.reset();
            if (!canContinue) {
                break;
            }
        } else if (!handledHere.contains(batch.get())) {
            ++handledElsewhere;
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
                                                 const write_ops::InsertCommandRequest& request,
                                                 size_t start,
                                                 size_t numDocs,
                                                 std::vector<write_ops::WriteError>* errors,
                                                 boost::optional<repl::OpTime>* opTime,
                                                 boost::optional<OID>* electionId,
                                                 bool* containsRetry) {
    std::vector<size_t> docsToRetry;
    absl::flat_hash_map<int, int> retryAttemptsForDup;
    do {
        docsToRetry = performUnorderedTimeseriesWrites(opCtx,
                                                       request,
                                                       start,
                                                       numDocs,
                                                       docsToRetry,
                                                       errors,
                                                       opTime,
                                                       electionId,
                                                       containsRetry,
                                                       retryAttemptsForDup);
        if (!retryAttemptsForDup.empty()) {
            timeseries::bucket_catalog::resetBucketOIDCounter();
        }
    } while (!docsToRetry.empty());
}

/**
 * Returns the number of documents that were inserted.
 */
size_t performOrderedTimeseriesWrites(OperationContext* opCtx,
                                      const write_ops::InsertCommandRequest& request,
                                      std::vector<write_ops::WriteError>* errors,
                                      boost::optional<repl::OpTime>* opTime,
                                      boost::optional<OID>* electionId,
                                      bool* containsRetry) {
    if (performOrderedTimeseriesWritesAtomically(
            opCtx, request, errors, opTime, electionId, containsRetry)) {
        if (!errors->empty()) {
            invariant(errors->size() == 1);
            return errors->front().getIndex();
        }
        return request.getDocuments().size();
    }

    for (size_t i = 0; i < request.getDocuments().size(); ++i) {
        performUnorderedTimeseriesWritesWithRetries(
            opCtx, request, i, 1, errors, opTime, electionId, containsRetry);
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
                    ns(request),
                    LogicalOp::opInsert,
                    Top::LockType::WriteLocked,
                    durationCount<Microseconds>(curOp.elapsedTimeExcludingPauses()),
                    curOp.isCommand(),
                    curOp.getReadWriteType());
    });

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        auto requestNs = ns(request);
        curOp.setNS_inlock(requestNs.isTimeseriesBucketsCollection()
                               ? requestNs.getTimeseriesViewNamespace()
                               : requestNs);
        curOp.setLogicalOp_inlock(LogicalOp::opInsert);
        curOp.ensureStarted();
        curOp.debug().additiveMetrics.ninserted = 0;
    }

    return performTimeseriesWrites(opCtx, request, &curOp);
}

write_ops::InsertCommandReply performTimeseriesWrites(
    OperationContext* opCtx, const write_ops::InsertCommandRequest& request, CurOp* curOp) {
    // If an expected collection UUID is provided, always fail because the user-facing time-series
    // namespace does not have a UUID.
    checkCollectionUUIDMismatch(opCtx,
                                request.getNamespace().isTimeseriesBucketsCollection()
                                    ? request.getNamespace().getTimeseriesViewNamespace()
                                    : request.getNamespace(),
                                nullptr,
                                request.getCollectionUUID());

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot insert into a time-series collection in a multi-document "
                             "transaction: "
                          << ns(request).toStringForErrorMsg(),
            !opCtx->inMultiDocumentTransaction());

    std::vector<write_ops::WriteError> errors;
    boost::optional<repl::OpTime> opTime;
    boost::optional<OID> electionId;
    bool containsRetry = false;

    write_ops::InsertCommandReply insertReply;
    auto& baseReply = insertReply.getWriteCommandReplyBase();

    // Prevent FCV upgrade/downgrade while performing a time-series write. There are several feature
    // flag checks in the layers below and they expect a consistent reading of the FCV to take the
    // correct write path.
    // TODO SERVER-70605: remove this FixedFCVRegion.
    FixedFCVRegion fixedFcv(opCtx);

    if (request.getOrdered()) {
        baseReply.setN(performOrderedTimeseriesWrites(
            opCtx, request, &errors, &opTime, &electionId, &containsRetry));
    } else {
        performUnorderedTimeseriesWritesWithRetries(opCtx,
                                                    request,
                                                    0,
                                                    request.getDocuments().size(),
                                                    &errors,
                                                    &opTime,
                                                    &electionId,
                                                    &containsRetry);
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

    curOp->debug().additiveMetrics.ninserted = baseReply.getN();
    globalOpCounters.gotInserts(baseReply.getN());
    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInserts(opCtx->getWriteConcern(),
                                                                        baseReply.getN());

    return insertReply;
}

void explainUpdate(OperationContext* opCtx,
                   UpdateRequest& updateRequest,
                   bool isTimeseriesViewRequest,
                   const SerializationContext& serializationContext,
                   const BSONObj& command,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) {

    // Explains of write commands are read-only, but we take write locks so that timing
    // info is more accurate.
    const auto collection = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, updateRequest.getNamespaceString(), AcquisitionPrerequisites::kWrite),
        MODE_IX);

    if (isTimeseriesViewRequest) {
        timeseries::timeseriesRequestChecks<UpdateRequest>(
            collection.getCollectionPtr(), &updateRequest, timeseries::updateRequestCheckFunction);
        timeseries::timeseriesHintTranslation<UpdateRequest>(collection.getCollectionPtr(),
                                                             &updateRequest);
    }

    ParsedUpdate parsedUpdate(opCtx,
                              &updateRequest,
                              collection.getCollectionPtr(),
                              false /* forgoOpCounterIncrements */,
                              isTimeseriesViewRequest);
    uassertStatusOK(parsedUpdate.parseRequest());

    auto exec = uassertStatusOK(
        getExecutorUpdate(&CurOp::get(opCtx)->debug(), collection, &parsedUpdate, verbosity));
    auto bodyBuilder = result->getBodyBuilder();

    Explain::explainStages(exec.get(),
                           collection.getCollectionPtr(),
                           verbosity,
                           BSONObj(),
                           SerializationContext::stateCommandReply(serializationContext),
                           command,
                           &bodyBuilder);
}

void explainDelete(OperationContext* opCtx,
                   DeleteRequest& deleteRequest,
                   bool isTimeseriesViewRequest,
                   const SerializationContext& serializationContext,
                   const BSONObj& command,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) {
    // Explains of write commands are read-only, but we take write locks so that timing
    // info is more accurate.
    const auto collection =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx, deleteRequest.getNsString(), AcquisitionPrerequisites::kWrite),
                          MODE_IX);

    if (isTimeseriesViewRequest) {
        timeseries::timeseriesRequestChecks<DeleteRequest>(
            collection.getCollectionPtr(), &deleteRequest, timeseries::deleteRequestCheckFunction);
        timeseries::timeseriesHintTranslation<DeleteRequest>(collection.getCollectionPtr(),
                                                             &deleteRequest);
    }

    ParsedDelete parsedDelete(
        opCtx, &deleteRequest, collection.getCollectionPtr(), isTimeseriesViewRequest);
    uassertStatusOK(parsedDelete.parseRequest());

    // Explain the plan tree.
    auto exec = uassertStatusOK(
        getExecutorDelete(&CurOp::get(opCtx)->debug(), collection, &parsedDelete, verbosity));
    auto bodyBuilder = result->getBodyBuilder();

    Explain::explainStages(exec.get(),
                           collection.getCollectionPtr(),
                           verbosity,
                           BSONObj(),
                           SerializationContext::stateCommandReply(serializationContext),
                           command,
                           &bodyBuilder);
}

}  // namespace mongo::write_ops_exec

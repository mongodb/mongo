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

#include "mongo/db/query/write_ops/write_ops_exec.h"

#include "mongo/base/counter.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/database_name.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/profile_collection.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_diagnostic_printer.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/insert.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_retryability.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/collection_pre_conditions_util.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/version_context.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/message.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
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

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/hash/hash.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

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
        auto executionTimeMicros = curOp->elapsedTimeExcludingPauses();
        curOp->debug().additiveMetrics.executionTime = executionTimeMicros;

        recordCurOpMetrics(opCtx);
        Top::getDecoration(opCtx).record(opCtx,
                                         curOp->getNSS(),
                                         curOp->getLogicalOp(),
                                         Top::LockType::WriteLocked,
                                         curOp->elapsedTimeExcludingPauses(),
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

        auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, ns, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        uassert(ErrorCodes::PrimarySteppedDown,
                str::stream() << "Not primary while writing to " << ns.toStringForErrorMsg(),
                repl::ReplicationCoordinator::get(opCtx->getServiceContext())
                    ->canAcceptWritesFor(opCtx, ns));

        if (!collection.exists()) {  // someone else may have beat us to it.
            uassertStatusOK(userAllowedCreateNS(opCtx, ns));
            // TODO (SERVER-77915): Remove once 8.0 becomes last LTS.
            // TODO (SERVER-82066): Update handling for direct connections.
            // TODO (SERVER-86254): Update handling for transactions and retryable writes.
            boost::optional<OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE>
                allowCollectionCreation;
            const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
            if (!fcvSnapshot.isVersionInitialized() ||
                !feature_flags::g80CollectionCreationPath.isEnabled(fcvSnapshot) ||
                !OperationShardingState::get(opCtx).isComingFromRouter(opCtx) ||
                (opCtx->inMultiDocumentTransaction() || opCtx->isRetryableWrite())) {
                allowCollectionCreation.emplace(opCtx, ns);
            }
            WriteUnitOfWork wuow(opCtx);
            CollectionOptions defaultCollectionOptions;
            if (auto fp = globalFailPointRegistry().find("clusterAllCollectionsByDefault"); fp &&
                fp->shouldFail() &&
                !clustered_util::requiresLegacyFormat(ns, defaultCollectionOptions)) {
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
//
// The "begin" and "end" iterators must remain valid until the active WriteUnitOfWork resolves.
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

    // If we abort this WriteUnitOfWork, we must make sure we never attempt to use these reserved
    // oplog slots again.
    shard_role_details::getRecoveryUnit(opCtx)->onRollback([begin, end](OperationContext* opCtx) {
        for (auto it = begin; it != end; it++) {
            it->oplogSlot = OplogSlot();
        }
    });
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

    // For multiple inserts not part of a multi-document transaction, the inserts will be
    // batched into a single applyOps oplog entry.
    if (!inTransaction && batchSize > 1 && !oplogDisabled) {
        oplogEntryGroupType = WriteUnitOfWork::kGroupForPossiblyRetryableOperations;
    }

    // Intentionally not using writeConflictRetry. That is handled by the caller so it can react to
    // oversized batches.
    WriteUnitOfWork wuow(opCtx, oplogEntryGroupType);

    // Capped collections may implicitly generate a delete in the same unit of work as an
    // insert. In order to avoid that delete generating a second timestamp in a WUOW which
    // has un-timestamped writes (which is a violation of multi-timestamp constraints),
    // we must reserve the timestamp for the insert in advance.
    // We take an exclusive lock on the metadata resource before reserving the timestamp.
    if (collection.getCollectionPtr()->needsCappedLock()) {
        Lock::ResourceLock heldUntilEndOfWUOW{
            opCtx, ResourceId(RESOURCE_METADATA, collection.getCollectionPtr()->ns()), MODE_X};
    }
    if (oplogEntryGroupType != WriteUnitOfWork::kGroupForPossiblyRetryableOperations &&
        !inTransaction && !oplogDisabled && collection.getCollectionPtr()->isCapped()) {
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
        // Fail the write for direct shard operations so that a RetryableWriteError label can be
        // returned and the write can be retried by the driver.
        if (!OperationShardingState::isComingFromRouter(opCtx) &&
            ex.code() == ErrorCodes::StaleConfig && opCtx->isRetryableWrite()) {
            throw;
        }

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
                                const timeseries::CollectionPreConditions& preConditions,
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

    boost::optional<CollectionAcquisition> collection;
    auto acquireCollection = [&] {
        while (true) {
            collection.emplace(mongo::acquireCollection(
                opCtx,
                CollectionAcquisitionRequest::fromOpCtx(
                    opCtx, nss, AcquisitionPrerequisites::kWrite, preConditions.expectedUUID()),
                fixLockModeForSystemDotViewsChanges(nss, MODE_IX)));
            timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
                opCtx, preConditions, *collection);
            if (collection->exists()) {
                break;
            }

            if (source == OperationSource::kTimeseriesInsert) {
                timeseries::write_ops::assertTimeseriesBucketsCollectionNotFound(nss);
            }

            if (source == OperationSource::kFromMigrate) {
                uasserted(
                    ErrorCodes::CannotCreateCollection,
                    "The implicit creation of a collection due to a migration is not allowed.");
            }

            collection.reset();  // unlock.
            makeCollection(opCtx, nss);
        }

        curOp.raiseDbProfileLevel(DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(nss.dbName()));
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
        // In a time-series context, this particular CollectionUUIDMismatch is re-thrown differently
        // because there is already a check for this error higher up, which means this error must
        // come from the guards installed to enforce that time-series operations are prepared
        // and committed on the same collection.
        if (ex.code() == ErrorCodes::CollectionUUIDMismatch &&
            source == OperationSource::kTimeseriesInsert) {
            uasserted(9748801, "Collection was changed during insert");
        }

        // We want to fail a write in the scenario where:
        // 1) a collection with the ns that we are inserting to doesn't exist, so we choose to
        // insert into it as normal collection and create it implicitly
        // 2) a collection with this ns is created as a time-series collection
        // 3) we acquire this time-series collection to insert into in the regular collection write
        // path.
        // We should fail the insert in this scenario to avoid writing normal documents into a
        // time-series collection. We also fail in any scenario where it originally was a
        // time-series/normal collection and was re-created as the alternative.
        if (ex.code() == 10685100 || ex.code() == 106851001) {
            throw;
        }
    }

    // Create an RAII object that prints the collection's shard key in the case of a tassert
    // or crash.
    ScopedDebugInfo shardKeyDiagnostics(
        "ShardKeyDiagnostics",
        diagnostic_printers::ShardKeyDiagnosticPrinter{
            collection.has_value() && collection->getShardingDescription().isSharded()
                ? collection.value().getShardingDescription().getKeyPattern()
                : BSONObj()});

    if (auto scoped = failAllInserts.scoped(); MONGO_unlikely(scoped.isActive())) {
        tassert(9276700,
                "failAllInserts failpoint active!",
                !scoped.getData().hasField("tassert") || !scoped.getData().getBoolField("tassert"));
        uasserted(ErrorCodes::InternalError, "failAllInserts failpoint active!");
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
                serviceOpCounters(opCtx).gotInserts(batch.size());
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
        serviceOpCounters(opCtx).gotInsert();
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

UpdateResult performUpdate(OperationContext* opCtx,
                           const NamespaceString& nss,
                           CurOp* curOp,
                           bool inTransaction,
                           bool remove,
                           bool upsert,
                           const boost::optional<mongo::UUID>& collectionUUID,
                           boost::optional<BSONObj>& docFound,
                           UpdateRequest* updateRequest,
                           const timeseries::CollectionPreConditions& preConditions,
                           bool isTimeseriesLogicalRequest) {
    auto nsString = preConditions.getTargetNs(nss);

    // TODO SERVER-76583: Remove this check.
    uassert(7314600,
            "Retryable findAndModify on a timeseries is not supported",
            !isTimeseriesLogicalRequest || !updateRequest->shouldReturnAnyDocs() ||
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


    auto collection =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx, nsString, AcquisitionPrerequisites::kWrite, collectionUUID),
                          MODE_IX);
    timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
        opCtx, preConditions, collection);
    auto dbName = nsString.dbName();
    Database* db = [&]() {
        AutoGetDb autoDb(opCtx, dbName, MODE_IX);
        return autoDb.ensureDbExists(opCtx);
    }();

    // Create an RAII object that prints the collection's shard key in the case of a tassert
    // or crash.
    ScopedDebugInfo shardKeyDiagnostics(
        "ShardKeyDiagnostics",
        diagnostic_printers::ShardKeyDiagnosticPrinter{
            collection.getShardingDescription().isSharded()
                ? collection.getShardingDescription().getKeyPattern()
                : BSONObj()});

    invariant(DatabaseHolder::get(opCtx)->getDb(opCtx, dbName));
    curOp->raiseDbProfileLevel(
        DatabaseProfileSettings::get(opCtx->getServiceContext()).getDatabaseProfileLevel(dbName));

    assertCanWrite_inlock(opCtx, nsString);

    if (!collection.exists() && upsert) {
        CollectionWriter collectionWriter(opCtx, &collection);
        uassertStatusOK(userAllowedCreateNS(opCtx, nsString));
        // TODO (SERVER-77915): Remove once 8.0 becomes last LTS.
        // TODO (SERVER-82066): Update handling for direct connections.
        // TODO (SERVER-86254): Update handling for transactions and retryable writes.
        boost::optional<OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE>
            allowCollectionCreation;
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        if (!fcvSnapshot.isVersionInitialized() ||
            !feature_flags::g80CollectionCreationPath.isEnabled(fcvSnapshot) ||
            !OperationShardingState::get(opCtx).isComingFromRouter(opCtx) ||
            (opCtx->inMultiDocumentTransaction() || opCtx->isRetryableWrite())) {
            allowCollectionCreation.emplace(opCtx, nsString);
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

    if (isTimeseriesLogicalRequest) {
        timeseries::timeseriesRequestChecks<UpdateRequest>(VersionContext::getDecoration(opCtx),
                                                           collection.getCollectionPtr(),
                                                           updateRequest,
                                                           timeseries::updateRequestCheckFunction);
        timeseries::timeseriesHintTranslation<UpdateRequest>(collection.getCollectionPtr(),
                                                             updateRequest);
    }

    ParsedUpdate parsedUpdate(opCtx,
                              updateRequest,
                              collection.getCollectionPtr(),
                              false /*forgoOpCounterIncrements*/,
                              isTimeseriesLogicalRequest);
    uassertStatusOK(parsedUpdate.parseRequest());

    // Create an RAII object that prints useful information about the ExpressionContext in the case
    // of a tassert or crash.
    ScopedDebugInfo expCtxDiagnostics(
        "ExpCtxDiagnostics", diagnostic_printers::ExpressionContextPrinter{parsedUpdate.expCtx()});

    if (auto scoped = failAllUpdates.scoped(); MONGO_unlikely(scoped.isActive())) {
        tassert(9276701,
                "failAllUpdates failpoint active!",
                !scoped.getData().hasField("tassert") || !scoped.getData().getBoolField("tassert"));
        uasserted(ErrorCodes::InternalError, "failAllUpdates failpoint active!");
    }

    const auto exec = uassertStatusOK(
        getExecutorUpdate(&curOp->debug(), collection, &parsedUpdate, boost::none /* verbosity
        */));
    // Capture diagnostics to be logged in the case of a failure.
    ScopedDebugInfo explainDiagnostics("explainDiagnostics",
                                       diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary(lk, exec->getPlanExplainer().getPlanSummary());
    }

    if (updateRequest->shouldReturnAnyDocs()) {
        docFound = exec->executeFindAndModify();
        curOp->debug().additiveMetrics.nreturned = docFound ? 1 : 0;
    } else {
        // The 'UpdateResult' object will be obtained later, so discard the return value.
        (void)exec->executeUpdate();
    }

    // Nothing after executing the plan executor should throw a WriteConflictException, so the
    // following bookkeeping with execution stats won't end up being done multiple times.

    PlanSummaryStats summaryStats;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summaryStats);
    if (collection.exists()) {
        CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
            collection.getCollectionPtr().get(),
            summaryStats.collectionScans,
            summaryStats.collectionScansNonTailable,
            summaryStats.indexesUsed);
    }
    auto updateResult = exec->getUpdateResult();

    write_ops_exec::recordUpdateResultInOpDebug(updateResult, &curOp->debug());

    curOp->debug().setPlanSummaryMetrics(std::move(summaryStats));

    if (updateResult.containsDotsAndDollarsField) {
        // If it's an upsert, increment 'inserts' metric, otherwise increment 'updates'.
        dotsAndDollarsFieldsCounters.incrementForUpsert(!updateResult.upsertedId.isEmpty());
    }

    if (curOp->shouldDBProfile()) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp->debug().execStats = std::move(stats);
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
                        boost::optional<BSONObj>& docFound,
                        const timeseries::CollectionPreConditions& preConditions,
                        bool isTimeseriesLogicalRequest) {
    auto nsString = preConditions.getTargetNs(nss);

    // TODO SERVER-76583: Remove this check.
    uassert(7308305,
            "Retryable findAndModify on a timeseries is not supported",
            !isTimeseriesLogicalRequest || !deleteRequest->getReturnDeleted() ||
                !opCtx->isRetryableWrite());

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangDuringBatchRemove, opCtx, "hangDuringBatchRemove", []() {
            LOGV2(7280401,
                  "Batch remove - hangDuringBatchRemove fail point enabled. Blocking until fail "
                  "point is disabled");
        });

    const auto collection =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx, nsString, AcquisitionPrerequisites::kWrite, collectionUUID),
                          MODE_IX);
    timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
        opCtx, preConditions, collection);
    // Create an RAII object that prints the collection's shard key in the case of a tassert
    // or crash.
    ScopedDebugInfo shardKeyDiagnostics(
        "ShardKeyDiagnostics",
        diagnostic_printers::ShardKeyDiagnosticPrinter{
            collection.getShardingDescription().isSharded()
                ? collection.getShardingDescription().getKeyPattern()
                : BSONObj()});

    const auto& collectionPtr = collection.getCollectionPtr();

    if (const auto& coll = collection.getCollectionPtr()) {
        // Transactions are not allowed to operate on capped collections.
        uassertStatusOK(checkIfTransactionOnCappedColl(opCtx, coll));
    }

    if (isTimeseriesLogicalRequest) {
        timeseries::timeseriesRequestChecks<DeleteRequest>(VersionContext::getDecoration(opCtx),
                                                           collection.getCollectionPtr(),
                                                           deleteRequest,
                                                           timeseries::deleteRequestCheckFunction);
        timeseries::timeseriesHintTranslation<DeleteRequest>(collection.getCollectionPtr(),
                                                             deleteRequest);
    }

    ParsedDelete parsedDelete(opCtx, deleteRequest, collectionPtr, isTimeseriesLogicalRequest);
    uassertStatusOK(parsedDelete.parseRequest());

    // Create an RAII object that prints useful information about the ExpressionContext in the case
    // of a tassert or crash.
    ScopedDebugInfo expCtxDiagnostics(
        "ExpCtxDiagnostics", diagnostic_printers::ExpressionContextPrinter{parsedDelete.expCtx()});

    if (auto scoped = failAllRemoves.scoped(); MONGO_unlikely(scoped.isActive())) {
        tassert(9276703,
                "failAllRemoves failpoint active!",
                !scoped.getData().hasField("tassert") || !scoped.getData().getBoolField("tassert"));
        uasserted(ErrorCodes::InternalError, "failAllRemoves failpoint active!");
    }

    auto dbName = nsString.dbName();
    if (DatabaseHolder::get(opCtx)->getDb(opCtx, dbName)) {
        curOp->raiseDbProfileLevel(DatabaseProfileSettings::get(opCtx->getServiceContext())
                                       .getDatabaseProfileLevel(dbName));
    }

    assertCanWrite_inlock(opCtx, nsString);

    const auto exec = uassertStatusOK(
        getExecutorDelete(&curOp->debug(), collection, &parsedDelete, boost::none /* verbosity
        */));
    // Capture diagnostics to be logged in the case of a failure.
    ScopedDebugInfo explainDiagnostics("explainDiagnostics",
                                       diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary(lk, exec->getPlanExplainer().getPlanSummary());
    }

    if (deleteRequest->getReturnDeleted()) {
        docFound = exec->executeFindAndModify();
        curOp->debug().additiveMetrics.nreturned = docFound ? 1 : 0;
    } else {
        // The number of deleted documents will be obtained from the plan executor later, so discard
        // the return value.
        (void)exec->executeDelete();
    }

    // Nothing after executing the PlanExecutor should throw a WriteConflictException, so the
    // following bookkeeping with execution stats won't end up being done multiple times.

    PlanSummaryStats summaryStats;
    exec->getPlanExplainer().getSummaryStats(&summaryStats);
    if (const auto& coll = collectionPtr) {
        CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
            coll.get(),
            summaryStats.collectionScans,
            summaryStats.collectionScansNonTailable,
            summaryStats.indexesUsed);
    }
    curOp->debug().setPlanSummaryMetrics(std::move(summaryStats));

    // Fill out OpDebug with the number of deleted docs.
    auto nDeleted = exec->getDeleteResult();
    curOp->debug().additiveMetrics.ndeleted = nDeleted;

    if (curOp->shouldDBProfile()) {
        auto&& explainer = exec->getPlanExplainer();
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp->debug().execStats = std::move(stats);
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
    const bool shouldProfile =
        curOp->completeAndLogOperation({MONGO_LOGV2_DEFAULT_COMPONENT},
                                       DatabaseProfileSettings::get(opCtx->getServiceContext())
                                           .getDatabaseProfileSettings(curOp->getNSS().dbName())
                                           .filter);

    if (shouldProfile) {
        // Stash the current transaction so that writes to the profile collection are not
        // done as part of the transaction.
        TransactionParticipant::SideTransactionBlock sideTxn(opCtx);
        profile_collection::profile(opCtx, CurOp::get(opCtx)->getNetworkOp());
    }
}

namespace {
StatusWith<int> getIndexCountForCollectionBatchTuning(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const boost::optional<UUID>& collectionUUID) {
    // If we're already in a write unit of work, the collection acquisition may hold on to the lock
    // (because of two-phase locking) and cause a lock conflict.  Also, this is used to adjust batch
    // size which is not meaningful when already in a WUOW at this point.  We return 1 which is
    // treated as meaning no tuning is needed.
    if (shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork())
        return 1;

    try {
        // Acquire collection with an unsharded collection acquisition concern, because we don't
        // care if the sharding data is stale; this is used for tuning only so it can be
        // best-effort.
        auto collection = mongo::acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(nss,
                                         collectionUUID,
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        if (collection.exists()) {
            return collection.getCollectionPtr()->getTotalIndexCount();
        }
    } catch (const DBException& e) {
        return e.toStatus();
    }
    return Status{ErrorCodes::NamespaceNotFound,
                  str::stream() << "Collection '" << nss.toStringForErrorMsg()
                                << "' does not exist when checking number of indexes."};
}

// Tune batch size by number of indexes.  If we fail to acquire the collection here we ignore it
// and let any error occur when we acquire the collection for write.
size_t getTunedMaxBatchSize(OperationContext* opCtx,
                            const write_ops::InsertCommandRequest& wholeOp) {
    const size_t origMaxBatchSize = internalInsertMaxBatchSize.load();
    size_t maxBatchSize = origMaxBatchSize;
    size_t lowestMaxBatchSize = std::max<size_t>(1, origMaxBatchSize / 10);

    // Avoid obtaining the index count if the tuning wouldn't do anything anyway.
    if (wholeOp.getDocuments().size() <= lowestMaxBatchSize)
        return maxBatchSize;

    StatusWith<int> swIndexCount = getIndexCountForCollectionBatchTuning(
        opCtx, wholeOp.getNamespace(), wholeOp.getCollectionUUID());
    if (swIndexCount.isOK()) {
        int indexCount = swIndexCount.getValue();
        if (indexCount > 1) {
            // Assume the max batch size paramter is tuned for a collection with single index, and
            // should be reduced for collections with more indexes.  But do not increase batch size
            // for 0-index clustered collections.  We also enforce a minimum of one-tenth of the
            // single-index batch size.
            maxBatchSize = std::max((origMaxBatchSize * 2) / (indexCount + 1), lowestMaxBatchSize);
            LOGV2_DEBUG(9204400,
                        2,
                        "Tuning max batch size according to number of indexes",
                        "internalInsertMaxBatchSize"_attr = origMaxBatchSize,
                        "nss"_attr = wholeOp.getNamespace(),
                        "indexCount"_attr = indexCount,
                        "maxBatchSize"_attr = maxBatchSize);
        }
    } else {
        LOGV2(9204401,
              "Error occurred while attempting to get number of indexes to tune max batch size for "
              "insert",
              "error"_attr = swIndexCount.getStatus(),
              "maxBatchSize"_attr = maxBatchSize);
    }
    return maxBatchSize;
}
}  // namespace

WriteResult performInserts(
    OperationContext* opCtx,
    const write_ops::InsertCommandRequest& wholeOp,
    boost::optional<const timeseries::CollectionPreConditions&> preConditionsOptional,
    OperationSource source) {
    auto actualNs = wholeOp.getNamespace();

    timeseries::CollectionPreConditions preConditions = preConditionsOptional
        ? *preConditionsOptional
        : timeseries::CollectionPreConditions::getCollectionPreConditions(
              opCtx, wholeOp.getNamespace(), wholeOp.getCollectionUUID());

    if (isRawDataOperation(opCtx)) {
        actualNs = preConditions.getTargetNs(wholeOp.getNamespace());
    }

    // Insert performs its own retries, so we should only be within a WriteUnitOfWork when run in a
    // transaction.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork() ||
              (txnParticipant && opCtx->inMultiDocumentTransaction()));

    auto& curOp = *CurOp::get(opCtx);
    ON_BLOCK_EXIT([&, &actualNs = actualNs] {
        // Timeseries inserts already did as part of performTimeseriesWrites.
        if (source != OperationSource::kTimeseriesInsert) {
            // This is the only part of finishCurOp we need to do for inserts because they
            // reuse the top-level curOp. The rest is handled by the top-level entrypoint.
            curOp.done();
            Top::getDecoration(opCtx).record(opCtx,
                                             actualNs,
                                             LogicalOp::opInsert,
                                             Top::LockType::WriteLocked,
                                             curOp.elapsedTimeExcludingPauses(),
                                             curOp.isCommand(),
                                             curOp.getReadWriteType());
        }
    });

    // Timeseries inserts already did as part of performTimeseriesWrites.
    if (source != OperationSource::kTimeseriesInsert) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS(lk, actualNs);
        curOp.setLogicalOp(lk, LogicalOp::opInsert);
        curOp.ensureStarted();
        // Initialize 'ninserted' for the operation if is not yet.
        curOp.debug().additiveMetrics.incrementNinserted(0);
    }

    uassertStatusOK(userAllowedWriteNS(opCtx, actualNs));

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
    const size_t maxBatchSize = getTunedMaxBatchSize(opCtx, wholeOp);
    const size_t maxBatchBytes = write_ops::insertVectorMaxBytes;

    batch.reserve(std::min(wholeOp.getDocuments().size(), maxBatchSize));

    // If 'wholeOp.getBypassEmptyTsReplacement()' is true or if 'source' is 'kFromMigrate', set
    // "bypassEmptyTsReplacement=true" for fixDocumentForInsert().
    const bool bypassEmptyTsReplacement = (source == OperationSource::kFromMigrate) ||
        static_cast<bool>(wholeOp.getBypassEmptyTsReplacement());

    for (auto&& doc : wholeOp.getDocuments()) {
        const auto currentOpIndex = nextOpIndex++;
        const bool isLastDoc = (&doc == &wholeOp.getDocuments().back());
        bool containsDotsAndDollarsField = false;

        auto fixedDoc = fixDocumentForInsert(
            opCtx, doc, bypassEmptyTsReplacement, &containsDotsAndDollarsField);

        const StmtId stmtId = getStmtIdForWriteOp(opCtx, wholeOp, currentOpIndex);
        const bool wasAlreadyExecuted =
            opCtx->isRetryableWrite() && txnParticipant.checkStatementExecuted(opCtx, stmtId);

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
                                                     actualNs,
                                                     preConditions,
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
            serviceOpCounters(opCtx).gotInsert();
            ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInsert(
                opCtx->getWriteConcern());
            try {
                uassertStatusOK(fixedDoc.getStatus());
                MONGO_UNREACHABLE;
            } catch (const DBException& ex) {
                out.canContinue = handleError(opCtx,
                                              ex,
                                              actualNs,
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
    // Capture diagnostics to be logged in the case of a failure.
    ScopedDebugInfo explainDiagnostics("explainDiagnostics",
                                       diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary(lk, exec->getPlanExplainer().getPlanSummary());
    }

    auto updateResult = exec->executeUpdate();

    PlanSummaryStats summary;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summary);
    if (const auto& coll = collection.getCollectionPtr()) {
        CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
            coll.get(),
            summary.collectionScans,
            summary.collectionScansNonTailable,
            summary.indexesUsed);
    }

    if (curOp.shouldDBProfile()) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp.debug().execStats = std::move(stats);
    }

    if (source != OperationSource::kTimeseriesInsert) {
        recordUpdateResultInOpDebug(updateResult, &curOp.debug());
    }
    curOp.debug().setPlanSummaryMetrics(std::move(summary));

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
static SingleWriteResult performSingleUpdateOp(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const timeseries::CollectionPreConditions& preConditions,
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
    const CollectionAcquisition collection = [&]() {
        const auto acquisitionRequest = CollectionAcquisitionRequest::fromOpCtx(
            opCtx, ns, AcquisitionPrerequisites::kWrite, preConditions.expectedUUID());
        while (true) {
            {
                auto acquisition = acquireCollection(
                    opCtx, acquisitionRequest, fixLockModeForSystemDotViewsChanges(ns, MODE_IX));
                timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
                    opCtx, preConditions, acquisition);
                if (acquisition.exists()) {
                    return acquisition;
                }

                // If this is an upsert, which is an insert, we must have a collection.
                // An update on a non-existent collection is okay and handled later.
                if (!updateRequest->isUpsert()) {
                    // Inexistent collection.
                    return acquisition;
                }

                if (source == OperationSource::kFromMigrate) {
                    uasserted(
                        ErrorCodes::CannotCreateCollection,
                        "The implicit creation of a collection due to a migration is not allowed.");
                }
            }
            makeCollection(opCtx, ns);
        }
    }();

    // Create an RAII object that prints the collection's shard key in the case of a tassert
    // or crash.
    ScopedDebugInfo shardKeyDiagnostics(
        "ShardKeyDiagnostics",
        diagnostic_printers::ShardKeyDiagnosticPrinter{
            collection.getShardingDescription().isSharded()
                ? collection.getShardingDescription().getKeyPattern()
                : BSONObj()});

    if (source == OperationSource::kTimeseriesUpdate) {
        timeseries::timeseriesRequestChecks<UpdateRequest>(VersionContext::getDecoration(opCtx),
                                                           collection.getCollectionPtr(),
                                                           updateRequest,
                                                           timeseries::updateRequestCheckFunction);
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

    // Create an RAII object that prints useful information about the ExpressionContext in the case
    // of a tassert or crash.
    ScopedDebugInfo expCtxDiagnostics(
        "ExpCtxDiagnostics", diagnostic_printers::ExpressionContextPrinter{parsedUpdate.expCtx()});

    if (auto scoped = failAllUpdates.scoped(); MONGO_unlikely(scoped.isActive())) {
        tassert(9276702,
                "failAllUpdates failpoint active!",
                !scoped.getData().hasField("tassert") || !scoped.getData().getBoolField("tassert"));
        uasserted(ErrorCodes::InternalError, "failAllUpdates failpoint active!");
    }

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWithLockDuringBatchUpdate, opCtx, "hangWithLockDuringBatchUpdate");

    auto& curOp = *CurOp::get(opCtx);

    if (DatabaseHolder::get(opCtx)->getDb(opCtx, ns.dbName())) {
        curOp.raiseDbProfileLevel(DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(ns.dbName()));
    }

    assertCanWrite_inlock(opCtx, ns);

    // No need to call writeConflictRetry() since it does not retry if in a transaction,
    // but calling it can cause WCE to be double counted.
    const auto inTransaction = opCtx->inMultiDocumentTransaction();
    if (updateRequest->getSort().isEmpty() || inTransaction) {
        return performSingleUpdateOpNoRetry(
            opCtx, source, curOp, collection, parsedUpdate, containsDotsAndDollarsField);
    } else {
        // Call writeConflictRetry() if we have a sort, since we express the sort with a limit of 1.
        // In the case that the predicate of the currently matching document changes due to a
        // concurrent modification, the query will be retried to see if there's another matching
        // document.
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
    const std::vector<StmtId>& stmtIds,
    const write_ops::UpdateOpEntry& op,
    const timeseries::CollectionPreConditions& preConditions,
    LegacyRuntimeConstants runtimeConstants,
    const boost::optional<BSONObj>& letParams,
    const OptionalBool& bypassEmptyTsReplacement,
    const boost::optional<UUID>& sampleId,
    OperationSource source,
    bool forgoOpCounterIncrements) {
    serviceOpCounters(opCtx).gotUpdate();
    auto& curOp = *CurOp::get(opCtx);
    if (source != OperationSource::kTimeseriesInsert) {
        ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForUpdate(
            opCtx->getWriteConcern());
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS(
            lk,
            (source == OperationSource::kTimeseriesUpdate && ns.isTimeseriesBucketsCollection())
                ? ns.getTimeseriesViewNamespace()
                : ns);
        curOp.setNetworkOp(lk, dbUpdate);
        curOp.setLogicalOp(lk, LogicalOp::opUpdate);
        curOp.setOpDescription(lk, op.toBSON());
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
    request.setBypassEmptyTsReplacement(bypassEmptyTsReplacement);
    request.setStmtIds(stmtIds);
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    request.setSource(source);
    if (sampleId) {
        request.setSampleId(sampleId);
    }

    int retryAttempts = 0;
    while (true) {
        ++retryAttempts;

        try {
            bool containsDotsAndDollarsField = false;
            auto ret = performSingleUpdateOp(opCtx,
                                             ns,
                                             preConditions,
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
                    opCtx, request, *cq, *ex.extraInfo<DuplicateKeyErrorInfo>(), retryAttempts)) {
                throw;
            }

            logAndBackoff(4640402,
                          ::mongo::logv2::LogComponent::kWrite,
                          logv2::LogSeverity::Debug(1),
                          retryAttempts,
                          "Caught DuplicateKey exception during upsert",
                          logAttrs(ns));
        } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>&) {
            // In a time-series context, this particular CollectionUUIDMismatch is re-thrown
            // differently because there is already a check for this error higher up, which means
            // this error must come from the guards installed to enforce that time-series operations
            // are prepared and committed on the same collection.
            uassert(9748802,
                    "Collection was changed during insert",
                    source != OperationSource::kTimeseriesInsert);
            throw;
        }
    }

    MONGO_UNREACHABLE;
}

void runTimeseriesRetryableUpdates(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const write_ops::UpdateCommandRequest& wholeOp,
                                   const timeseries::CollectionPreConditions& preConditions,
                                   std::shared_ptr<executor::TaskExecutor> executor,
                                   write_ops_exec::WriteResult* reply) {
    if (preConditions.isLegacyTimeseriesCollection()) {
        checkCollectionUUIDMismatch(opCtx,
                                    preConditions.getTargetNs(nss).getTimeseriesViewNamespace(),
                                    nullptr,
                                    preConditions.expectedUUID());
    }
    size_t nextOpIndex = 0;
    for (auto&& singleOp : wholeOp.getUpdates()) {
        auto singleUpdateOp = timeseries::write_ops::buildSingleUpdateOp(wholeOp, nextOpIndex);
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
                                             preConditions.getTargetNs(nss),
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

WriteResult performUpdates(
    OperationContext* opCtx,
    const write_ops::UpdateCommandRequest& wholeOp,
    boost::optional<const timeseries::CollectionPreConditions&> preConditionsOptional,
    OperationSource source) {

    tassert(10912000,
            fmt::format("Performing a logical time-series update operation without passing in the "
                        "CollectionPreConditions object with request on collection {}",
                        wholeOp.getNamespace().toStringForErrorMsg()),
            source != OperationSource::kTimeseriesUpdate || preConditionsOptional);

    timeseries::CollectionPreConditions preConditions = preConditionsOptional
        ? *preConditionsOptional
        : timeseries::CollectionPreConditions::getCollectionPreConditions(
              opCtx, wholeOp.getNamespace(), wholeOp.getCollectionUUID());

    auto ns = preConditions.getTargetNs(wholeOp.getNamespace());

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
            if (auto entry =
                    txnParticipant.checkStatementExecutedAndFetchOplogEntry(opCtx, stmtId)) {
                // Set containsRetry to true for all types of operations except for user-facing
                // time-series updates on a non-sharded cluster. For non-sharded user time-series
                // updates, handles the metrics of the command at the caller since each statement
                // will run as a command through the internal transaction API.
                containsRetry = source != OperationSource::kTimeseriesUpdate ||
                    !preConditions.getIsTimeseriesLogicalRequest() || wholeOp.getShardVersion();
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

        // Begin query planning timing once we have the nested CurOp.
        CurOp::get(opCtx)->beginQueryPlanningTimer();

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
                                                     stmtIds,
                                                     singleOp,
                                                     preConditions,
                                                     runtimeConstants,
                                                     wholeOp.getLet(),
                                                     wholeOp.getBypassEmptyTsReplacement(),
                                                     sampleId,
                                                     source,
                                                     forgoOpCounterIncrements);
            out.results.emplace_back(reply);
            forgoOpCounterIncrements = true;
            lastOpFixer.finishedOpSuccessfully();

            if (singleOp.getMulti()) {
                getQueryCounters(opCtx).updateManyCount.increment(1);
                collectMultiUpdateDeleteMetrics(timer->elapsed(), reply.getNModified());
            }
        } catch (const ExceptionFor<ErrorCodes::TimeseriesBucketCompressionFailed>& ex) {
            // Do not handle errors for time-series bucket compressions. They need to be transparent
            // to users to not interfere with any decisions around operation retry. It is OK to
            // leave bucket uncompressed in these edge cases. We just record the status to the
            // result vector so we can keep track of statistics for failed bucket compressions.
            if (source == OperationSource::kTimeseriesBucketCompression) {
                out.results.emplace_back(ex.toStatus());
                break;
            } else if (source == OperationSource::kTimeseriesInsert) {
                // Special case for failed attempt to compress a reopened bucket.
                throw;
            } else if (source == OperationSource::kTimeseriesUpdate) {
                auto& bucketCatalog = timeseries::bucket_catalog::GlobalBucketCatalog::get(
                    opCtx->getServiceContext());
                timeseries::bucket_catalog::freeze(
                    bucketCatalog,
                    timeseries::bucket_catalog::BucketId{
                        ex->collectionUUID(), ex->bucketId(), ex->keySignature()});
                LOGV2_WARNING(8793901,
                              "Encountered corrupt bucket while performing update",
                              "bucketId"_attr = ex->bucketId());
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

static SingleWriteResult performSingleDeleteOp(
    OperationContext* opCtx,
    const NamespaceString& ns,
    StmtId stmtId,
    const write_ops::DeleteOpEntry& op,
    const LegacyRuntimeConstants& runtimeConstants,
    const boost::optional<BSONObj>& letParams,
    const timeseries::CollectionPreConditions& preConditions,
    OperationSource source) {
    uassert(ErrorCodes::InvalidOptions,
            "Cannot use (or request) retryable writes with limit=0",
            !opCtx->isRetryableWrite() || !op.getMulti() || stmtId == kUninitializedStmtId);

    serviceOpCounters(opCtx).gotDelete();
    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForDelete(opCtx->getWriteConcern());
    auto& curOp = *CurOp::get(opCtx);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setNS(lk,
                    (preConditions.isLegacyTimeseriesCollection() &&
                             source == OperationSource::kTimeseriesDelete
                         ? ns.getTimeseriesViewNamespace()
                         : ns));
        curOp.setNetworkOp(lk, dbDelete);
        curOp.setLogicalOp(lk, LogicalOp::opDelete);
        curOp.setOpDescription(lk, op.toBSON());
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

    auto acquisitionRequest = CollectionAcquisitionRequest::fromOpCtx(
        opCtx, ns, AcquisitionPrerequisites::kWrite, preConditions.expectedUUID());
    const auto collection = acquireCollection(
        opCtx, acquisitionRequest, fixLockModeForSystemDotViewsChanges(ns, MODE_IX));
    timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
        opCtx, preConditions, collection);

    // Create an RAII object that prints the collection's shard key in the case of a tassert
    // or crash.
    ScopedDebugInfo shardKeyDiagnostics(
        "ShardKeyDiagnostics",
        diagnostic_printers::ShardKeyDiagnosticPrinter{
            collection.getShardingDescription().isSharded()
                ? collection.getShardingDescription().getKeyPattern()
                : BSONObj()});

    if (source == OperationSource::kTimeseriesDelete) {
        timeseries::timeseriesRequestChecks<DeleteRequest>(VersionContext::getDecoration(opCtx),
                                                           collection.getCollectionPtr(),
                                                           &request,
                                                           timeseries::deleteRequestCheckFunction);
        timeseries::timeseriesHintTranslation<DeleteRequest>(collection.getCollectionPtr(),
                                                             &request);
    }

    ParsedDelete parsedDelete(opCtx,
                              &request,
                              collection.getCollectionPtr(),
                              source == OperationSource::kTimeseriesDelete);
    uassertStatusOK(parsedDelete.parseRequest());

    // Create an RAII object that prints useful information about the ExpressionContext in the case
    // of a tassert or crash.
    ScopedDebugInfo expCtxDiagnostics(
        "ExpCtxDiagnostics", diagnostic_printers::ExpressionContextPrinter{parsedDelete.expCtx()});

    if (auto scoped = failAllRemoves.scoped(); MONGO_unlikely(scoped.isActive())) {
        tassert(9276704,
                "failAllRemoves failpoint active!",
                !scoped.getData().hasField("tassert") || !scoped.getData().getBoolField("tassert"));
        uasserted(ErrorCodes::InternalError, "failAllRemoves failpoint active!");
    }

    if (DatabaseHolder::get(opCtx)->getDb(opCtx, ns.dbName())) {
        curOp.raiseDbProfileLevel(DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(ns.dbName()));
    }

    assertCanWrite_inlock(opCtx, ns);

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangWithLockDuringBatchRemove, opCtx, "hangWithLockDuringBatchRemove");

    auto exec = uassertStatusOK(
        getExecutorDelete(&curOp.debug(), collection, &parsedDelete, boost::none /* verbosity */));
    // Capture diagnostics to be logged in the case of a failure.
    ScopedDebugInfo explainDiagnostics("explainDiagnostics",
                                       diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setPlanSummary(lk, exec->getPlanExplainer().getPlanSummary());
    }

    auto nDeleted = exec->executeDelete();
    curOp.debug().additiveMetrics.ndeleted = nDeleted;

    PlanSummaryStats summary;
    auto&& explainer = exec->getPlanExplainer();
    explainer.getSummaryStats(&summary);
    if (const auto& coll = collection.getCollectionPtr()) {
        CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
            coll.get(),
            summary.collectionScans,
            summary.collectionScansNonTailable,
            summary.indexesUsed);
    }
    curOp.debug().setPlanSummaryMetrics(std::move(summary));

    if (curOp.shouldDBProfile()) {
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        curOp.debug().execStats = std::move(stats);
    }

    SingleWriteResult result;
    result.setN(nDeleted);
    return result;
}

WriteResult performDeletes(
    OperationContext* opCtx,
    const write_ops::DeleteCommandRequest& wholeOp,
    boost::optional<const timeseries::CollectionPreConditions&> preConditionsOptional,
    OperationSource source) {

    timeseries::CollectionPreConditions preConditions = preConditionsOptional
        ? *preConditionsOptional
        : timeseries::CollectionPreConditions::getCollectionPreConditions(
              opCtx, wholeOp.getNamespace(), wholeOp.getCollectionUUID());

    auto ns = preConditions.getTargetNs(wholeOp.getNamespace());
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
        if (opCtx->isRetryableWrite() && txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
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

        // Begin query planning timing once we have the nested CurOp.
        CurOp::get(opCtx)->beginQueryPlanningTimer();

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
                                                                    stmtId,
                                                                    singleOp,
                                                                    runtimeConstants,
                                                                    wholeOp.getLet(),
                                                                    preConditions,
                                                                    source);
            out.results.push_back(reply);
            lastOpFixer.finishedOpSuccessfully();

            if (singleOp.getMulti()) {
                getQueryCounters(opCtx).deleteManyCount.increment(1);
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

bool shouldRetryDuplicateKeyException(OperationContext* opCtx,
                                      const UpdateRequest& updateRequest,
                                      const CanonicalQuery& cq,
                                      const DuplicateKeyErrorInfo& errorInfo,
                                      int retryAttempts) {
    // In order to be retryable, the update must be an upsert with multi:false.
    if (!updateRequest.isUpsert() || updateRequest.isMulti()) {
        return false;
    }

    // In multi document transactions, there is an outer WriteUnitOfWork and inner WriteUnitOfWork.
    // The inner WriteUnitOfWork exists per-document. Aborting the inner one necessitates aborting
    // the outer one. Otherwise, retrying the inner one will read the writes of the
    // previously-aborted inner WriteUnitOfWork.
    if (opCtx->inMultiDocumentTransaction()) {
        return false;
    }

    // There was a bug where an upsert sending a document into a partial/sparse unique index would
    // retry indefinitely. To avoid this, cap the number of retries.
    int upsertMaxRetryAttemptsOnDuplicateKeyError =
        write_ops::gUpsertMaxRetryAttemptsOnDuplicateKeyError.load();
    if (retryAttempts > upsertMaxRetryAttemptsOnDuplicateKeyError) {
        LOGV2(9552300,
              "Upsert hit max number of retries on duplicate key exception, as determined by "
              "server parameter upsertMaxRetryAttemptsOnDuplicateKeyError. No further retry will "
              "be attempted for this query.",
              "upsertMaxRetryAttemptsOnDuplicateKeyError"_attr =
                  upsertMaxRetryAttemptsOnDuplicateKeyError);
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

    const auto& keyPattern = errorInfo.getKeyPattern();
    if (equalities.size() != static_cast<size_t>(keyPattern.nFields())) {
        return false;
    }

    // Check that collation of the query matches the unique index. To avoid calling
    // CollatorFactoryInterface when possible, first check the simple collator case.
    bool queryHasSimpleCollator = CollatorInterface::isSimpleCollator(cq.getCollator());
    bool indexHasSimpleCollator = errorInfo.getCollation().isEmpty();
    if (queryHasSimpleCollator != indexHasSimpleCollator) {
        return false;
    }

    if (!indexHasSimpleCollator) {
        auto indexCollator =
            uassertStatusOK(CollatorFactoryInterface::get(cq.getOpCtx()->getServiceContext())
                                ->makeFromBSON(errorInfo.getCollation()));
        if (!CollatorInterface::collatorsMatch(cq.getCollator(), indexCollator.get())) {
            return false;
        }
    }

    const auto& keyValue = errorInfo.getDuplicatedKeyValue();

    BSONObjIterator keyPatternIter(keyPattern);
    BSONObjIterator keyValueIter(keyValue);
    while (keyPatternIter.more() && keyValueIter.more()) {
        auto keyPatternElem = keyPatternIter.next();
        auto keyValueElem = keyValueIter.next();

        auto keyName = keyPatternElem.fieldNameStringData();
        auto equalityIt = equalities.find(keyName);
        if (equalityIt == equalities.end()) {
            return false;
        }
        const BSONElement& equalityElem = equalityIt->second->getData();

        // If the index have collation and we are comparing strings, we need to compare
        // ComparisonStrings instead of the raw value to respect collation.
        if (!indexHasSimpleCollator && equalityElem.type() == BSONType::string) {
            if (keyValueElem.type() != BSONType::string) {
                return false;
            }
            auto equalityComparisonString =
                cq.getCollator()->getComparisonString(equalityElem.valueStringData());
            if (equalityComparisonString != keyValueElem.valueStringData()) {
                return false;
            }
        } else {
            // Comparison which obeys field ordering but ignores field name.
            BSONElementComparator cmp{BSONElementComparator::FieldNamesMode::kIgnore, nullptr};
            if (cmp.evaluate(equalityElem != keyValueElem)) {
                return false;
            }
        }
    }
    invariant(!keyPatternIter.more());
    invariant(!keyValueIter.more());

    return true;
}

void explainUpdate(OperationContext* opCtx,
                   UpdateRequest& updateRequest,
                   bool isTimeseriesViewRequest,
                   const SerializationContext& serializationContext,
                   const BSONObj& command,
                   const timeseries::CollectionPreConditions& preConditions,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) {

    // Explains of write commands are read-only, but we take write locks so that timing
    // info is more accurate.
    const auto collection = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx,
                                                updateRequest.getNamespaceString(),
                                                AcquisitionPrerequisites::kWrite,
                                                preConditions.expectedUUID()),
        MODE_IX);

    timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
        opCtx, preConditions, collection);

    if (isTimeseriesViewRequest) {
        timeseries::timeseriesRequestChecks<UpdateRequest>(VersionContext::getDecoration(opCtx),
                                                           collection.getCollectionPtr(),
                                                           &updateRequest,
                                                           timeseries::updateRequestCheckFunction);
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

    // Capture diagnostics to be logged in the case of a failure.
    ScopedDebugInfo explainDiagnostics("explainDiagnostics",
                                       diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});
    Explain::explainStages(exec.get(),
                           collection,
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
                   const timeseries::CollectionPreConditions& preConditions,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) {
    // Explains of write commands are read-only, but we take write locks so that timing
    // info is more accurate.
    const auto collection =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest::fromOpCtx(opCtx,
                                                                  deleteRequest.getNsString(),
                                                                  AcquisitionPrerequisites::kWrite,
                                                                  preConditions.expectedUUID()),
                          MODE_IX);

    if (isTimeseriesViewRequest) {
        timeseries::timeseriesRequestChecks<DeleteRequest>(VersionContext::getDecoration(opCtx),
                                                           collection.getCollectionPtr(),
                                                           &deleteRequest,
                                                           timeseries::deleteRequestCheckFunction);
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

    // Capture diagnostics to be logged in the case of a failure.
    ScopedDebugInfo explainDiagnostics("explainDiagnostics",
                                       diagnostic_printers::ExplainDiagnosticPrinter{exec.get()});
    Explain::explainStages(exec.get(),
                           collection,
                           verbosity,
                           BSONObj(),
                           SerializationContext::stateCommandReply(serializationContext),
                           command,
                           &bodyBuilder);
}

}  // namespace mongo::write_ops_exec

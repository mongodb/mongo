/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/s/range_deletion_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/balancer_stats_registry.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeDoingDeletion);
MONGO_FAIL_POINT_DEFINE(hangAfterDoingDeletion);
MONGO_FAIL_POINT_DEFINE(suspendRangeDeletion);
MONGO_FAIL_POINT_DEFINE(throwWriteConflictExceptionInDeleteRange);
MONGO_FAIL_POINT_DEFINE(throwInternalErrorInDeleteRange);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionOnRecipientInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionLocallyInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible);
/**
 * Performs the deletion of up to numDocsToRemovePerBatch entries within the range in progress. Must
 * be called under the collection lock.
 *
 * Returns the number of documents deleted, 0 if done with the range, or bad status if deleting
 * the range failed.
 */
StatusWith<int> deleteNextBatch(OperationContext* opCtx,
                                const CollectionAcquisition& collection,
                                BSONObj const& keyPattern,
                                ChunkRange const& range,
                                int numDocsToRemovePerBatch) {
    invariant(collection.exists());

    auto const nss = collection.nss();
    auto const uuid = collection.uuid();

    // The IndexChunk has a keyPattern that may apply to more than one index - we need to
    // select the index and get the full index keyPattern here.
    const auto shardKeyIdx = findShardKeyPrefixedIndex(
        opCtx, collection.getCollectionPtr(), keyPattern, /*requireSingleKey=*/false);
    if (!shardKeyIdx) {
        // Do not log that the shard key is missing for hashed shard key patterns.
        if (!ShardKeyPattern::isHashedPatternEl(keyPattern.firstElement())) {
            LOGV2_ERROR(23765,
                        "Unable to find range shard key index",
                        "keyPattern"_attr = keyPattern,
                        logAttrs(nss));

            // When a shard key index is not found, the range deleter moves the task to the bottom
            // of the range deletion queue. This sleep is aimed at avoiding logging too aggressively
            // in order to prevent log files to increase too much in size.
            opCtx->sleepFor(Seconds(5));
        }

        iasserted(ErrorCodes::IndexNotFound,
                  str::stream() << "Unable to find shard key index"
                                << " for " << nss.toStringForErrorMsg() << " and key pattern `"
                                << keyPattern.toString() << "'");
    }

    const auto rangeDeleterPriority = rangeDeleterHighPriority.load()
        ? AdmissionContext::Priority::kExempt
        : AdmissionContext::Priority::kLow;

    ScopedAdmissionPriority priority{opCtx, rangeDeleterPriority};

    // Extend bounds to match the index we found
    const KeyPattern indexKeyPattern(shardKeyIdx->keyPattern());
    const auto extend = [&](const auto& key) {
        return Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(key, false));
    };

    const auto min = extend(range.getMin());
    const auto max = extend(range.getMax());

    LOGV2_DEBUG(6180601,
                1,
                "Begin removal of range",
                logAttrs(nss),
                "collectionUUID"_attr = uuid,
                "range"_attr = redact(range.toString()));

    auto deleteStageParams = std::make_unique<DeleteStageParams>();
    deleteStageParams->fromMigrate = true;
    deleteStageParams->isMulti = true;
    deleteStageParams->returnDeleted = true;

    auto exec =
        InternalPlanner::deleteWithShardKeyIndexScan(opCtx,
                                                     collection,
                                                     std::move(deleteStageParams),
                                                     *shardKeyIdx,
                                                     min,
                                                     max,
                                                     BoundInclusion::kIncludeStartKeyOnly,
                                                     PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                     InternalPlanner::FORWARD);

    if (MONGO_unlikely(hangBeforeDoingDeletion.shouldFail())) {
        LOGV2(23768, "Hit hangBeforeDoingDeletion failpoint");
        hangBeforeDoingDeletion.pauseWhileSet(opCtx);
    }

    long long bytesDeleted = 0;
    int numDocsDeleted = 0;
    do {
        BSONObj deletedObj;

        if (throwWriteConflictExceptionInDeleteRange.shouldFail()) {
            throwWriteConflictException(
                str::stream() << "Hit failpoint '"
                              << throwWriteConflictExceptionInDeleteRange.getName() << "'.");
        }

        if (throwInternalErrorInDeleteRange.shouldFail()) {
            uasserted(ErrorCodes::InternalError, "Failing for test");
        }

        PlanExecutor::ExecState state;
        try {
            state = exec->getNext(&deletedObj, nullptr);
        } catch (const DBException& ex) {
            auto&& explainer = exec->getPlanExplainer();
            auto&& [stats, _] =
                explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
            LOGV2_WARNING(6180602,
                          "Cursor error while trying to delete range",
                          logAttrs(nss),
                          "collectionUUID"_attr = uuid,
                          "range"_attr = redact(range.toString()),
                          "stats"_attr = redact(stats),
                          "error"_attr = redact(ex.toStatus()));
            throw;
        }

        if (state == PlanExecutor::IS_EOF) {
            break;
        }

        bytesDeleted += deletedObj.objsize();
        invariant(PlanExecutor::ADVANCED == state);
    } while (++numDocsDeleted < numDocsToRemovePerBatch);

    ShardingStatistics::get(opCtx).countDocsDeletedByRangeDeleter.addAndFetch(numDocsDeleted);
    ShardingStatistics::get(opCtx).countBytesDeletedByRangeDeleter.addAndFetch(bytesDeleted);

    return numDocsDeleted;
}

void ensureRangeDeletionTaskStillExists(OperationContext* opCtx,
                                        const UUID& collectionUuid,
                                        const ChunkRange& range) {
    // While at this point we are guaranteed for our operation context to be killed if there is a
    // step-up or stepdown, it is still possible that a stepdown and a subsequent step-up happened
    // prior to acquiring the global IX lock. The range deletion task document prevents a moveChunk
    // operation from migrating an overlapping range to this shard. If the range deletion task
    // document has already been deleted, then it is possible for the range in the user collection
    // to now be owned by this shard and for proceeding with the range deletion to result in data
    // corruption. The scheme for checking whether the range deletion task document still exists
    // relies on the executor only having a single thread and that thread being solely responsible
    // for deleting the range deletion task document.
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    const auto query =
        BSON(RangeDeletionTask::kCollectionUuidFieldName
             << collectionUuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey
             << range.getMin() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
             << range.getMax() << RangeDeletionTask::kPendingFieldName << BSON("$exists" << false));
    auto count = store.count(opCtx, query);

    uassert(ErrorCodes::RangeDeletionAbandonedBecauseTaskDocumentDoesNotExist,
            "Range deletion task no longer exists",
            count > 0);

    // We are now guaranteed that either (a) the range deletion task document will continue to exist
    // for the lifetime of this operation context, or (b) this operation context will be killed if
    // it is possible for the range deletion task document to have been deleted while we weren't
    // holding any locks.
}

void markRangeDeletionTaskAsProcessing(OperationContext* opCtx,
                                       const UUID& collectionUuid,
                                       const ChunkRange& range) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    const auto query =
        BSON(RangeDeletionTask::kCollectionUuidFieldName
             << collectionUuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey
             << range.getMin() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
             << range.getMax() << RangeDeletionTask::kPendingFieldName << BSON("$exists" << false));

    static const auto update =
        BSON("$set" << BSON(RangeDeletionTask::kProcessingFieldName
                            << true << RangeDeletionTask::kWhenToCleanFieldName
                            << CleanWhen_serializer(CleanWhenEnum::kNow)));

    try {
        store.update(opCtx, query, update, WriteConcerns::kLocalWriteConcern);
    } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
        // The collection may have been dropped or the document could have been manually deleted
    }
}

std::vector<RangeDeletionTask> getPersistentRangeDeletionTasks(OperationContext* opCtx,
                                                               const NamespaceString& nss) {
    std::vector<RangeDeletionTask> tasks;

    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    auto query = BSON(RangeDeletionTask::kNssFieldName
                      << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));

    store.forEach(opCtx, query, [&](const RangeDeletionTask& deletionTask) {
        tasks.push_back(deletionTask);
        return true;
    });

    return tasks;
}

BSONObj getQueryFilterForRangeDeletionTask(const UUID& collectionUuid, const ChunkRange& range) {
    return BSON(RangeDeletionTask::kCollectionUuidFieldName
                << collectionUuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey
                << range.getMin() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
                << range.getMax());
}

// Add `migrationId` to the query filter in order to be resilient to delayed network retries: only
// relying on collection's UUID and range may lead to undesired updates/deletes on tasks created by
// future migrations.
BSONObj getQueryFilterForRangeDeletionTaskOnRecipient(const UUID& collectionUuid,
                                                      const ChunkRange& range,
                                                      const UUID& migrationId) {
    return getQueryFilterForRangeDeletionTask(collectionUuid, range)
        .addFields(BSON(RangeDeletionTask::kIdFieldName << migrationId));
}


}  // namespace

namespace rangedeletionutil {

Status deleteRangeInBatches(OperationContext* opCtx,
                            const DatabaseName& dbName,
                            const UUID& collectionUuid,
                            const BSONObj& keyPattern,
                            const ChunkRange& range) {
    suspendRangeDeletion.pauseWhileSet(opCtx);

    bool allDocsRemoved = false;
    // Delete all batches in this range unless a stepdown error occurs. Do not yield the
    // executor to ensure that this range is fully deleted before another range is
    // processed.
    while (!allDocsRemoved) {
        try {
            int numDocsToRemovePerBatch = rangeDeleterBatchSize.load();
            if (numDocsToRemovePerBatch <= 0) {
                numDocsToRemovePerBatch = kRangeDeleterBatchSizeDefault;
            }

            Milliseconds delayBetweenBatches(rangeDeleterBatchDelayMS.load());

            ensureRangeDeletionTaskStillExists(opCtx, collectionUuid, range);

            markRangeDeletionTaskAsProcessing(opCtx, collectionUuid, range);

            int numDeleted;
            const auto nss = [&]() {
                try {
                    const auto nssOrUuid = NamespaceStringOrUUID{dbName, collectionUuid};
                    const auto collection =
                        acquireCollection(opCtx,
                                          {nssOrUuid,
                                           AcquisitionPrerequisites::kPretendUnsharded,
                                           repl::ReadConcernArgs::get(opCtx),
                                           AcquisitionPrerequisites::kWrite},
                                          MODE_IX);

                    LOGV2_DEBUG(6777800,
                                1,
                                "Starting batch deletion",
                                logAttrs(collection.nss()),
                                "collectionUUID"_attr = collectionUuid,
                                "range"_attr = redact(range.toString()),
                                "numDocsToRemovePerBatch"_attr = numDocsToRemovePerBatch,
                                "delayBetweenBatches"_attr = delayBetweenBatches);

                    numDeleted = uassertStatusOK(deleteNextBatch(
                        opCtx, collection, keyPattern, range, numDocsToRemovePerBatch));

                    return collection.nss();
                } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // Throw specific error code that stops range deletions in case of errors
                    uasserted(
                        ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist,
                        "Collection has been dropped since enqueuing this range "
                        "deletion task. No need to delete documents.");
                }
            }();

            persistUpdatedNumOrphans(opCtx, collectionUuid, range, -numDeleted);

            if (MONGO_unlikely(hangAfterDoingDeletion.shouldFail())) {
                hangAfterDoingDeletion.pauseWhileSet(opCtx);
            }

            LOGV2_DEBUG(23769,
                        1,
                        "Deleted documents in pass",
                        "numDeleted"_attr = numDeleted,
                        logAttrs(nss),
                        "collectionUUID"_attr = collectionUuid,
                        "range"_attr = redact(range.toString()));

            if (numDeleted > 0) {
                // (SERVER-62368) The range-deleter executor is mono-threaded, so
                // sleeping synchronously for `delayBetweenBatches` ensures that no
                // other batch is going to be cleared up before the expected delay.
                opCtx->sleepFor(delayBetweenBatches);
            }

            allDocsRemoved = numDeleted < numDocsToRemovePerBatch;
        } catch (const DBException& ex) {
            // Errors other than those indicating stepdown and those that indicate that the
            // range deletion can no longer occur should be retried.
            auto errorCode = ex.code();
            if (errorCode ==
                    ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist ||
                errorCode == ErrorCodes::RangeDeletionAbandonedBecauseTaskDocumentDoesNotExist ||
                errorCode == ErrorCodes::IndexNotFound ||
                errorCode == ErrorCodes::KeyPatternShorterThanBound ||
                ErrorCodes::isShutdownError(errorCode) ||
                ErrorCodes::isNotPrimaryError(errorCode) ||
                !opCtx->checkForInterruptNoAssert().isOK()) {
                return ex.toStatus();
            };
        }
    }
    return Status::OK();
}

void snapshotRangeDeletionsForRename(OperationContext* opCtx,
                                     const NamespaceString& fromNss,
                                     const NamespaceString& toNss) {
    // Clear out eventual snapshots associated with the target collection: always restart from a
    // clean state in case of stepdown or primary killed.
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionForRenameNamespace);
    store.remove(opCtx,
                 BSON(RangeDeletionTask::kNssFieldName << NamespaceStringUtil::serialize(
                          toNss, SerializationContext::stateDefault())));

    auto rangeDeletionTasks = getPersistentRangeDeletionTasks(opCtx, fromNss);
    for (auto& task : rangeDeletionTasks) {
        // Associate task to the new namespace
        task.setNss(toNss);
        // Assign a new id to prevent duplicate key conflicts with the source range deletion task
        task.setId(UUID::gen());
        store.add(opCtx, task);
    }
}

void restoreRangeDeletionTasksForRename(OperationContext* opCtx, const NamespaceString& nss) {
    PersistentTaskStore<RangeDeletionTask> rangeDeletionsForRenameStore(
        NamespaceString::kRangeDeletionForRenameNamespace);
    PersistentTaskStore<RangeDeletionTask> rangeDeletionsStore(
        NamespaceString::kRangeDeletionNamespace);

    const auto query = BSON(RangeDeletionTask::kNssFieldName << NamespaceStringUtil::serialize(
                                nss, SerializationContext::stateDefault()));

    rangeDeletionsForRenameStore.forEach(opCtx, query, [&](const RangeDeletionTask& deletionTask) {
        // Upsert the range deletion document so that:
        // - If no document for the same range exists, a task will be registered by the range
        // deleter insert observer.
        // - If a document for the same range already exists, no new task will be registered on
        // the range deleter service because the update observer only registers when the update
        // action is 'unset the pending field'.
        auto& uuid = deletionTask.getCollectionUuid();
        auto& range = deletionTask.getRange();
        auto upsertQuery =
            BSON(RangeDeletionTask::kCollectionUuidFieldName
                 << uuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey
                 << range.getMin() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
                 << range.getMax());
        // Remove _id because it's an immutable field so it can't be part of an update.
        // But include it as part of the upsert because the _id field is expected to be a uuid
        // (as opposed to the default OID) in case a new document is inserted.
        auto upsertDocument = deletionTask.toBSON().removeField(RangeDeletionTask::kIdFieldName);
        rangeDeletionsStore.upsert(opCtx,
                                   upsertQuery,
                                   BSON("$set"
                                        << upsertDocument << "$setOnInsert"
                                        << BSON(RangeDeletionTask::kIdFieldName << UUID::gen())));
        return true;
    });
}

void deleteRangeDeletionTasksForRename(OperationContext* opCtx,
                                       const NamespaceString& fromNss,
                                       const NamespaceString& toNss) {
    // Delete already restored snapshots associated to the target collection
    PersistentTaskStore<RangeDeletionTask> rangeDeletionsForRenameStore(
        NamespaceString::kRangeDeletionForRenameNamespace);
    rangeDeletionsForRenameStore.remove(
        opCtx,
        BSON(RangeDeletionTask::kNssFieldName
             << NamespaceStringUtil::serialize(toNss, SerializationContext::stateDefault())));
}


void persistUpdatedNumOrphans(OperationContext* opCtx,
                              const UUID& collectionUuid,
                              const ChunkRange& range,
                              long long changeInOrphans) {
    const auto query = getQueryFilterForRangeDeletionTask(collectionUuid, range);
    try {
        PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
        ScopedRangeDeleterLock rangeDeleterLock(opCtx, LockMode::MODE_IX);
        // The DBDirectClient will not retry WriteConflictExceptions internally while holding an X
        // mode lock, so we need to retry at this level.
        writeConflictRetry(
            opCtx, "updateOrphanCount", NamespaceString::kRangeDeletionNamespace, [&] {
                store.update(opCtx,
                             query,
                             BSON("$inc" << BSON(RangeDeletionTask::kNumOrphanDocsFieldName
                                                 << changeInOrphans)),
                             WriteConcerns::kLocalWriteConcern);
            });
        BalancerStatsRegistry::get(opCtx)->updateOrphansCount(collectionUuid, changeInOrphans);
    } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
        // When upgrading or downgrading, there may be no documents with the orphan count field.
    }
}

void removePersistentRangeDeletionTask(OperationContext* opCtx,
                                       const UUID& collectionUuid,
                                       const ChunkRange& range) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    auto overlappingRangeDeletionsQuery = BSON(
        RangeDeletionTask::kCollectionUuidFieldName
        << collectionUuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey << GTE
        << range.getMin() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey << LTE
        << range.getMax());
    store.remove(opCtx, overlappingRangeDeletionsQuery);
}

void removePersistentRangeDeletionTasksByUUID(OperationContext* opCtx, const UUID& collectionUuid) {
    DBDirectClient dbClient(opCtx);

    auto query = BSON(RangeDeletionTask::kCollectionUuidFieldName << collectionUuid);
    auto commandResponse = dbClient.runCommand([&] {
        write_ops::DeleteCommandRequest deleteOp(NamespaceString::kRangeDeletionNamespace);

        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(true);
            return entry;
        }()});

        return deleteOp.serialize({});
    }());

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));
}

BSONObj overlappingRangeDeletionsQuery(const ChunkRange& range, const UUID& uuid) {
    return BSON(RangeDeletionTask::kCollectionUuidFieldName
                << uuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey << LT
                << range.getMax() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
                << GT << range.getMin());
}

size_t checkForConflictingDeletions(OperationContext* opCtx,
                                    const ChunkRange& range,
                                    const UUID& uuid) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    return store.count(opCtx, overlappingRangeDeletionsQuery(range, uuid));
}

void persistRangeDeletionTaskLocally(OperationContext* opCtx,
                                     const RangeDeletionTask& deletionTask,
                                     const WriteConcernOptions& writeConcern) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    try {
        store.add(opCtx, deletionTask, writeConcern);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(31375,
                  str::stream() << "While attempting to write range deletion task for migration "
                                << ", found document with the same migration id. Attempted range "
                                   "deletion task: "
                                << deletionTask.toBSON());
    }
}

long long retrieveNumOrphansFromShard(OperationContext* opCtx,
                                      const ShardId& shardId,
                                      const UUID& migrationId) {
    const auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
    FindCommandRequest findCommand(NamespaceString::kRangeDeletionNamespace);
    findCommand.setFilter(BSON("_id" << migrationId));
    findCommand.setReadConcern(BSONObj());
    Shard::QueryResponse rangeDeletionResponse =
        uassertStatusOK(recipientShard->runExhaustiveCursorCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            NamespaceString::kRangeDeletionNamespace.dbName(),
            findCommand.toBSON(BSONObj()),
            Milliseconds(-1)));
    if (rangeDeletionResponse.docs.empty()) {
        // In case of shutdown/stepdown, the recipient may have already deleted its range deletion
        // document. A previous call to this function will have already returned the correct number
        // of orphans, so we can simply return 0.
        LOGV2_DEBUG(6376301,
                    2,
                    "No matching document found for migration",
                    "recipientId"_attr = shardId,
                    "migrationId"_attr = migrationId);
        return 0;
    }
    const auto numOrphanDocsElem =
        rangeDeletionResponse.docs[0].getField(RangeDeletionTask::kNumOrphanDocsFieldName);
    return numOrphanDocsElem.safeNumberLong();
}

boost::optional<KeyPattern> getShardKeyPatternFromRangeDeletionTask(OperationContext* opCtx,
                                                                    const UUID& migrationId) {
    DBDirectClient client(opCtx);
    FindCommandRequest findCommand(NamespaceString::kRangeDeletionNamespace);
    findCommand.setFilter(BSON("_id" << migrationId));
    auto cursor = client.find(std::move(findCommand));
    if (!cursor->more()) {
        // If the range deletion task doesn't exist then the migration must have been aborted, so
        // we won't need the shard key pattern anyways.
        return boost::none;
    }
    auto rdt = RangeDeletionTask::parse(IDLParserContext("MigrationRecovery"), cursor->next());
    return rdt.getKeyPattern();
}

void deleteRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                        const ShardId& recipientId,
                                        const UUID& collectionUuid,
                                        const ChunkRange& range,
                                        const UUID& migrationId) {
    const auto queryFilter =
        getQueryFilterForRangeDeletionTaskOnRecipient(collectionUuid, range, migrationId);
    write_ops::DeleteCommandRequest deleteOp(NamespaceString::kRangeDeletionNamespace);
    write_ops::DeleteOpEntry query(queryFilter, false /*multi*/);
    deleteOp.setDeletes({query});

    hangInDeleteRangeDeletionOnRecipientInterruptible.pauseWhileSet(opCtx);

    auto cmd = deleteOp.toBSON(
        BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));
    sharding_util::invokeCommandOnShardWithIdempotentRetryPolicy(
        opCtx, recipientId, NamespaceString::kRangeDeletionNamespace.dbName(), cmd);

    if (hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible.shouldFail()) {
        hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when deleting range deletion on recipient");
    }
}

void deleteRangeDeletionTaskLocally(OperationContext* opCtx,
                                    const UUID& collectionUuid,
                                    const ChunkRange& range,
                                    const WriteConcernOptions& writeConcern) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    const auto query = getQueryFilterForRangeDeletionTask(collectionUuid, range);
    store.remove(opCtx, query, writeConcern);

    if (hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible.shouldFail()) {
        hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when deleting range deletion locally");
    }
}

void markAsReadyRangeDeletionTaskLocally(OperationContext* opCtx,
                                         const UUID& collectionUuid,
                                         const ChunkRange& range) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    const auto query =
        BSON(RangeDeletionTask::kCollectionUuidFieldName
             << collectionUuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey
             << range.getMin() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
             << range.getMax());
    auto update = BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << ""));

    hangInReadyRangeDeletionLocallyInterruptible.pauseWhileSet(opCtx);
    try {
        store.update(opCtx, query, update);
    } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
        // If we are recovering the migration, the range-deletion may have already finished. So its
        // associated document may already have been removed.
    }

    if (hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible.shouldFail()) {
        hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when initiating range deletion locally");
    }
}

void markAsReadyRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                             const ShardId& recipientId,
                                             const UUID& collectionUuid,
                                             const ChunkRange& range,
                                             const UUID& migrationId) {
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kRangeDeletionNamespace);
    const auto queryFilter =
        getQueryFilterForRangeDeletionTaskOnRecipient(collectionUuid, range, migrationId);
    auto updateModification =
        write_ops::UpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << "") << "$set"
                          << BSON(RangeDeletionTask::kWhenToCleanFieldName
                                  << CleanWhen_serializer(CleanWhenEnum::kNow)))));
    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(false);
    updateOp.setUpdates({updateEntry});

    sharding_util::retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
        opCtx, "ready remote range deletion", [&](OperationContext* newOpCtx) {
            auto cmd = updateOp.toBSON(
                BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));
            try {
                sharding_util::invokeCommandOnShardWithIdempotentRetryPolicy(
                    newOpCtx, recipientId, NamespaceString::kRangeDeletionNamespace.dbName(), cmd);
            } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& exShardNotFound) {
                LOGV2_DEBUG(4620232,
                            1,
                            "Failed to mark range deletion task on recipient shard as ready",
                            "collectionUuid"_attr = collectionUuid,
                            "range"_attr = range,
                            "error"_attr = exShardNotFound);
                return;
            }

            if (hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible.shouldFail()) {
                hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible.pauseWhileSet(
                    newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when initiating range deletion on recipient");
            }
        });
}
}  // namespace rangedeletionutil
}  // namespace mongo

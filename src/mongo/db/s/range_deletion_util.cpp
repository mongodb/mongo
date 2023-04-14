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

#include <algorithm>
#include <boost/optional.hpp>
#include <utility>

#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/balancer_stats_registry.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/remove_saver.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeDoingDeletion);
MONGO_FAIL_POINT_DEFINE(hangAfterDoingDeletion);
MONGO_FAIL_POINT_DEFINE(suspendRangeDeletion);
MONGO_FAIL_POINT_DEFINE(throwWriteConflictExceptionInDeleteRange);
MONGO_FAIL_POINT_DEFINE(throwInternalErrorInDeleteRange);

/**
 * Performs the deletion of up to numDocsToRemovePerBatch entries within the range in progress. Must
 * be called under the collection lock.
 *
 * Returns the number of documents deleted, 0 if done with the range, or bad status if deleting
 * the range failed.
 */
StatusWith<int> deleteNextBatch(OperationContext* opCtx,
                                const CollectionPtr& collection,
                                BSONObj const& keyPattern,
                                ChunkRange const& range,
                                int numDocsToRemovePerBatch) {
    invariant(collection);

    auto const nss = collection->ns();

    // The IndexChunk has a keyPattern that may apply to more than one index - we need to
    // select the index and get the full index keyPattern here.
    const auto shardKeyIdx =
        findShardKeyPrefixedIndex(opCtx, collection, keyPattern, /*requireSingleKey=*/false);
    if (!shardKeyIdx) {
        LOGV2_ERROR(
            23765, "Unable to find shard key index", "keyPattern"_attr = keyPattern, logAttrs(nss));

        // When a shard key index is not found, the range deleter gets stuck and indefinitely logs
        // an error message. This sleep is aimed at avoiding logging too aggressively in order to
        // prevent log files to increase too much in size.
        opCtx->sleepFor(Seconds(5));

        uasserted(ErrorCodes::IndexNotFound,
                  str::stream() << "Unable to find shard key index"
                                << " for " << nss.ns() << " and key pattern `"
                                << keyPattern.toString() << "'");
    }

    // Extend bounds to match the index we found
    const KeyPattern indexKeyPattern(shardKeyIdx->keyPattern());
    const auto extend = [&](const auto& key) {
        return Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(key, false));
    };

    const auto min = extend(range.getMin());
    const auto max = extend(range.getMax());

    LOGV2_DEBUG(23766,
                1,
                "Begin removal of {min} to {max} in {namespace}",
                "Begin removal of range",
                "min"_attr = min,
                "max"_attr = max,
                logAttrs(nss));

    auto deleteStageParams = std::make_unique<DeleteStageParams>();
    deleteStageParams->fromMigrate = true;
    deleteStageParams->isMulti = true;
    deleteStageParams->returnDeleted = true;

    if (serverGlobalParams.moveParanoia) {
        deleteStageParams->removeSaver =
            std::make_unique<RemoveSaver>("moveChunk", nss.ns().toString(), "cleaning");
    }

    auto exec =
        InternalPlanner::deleteWithShardKeyIndexScan(opCtx,
                                                     &collection,
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

    int numDeleted = 0;
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
            LOGV2_WARNING(23776,
                          "Cursor error while trying to delete {min} to {max} in {namespace}, "
                          "stats: {stats}, error: {error}",
                          "Cursor error while trying to delete range",
                          "min"_attr = redact(min),
                          "max"_attr = redact(max),
                          logAttrs(nss),
                          "stats"_attr = redact(stats),
                          "error"_attr = redact(ex.toStatus()));
            throw;
        }

        if (state == PlanExecutor::IS_EOF) {
            break;
        }

        invariant(PlanExecutor::ADVANCED == state);
        ShardingStatistics::get(opCtx).countDocsDeletedOnDonor.addAndFetch(1);

    } while (++numDeleted < numDocsToRemovePerBatch);

    return numDeleted;
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

/**
 * Delete the range in a sequence of batches until there are no more documents to delete or deletion
 * returns an error.
 */
ExecutorFuture<void> deleteRangeInBatchesWithExecutor(
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const BSONObj& keyPattern,
    const ChunkRange& range) {
    return ExecutorFuture<void>(executor).then([=] {
        return withTemporaryOperationContext(
            [=](OperationContext* opCtx) {
                return deleteRangeInBatches(
                    opCtx, DatabaseName{nss.db()}, collectionUuid, keyPattern, range);
            },
            DatabaseName{nss.db()},
            collectionUuid);
    });
}

ExecutorFuture<void> waitForDeletionsToMajorityReplicate(
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const ChunkRange& range) {
    return withTemporaryOperationContext(
        [=](OperationContext* opCtx) {
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

            LOGV2_DEBUG(5346202,
                        1,
                        "Waiting for majority replication of local deletions",
                        logAttrs(nss),
                        "collectionUUID"_attr = collectionUuid,
                        "range"_attr = redact(range.toString()),
                        "clientOpTime"_attr = clientOpTime);

            // Asynchronously wait for majority write concern.
            return WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(clientOpTime, CancellationToken::uncancelable())
                .thenRunOn(executor);
        },
        DatabaseName{nss.db()},
        collectionUuid);
}

std::vector<RangeDeletionTask> getPersistentRangeDeletionTasks(OperationContext* opCtx,
                                                               const NamespaceString& nss) {
    std::vector<RangeDeletionTask> tasks;

    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    auto query = BSON(RangeDeletionTask::kNssFieldName << nss.ns());

    store.forEach(opCtx, query, [&](const RangeDeletionTask& deletionTask) {
        tasks.push_back(std::move(deletionTask));
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

}  // namespace

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
                    AutoGetCollection collection(
                        opCtx, NamespaceStringOrUUID{dbName.toString(), collectionUuid}, MODE_IX);

                    LOGV2_DEBUG(6777800,
                                1,
                                "Starting batch deletion",
                                logAttrs(collection.getNss()),
                                "collectionUUID"_attr = collectionUuid,
                                "range"_attr = redact(range.toString()),
                                "numDocsToRemovePerBatch"_attr = numDocsToRemovePerBatch,
                                "delayBetweenBatches"_attr = delayBetweenBatches);

                    numDeleted = uassertStatusOK(deleteNextBatch(opCtx,
                                                                 collection.getCollection(),
                                                                 keyPattern,
                                                                 range,
                                                                 numDocsToRemovePerBatch));

                    return collection.getNss();
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
                        "range"_attr = range.toString());

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
    store.remove(opCtx, BSON(RangeDeletionTask::kNssFieldName << toNss.ns()));

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

    const auto query = BSON(RangeDeletionTask::kNssFieldName << nss.ns());

    rangeDeletionsForRenameStore.forEach(opCtx, query, [&](const RangeDeletionTask& deletionTask) {
        try {
            rangeDeletionsStore.add(opCtx, deletionTask);
        } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
            // Task already scheduled in a previous call of this method
        }
        return true;
    });
}

void deleteRangeDeletionTasksForRename(OperationContext* opCtx,
                                       const NamespaceString& fromNss,
                                       const NamespaceString& toNss) {
    // Delete already restored snapshots associated to the target collection
    PersistentTaskStore<RangeDeletionTask> rangeDeletionsForRenameStore(
        NamespaceString::kRangeDeletionForRenameNamespace);
    rangeDeletionsForRenameStore.remove(opCtx,
                                        BSON(RangeDeletionTask::kNssFieldName << toNss.ns()));
}

SharedSemiFuture<void> removeDocumentsInRange(
    const std::shared_ptr<executor::TaskExecutor>& executor,
    SemiFuture<void> waitForActiveQueriesToComplete,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const BSONObj& keyPattern,
    const ChunkRange& range,
    Seconds delayForActiveQueriesOnSecondariesToComplete) {
    return std::move(waitForActiveQueriesToComplete)
        .thenRunOn(executor)
        .onError([&](Status s) {
            // The code does not expect the input future to have an error set on it, so we
            // invariant here to prevent future misuse (no pun intended).
            invariant(s.isOK());
        })
        .then([=]() mutable {
            // Wait for possibly ongoing queries on secondaries to complete.
            return sleepUntil(executor,
                              executor->now() + delayForActiveQueriesOnSecondariesToComplete);
        })
        .then([=]() mutable {
            LOGV2_DEBUG(23772,
                        1,
                        "Beginning deletion of documents",
                        logAttrs(nss),
                        "range"_attr = redact(range.toString()));

            return deleteRangeInBatchesWithExecutor(
                       executor, nss, collectionUuid, keyPattern, range)
                .onCompletion([=](Status s) {
                    if (!s.isOK() &&
                        s.code() !=
                            ErrorCodes::RangeDeletionAbandonedBecauseTaskDocumentDoesNotExist) {
                        // Propagate any errors to the onCompletion() handler below.
                        return ExecutorFuture<void>(executor, s);
                    }

                    // We wait for majority write concern even if the range deletion task document
                    // doesn't exist to guarantee the deletion (which must have happened earlier) is
                    // visible to the caller at non-local read concerns.
                    return waitForDeletionsToMajorityReplicate(executor, nss, collectionUuid, range)
                        .then([=] {
                            LOGV2_DEBUG(5346201,
                                        1,
                                        "Finished waiting for majority for deleted batch",
                                        logAttrs(nss),
                                        "range"_attr = redact(range.toString()));
                            // Propagate any errors to the onCompletion() handler below.
                            return s;
                        });
                });
        })
        .onCompletion([=](Status s) {
            if (s.isOK()) {
                LOGV2_DEBUG(23773,
                            1,
                            "Completed deletion of documents in {namespace} range {range}",
                            "Completed deletion of documents",
                            logAttrs(nss),
                            "range"_attr = redact(range.toString()));
            } else {
                LOGV2(23774,
                      "Failed to delete documents in {namespace} range {range} due to {error}",
                      "Failed to delete documents",
                      logAttrs(nss),
                      "range"_attr = redact(range.toString()),
                      "error"_attr = redact(s));
            }

            if (s.code() == ErrorCodes::RangeDeletionAbandonedBecauseTaskDocumentDoesNotExist) {
                return Status::OK();
            }

            if (!s.isOK() &&
                s.code() !=
                    ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist) {
                // Propagate any errors to callers waiting on the result.
                return s;
            }

            try {
                withTemporaryOperationContext(
                    [&](OperationContext* opCtx) {
                        removePersistentRangeDeletionTask(opCtx, collectionUuid, range);
                    },
                    DatabaseName{nss.db()},
                    collectionUuid);
            } catch (const DBException& e) {
                LOGV2_ERROR(23770,
                            "Failed to delete range deletion task for range {range} in collection "
                            "{namespace} due to {error}",
                            "Failed to delete range deletion task",
                            "range"_attr = range,
                            logAttrs(nss),
                            "error"_attr = e.what());

                return e.toStatus();
            }

            LOGV2_DEBUG(23775,
                        1,
                        "Completed removal of persistent range deletion task for {namespace} "
                        "range {range}",
                        "Completed removal of persistent range deletion task",
                        logAttrs(nss),
                        "range"_attr = redact(range.toString()));

            // Propagate any errors to callers waiting on the result.
            return s;
        })
        .semi()
        .share();
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
            opCtx, "updateOrphanCount", NamespaceString::kRangeDeletionNamespace.ns(), [&] {
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

    auto overlappingRangeQuery = BSON(
        RangeDeletionTask::kCollectionUuidFieldName
        << collectionUuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey << GTE
        << range.getMin() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey << LTE
        << range.getMax());
    store.remove(opCtx, overlappingRangeQuery);
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

}  // namespace mongo

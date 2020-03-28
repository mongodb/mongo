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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

#include "mongo/platform/basic.h"

#include "mongo/db/s/range_deletion_util.h"

#include <algorithm>
#include <utility>

#include <boost/optional.hpp>

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/persistent_task_store.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/wait_for_majority_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/remove_saver.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"

namespace mongo {

namespace {
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kWriteConcernTimeoutSharding);

MONGO_FAIL_POINT_DEFINE(hangBeforeDoingDeletion);
MONGO_FAIL_POINT_DEFINE(suspendRangeDeletion);
MONGO_FAIL_POINT_DEFINE(throwWriteConflictExceptionInDeleteRange);
MONGO_FAIL_POINT_DEFINE(throwInternalErrorInDeleteRange);

/**
 * Returns whether the currentCollection has the same UUID as the expectedCollectionUuid. Used to
 * ensure that the collection has not been dropped or dropped and recreated since the range was
 * enqueued for deletion.
 */
bool collectionUuidHasChanged(const NamespaceString& nss,
                              Collection* currentCollection,
                              UUID expectedCollectionUuid) {

    if (!currentCollection) {
        LOGV2_DEBUG(23763,
                    1,
                    "Abandoning range deletion task for {nss_ns} with UUID "
                    "{expectedCollectionUuid} because the collection has been dropped",
                    "nss_ns"_attr = nss.ns(),
                    "expectedCollectionUuid"_attr = expectedCollectionUuid);
        return true;
    }

    if (currentCollection->uuid() != expectedCollectionUuid) {
        LOGV2_DEBUG(
            23764,
            1,
            "Abandoning range deletion task for {nss_ns} with UUID {expectedCollectionUuid} "
            "because UUID of {nss_ns2}has changed (current is {currentCollection_uuid})",
            "nss_ns"_attr = nss.ns(),
            "expectedCollectionUuid"_attr = expectedCollectionUuid,
            "nss_ns2"_attr = nss.ns(),
            "currentCollection_uuid"_attr = currentCollection->uuid());
        return true;
    }

    return false;
}

/**
 * Performs the deletion of up to numDocsToRemovePerBatch entries within the range in progress. Must
 * be called under the collection lock.
 *
 * Returns the number of documents deleted, 0 if done with the range, or bad status if deleting
 * the range failed.
 */
StatusWith<int> deleteNextBatch(OperationContext* opCtx,
                                Collection* collection,
                                BSONObj const& keyPattern,
                                ChunkRange const& range,
                                int numDocsToRemovePerBatch) {
    invariant(collection != nullptr);

    auto const& nss = collection->ns();

    // The IndexChunk has a keyPattern that may apply to more than one index - we need to
    // select the index and get the full index keyPattern here.
    auto catalog = collection->getIndexCatalog();
    const IndexDescriptor* idx = catalog->findShardKeyPrefixedIndex(opCtx, keyPattern, false);
    if (!idx) {
        std::string msg = str::stream()
            << "Unable to find shard key index for " << keyPattern.toString() << " in " << nss.ns();
        LOGV2(23765, "{msg}", "msg"_attr = msg);
        return {ErrorCodes::InternalError, msg};
    }

    // Extend bounds to match the index we found
    const KeyPattern indexKeyPattern(idx->keyPattern());
    const auto extend = [&](const auto& key) {
        return Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(key, false));
    };

    const auto min = extend(range.getMin());
    const auto max = extend(range.getMax());

    LOGV2_DEBUG(23766,
                1,
                "begin removal of {min} to {max} in {nss_ns}",
                "min"_attr = min,
                "max"_attr = max,
                "nss_ns"_attr = nss.ns());

    const auto indexName = idx->indexName();
    const IndexDescriptor* descriptor =
        collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    if (!descriptor) {
        std::string msg = str::stream()
            << "shard key index with name " << indexName << " on '" << nss.ns() << "' was dropped";
        LOGV2(23767, "{msg}", "msg"_attr = msg);
        return {ErrorCodes::InternalError, msg};
    }

    auto deleteStageParams = std::make_unique<DeleteStageParams>();
    deleteStageParams->fromMigrate = true;
    deleteStageParams->isMulti = true;
    deleteStageParams->returnDeleted = true;

    if (serverGlobalParams.moveParanoia) {
        deleteStageParams->removeSaver =
            std::make_unique<RemoveSaver>("moveChunk", nss.ns(), "cleaning");
    }

    auto exec = InternalPlanner::deleteWithIndexScan(opCtx,
                                                     collection,
                                                     std::move(deleteStageParams),
                                                     descriptor,
                                                     min,
                                                     max,
                                                     BoundInclusion::kIncludeStartKeyOnly,
                                                     PlanExecutor::YIELD_MANUAL,
                                                     InternalPlanner::FORWARD);

    if (MONGO_unlikely(hangBeforeDoingDeletion.shouldFail())) {
        LOGV2(23768, "Hit hangBeforeDoingDeletion failpoint");
        hangBeforeDoingDeletion.pauseWhileSet(opCtx);
    }

    PlanYieldPolicy planYieldPolicy(exec.get(), PlanExecutor::YIELD_MANUAL);

    int numDeleted = 0;
    do {
        BSONObj deletedObj;

        if (throwWriteConflictExceptionInDeleteRange.shouldFail()) {
            throw WriteConflictException();
        }

        if (throwInternalErrorInDeleteRange.shouldFail()) {
            uasserted(ErrorCodes::InternalError, "Failing for test");
        }

        PlanExecutor::ExecState state = exec->getNext(&deletedObj, nullptr);

        if (state == PlanExecutor::IS_EOF) {
            break;
        }

        if (state == PlanExecutor::FAILURE) {
            LOGV2_WARNING(
                23776,
                "{PlanExecutor_statestr_state} - cursor error while trying to delete {min} to "
                "{max} in {nss}: FAILURE, stats: {Explain_getWinningPlanStats_exec_get}",
                "PlanExecutor_statestr_state"_attr = PlanExecutor::statestr(state),
                "min"_attr = redact(min),
                "max"_attr = redact(max),
                "nss"_attr = nss,
                "Explain_getWinningPlanStats_exec_get"_attr =
                    Explain::getWinningPlanStats(exec.get()));
            break;
        }

        invariant(PlanExecutor::ADVANCED == state);
        ShardingStatistics::get(opCtx).countDocsDeletedOnDonor.addAndFetch(1);

    } while (++numDeleted < numDocsToRemovePerBatch);

    return numDeleted;
}


template <typename Callable>
auto withTemporaryOperationContext(Callable&& callable) {
    ThreadClient tc(migrationutil::kRangeDeletionThreadName, getGlobalServiceContext());
    {
        stdx::lock_guard<Client> lk(*tc.get());
        tc->setSystemOperationKillable(lk);
    }
    auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
    auto opCtx = uniqueOpCtx.get();

    {
        // We acquire the global IX lock and then immediately release it to ensure this operation
        // would be killed by the RstlKillOpThread during step-up or stepdown. Note that the
        // RstlKillOpThread kills any operations on step-up or stepdown for which
        // Locker::wasGlobalLockTakenInModeConflictingWithWrites() returns true.
        Lock::GlobalLock lk(opCtx, MODE_IX);
    }
    invariant(opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());

    return callable(opCtx);
}

void ensureRangeDeletionTaskStillExists(OperationContext* opCtx, const UUID& migrationId) {
    // While at this point we are guaranteed for our operation context to be killed if there is a
    // step-up or stepdown, it is still possible that a stepdown and a subsequent step-up happened
    // prior to acquiring the global IX lock. The range deletion task document prevents a moveChunk
    // operation from migrating an overlapping range to this shard. If the range deletion task
    // document has already been deleted, then it is possible for the range in the user collection
    // to now be owned by this shard and for proceeding with the range deletion to result in data
    // corruption. The scheme for checking whether the range deletion task document still exists
    // relies on the executor only having a single thread and that thread being solely responsible
    // for deleting the range deletion task document.
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);
    auto count = store.count(opCtx,
                             QUERY(RangeDeletionTask::kIdFieldName
                                   << migrationId << RangeDeletionTask::kPendingFieldName
                                   << BSON("$exists" << false)));
    invariant(count == 0 || count == 1, "found duplicate range deletion tasks");
    uassert(ErrorCodes::RangeDeletionAbandonedBecauseTaskDocumentDoesNotExist,
            "Range deletion task no longer exists",
            count == 1);

    // We are now guaranteed that either (a) the range deletion task document will continue to exist
    // for the lifetime of this operation context, or (b) this operation context will be killed if
    // it is possible for the range deletion task document to have been deleted while we weren't
    // holding any locks.
}

/**
 * Delete the range in a sequence of batches until there are no more documents to
 * delete or deletion returns an error.
 */
ExecutorFuture<void> deleteRangeInBatches(const std::shared_ptr<executor::TaskExecutor>& executor,
                                          const NamespaceString& nss,
                                          const UUID& collectionUuid,
                                          const BSONObj& keyPattern,
                                          const ChunkRange& range,
                                          const boost::optional<UUID>& migrationId,
                                          int numDocsToRemovePerBatch,
                                          Milliseconds delayBetweenBatches) {
    return AsyncTry([=] {
               return withTemporaryOperationContext([=](OperationContext* opCtx) {
                   if (migrationId) {
                       ensureRangeDeletionTaskStillExists(opCtx, *migrationId);
                   }

                   AutoGetCollection autoColl(opCtx, nss, MODE_IX);
                   auto* const collection = autoColl.getCollection();

                   // Ensure the collection exists and has not been dropped or dropped and
                   // recreated.
                   uassert(ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist,
                           "Collection has been dropped since enqueuing this range "
                           "deletion task. No need to delete documents.",
                           !collectionUuidHasChanged(nss, collection, collectionUuid));

                   auto numDeleted = uassertStatusOK(deleteNextBatch(
                       opCtx, collection, keyPattern, range, numDocsToRemovePerBatch));

                   LOGV2_DEBUG(23769,
                               2,
                               "Deleted {numDeleted} documents in pass in namespace {nss_ns} with "
                               "UUID  {collectionUuid} for range {range}",
                               "numDeleted"_attr = numDeleted,
                               "nss_ns"_attr = nss.ns(),
                               "collectionUuid"_attr = collectionUuid,
                               "range"_attr = range.toString());

                   return numDeleted;
               });
           })
        .until([](StatusWith<int> swNumDeleted) {
            // Continue iterating until there are no more documents to delete, retrying on
            // any error that doesn't indicate that this node is stepping down.
            return (swNumDeleted.isOK() && swNumDeleted.getValue() == 0) ||
                swNumDeleted.getStatus() ==
                ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist ||
                swNumDeleted.getStatus() ==
                ErrorCodes::RangeDeletionAbandonedBecauseTaskDocumentDoesNotExist ||
                ErrorCodes::isShutdownError(swNumDeleted.getStatus()) ||
                ErrorCodes::isNotMasterError(swNumDeleted.getStatus());
        })
        .withDelayBetweenIterations(delayBetweenBatches)
        .on(executor)
        .ignoreValue();
}

/**
 * Notify the secondaries that this range is being deleted. Secondaries will watch for this update,
 * and kill any queries that may depend on documents in the range -- excepting any queries with a
 * read-concern option 'ignoreChunkMigration'.
 */
void notifySecondariesThatDeletionIsOccurring(const NamespaceString& nss,
                                              const UUID& collectionUuid,
                                              const ChunkRange& range) {
    withTemporaryOperationContext([&](OperationContext* opCtx) {
        AutoGetCollection autoAdmin(opCtx, NamespaceString::kServerConfigurationNamespace, MODE_IX);
        Helpers::upsert(opCtx,
                        NamespaceString::kServerConfigurationNamespace.ns(),
                        BSON("_id"
                             << "startRangeDeletion"
                             << "ns" << nss.ns() << "uuid" << collectionUuid << "min"
                             << range.getMin() << "max" << range.getMax()));
    });
}

void removePersistentRangeDeletionTask(const NamespaceString& nss, UUID migrationId) {
    withTemporaryOperationContext([&](OperationContext* opCtx) {
        PersistentTaskStore<RangeDeletionTask> store(opCtx,
                                                     NamespaceString::kRangeDeletionNamespace);

        store.remove(opCtx, QUERY(RangeDeletionTask::kIdFieldName << migrationId));
    });
}

ExecutorFuture<void> waitForDeletionsToMajorityReplicate(
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const ChunkRange& range) {
    return withTemporaryOperationContext([=](OperationContext* opCtx) {
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

        LOGV2_DEBUG(23771,
                    2,
                    "Waiting for majority replication of local deletions in namespace {nss_ns} "
                    "with UUID  {collectionUuid} for range {range}",
                    "nss_ns"_attr = nss.ns(),
                    "collectionUuid"_attr = collectionUuid,
                    "range"_attr = redact(range.toString()));

        // Asynchronously wait for majority write concern.
        return WaitForMajorityService::get(opCtx->getServiceContext())
            .waitUntilMajority(clientOpTime)
            .thenRunOn(executor);
    });
}

}  // namespace

SharedSemiFuture<void> removeDocumentsInRange(
    const std::shared_ptr<executor::TaskExecutor>& executor,
    SemiFuture<void> waitForActiveQueriesToComplete,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const BSONObj& keyPattern,
    const ChunkRange& range,
    boost::optional<UUID> migrationId,
    int numDocsToRemovePerBatch,
    Seconds delayForActiveQueriesOnSecondariesToComplete,
    Milliseconds delayBetweenBatches) {
    return std::move(waitForActiveQueriesToComplete)
        .thenRunOn(executor)
        .onError([&](Status s) {
            // The code does not expect the input future to have an error set on it, so we
            // invariant here to prevent future misuse (no pun intended).
            invariant(s.isOK());
        })
        .then([=]() mutable {
            suspendRangeDeletion.pauseWhileSet();
            // Wait for possibly ongoing queries on secondaries to complete.
            return sleepUntil(executor,
                              executor->now() + delayForActiveQueriesOnSecondariesToComplete);
        })
        .then([=]() mutable {
            LOGV2_DEBUG(23772,
                        2,
                        "Beginning deletion of any documents in {nss_ns} range {range} with  "
                        "numDocsToRemovePerBatch {numDocsToRemovePerBatch}",
                        "nss_ns"_attr = nss.ns(),
                        "range"_attr = redact(range.toString()),
                        "numDocsToRemovePerBatch"_attr = numDocsToRemovePerBatch);

            notifySecondariesThatDeletionIsOccurring(nss, collectionUuid, range);

            return deleteRangeInBatches(executor,
                                        nss,
                                        collectionUuid,
                                        keyPattern,
                                        range,
                                        migrationId,
                                        numDocsToRemovePerBatch,
                                        delayBetweenBatches)
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
                            // Propagate any errors to the onCompletion() handler below.
                            return s;
                        });
                });
        })
        .onCompletion([=](Status s) {
            if (s.isOK()) {
                LOGV2_DEBUG(23773,
                            2,
                            "Completed deletion of documents in {nss_ns} range {range}",
                            "nss_ns"_attr = nss.ns(),
                            "range"_attr = redact(range.toString()));
            } else {
                LOGV2(23774,
                      "Failed to delete of documents in {nss_ns} range {range}{causedBy_s}",
                      "nss_ns"_attr = nss.ns(),
                      "range"_attr = redact(range.toString()),
                      "causedBy_s"_attr = causedBy(redact(s)));
            }

            if (s.code() == ErrorCodes::RangeDeletionAbandonedBecauseTaskDocumentDoesNotExist) {
                return Status::OK();
            }

            if (!migrationId ||
                (!s.isOK() &&
                 s.code() !=
                     ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist)) {
                // Propagate any errors to callers waiting on the result.
                return s;
            }

            try {
                removePersistentRangeDeletionTask(nss, std::move(*migrationId));
            } catch (const DBException& e) {
                LOGV2(23770,
                      "Failed to delete range deletion task for range {range} in collection "
                      "{nss}{causedBy_e_what}",
                      "range"_attr = range,
                      "nss"_attr = nss,
                      "causedBy_e_what"_attr = causedBy(e.what()));

                return e.toStatus();
            }

            LOGV2_DEBUG(23775,
                        1,
                        "Completed removal of persistent range deletion task for {nss_ns} "
                        "range {range}",
                        "nss_ns"_attr = nss.ns(),
                        "range"_attr = redact(range.toString()));

            // Propagate any errors to callers waiting on the result.
            return s;
        })
        .semi()
        .share();
}

}  // namespace mongo

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/balancer_stats_registry.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/range_deleter_service_op_observer.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {
namespace {
const auto rangeDeleterServiceDecorator = ServiceContext::declareDecoration<RangeDeleterService>();

BSONObj getShardKeyPattern(OperationContext* opCtx,
                           const DatabaseName& dbName,
                           const UUID& collectionUuid) {
    while (true) {
        opCtx->checkForInterrupt();
        boost::optional<NamespaceString> optNss;
        {
            AutoGetCollection collection(
                opCtx, NamespaceStringOrUUID{dbName.toString(), collectionUuid}, MODE_IS);

            auto optMetadata = CollectionShardingRuntime::get(opCtx, collection.getNss())
                                   ->getCurrentMetadataIfKnown();
            if (optMetadata && optMetadata->isSharded()) {
                return optMetadata->getShardKeyPattern().toBSON();
            }
            optNss = collection.getNss();
        }

        onShardVersionMismatchNoExcept(opCtx, *optNss, boost::none).ignore();
        continue;
    }
}

}  // namespace

const ReplicaSetAwareServiceRegistry::Registerer<RangeDeleterService>
    rangeDeleterServiceRegistryRegisterer("RangeDeleterService");

RangeDeleterService* RangeDeleterService::get(ServiceContext* serviceContext) {
    return &rangeDeleterServiceDecorator(serviceContext);
}

RangeDeleterService* RangeDeleterService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void RangeDeleterService::onStepUpComplete(OperationContext* opCtx, long long term) {
    if (!feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCV()) {
        return;
    }

    if (disableResumableRangeDeleter.load()) {
        LOGV2_INFO(
            6872508,
            "Not resuming range deletions on step-up because `disableResumableRangeDeleter=true`");
        return;
    }

    auto lock = _acquireMutexUnconditionally();
    dassert(_state.load() == kDown, "Service expected to be down before stepping up");

    _state.store(kInitializing);

    if (_executor) {
        // Join previously shutted down executor before reinstantiating it
        _executor->join();
        _executor.reset();

        // Reset potential in-memory state referring a previous term
        _rangeDeletionTasks.clear();
    } else {
        // Initializing the op observer, only executed once at the first step-up
        auto opObserverRegistry =
            checked_cast<OpObserverRegistry*>(opCtx->getServiceContext()->getOpObserver());
        opObserverRegistry->addObserver(std::make_unique<RangeDeleterServiceOpObserver>());
    }

    const std::string kExecName("RangeDeleterServiceExecutor");
    auto net = executor::makeNetworkInterface(kExecName);
    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
    auto taskExecutor =
        std::make_shared<executor::ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
    _executor = std::move(taskExecutor);
    _executor->startup();

    _recoverRangeDeletionsOnStepUp(opCtx);
}

void RangeDeleterService::_recoverRangeDeletionsOnStepUp(OperationContext* opCtx) {
    if (disableResumableRangeDeleter.load()) {
        _state.store(kDown);
        return;
    }

    LOGV2(6834800, "Resubmitting range deletion tasks");

    ServiceContext* serviceContext = opCtx->getServiceContext();

    ExecutorFuture<void>(_executor)
        .then([serviceContext, this] {
            ThreadClient tc("ResubmitRangeDeletionsOnStepUp", serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }
            auto opCtx = tc->makeOperationContext();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            ScopedRangeDeleterLock rangeDeleterLock(opCtx.get());
            DBDirectClient client(opCtx.get());

            int nRescheduledTasks = 0;

            // (1) register range deletion tasks marked as "processing"
            auto processingTasksCompletionFuture = [&] {
                std::vector<ExecutorFuture<void>> processingTasksCompletionFutures;
                FindCommandRequest findCommand(NamespaceString::kRangeDeletionNamespace);
                findCommand.setFilter(BSON(RangeDeletionTask::kProcessingFieldName << true));
                auto cursor = client.find(std::move(findCommand));

                while (cursor->more()) {
                    auto completionFuture = this->registerTask(
                        RangeDeletionTask::parse(IDLParserContext("rangeDeletionRecovery"),
                                                 cursor->next()),
                        SemiFuture<void>::makeReady(),
                        true /* fromResubmitOnStepUp */);
                    nRescheduledTasks++;
                    processingTasksCompletionFutures.push_back(
                        completionFuture.thenRunOn(_executor));
                }

                if (nRescheduledTasks > 1) {
                    LOGV2_WARNING(6834801,
                                  "Rescheduling several range deletions marked as processing. "
                                  "Orphans count may be off while they are not drained",
                                  "numRangeDeletionsMarkedAsProcessing"_attr = nRescheduledTasks);
                }

                return processingTasksCompletionFutures.size() > 0
                    ? whenAllSucceed(std::move(processingTasksCompletionFutures)).share()
                    : SemiFuture<void>::makeReady().share();
            }();

            // (2) register all other "non-pending" tasks
            {
                FindCommandRequest findCommand(NamespaceString::kRangeDeletionNamespace);
                findCommand.setFilter(BSON(RangeDeletionTask::kProcessingFieldName
                                           << BSON("$ne" << true)
                                           << RangeDeletionTask::kPendingFieldName
                                           << BSON("$ne" << true)));
                auto cursor = client.find(std::move(findCommand));
                while (cursor->more()) {
                    (void)this->registerTask(
                        RangeDeletionTask::parse(IDLParserContext("rangeDeletionRecovery"),
                                                 cursor->next()),
                        processingTasksCompletionFuture.thenRunOn(_executor).semi(),
                        true /* fromResubmitOnStepUp */);
                }
            }

            LOGV2_INFO(6834802,
                       "Finished resubmitting range deletion tasks",
                       "nRescheduledTasks"_attr = nRescheduledTasks);

            auto lock = _acquireMutexUnconditionally();
            // Since the recovery is only spawned on step-up but may complete later, it's not
            // assumable that the node is still primary when the all resubmissions finish
            if (_state.load() != kDown) {
                this->_rangeDeleterServiceUpCondVar_FOR_TESTING.notify_all();
                this->_state.store(kUp);
            }
        })
        .getAsync([](auto) {});
}

void RangeDeleterService::onStepDown() {
    if (!feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCV()) {
        return;
    }

    auto lock = _acquireMutexUnconditionally();

    // It may happen for the `onStepDown` hook to be invoked on a SECONDARY node transitioning
    // to ROLLBACK, hence the executor may have never been initialized
    if (_executor) {
        _executor->shutdown();
    }

    _state.store(kDown);
}

void RangeDeleterService::onShutdown() {
    if (!feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCV()) {
        return;
    }

    auto lock = _acquireMutexUnconditionally();

    if (_executor) {
        _executor->shutdown();
        _executor->join();
    }

    _state.store(kDown);
}

BSONObj RangeDeleterService::dumpState() {
    auto lock = _acquireMutexUnconditionally();

    BSONObjBuilder builder;
    for (const auto& [collUUID, chunkRanges] : _rangeDeletionTasks) {
        BSONArrayBuilder subBuilder(builder.subarrayStart(collUUID.toString()));
        for (const auto& chunkRange : chunkRanges) {
            subBuilder.append(chunkRange->toBSON());
        }
    }
    return builder.obj();
}

long long RangeDeleterService::totalNumOfRegisteredTasks() {
    auto lock = _acquireMutexUnconditionally();

    long long counter = 0;
    for (const auto& [collUUID, ranges] : _rangeDeletionTasks) {
        counter += ranges.size();
    }
    return counter;
}

SharedSemiFuture<void> RangeDeleterService::registerTask(
    const RangeDeletionTask& rdt,
    SemiFuture<void>&& waitForActiveQueriesToComplete,
    bool fromResubmitOnStepUp) {

    if (disableResumableRangeDeleter.load()) {
        LOGV2_INFO(6872509,
                   "Not scheduling range deletion because `disableResumableRangeDeleter=true`");
        return SemiFuture<void>::makeReady(
                   Status(ErrorCodes::ResumableRangeDeleterDisabled,
                          "Not submitting any range deletion task because the "
                          "disableResumableRangeDeleter server parameter is set to true"))
            .share();
    }

    // Block the scheduling of the task while populating internal data structures
    SharedPromise<void> blockUntilRegistered;

    auto chainCompletionFuture =
        blockUntilRegistered.getFuture()
            .semi()
            .thenRunOn(_executor)
            .onError([serializedTask = rdt.toBSON()](Status errStatus) {
                // The above futures can only fail with those specific codes (futures notifying
                // the end of ongoing queries on a range will never be set to an error):
                // - 67635: the task was already previously scheduled
                // - BrokenPromise: the executor is shutting down
                // - Cancellation error: the node is shutting down or a stepdown happened
                if (errStatus.code() != 67635 && errStatus != ErrorCodes::BrokenPromise &&
                    !ErrorCodes::isCancellationError(errStatus)) {
                    LOGV2_ERROR(6784800,
                                "Range deletion scheduling failed with unexpected error",
                                "error"_attr = errStatus,
                                "rangeDeletion"_attr = serializedTask);
                }
                return errStatus;
            })
            .then([waitForOngoingQueries = std::move(waitForActiveQueriesToComplete).share()]() {
                return waitForOngoingQueries;
            })
            .then([this, when = rdt.getWhenToClean()]() {
                // Step 2: schedule wait for secondaries orphans cleanup delay
                const auto delayForActiveQueriesOnSecondariesToComplete =
                    when == CleanWhenEnum::kDelayed ? Seconds(orphanCleanupDelaySecs.load())
                                                    : Seconds(0);

                return sleepUntil(_executor,
                                  _executor->now() + delayForActiveQueriesOnSecondariesToComplete)
                    .share();
            })
            .then([this,
                   dbName = rdt.getNss().dbName(),
                   collectionUuid = rdt.getCollectionUuid(),
                   range = rdt.getRange(),
                   optKeyPattern = rdt.getKeyPattern()]() {
                return withTemporaryOperationContext(
                    [&](OperationContext* opCtx) {
                        // A task is considered completed when all the following conditions are met:
                        // - All orphans have been deleted
                        // - The deletions have been majority committed
                        // - The range deletion task document has been deleted
                        bool taskCompleted = false;

                        while (!taskCompleted) {
                            try {
                                // Perform the actual range deletion
                                bool orphansRemovalCompleted = false;
                                while (!orphansRemovalCompleted) {
                                    try {
                                        LOGV2_DEBUG(
                                            6872501,
                                            2,
                                            "Beginning deletion of documents in orphan range",
                                            "dbName"_attr = dbName,
                                            "collectionUUID"_attr = collectionUuid.toString(),
                                            "range"_attr = redact(range.toString()));

                                        auto shardKeyPattern =
                                            (optKeyPattern ? *optKeyPattern
                                                           : getShardKeyPattern(
                                                                 opCtx, dbName, collectionUuid));

                                        uassertStatusOK(
                                            deleteRangeInBatches(opCtx,
                                                                 dbName,
                                                                 collectionUuid,
                                                                 shardKeyPattern.toBSON(),
                                                                 range));
                                        orphansRemovalCompleted = true;
                                    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                                        // No orphaned documents to remove from a dropped collection
                                        orphansRemovalCompleted = true;
                                    } catch (
                                        ExceptionFor<
                                            ErrorCodes::
                                                RangeDeletionAbandonedBecauseTaskDocumentDoesNotExist>&) {
                                        // No orphaned documents to remove from a dropped collection
                                        orphansRemovalCompleted = true;
                                    } catch (
                                        ExceptionFor<
                                            ErrorCodes::
                                                RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist>&) {
                                        // The task can be considered completed because the range
                                        // deletion document doesn't exist
                                        orphansRemovalCompleted = true;
                                    } catch (const DBException& e) {
                                        LOGV2_ERROR(6872502,
                                                    "Failed to delete documents in orphan range",
                                                    "dbName"_attr = dbName,
                                                    "collectionUUID"_attr =
                                                        collectionUuid.toString(),
                                                    "range"_attr = redact(range.toString()),
                                                    "error"_attr = e);
                                        throw;
                                    }
                                }

                                {
                                    repl::ReplClientInfo::forClient(opCtx->getClient())
                                        .setLastOpToSystemLastOpTime(opCtx);
                                    auto clientOpTime =
                                        repl::ReplClientInfo::forClient(opCtx->getClient())
                                            .getLastOp();

                                    LOGV2_DEBUG(
                                        6872503,
                                        2,
                                        "Waiting for majority replication of local deletions",
                                        "dbName"_attr = dbName,
                                        "collectionUUID"_attr = collectionUuid,
                                        "range"_attr = redact(range.toString()),
                                        "clientOpTime"_attr = clientOpTime);

                                    // Synchronously wait for majority before removing the range
                                    // deletion task document: oplog gets applied in parallel for
                                    // different collections, so it's important not to apply
                                    // out of order the deletions of orphans and the removal of the
                                    // entry persisted in `config.rangeDeletions`
                                    WaitForMajorityService::get(opCtx->getServiceContext())
                                        .waitUntilMajority(clientOpTime,
                                                           CancellationToken::uncancelable())
                                        .get(opCtx);
                                }

                                // Remove persistent range deletion task
                                try {
                                    removePersistentRangeDeletionTask(opCtx, collectionUuid, range);

                                    LOGV2_DEBUG(
                                        6872504,
                                        2,
                                        "Completed removal of persistent range deletion task",
                                        "dbName"_attr = dbName,
                                        "collectionUUID"_attr = collectionUuid.toString(),
                                        "range"_attr = redact(range.toString()));

                                } catch (const DBException& e) {
                                    LOGV2_ERROR(6872505,
                                                "Failed to remove persistent range deletion task",
                                                "dbName"_attr = dbName,
                                                "collectionUUID"_attr = collectionUuid.toString(),
                                                "range"_attr = redact(range.toString()),
                                                "error"_attr = e);
                                    throw;
                                }
                            } catch (const DBException& e) {
                                // Fail in case of shutdown/stepdown errors as the range
                                // deletion will be resumed on the next step up
                                if (ErrorCodes::isShutdownError(e.code()) ||
                                    ErrorCodes::isNotPrimaryError(e.code())) {
                                    return e.toStatus();
                                }

                                // Iterate again in case of any other error
                                continue;
                            }

                            taskCompleted = true;
                        }

                        return Status::OK();
                    },
                    dbName,
                    collectionUuid,
                    true);
            })
            // IMPORTANT: no continuation should be added to this chain after this point
            // in order to make sure range deletions order is preserved.
            .semi()
            .share();

    auto [taskCompletionFuture, inserted] = [&]() -> std::pair<SharedSemiFuture<void>, bool> {
        auto lock = fromResubmitOnStepUp ? _acquireMutexUnconditionally()
                                         : _acquireMutexFailIfServiceNotUp();
        auto [registeredTask, inserted] = _rangeDeletionTasks[rdt.getCollectionUuid()].insert(
            std::make_shared<RangeDeletion>(RangeDeletion(rdt, chainCompletionFuture)));
        auto retFuture = static_cast<RangeDeletion*>(registeredTask->get())->getCompletionFuture();
        return {retFuture, inserted};
    }();

    if (inserted) {
        // The range deletion task has been registered, so the chain execution can be unblocked
        blockUntilRegistered.setFrom(Status::OK());
    } else {
        // Tried to register a duplicate range deletion task: invalidate the chain
        auto errStatus =
            Status(ErrorCodes::Error(67635), "Not scheduling duplicated range deletion");
        LOGV2_WARNING(6804200,
                      "Tried to register duplicate range deletion task. This results in a no-op.",
                      "collectionUUID"_attr = rdt.getCollectionUuid(),
                      "range"_attr = rdt.getRange());
        blockUntilRegistered.setFrom(errStatus);
    }

    return taskCompletionFuture;
}

void RangeDeleterService::deregisterTask(const UUID& collUUID, const ChunkRange& range) {
    auto lock = _acquireMutexFailIfServiceNotUp();
    _rangeDeletionTasks[collUUID].erase(std::make_shared<ChunkRange>(range));
    if (_rangeDeletionTasks[collUUID].size() == 0) {
        _rangeDeletionTasks.erase(collUUID);
    }
}

int RangeDeleterService::getNumRangeDeletionTasksForCollection(const UUID& collectionUUID) {
    auto lock = _acquireMutexFailIfServiceNotUp();
    auto tasksSet = _rangeDeletionTasks.find(collectionUUID);
    if (tasksSet == _rangeDeletionTasks.end()) {
        return 0;
    }
    return tasksSet->second.size();
}

SharedSemiFuture<void> RangeDeleterService::getOverlappingRangeDeletionsFuture(
    const UUID& collectionUUID, const ChunkRange& range) {

    if (disableResumableRangeDeleter.load()) {
        return SemiFuture<void>::makeReady(
                   Status(ErrorCodes::ResumableRangeDeleterDisabled,
                          "Not submitting any range deletion task because the "
                          "disableResumableRangeDeleter server parameter is set to true"))
            .share();
    }

    auto lock = _acquireMutexFailIfServiceNotUp();

    auto mapEntry = _rangeDeletionTasks.find(collectionUUID);
    if (mapEntry == _rangeDeletionTasks.end() || mapEntry->second.size() == 0) {
        // No tasks scheduled for the specified collection
        return SemiFuture<void>::makeReady().share();
    }

    std::vector<ExecutorFuture<void>> overlappingRangeDeletionsFutures;

    auto& rangeDeletions = mapEntry->second;
    const auto rangeSharedPtr = std::make_shared<ChunkRange>(range);
    auto forwardIt = rangeDeletions.lower_bound(rangeSharedPtr);
    if (forwardIt != rangeDeletions.begin()) {
        forwardIt--;
    }

    while (forwardIt != rangeDeletions.end() && forwardIt->get()->overlapWith(range)) {
        auto future = static_cast<RangeDeletion*>(forwardIt->get())->getCompletionFuture();
        // Scheduling wait on the current executor so that it gets invalidated on step-down
        overlappingRangeDeletionsFutures.push_back(future.thenRunOn(_executor));
        forwardIt++;
    }

    if (overlappingRangeDeletionsFutures.size() == 0) {
        return SemiFuture<void>::makeReady().share();
    }
    return whenAllSucceed(std::move(overlappingRangeDeletionsFutures)).share();
}

}  // namespace mongo

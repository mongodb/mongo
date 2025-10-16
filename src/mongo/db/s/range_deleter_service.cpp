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

#include "mongo/base/checked_cast.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/range_deleter_service_op_observer.h"
#include "mongo/db/s/range_deletion.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <iterator>
#include <tuple>
#include <type_traits>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {
namespace {
const auto rangeDeleterServiceDecorator = ServiceContext::declareDecoration<RangeDeleterService>();

const Seconds kCheckForEnabledServiceInterval(10);
const Seconds kMissingIndexRetryInterval(10);

BSONObj getShardKeyPattern(OperationContext* opCtx,
                           const DatabaseName& dbName,
                           const UUID& collectionUuid) {
    while (true) {
        opCtx->checkForInterrupt();
        boost::optional<NamespaceString> optNss;
        {
            AutoGetCollection collection(
                opCtx, NamespaceStringOrUUID{dbName, collectionUuid}, MODE_IS);

            auto optMetadata = CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(
                                   opCtx, collection.getNss())
                                   ->getCurrentMetadataIfKnown();
            if (optMetadata && optMetadata->isSharded()) {
                return optMetadata->getShardKeyPattern().toBSON();
            }
            optNss = collection.getNss();
        }

        FilteringMetadataCache::get(opCtx)
            ->onCollectionPlacementVersionMismatch(opCtx, *optNss, boost::none)
            .ignore();
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

RangeDeleterService::ReadyRangeDeletionsProcessor::ReadyRangeDeletionsProcessor(
    OperationContext* opCtx, std::shared_ptr<executor::TaskExecutor> executor)
    : _service(opCtx->getServiceContext()),
      _thread([this] { _runRangeDeletions(); }),
      _executor(executor) {}

RangeDeleterService::ReadyRangeDeletionsProcessor::~ReadyRangeDeletionsProcessor() {
    shutdown();
    invariant(_thread.joinable());
    _thread.join();
    invariant(!_threadOpCtxHolder,
              "Thread operation context is still alive after joining main thread");
}

void RangeDeleterService::ReadyRangeDeletionsProcessor::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (_state == kStopped)
        return;

    _state = kStopped;

    if (_threadOpCtxHolder) {
        stdx::lock_guard<Client> scopedClientLock(*_threadOpCtxHolder->getClient());
        _threadOpCtxHolder->markKilled(ErrorCodes::Interrupted);
    }
}

bool RangeDeleterService::ReadyRangeDeletionsProcessor::_stopRequested() const {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    return _state == kStopped;
}

void RangeDeleterService::ReadyRangeDeletionsProcessor::emplaceRangeDeletion(
    const RangeDeletionTask& rdt) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (_state != kRunning) {
        return;
    }
    _queue.push(rdt);
    _condVar.notify_all();
}

void RangeDeleterService::ReadyRangeDeletionsProcessor::_completedRangeDeletion() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    dassert(!_queue.empty());
    _queue.pop();
}

void RangeDeleterService::ReadyRangeDeletionsProcessor::_runRangeDeletions() {
    ThreadClient threadClient(rangedeletionutil::kRangeDeletionThreadName,
                              _service->getService(ClusterRole::ShardServer));

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state != kRunning) {
            return;
        }
        _threadOpCtxHolder = cc().makeOperationContext();
    }

    auto opCtx = _threadOpCtxHolder.get();

    ON_BLOCK_EXIT([this]() {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _threadOpCtxHolder.reset();
    });

    while (!_stopRequested()) {
        {
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            try {
                opCtx->waitForConditionOrInterrupt(_condVar, lock, [&] { return !_queue.empty(); });
            } catch (const DBException& ex) {
                dassert(!opCtx->checkForInterruptNoAssert().isOK(),
                        str::stream() << "Range deleter thread failed with unexpected exception "
                                      << ex.toStatus());
                break;
            }
        }

        // Once passing this check, the range deletion will be processed without being halted, even
        // if the range deleter gets disabled halfway through.
        if (RangeDeleterService::get(opCtx)->isDisabled()) {
            MONGO_IDLE_THREAD_BLOCK;
            sleepFor(kCheckForEnabledServiceInterval);
            continue;
        }

        auto task = _queue.front();
        const auto dbName = task.getNss().dbName();
        const auto collectionUuid = task.getCollectionUuid();
        const auto range = task.getRange();
        const auto optKeyPattern = task.getKeyPattern();

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
                        NamespaceString nss;
                        {
                            AutoGetCollection collection(
                                opCtx, NamespaceStringOrUUID{dbName, collectionUuid}, MODE_IS);
                            // It's possible for the namespace to become outdated if a concurrent
                            // rename of collection occurs, because rangeDeletion is not
                            // synchronized with DDL operations. We are using the nss variable
                            // solely for logging purposes.
                            nss = collection.getNss();
                        }
                        LOGV2_INFO(6872501,
                                   "Beginning deletion of documents in orphan range",
                                   "namespace"_attr = nss,
                                   "collectionUUID"_attr = collectionUuid.toString(),
                                   "range"_attr = redact(range.toString()));

                        auto shardKeyPattern =
                            (optKeyPattern ? (*optKeyPattern).toBSON()
                                           : getShardKeyPattern(opCtx, dbName, collectionUuid));

                        auto numDocsAndBytesDeleted =
                            uassertStatusOK(rangedeletionutil::deleteRangeInBatches(
                                opCtx, dbName, collectionUuid, shardKeyPattern, range));
                        LOGV2_INFO(9239400,
                                   "Finished deletion of documents in orphan range",
                                   "namespace"_attr = nss,
                                   "collectionUUID"_attr = collectionUuid.toString(),
                                   "range"_attr = redact(range.toString()),
                                   "docsDeleted"_attr = numDocsAndBytesDeleted.first,
                                   "bytesDeleted"_attr = numDocsAndBytesDeleted.second);
                        orphansRemovalCompleted = true;
                    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                        // No orphaned documents to remove from a dropped collection
                        orphansRemovalCompleted = true;
                    } catch (ExceptionFor<
                             ErrorCodes::RangeDeletionAbandonedBecauseTaskDocumentDoesNotExist>&) {
                        // No orphaned documents to remove from a dropped collection
                        orphansRemovalCompleted = true;
                    } catch (ExceptionFor<
                             ErrorCodes::
                                 RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist>&) {
                        // The task can be considered completed because the range
                        // deletion document doesn't exist
                        orphansRemovalCompleted = true;
                    } catch (const DBException& e) {
                        if (e.code() != ErrorCodes::IndexNotFound) {
                            // It is expected that we reschedule the range deletion task to the
                            // bottom of the queue if the index is missing and do not need to log
                            // this message.
                            LOGV2_ERROR(6872502,
                                        "Failed to delete documents in orphan range",
                                        "dbName"_attr = dbName,
                                        "collectionUUID"_attr = collectionUuid.toString(),
                                        "range"_attr = redact(range.toString()),
                                        "error"_attr = e);
                        }
                        throw;
                    }
                }

                {
                    repl::ReplClientInfo::forClient(opCtx->getClient())
                        .setLastOpToSystemLastOpTime(opCtx);
                    auto clientOpTime =
                        repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

                    LOGV2_DEBUG(6872503,
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
                        .waitUntilMajorityForWrite(clientOpTime, CancellationToken::uncancelable())
                        .get(opCtx);
                }

                // Remove persistent range deletion task
                try {
                    auto* self = RangeDeleterService::get(opCtx);
                    auto task = self->completeTask(collectionUuid, range);
                    if (task) {
                        rangedeletionutil::removePersistentTask(opCtx, task->getTaskId());
                    }

                    LOGV2_DEBUG(6872504,
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
            } catch (const ExceptionFor<ErrorCodes::IndexNotFound>&) {
                // We cannot complete this range deletion right now because we do not have an index
                // built on the shard key. This situation is expected for a hashed shard key and
                // recoverable for a range shard key. This index may be rebuilt in the future, so
                // reschedule the task at the end of the queue.
                _completedRangeDeletion();

                sleepFor(_executor, kMissingIndexRetryInterval)
                    .getAsync([this, task](Status status) {
                        if (!status.isOK()) {
                            LOGV2_WARNING(9962300,
                                          "Encountered an error while retrying a range deletion "
                                          "task that previously failed due to missing index",
                                          "status"_attr = status,
                                          "task"_attr = task.toBSON());
                            return;
                        }

                        emplaceRangeDeletion(task);
                    });

                break;
            } catch (const DBException&) {
                // Release the thread only in case the operation context has been interrupted, as
                // interruption only happens on shutdown/stepdown (this is fine because range
                // deletions will be resumed on the next step up)
                if (_stopRequested()) {
                    break;
                }

                // Iterate again in case of any other error
                continue;
            }

            taskCompleted = true;
            _completedRangeDeletion();
        }
    }
}

void RangeDeleterService::onStartup(OperationContext* opCtx) {
    auto opObserverRegistry =
        checked_cast<OpObserverRegistry*>(opCtx->getServiceContext()->getOpObserver());
    opObserverRegistry->addObserver(std::make_unique<RangeDeleterServiceOpObserver>());
}

void RangeDeleterService::onStepUpComplete(OperationContext* opCtx, long long term) {
    // Wait until all tasks and thread from previous term drain
    _joinAndResetState();

    auto lock = _acquireMutexUnconditionally();
    dassert(_state == kDown, "Service expected to be down before stepping up");

    _state = kReadyForInitialization;

    const std::string kExecName("RangeDeleterServiceExecutor");
    auto net = executor::makeNetworkInterface(kExecName);
    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
    auto taskExecutor = executor::ThreadPoolTaskExecutor::create(std::move(pool), std::move(net));
    _executor = std::move(taskExecutor);
    _executor->startup();

    // Initialize the range deletion processor to allow enqueueing ready task
    _readyRangeDeletionsProcessorPtr =
        std::make_unique<ReadyRangeDeletionsProcessor>(opCtx, _executor);

    _recoverRangeDeletionsOnStepUp(opCtx);
}

bool RangeDeleterService::isDisabled() {
    return disableResumableRangeDeleter.load();
}

void RangeDeleterService::_recoverRangeDeletionsOnStepUp(OperationContext* opCtx) {
    _stepUpCompletedFuture =
        ExecutorFuture<void>(_executor)
            .then([serviceContext = opCtx->getServiceContext(), this] {
                ThreadClient tc("ResubmitRangeDeletionsOnStepUp",
                                serviceContext->getService(ClusterRole::ShardServer));

                {
                    auto lock = _acquireMutexUnconditionally();
                    if (_state != kReadyForInitialization) {
                        return;
                    }
                    _state = kInitializing;
                    _initOpCtxHolder = tc->makeOperationContext();
                }

                ON_BLOCK_EXIT([this] {
                    auto lock = _acquireMutexUnconditionally();
                    _initOpCtxHolder.reset();
                });

                auto opCtx{_initOpCtxHolder.get()};

                LOGV2(6834800, "Resubmitting range deletion tasks");

                // The Scoped lock is needed to serialize with concurrent range deletions
                ScopedRangeDeleterLock rangeDeleterLock(opCtx, MODE_S);
                // The collection lock is needed to serialize with migrations trying to
                // schedule range deletions by updating the 'pending' field
                AutoGetCollection collRangeDeletionLock(
                    opCtx, NamespaceString::kRangeDeletionNamespace, MODE_S);

                DBDirectClient client(opCtx);
                int nRescheduledTasks = 0;

                // (1) register range deletion tasks marked as "processing"
                auto processingTasksCompletionFuture = [&] {
                    std::vector<ExecutorFuture<void>> processingTasksCompletionFutures;
                    FindCommandRequest findCommand(NamespaceString::kRangeDeletionNamespace);
                    findCommand.setFilter(BSON(RangeDeletionTask::kProcessingFieldName << true));
                    auto cursor = client.find(std::move(findCommand));

                    while (cursor->more()) {
                        auto completionFuture = this->registerTask(
                            RangeDeletionTask::parse(cursor->next(),
                                                     IDLParserContext("rangeDeletionRecovery")),
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
                                      "numRangeDeletionsMarkedAsProcessing"_attr =
                                          nRescheduledTasks);
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
                            RangeDeletionTask::parse(cursor->next(),
                                                     IDLParserContext("rangeDeletionRecovery")),
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
                if (_state != kDown) {
                    this->_state = kUp;
                }
            })
            .share();
}

void RangeDeleterService::_joinAndResetState() {
    invariant(_state == kDown);
    // Join the thread spawned on step-up to resume range deletions
    _stepUpCompletedFuture.getNoThrow().ignore();

    // Join and destruct the executor
    if (_executor) {
        _executor->join();
        _executor.reset();
    }

    // Join and destruct the processor
    _readyRangeDeletionsProcessorPtr.reset();

    // Clear range deletions potentially created during recovery
    _rangeDeletionTasks.clear();
}

void RangeDeleterService::_stopService() {
    auto lock = _acquireMutexUnconditionally();
    if (_state == kDown)
        return;

    _state = kDown;
    if (_initOpCtxHolder) {
        stdx::lock_guard<Client> lk(*_initOpCtxHolder->getClient());
        _initOpCtxHolder->markKilled(ErrorCodes::Interrupted);
    }

    if (_executor) {
        _executor->shutdown();
    }

    // Shutdown the range deletion processor to interrupt range deletions
    if (_readyRangeDeletionsProcessorPtr) {
        _readyRangeDeletionsProcessorPtr->shutdown();
    }

    // Clear range deletion tasks map in order to notify potential waiters on completion futures
    _rangeDeletionTasks.clear();
}

void RangeDeleterService::onStepDown() {
    _stopService();
}

void RangeDeleterService::onShutdown() {
    _stopService();
    _joinAndResetState();
}

BSONObj RangeDeleterService::dumpState() {
    auto lock = _acquireMutexFailIfServiceNotUp();
    return _rangeDeletionTasks.getAllTasksBSON();
}

long long RangeDeleterService::totalNumOfRegisteredTasks() {
    auto lock = _acquireMutexFailIfServiceNotUp();
    return _rangeDeletionTasks.getTaskCount();
}

SharedSemiFuture<void> RangeDeleterService::registerTask(
    const RangeDeletionTask& rdt,
    SemiFuture<void>&& waitForActiveQueriesToComplete,
    bool fromResubmitOnStepUp,
    bool pending) {
    auto scheduleRangeDeletionChain = [&](SharedSemiFuture<void> pendingFuture) {
        (void)pendingFuture.thenRunOn(_executor)
            .then([this,
                   waitForOngoingQueries = std::move(waitForActiveQueriesToComplete).share()]() {
                // Step 1: wait for ongoing queries retaining the range to drain
                return waitForOngoingQueries;
            })
            .then([this,
                   collectionUUID = rdt.getCollectionUuid(),
                   range = rdt.getRange(),
                   when = rdt.getWhenToClean()]() {
                LOGV2_DEBUG(7536601,
                            2,
                            "Finished waiting for ongoing queries for range deletion task",
                            "collectionUUID"_attr = collectionUUID,
                            "range"_attr = redact(range.toString()));

                // Step 2: schedule wait for secondaries orphans cleanup delay
                const auto delayForActiveQueriesOnSecondariesToComplete =
                    when == CleanWhenEnum::kDelayed ? Seconds(orphanCleanupDelaySecs.load())
                                                    : Seconds(0);

                return sleepUntil(_executor,
                                  _executor->now() + delayForActiveQueriesOnSecondariesToComplete)
                    .share();
            })
            .then([this, rdt = rdt]() {
                // Step 3: schedule the actual range deletion task
                auto lock = _acquireMutexUnconditionally();
                if (_state != kDown) {
                    LOGV2_DEBUG(7536602,
                                2,
                                "Scheduling range deletion task",
                                "collectionUUID"_attr = rdt.getCollectionUuid(),
                                "range"_attr = redact(rdt.getRange().toString()));

                    invariant(_readyRangeDeletionsProcessorPtr,
                              "The range deletions processor is not initialized");
                    _readyRangeDeletionsProcessorPtr->emplaceRangeDeletion(rdt);
                }
            });
    };

    auto lock =
        fromResubmitOnStepUp ? _acquireMutexUnconditionally() : _acquireMutexFailIfServiceNotUp();

    LOGV2_DEBUG(7536600,
                2,
                "Registering range deletion task",
                "collectionUUID"_attr = rdt.getCollectionUuid(),
                "range"_attr = redact(rdt.getRange().toString()),
                "pending"_attr = pending);

    auto [task, registrationResult] = _rangeDeletionTasks.registerTask(rdt);

    // Register the task on the service only once, duplicate registrations will join.
    if (registrationResult == RangeDeletionTaskTracker::kRegisteredNewTask) {
        scheduleRangeDeletionChain(task->getPendingFuture());
    }

    // Allow future chain to progress in case the task is flagged as non-pending
    if (!pending) {
        task->clearPending();
    }

    return task->getCompletionFuture();
}

std::shared_ptr<RangeDeletion> RangeDeleterService::completeTask(const UUID& collUUID,
                                                                 const ChunkRange& range) {
    auto lock = _acquireMutexFailIfServiceNotUp();
    auto task = _rangeDeletionTasks.removeTask(collUUID, range);
    if (task) {
        task->markComplete();
    }
    return task;
}

int RangeDeleterService::getNumRangeDeletionTasksForCollection(const UUID& collectionUUID) {
    auto lock = _acquireMutexFailIfServiceNotUp();
    return _rangeDeletionTasks.getTaskCountForCollection(collectionUUID);
}

SharedSemiFuture<void> RangeDeleterService::getOverlappingRangeDeletionsFuture(
    const UUID& collectionUUID, const ChunkRange& range) {
    if (isDisabled()) {
        return SemiFuture<void>::makeReady(
                   Status(ErrorCodes::ResumableRangeDeleterDisabled,
                          "Not waiting for overlapping range deletion tasks because the resumable "
                          "range deleter is disabled"))
            .share();
    }

    auto lock = _acquireMutexFailIfServiceNotUp();

    auto tasks = _rangeDeletionTasks.getOverlappingTasks(collectionUUID, range);
    if (tasks.empty()) {
        return SemiFuture<void>::makeReady().share();
    }

    std::vector<ExecutorFuture<void>> futures;
    for (const auto& [_, task] : tasks) {
        futures.emplace_back(task->getCompletionFuture().thenRunOn(_executor));
    }
    return whenAllSucceed(std::move(futures)).share();
}

}  // namespace mongo

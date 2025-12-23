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
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/range_deleter_service_op_observer.h"
#include "mongo/db/s/range_deletion.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/shard_role/transaction_resources.h"
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

void resetTermScopedPromise(WithLock,
                            boost::optional<SharedPromise<void>>& promise,
                            StringData message) {
    if (promise.has_value() && !promise->getFuture().isReady()) {
        promise->setError({ErrorCodes::PrimarySteppedDown, message});
    }
    promise = boost::none;
}

void ensureSet(WithLock, SharedPromise<void>& promise) {
    if (!promise.getFuture().isReady()) {
        promise.emplaceValue();
    }
}

}  // namespace

const ReplicaSetAwareServiceRegistry::Registerer<RangeDeleterService>
    rangeDeleterServiceRegistryRegisterer("RangeDeleterService",
                                          {"ShardingInitializationMongoDRegistry"});

RangeDeleterService* RangeDeleterService::get(ServiceContext* serviceContext) {
    return &rangeDeleterServiceDecorator(serviceContext);
}

RangeDeleterService* RangeDeleterService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void RangeDeleterService::onStartup(OperationContext* opCtx) {
    auto opObserverRegistry =
        checked_cast<OpObserverRegistry*>(opCtx->getServiceContext()->getOpObserver());
    opObserverRegistry->addObserver(std::make_unique<RangeDeleterServiceOpObserver>());
}

void RangeDeleterService::onStepUpBegin(OperationContext* opCtx, long long term) {
    registerRecoveryJob(term);

    auto lock = _acquireMutexUnconditionally();
    _termInitializationPromise.emplace();
    _serviceUpPromise.emplace();
}

void RangeDeleterService::onStepUpComplete(OperationContext* opCtx, long long term) {
    // Wait until all tasks and thread from previous term drain
    _joinAndResetState();
    _activeTerm = _recoveryState.notifyStartOfTerm(term);

    auto lock = _acquireMutexUnconditionally();
    dassert(_state == kDown, "Service expected to be down before stepping up");

    _state = kReadyForInitialization;

    const std::string kExecName("RangeDeleterServiceExecutor");
    auto net = executor::makeNetworkInterface(kExecName);
    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
    auto taskExecutor = executor::ThreadPoolTaskExecutor::create(std::move(pool), std::move(net));
    _executor = std::move(taskExecutor);
    _executor->startup();

    // Initialize the range deletion processor to allow enqueueing ready task.
    _readyRangeDeletionsProcessorPtr =
        std::make_unique<ReadyRangeDeletionsProcessor>(opCtx, _executor);

    _recoveryState.getRecoveryFuture(term)
        .thenRunOn(_executor)
        .then([this, term](RangeDeletionRecoveryTracker::Outcome outcome) {
            LOGV2_INFO(11079601,
                       "Range deleter service task recovery finished",
                       "term"_attr = term,
                       "outcome"_attr = outcome);
            auto lock = _acquireMutexUnconditionally();
            // Since the recovery is only spawned on step-up but may complete later, it's not
            // guaranteed that the node is still primary when the all resubmissions finish.
            if (_state != kDown) {
                _state = kUp;
                LOGV2_INFO(11079600, "Range deleter service is now up", "term"_attr = term);
                _readyRangeDeletionsProcessorPtr->beginProcessing();
                if (_serviceUpPromise.has_value()) {
                    ensureSet(lock, *_serviceUpPromise);
                }
            }
        })
        .getAsync([](auto) {});

    ensureSet(lock, *_termInitializationPromise);

    _launchRangeDeletionRecoveryTask(opCtx, term);
}

bool RangeDeleterService::isDisabled() {
    return disableResumableRangeDeleter.load();
}

void RangeDeleterService::_launchRangeDeletionRecoveryTask(OperationContext* opCtx,
                                                           long long term) {
    ExecutorFuture<void>(_executor)
        .then([serviceContext = opCtx->getServiceContext(), this, term] {
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
            FindCommandRequest findCommand(NamespaceString::kRangeDeletionNamespace);
            findCommand.setFilter(BSON(RangeDeletionTask::kProcessingFieldName << true));
            auto cursor = client.find(std::move(findCommand));

            while (cursor->more()) {
                (void)this->registerTask(
                    RangeDeletionTask::parse(cursor->next(),
                                             IDLParserContext("rangeDeletionRecovery")),
                    SemiFuture<void>::makeReady());
                nRescheduledTasks++;
            }

            if (nRescheduledTasks > 1) {
                LOGV2_WARNING(6834801,
                              "Rescheduling several range deletions marked as processing. "
                              "Orphans count may be off while they are not drained",
                              "numRangeDeletionsMarkedAsProcessing"_attr = nRescheduledTasks);
            }

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
                        SemiFuture<void>::makeReady());
                }
            }

            LOGV2_INFO(6834802,
                       "Finished resubmitting range deletion tasks",
                       "nRescheduledTasks"_attr = nRescheduledTasks);
            notifyRecoveryJobComplete(term);
        })
        .getAsync([](auto) {});
}

void RangeDeleterService::_joinAndResetState() {
    invariant(_state == kDown);

    // Join and destruct the executor
    if (_executor) {
        _executor->join();
        _executor.reset();
    }

    // Join and destruct the processor
    _readyRangeDeletionsProcessorPtr.reset();

    // Clear range deletions potentially created during recovery.
    {
        auto lock = _acquireMutexUnconditionally();
        _rangeDeletionTasks.clear();
    }
}

void RangeDeleterService::_stopService() {
    auto lock = _acquireMutexUnconditionally();

    resetTermScopedPromise(
        lock, _termInitializationPromise, "Term ended before RangeDeleterService could initialize");
    resetTermScopedPromise(
        lock, _serviceUpPromise, "Term ended before RangeDeleterService could be brought up");

    if (_state == kDown)
        return;

    _state = kDown;
    _activeTerm.reset();

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

SemiFuture<void> RangeDeleterService::getTermInitializationFuture() {
    auto lock = _acquireMutexUnconditionally();
    return _getTermInitializationFuture(lock);
}

SemiFuture<void> RangeDeleterService::_getTermInitializationFuture(WithLock) {
    uassert(ErrorCodes::NotWritablePrimary,
            "RangeDeleterService is not initializing because this node is not primary",
            _termInitializationPromise.has_value());
    return _termInitializationPromise->getFuture().semi();
}

SemiFuture<void> RangeDeleterService::getServiceUpFuture() {
    auto lock = _acquireMutexUnconditionally();
    uassert(ErrorCodes::NotWritablePrimary,
            "RangeDeleterService is not recovering because this node is not primary",
            _serviceUpPromise.has_value());
    return _serviceUpPromise->getFuture().semi();
}

void RangeDeleterService::registerRecoveryJob(long long term) {
    _recoveryState.registerRecoveryJob(term);
}
void RangeDeleterService::notifyRecoveryJobComplete(long long term) {
    _recoveryState.notifyRecoveryJobComplete(term);
}

SharedSemiFuture<void> RangeDeleterService::registerTask(
    const RangeDeletionTask& rdt,
    SemiFuture<void>&& waitForActiveQueriesToComplete,
    TaskPending pending) {

    auto scheduleRangeDeletionChain = [&](SharedSemiFuture<void> pendingFuture) {
        (void)pendingFuture.thenRunOn(_executor)
            .then([this]() {
                // Wait for all recovery tasks to be registered first.
                return getServiceUpFuture();
            })
            .then([this,
                   collectionUuid = rdt.getCollectionUuid(),
                   range = rdt.getRange(),
                   registrationTime = rdt.getTimestamp().value_or(
                       Timestamp(getGlobalServiceContext()->getFastClockSource()->now())),
                   taskId = rdt.getId()]() {
                // Acquire lock to safely access the range deletion tasks tracker.
                auto lock = _acquireMutexUnconditionally();
                auto overlappingTasks =
                    _rangeDeletionTasks.getOverlappingTasks(collectionUuid, range);

                std::vector<ExecutorFuture<void>> futures;
                for (const auto& [_, task] : overlappingTasks) {
                    // The current task is now in the map since we call
                    // getOverlappingTasks() after registration. We do not want to wait on
                    // ourselves, so skip.
                    if (task->getTaskId() == taskId) {
                        continue;
                    }
                    if ((task->getRegistrationTime() < registrationTime) ||
                        (task->getRegistrationTime() == registrationTime &&
                         taskId < task->getTaskId())) {
                        futures.emplace_back(task->getCompletionFuture().thenRunOn(_executor));
                    }
                }
                // We want to wait for all overlapping range deletion tasks to finish before
                // proceeding with this task. This is because the range deleter service assumes that
                // there are no overlapping range deletion tasks and attempting to union tasks or
                // splice incoming tasks could lead to unexpected behavior with the current
                // implementation.
                if (futures.empty()) {
                    return SemiFuture<std::vector<Status>>::makeReady(std::vector<Status>{});
                }
                return whenAll(std::move(futures));
            })
            .then([this, waitForOngoingQueries = std::move(waitForActiveQueriesToComplete).share()](
                      std::vector<Status> /*statuses*/) {
                // We do not care about the statuses of the overlapping tasks we waited on.
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

    auto lock = _acquireMutexUnconditionally();
    uassert(ErrorCodes::NotYetInitialized,
            "RangeDeleterService is not yet initialized for the current term",
            _getTermInitializationFuture(lock).isReady());

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
    if (pending == TaskPending::kNotPending) {
        task->clearPending();
    }

    if (isDisabled()) {
        return SemiFuture<void>::makeReady(
                   Status(ErrorCodes::ResumableRangeDeleterDisabled,
                          "Not waiting to complete the range deletion task because the resumable "
                          "range deleter is disabled"))
            .share();
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

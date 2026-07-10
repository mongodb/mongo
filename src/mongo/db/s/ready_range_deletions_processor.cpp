/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/ready_range_deletions_processor.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/concurrency/idle_thread_block.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
constexpr auto kRangeDeletionThreadName = "range-deleter"sv;
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
            ->onShardVersionMismatch(opCtx, *optNss, boost::none)
            .ignore();
        continue;
    }
}

admission::execution_control::ScopedTicketAdmissionStatsRecorder::OnUpdateFn
makeRangeDeleterTicketStatsUpdater(ShardingStatistics& shardingStats) {
    return [&shardingStats](const admission::execution_control::TicketAdmissionStats& delta) {
        shardingStats.rangeDeleterTicketAdmissions.fetchAndAddRelaxed(delta.admissions);
        shardingStats.rangeDeleterLowPriorityTicketAdmissions.fetchAndAddRelaxed(
            delta.lowPriorityAdmissions);
        if (delta.startedQueueing || delta.finishedQueueing) {
            shardingStats.rangeDeleterTicketQueueTime.onCountChange(delta.startedQueueing -
                                                                    delta.finishedQueueing);
        }
        if (delta.admissions || delta.releases) {
            shardingStats.rangeDeleterTicketProcessingTime.onCountChange(delta.admissions -
                                                                         delta.releases);
        }
    };
}
}  // namespace

ReadyRangeDeletionsProcessor::ReadyRangeDeletionsProcessor(
    OperationContext* opCtx, std::shared_ptr<executor::TaskExecutor> executor)
    : _service(opCtx->getServiceContext()),
      _thread([this] {
          if (!_beginProcessingSignal.getFuture().getNoThrow().isOK()) {
              return;
          }
          _runRangeDeletions();
      }),
      _executor(executor) {}

ReadyRangeDeletionsProcessor::~ReadyRangeDeletionsProcessor() {
    shutdown();
    invariant(_thread.joinable());
    _thread.join();
    invariant(!_threadOpCtxHolder,
              "Thread operation context is still alive after joining main thread");
}

void ReadyRangeDeletionsProcessor::beginProcessing() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_beginProcessingSignal.getFuture().isReady()) {
        _beginProcessingSignal.emplaceValue();
    }
}

void ReadyRangeDeletionsProcessor::shutdown() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_state == kStopped)
        return;
    _transitionState(lock, kStopped);

    if (!_beginProcessingSignal.getFuture().isReady()) {
        _beginProcessingSignal.setError(Status{ErrorCodes::InterruptedAtShutdown,
                                               "ReadyRangeDeletionsProcessor is shutting down"});
    }

    if (_threadOpCtxHolder) {
        std::lock_guard<Client> scopedClientLock(*_threadOpCtxHolder->getClient());
        _threadOpCtxHolder->markKilled(ErrorCodes::Interrupted);
    }

    if (_batchOpCtx) {
        std::lock_guard<Client> scopedClientLock(*_batchOpCtx->getClient());
        _batchOpCtx->markKilled(ErrorCodes::Interrupted);
    }
}

bool ReadyRangeDeletionsProcessor::_stopRequested() const {
    std::unique_lock<std::mutex> lock(_mutex);
    return _state == kStopped;
}

void ReadyRangeDeletionsProcessor::_transitionState(WithLock, State newState) {
    if (_state == newState) {
        return;
    }
    if (!_validateStateTransition(_state, newState)) {
        return;
    }
    LOGV2(11420000,
          "ReadyRangeDeletionsProcessor transitioned state",
          "oldState"_attr = _state,
          "newState"_attr = newState);
    _state = newState;
}

bool ReadyRangeDeletionsProcessor::_validateStateTransition(State oldState, State newState) const {
    try {
        tassert(11420001,
                "Invalid state transition requested in ReadyRangeDeletionsProcessor",
                _isStateTransitionValid(oldState, newState));
        return true;
    } catch (const AssertionException&) {
        return false;
    }
}

bool ReadyRangeDeletionsProcessor::_isStateTransitionValid(State oldState, State newState) const {
    switch (oldState) {
        case kInitializing:
            return newState == kRunning || newState == kStopped;
        case kRunning:
            return newState == kStopped;
        case kStopped:
            return false;
    }
    MONGO_UNREACHABLE;
}

void ReadyRangeDeletionsProcessor::emplaceRangeDeletion(const RangeDeletionTask& rdt) {
    std::unique_lock<std::mutex> lock(_mutex);
    if (_state == kStopped) {
        return;
    }
    _queue.push(rdt);
    _condVar.notify_all();
}

void ReadyRangeDeletionsProcessor::_completedRangeDeletion() {
    std::unique_lock<std::mutex> lock(_mutex);
    tassert(ErrorCodes::InternalError,
            "Queue cannot be empty while popping queue element",
            !_queue.empty());
    _queue.pop();
}

RangeDeletionTask ReadyRangeDeletionsProcessor::_peekFront() const {
    std::unique_lock<std::mutex> lock(_mutex);
    tassert(ErrorCodes::InternalError,
            "Queue cannot be empty while peeking at front element",
            !_queue.empty());
    return _queue.front();
}

// TODO: SERVER-123812 refactor connection between range deleter and resharding
bool ReadyRangeDeletionsProcessor::_shouldDeferRangeDeletionForResharding(NamespaceString nss,
                                                                          OperationContext* opCtx) {
    auto ff = resharding::gFeatureFlagReshardingRegistry.isEnabled();
    auto rsOp = LocalReshardingOperationsRegistry::get().getOperation(nss).has_value();

    return ff && rsOp;
}

void ReadyRangeDeletionsProcessor::_rescheduleRangeDeletion(const RangeDeletionTask& task,
                                                            Seconds delay,
                                                            std::string_view reason) {
    _completedRangeDeletion();

    sleepFor(_executor, delay).getAsync([this, task, reason](Status status) {
        if (!status.isOK()) {
            LOGV2_WARNING(9962300,
                          "Encountered an error while rescheduling a range deletion task",
                          "reason"_attr = reason,
                          "status"_attr = status,
                          "task"_attr = task.toBSON());
            // We don't need to re-emplace the range deletion task if the executor shuts down
            // because the task is still persisted to disk. When the next executor for
            // RangeDeleterService starts up, it wil reschedule tasks based on the disk
            return;
        }

        emplaceRangeDeletion(task);
    });
}

void ReadyRangeDeletionsProcessor::_runRangeDeletions() {
    ThreadClient threadClient(kRangeDeletionThreadName, _service->getService());

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_state != kInitializing) {
            return;
        }
        _transitionState(lock, kRunning);
        _threadOpCtxHolder = cc().makeOperationContext();
    }

    auto opCtx = _threadOpCtxHolder.get();

    ON_BLOCK_EXIT([this]() {
        std::lock_guard<std::mutex> lock(_mutex);
        _threadOpCtxHolder.reset();
    });

    HierarchicalCancelableOperationContextFactory opCtxFactory(opCtx->getCancellationToken(),
                                                               _executor);

    // The MaxKey orphan guard's blocked set must be populated before any task is deleted (see
    // RangeDeleterService::classifyBlockedMaxKeyTasks). Classify lazily below, after the disabled
    // gate and before processing the first task, retrying on transient failure.
    bool classifiedBlockedMaxKeyTasks = false;

    while (!_stopRequested()) {
        {
            std::unique_lock<std::mutex> lock(_mutex);
            try {
                opCtx->waitForConditionOrInterrupt(_condVar, lock, [&] { return !_queue.empty(); });
            } catch (const DBException& ex) {
                dassert(!opCtx->checkForInterruptNoAssert().isOK(),
                        str::stream() << "Range deleter thread failed with unexpected exception "
                                      << ex.toStatus());
                break;
            }
        }

        if (RangeDeleterService::get(opCtx)->isDisabled()) {
            MONGO_IDLE_THREAD_BLOCK;
            sleepFor(kCheckForEnabledServiceInterval);
            continue;
        }

        if (!classifiedBlockedMaxKeyTasks) {
            try {
                RangeDeleterService::get(opCtx)->classifyBlockedMaxKeyTasks(opCtx);
                classifiedBlockedMaxKeyTasks = true;
            } catch (const DBException& e) {
                LOGV2_WARNING(13018005,
                              "MaxKey orphan guard: classification failed; retrying before "
                              "processing range deletions",
                              "error"_attr = redact(e.toStatus()));
                // Interruptible backoff so a stepdown/shutdown during the retry is observed
                // promptly instead of blocking the join for the full interval.
                try {
                    opCtx->sleepFor(kCheckForEnabledServiceInterval);
                } catch (const DBException&) {
                    break;
                }
                continue;
            }
        }

        const RangeDeletionTask task = _peekFront();
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
                bool orphansRemovalCompleted = false;
                NamespaceString possiblyStaleNss;
                try {
                    // Should only use possiblyStaleNss for logging and the initial resharding check
                    // collectionUUID is the source of truth for range deletions
                    possiblyStaleNss =
                        AutoGetCollection(
                            opCtx, NamespaceStringOrUUID{dbName, collectionUuid}, MODE_IS)
                            .getNss();
                    if (_shouldDeferRangeDeletionForResharding(possiblyStaleNss, opCtx)) {
                        LOGV2_INFO(11485100,
                                   "Range deletion task paused for namespace while "
                                   "resharding takes place",
                                   "namespace"_attr = possiblyStaleNss,
                                   "collectionUUID"_attr = collectionUuid.toString(),
                                   "task"_attr = task.toBSON());

                        _rescheduleRangeDeletion(
                            task, kCheckForEnabledServiceInterval, "active resharding"sv);
                        break;
                    }
                } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // no range deletions will occur if the collection doesn't exist,
                    // but we still need to delete the persistent range deletion task
                    orphansRemovalCompleted = true;
                }

                // MaxKey orphan guard: for a task classified at step-up as blocked, delete the
                // ordinary docs in the global-max chunk but preserve those whose leading shard-key
                // field is MaxKey (potentially never cloned). The task then completes normally.
                bool preserveMaxKeyPrefixedDocs = false;
                if (!orphansRemovalCompleted && feature_flags::gMaxKeyOrphanGuard.isEnabled() &&
                    skipRangeDeletionForMaxKeyChunks.load() &&
                    RangeDeleterService::get(opCtx)->isMaxKeyBlocked(task.getId())) {
                    LOGV2(
                        13018000,
                        "Preserving MaxKey-prefixed documents while deleting the rest of the range",
                        "namespace"_attr = possiblyStaleNss,
                        "collectionUUID"_attr = collectionUuid.toString(),
                        "range"_attr = redact(range.toString()));
                    preserveMaxKeyPrefixedDocs = true;
                }

                // Perform the actual range deletion
                while (!orphansRemovalCompleted) {
                    try {
                        LOGV2_INFO(6872501,
                                   "Beginning deletion of documents in orphan range",
                                   "namespace"_attr = possiblyStaleNss,
                                   "collectionUUID"_attr = collectionUuid.toString(),
                                   "range"_attr = redact(range.toString()));

                        auto shardKeyPattern =
                            (optKeyPattern ? (*optKeyPattern).toBSON()
                                           : getShardKeyPattern(opCtx, dbName, collectionUuid));

                        {
                            // There can only be one operation context per client, and because we
                            // don't want to miss the shutdown signal, let's create an alternate
                            // client for the batch.
                            auto newClient =
                                _service->getService()->makeClient("range-deleter-batch");
                            AlternativeClientRegion acr(newClient);
                            auto batchOpCtxHolder = opCtxFactory.makeOperationContext(&cc());
                            auto* batchOpCtx = batchOpCtxHolder.get();

                            // Publish the batch opCtx so shutdown()/stepdown can interrupt it
                            // directly, and clear it when the batch scope exits.
                            {
                                std::lock_guard<std::mutex> lock(_mutex);
                                if (_state == kStopped) {
                                    // A stepdown raced us between the last check and here; the
                                    // registration would never be observed by shutdown(). Kill this
                                    // opCtx now so the batch does not block indefinitely.
                                    std::lock_guard<Client> clientLock(*batchOpCtx->getClient());
                                    batchOpCtx->markKilled(ErrorCodes::Interrupted);
                                } else {
                                    _batchOpCtx = batchOpCtx;
                                }
                            }
                            ON_BLOCK_EXIT([this] {
                                std::lock_guard<std::mutex> lock(_mutex);
                                _batchOpCtx = nullptr;
                            });

                            // Keep the serverStatus ticket metrics up to date as the deletion
                            // acquires and releases execution tickets, so a range deletion
                            // stalled on ticket admission is visible in serverStatus/FTDC while
                            // it is stalled. The local stats are attached to the completion log
                            // line below.
                            auto& shardingStats = ShardingStatistics::get(batchOpCtx);
                            admission::execution_control::ScopedTicketAdmissionStatsRecorder
                                ticketStatsRecorder(
                                    batchOpCtx, makeRangeDeleterTicketStatsUpdater(shardingStats));

                            auto numDocsAndBytesDeleted =
                                uassertStatusOK(rangedeletionutil::deleteRangeInBatches(
                                    batchOpCtx,
                                    dbName,
                                    collectionUuid,
                                    shardKeyPattern,
                                    range,
                                    preserveMaxKeyPrefixedDocs));
                            const auto& ticketStats = ticketStatsRecorder.stats();
                            LOGV2_INFO(9239400,
                                       "Finished deletion of documents in orphan range",
                                       "namespace"_attr = possiblyStaleNss,
                                       "collectionUUID"_attr = collectionUuid.toString(),
                                       "range"_attr = redact(range.toString()),
                                       "docsDeleted"_attr = numDocsAndBytesDeleted.first,
                                       "bytesDeleted"_attr = numDocsAndBytesDeleted.second,
                                       "timeQueuedForTicketsMicros"_attr =
                                           ticketStats.timeQueuedMicros,
                                       "timeProcessingWithTicketsMicros"_attr =
                                           ticketStats.timeProcessingMicros,
                                       "ticketAdmissions"_attr = ticketStats.admissions,
                                       "lowPriorityTicketAdmissions"_attr =
                                           ticketStats.lowPriorityAdmissions,
                                       "ticketQueueEntries"_attr = ticketStats.startedQueueing);
                        }
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

                if (preserveMaxKeyPrefixedDocs) {
                    ShardingStatistics::get(opCtx)
                        .countRangeDeletionTasksPreservingMaxKeyOrphans.fetchAndAdd(1);
                }
            } catch (const ExceptionFor<ErrorCodes::IndexNotFound>&) {
                // We cannot complete this range deletion right now because we do not have an index
                // built on the shard key. This situation is expected for a hashed shard key and
                // recoverable for a range shard key. This index may be rebuilt in the future, so
                // reschedule the task at the end of the queue.
                _rescheduleRangeDeletion(task, kMissingIndexRetryInterval, "missing index"sv);
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

}  // namespace mongo

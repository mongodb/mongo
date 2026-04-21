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

#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/concurrency/idle_thread_block.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {

namespace {
constexpr auto kRangeDeletionThreadName = "range-deleter"_sd;
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
    // the FCV could change after this snapshot is taken, but since deferring range deletions
    // is a best-effort optimization and not a safety issue, we accept this limitation
    auto ff = resharding::gFeatureFlagReshardingRegistry.isEnabledUseLatestFCVWhenUninitialized(
        VersionContext::getDecoration(opCtx),
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    auto rsOp = LocalReshardingOperationsRegistry::get().getOperation(nss).has_value();

    return ff && rsOp;
}

void ReadyRangeDeletionsProcessor::_rescheduleRangeDeletion(const RangeDeletionTask& task,
                                                            Seconds delay,
                                                            StringData reason) {
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
                            task, kCheckForEnabledServiceInterval, "active resharding"_sd);
                        break;
                    }
                } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // no range deletions will occur if the collection doesn't exist,
                    // but we still need to delete the persistent range deletion task
                    orphansRemovalCompleted = true;
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
                            auto numDocsAndBytesDeleted =
                                uassertStatusOK(rangedeletionutil::deleteRangeInBatches(
                                    batchOpCtx, dbName, collectionUuid, shardKeyPattern, range));
                            LOGV2_INFO(9239400,
                                       "Finished deletion of documents in orphan range",
                                       "namespace"_attr = possiblyStaleNss,
                                       "collectionUUID"_attr = collectionUuid.toString(),
                                       "range"_attr = redact(range.toString()),
                                       "docsDeleted"_attr = numDocsAndBytesDeleted.first,
                                       "bytesDeleted"_attr = numDocsAndBytesDeleted.second);
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
            } catch (const ExceptionFor<ErrorCodes::IndexNotFound>&) {
                // We cannot complete this range deletion right now because we do not have an index
                // built on the shard key. This situation is expected for a hashed shard key and
                // recoverable for a range shard key. This index may be rebuilt in the future, so
                // reschedule the task at the end of the queue.
                _rescheduleRangeDeletion(task, kMissingIndexRetryInterval, "missing index"_sd);
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

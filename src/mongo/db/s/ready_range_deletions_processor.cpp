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

#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/logv2/log.h"
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
      _thread([this] { _runRangeDeletions(); }),
      _executor(executor) {}

ReadyRangeDeletionsProcessor::~ReadyRangeDeletionsProcessor() {
    shutdown();
    invariant(_thread.joinable());
    _thread.join();
    invariant(!_threadOpCtxHolder,
              "Thread operation context is still alive after joining main thread");
}

void ReadyRangeDeletionsProcessor::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (_state == kStopped)
        return;

    _state = kStopped;

    if (_threadOpCtxHolder) {
        stdx::lock_guard<Client> scopedClientLock(*_threadOpCtxHolder->getClient());
        _threadOpCtxHolder->markKilled(ErrorCodes::Interrupted);
    }
}

bool ReadyRangeDeletionsProcessor::_stopRequested() const {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    return _state == kStopped;
}

void ReadyRangeDeletionsProcessor::emplaceRangeDeletion(const RangeDeletionTask& rdt) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (_state != kRunning) {
        return;
    }
    _queue.push(rdt);
    _condVar.notify_all();
}

void ReadyRangeDeletionsProcessor::_completedRangeDeletion() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    dassert(!_queue.empty());
    _queue.pop();
}

void ReadyRangeDeletionsProcessor::_runRangeDeletions() {
    ThreadClient threadClient(kRangeDeletionThreadName,
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

}  // namespace mongo

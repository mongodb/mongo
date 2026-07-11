// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/storage_oplog_manager.h"

#include "mongo/db/client.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"

#include <limits>
#include <memory>
#include <mutex>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

// Arbitrary. Using the storageGlobalParams.journalCommitIntervalMs default, which used to
// dynamically control the visibility thread's delay back when the visibility thread also flushed
// the journal.
constexpr int kDelayMillis = 100;

}  // namespace

void StorageOplogManager::start(OperationContext* opCtx,
                                const KVEngine& engine,
                                RecordStore& oplog) {
    _oplogIdent = std::string{oplog.getIdent()};

    // Prime the oplog read timestamp.
    std::unique_ptr<SeekableRecordCursor> reverseOplogCursor = oplog.getCursor(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx), false /* false = reverse cursor */);
    auto lastRecord = reverseOplogCursor->next();
    if (lastRecord) {
        // Although the oplog may have holes, using the top of the oplog should be safe. In the
        // event of a secondary crashing, replication recovery will truncate the oplog, resetting
        // visibility to the truncate point. In the event of a primary crashing, it will perform
        // rollback before servicing oplog reads.
        auto topOfOplogTimestamp = Timestamp(lastRecord->id.getLong());
        setOplogReadTimestamp(topOfOplogTimestamp);
        LOGV2_DEBUG(22368,
                    1,
                    "Initializing the oplog read timestamp (oplog visibility).",
                    "oplogReadTimestamp"_attr = topOfOplogTimestamp);
    } else if (engine.isReplSet()) {
        // Avoid setting oplog visibility to 0. That means "everything is visible".
        setOplogReadTimestamp(Timestamp(StorageEngine::kMinimumTimestamp));
    } else {
        // Use max Timestamp to disable oplog visibility in standalone mode. The read timestamp may
        // be interpreted as signed so we need to use signed int64_t max to make sure it is always
        // larger than any user 'ts' field.
        setOplogReadTimestamp(Timestamp(std::numeric_limits<int64_t>::max()));
        return;
    }

    std::lock_guard<std::mutex> lk(_oplogVisibilityStateMutex);
    invariant(!_oplog);
    _oplog = &oplog;
    LOGV2(12035900, "Oplog visibility thread is starting");
    _oplogVisibilityThread =
        stdx::thread([this, service = opCtx->getServiceContext()->getService(), &engine] {
            Client::initThread("OplogVisibilityThread", service);

            std::unique_lock<std::mutex> lk(_oplogVisibilityStateMutex);
            while (_oplog) {
                switch (_updateVisibility(lk, engine, *_oplog->capped())) {
                    case VisibilityUpdateResult::NotUpdated:
                        continue;
                    case VisibilityUpdateResult::Updated:
                        // Wake up any awaitData cursors and tell them more data might be visible
                        // now. We normally notify waiters on capped collection inserts/updates, but
                        // oplog entries will not become visible immediately upon insert, so we
                        // notify waiters here as well, when new oplog entries actually become
                        // visible to cursors.
                        _oplog->capped()->notifyWaitersIfNeeded();
                        continue;
                    case VisibilityUpdateResult::Stopped:
                        return;
                }
            }
            LOGV2(22372, "Oplog visibility thread is stopping");
        });
}

void StorageOplogManager::stop(const RecordStore* oplog) {
    {
        std::lock_guard lk(_oplogVisibilityStateMutex);
        if (!_oplog) {
            return;
        }

        if (oplog && oplog != _oplog) {
            LOGV2_DEBUG(11996400,
                        1,
                        "Not stopping oplog visibility thread because oplog pointer did not match");
            return;
        }
        _oplog = nullptr;
    }

    if (_oplogVisibilityThread.joinable()) {
        _oplogVisibilityThreadCV.notify_one();
        _oplogVisibilityThread.join();
    }
}

void StorageOplogManager::triggerOplogVisibilityUpdate() {
    std::lock_guard<std::mutex> lk(_oplogVisibilityStateMutex);
    if (!_triggerOplogVisibilityUpdate) {
        _triggerOplogVisibilityUpdate = true;
        _oplogVisibilityThreadCV.notify_one();
    }
}

void StorageOplogManager::waitForAllEarlierOplogWritesToBeVisible(
    const RecordStore* oplogRecordStore, OperationContext* opCtx) {
    invariant(!shard_role_details::getRecoveryUnit(opCtx)->inUnitOfWork());

    // In order to reliably detect rollback situations, we need to fetch the latestVisibleTimestamp
    // prior to querying the end of the oplog.
    auto currentLatestVisibleTimestamp = getOplogReadTimestamp();

    // Use a reverse oplog cursor that is not subject to the oplog visibility rules to see the
    // latest oplog entry timestamp. Then we will wait for that timestamp to become visible.
    //
    std::unique_ptr<SeekableRecordCursor> cursor = oplogRecordStore->getCursor(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx), false /* select a reverse cursor */);
    auto lastOplogRecord = cursor->next();
    if (!lastOplogRecord) {
        LOGV2_DEBUG(22369, 2, "The oplog does not exist. Not going to wait for oplog visibility.");
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
        return;
    }
    const auto& waitingFor = lastOplogRecord->id;

    // Close transaction before we wait.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    std::unique_lock<std::mutex> lk(_oplogVisibilityStateMutex);

    // Prevent any scheduled oplog visibility updates from being delayed for batching and blocking
    // this wait excessively.
    ++_opsWaitingForOplogVisibilityUpdate;
    invariant(_opsWaitingForOplogVisibilityUpdate > 0);
    ScopeGuard exitGuard([&] { --_opsWaitingForOplogVisibilityUpdate; });

    // Out of order writes to the oplog always call triggerOplogVisibilityUpdate() on commit to
    // prompt the OplogVisibilityThread to run and update the oplog visibility. We simply need to
    // wait until all of the writes behind and including 'waitingFor' commit so there are no oplog
    // holes.
    opCtx->waitForConditionOrInterrupt(_oplogEntriesBecameVisibleCV, lk, [&] {
        auto newLatestVisibleTimestamp = getOplogReadTimestamp();
        if (newLatestVisibleTimestamp < currentLatestVisibleTimestamp) {
            LOGV2_DEBUG(22370,
                        1,
                        "The latest visible oplog entry went backwards in time. A rollback likely "
                        "occurred.",
                        "latestVisibleOplogEntryTimestamp"_attr =
                            Timestamp(newLatestVisibleTimestamp),
                        "previouslyFoundLatestVisibleOplogEntryTimestamp"_attr =
                            Timestamp(currentLatestVisibleTimestamp));
            // We cannot wait for a write that no longer exists, so we are finished.
            return true;
        }
        currentLatestVisibleTimestamp = newLatestVisibleTimestamp;
        RecordId newLatestVisible = RecordId(currentLatestVisibleTimestamp);
        if (newLatestVisible < waitingFor) {
            LOGV2_DEBUG(22371,
                        2,
                        "Operation is waiting for an entry to become visible in the oplog.",
                        "awaitedOplogEntryTimestamp"_attr = Timestamp(waitingFor.getLong()),
                        "currentLatestVisibleOplogEntryTimestamp"_attr =
                            Timestamp(currentLatestVisibleTimestamp));
        }
        return newLatestVisible >= waitingFor;
    });
}

StorageOplogManager::VisibilityUpdateResult StorageOplogManager::_updateVisibility(
    std::unique_lock<std::mutex>& lk, const KVEngine& engine, const RecordStore::Capped& oplog) {
    {
        MONGO_IDLE_THREAD_BLOCK;
        _oplogVisibilityThreadCV.wait(lk,
                                      [this] { return !_oplog || _triggerOplogVisibilityUpdate; });

        // If we are not shutting down and nobody is actively waiting for the oplog to become
        // visible, delay a bit to batch more requests into one update and reduce system load.
        auto now = Date_t::now();
        auto deadline = now + Milliseconds{kDelayMillis};

        // Check once a millisecond, up to the delay deadline, whether the delay should be
        // preempted because of waiting callers or shutdown.
        while (now < deadline &&
               !_oplogVisibilityThreadCV.wait_until(
                   lk,
                   now.toSystemTimePoint(),
                   [this, &oplog] {
                       return !_oplog || _opsWaitingForOplogVisibilityUpdate || oplog.hasWaiters();
                   })) {
            now += Milliseconds{1};
        }
    }

    if (!_oplog) {
        return VisibilityUpdateResult::Stopped;
    }

    invariant(_triggerOplogVisibilityUpdate);
    _triggerOplogVisibilityUpdate = false;

    // Fetch the all_durable timestamp from the storage engine, which is guaranteed not to have
    // any holes behind it in-memory.
    auto allDurable = engine.getAllDurableTimestamp().asULL();
    if (allDurable <= getOplogReadTimestamp()) {
        return VisibilityUpdateResult::NotUpdated;
    }

    // Publish the new timestamp value.
    _setOplogReadTimestamp(lk, allDurable);
    return VisibilityUpdateResult::Updated;
}

std::uint64_t StorageOplogManager::getOplogReadTimestamp() const {
    return _oplogReadTimestamp.load();
}

void StorageOplogManager::setOplogReadTimestamp(Timestamp ts) {
    std::lock_guard<std::mutex> lk(_oplogVisibilityStateMutex);
    _setOplogReadTimestamp(lk, ts.asULL());
}

void StorageOplogManager::_setOplogReadTimestamp(WithLock, uint64_t newTimestamp) {
    _oplogReadTimestamp.store(newTimestamp);
    _oplogEntriesBecameVisibleCV.notify_all();
    LOGV2_DEBUG(22374,
                2,
                "Updating the oplogReadTimestamp.",
                "newOplogReadTimestamp"_attr = Timestamp(newTimestamp));
}

std::string_view StorageOplogManager::getIdent() const {
    return _oplogIdent;
}

}  // namespace mongo

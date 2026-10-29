// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_visibility_manager.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

namespace {

/**
 * Returns a lambda expression that notifies capped waiters if visibility is changed.
 */
auto notifyCappedWaitersIfVisibilityChanged(const bool& visibilityChanged, RecordStore* rs) {
    return [&visibilityChanged, rs = rs] {
        if (visibilityChanged) {
            invariant(rs);
            rs->capped()->notifyWaitersIfNeeded();
        }
    };
}

}  // namespace

RecordStore* OplogVisibilityManager::getRecordStore() const {
    return _rs;
}

void OplogVisibilityManager::reInit(RecordStore* rs, const Timestamp& initialTs) {
    std::lock_guard<std::mutex> lock(_mutex);
    invariant(_oplogTimestampList.empty());
    _oplogVisibilityTimestamp.store(initialTs);
    _rs = rs;
}

void OplogVisibilityManager::clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    invariant(_oplogTimestampList.empty());
    _rs = nullptr;
}

OplogVisibilityManager::const_iterator OplogVisibilityManager::trackTimestamps(
    const Timestamp& first, const Timestamp& last) {
    bool visibilityChanged = false;
    ON_BLOCK_EXIT(notifyCappedWaitersIfVisibilityChanged(visibilityChanged, _rs));

    std::lock_guard<std::mutex> lock(_mutex);
    invariant(first <= last && first > _latestTimeSeen,
              str::stream() << "first timestamp: " << first.toString()
                            << ", last timestamp: " << last.toString()
                            << ", lastestTimeSeen: " << _latestTimeSeen.toString());

    if (_oplogTimestampList.empty()) {
        visibilityChanged = _setOplogVisibilityTimestamp(lock, first - 1);
    }

    _latestTimeSeen = last;

    return _oplogTimestampList.insert(Timestamp(first));
}

void OplogVisibilityManager::untrackTimestamps(OplogVisibilityManager::const_iterator pos) {
    bool visibilityChanged = false;
    ON_BLOCK_EXIT(notifyCappedWaitersIfVisibilityChanged(visibilityChanged, _rs));

    std::lock_guard<std::mutex> lock(_mutex);
    const bool isFront = _oplogTimestampList.erase(pos);

    // Visibility has not changed.
    if (!isFront) {
        return;
    }

    // There are no more timestamps to track, so visibility is updated to _latestTimeSeen.
    if (_oplogTimestampList.empty()) {
        visibilityChanged = _setOplogVisibilityTimestamp(lock, _latestTimeSeen);
        return;
    }

    // Visibility is updated to the oldest timestamp in the list minus 1.
    visibilityChanged = _setOplogVisibilityTimestamp(lock, _oplogTimestampList.front() - 1);
}

Timestamp OplogVisibilityManager::getOplogVisibilityTimestamp() const {
    return _oplogVisibilityTimestamp.load();
}

void OplogVisibilityManager::setOplogVisibilityTimestamp(const Timestamp& visibilityTimestamp) {
    bool visibilityChanged = false;
    ON_BLOCK_EXIT(notifyCappedWaitersIfVisibilityChanged(visibilityChanged, _rs));

    std::lock_guard<std::mutex> lock(_mutex);

    visibilityChanged = _setOplogVisibilityTimestamp(lock, visibilityTimestamp);

    // Cannot advance visibility timestamp if there are other timestamps being tracked.
    if (visibilityChanged) {
        invariant(_oplogTimestampList.empty());
    }
}

bool OplogVisibilityManager::_setOplogVisibilityTimestamp(WithLock lock,
                                                          const Timestamp& visibilityTimestamp) {
    const auto prevVisibilityTimestamp = _oplogVisibilityTimestamp.swap(visibilityTimestamp);
    _oplogEntriesBecameVisibleCV.notify_all();
    return prevVisibilityTimestamp < visibilityTimestamp;
}

void OplogVisibilityManager::waitForTimestampToBeVisible(OperationContext* opCtx,
                                                         const Timestamp& waitingFor) {
    auto currentVisibilityTimestamp = getOplogVisibilityTimestamp();

    std::unique_lock<std::mutex> lock(_mutex);

    opCtx->waitForConditionOrInterrupt(_oplogEntriesBecameVisibleCV, lock, [&] {
        const auto newVisibilityTimestamp = getOplogVisibilityTimestamp();
        if (newVisibilityTimestamp < currentVisibilityTimestamp) {
            LOGV2_DEBUG(9281501,
                        1,
                        "The latest visible oplog entry went backwards in time. A rollback likely "
                        "occurred.",
                        "latestVisibleOplogEntryTimestamp"_attr = newVisibilityTimestamp,
                        "previouslyFoundLatestVisibleOplogEntryTimestamp"_attr =
                            currentVisibilityTimestamp);
            // We cannot wait for a write that no longer exists, so we are finished.
            return true;
        }
        currentVisibilityTimestamp = newVisibilityTimestamp;
        if (newVisibilityTimestamp < waitingFor) {
            LOGV2_DEBUG(9281502,
                        2,
                        "Operation is waiting for an entry to become visible in the oplog.",
                        "awaitedOplogEntryTimestamp"_attr = waitingFor,
                        "currentLatestVisibleOplogEntryTimestamp"_attr =
                            currentVisibilityTimestamp);
            return false;
        }
        return true;
    });
}

void OplogVisibilityManager::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                                     bool primaryOnly) {


    if (primaryOnly &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                              DatabaseName::kAdmin))
        return;

    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // Use a reverse oplog cursor that is not subject to the oplog visibility rules to see the
    // latest oplog entry timestamp. Then we will wait for that timestamp to become visible.
    std::unique_ptr<SeekableRecordCursor> cursor = _rs->getCursor(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx), false /* select a reverse cursor */);
    auto lastOplogRecord = cursor->next();
    if (!lastOplogRecord) {
        LOGV2_DEBUG(
            9281500, 2, "The oplog does not exist. Not going to wait for oplog visibility.");
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
        return;
    }

    const auto& waitingFor = lastOplogRecord->id;

    // Close transaction before we wait.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    waitForTimestampToBeVisible(opCtx, Timestamp(waitingFor.getLong()));
}

}  // namespace repl
}  // namespace mongo

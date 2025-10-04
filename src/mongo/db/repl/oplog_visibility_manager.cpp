/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_visibility_manager.h"

#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/repl/replication_coordinator.h"
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
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_oplogTimestampList.empty());
    _oplogVisibilityTimestamp.store(initialTs);
    _rs = rs;
}

void OplogVisibilityManager::clear() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_oplogTimestampList.empty());
    _rs = nullptr;
}

OplogVisibilityManager::const_iterator OplogVisibilityManager::trackTimestamps(
    const Timestamp& first, const Timestamp& last) {
    bool visibilityChanged = false;
    ON_BLOCK_EXIT(notifyCappedWaitersIfVisibilityChanged(visibilityChanged, _rs));

    stdx::lock_guard<stdx::mutex> lock(_mutex);
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

    stdx::lock_guard<stdx::mutex> lock(_mutex);
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

    stdx::lock_guard<stdx::mutex> lock(_mutex);

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

    stdx::unique_lock<stdx::mutex> lock(_mutex);

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

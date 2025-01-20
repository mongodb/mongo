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

#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"

// IWYU pragma: no_include "cxxabi.h"
#include <limits>
#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util_core.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

void WiredTigerOplogManager::initialize(OperationContext* opCtx, RecordStore* oplogRecordStore) {
    // Prime the oplog read timestamp.
    std::unique_ptr<SeekableRecordCursor> reverseOplogCursor =
        oplogRecordStore->getCursor(opCtx, false /* false = reverse cursor */);
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
    } else if (repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
        // Avoid setting oplog visibility to 0. That means "everything is visible".
        setOplogReadTimestamp(Timestamp(StorageEngine::kMinimumTimestamp));
    } else {
        // Use max Timestamp to disable oplog visibility in standalone mode. The read timestamp may
        // be interpreted as signed so we need to use signed int64_t max to make sure it is always
        // larger than any user 'ts' field.
        setOplogReadTimestamp(Timestamp(std::numeric_limits<int64_t>::max()));
    }

    _oplogRecordStore = oplogRecordStore;
}

void WiredTigerOplogManager::triggerOplogVisibilityUpdate(KVEngine* engine,
                                                          Timestamp commitTimestamp) {
    {
        stdx::lock_guard<stdx::mutex> lk(_oplogVisibilityStateMutex);

        // Do nothing if the visibility timestamp has already been advanced to the last committed
        // timestamp.
        const auto currentVisibleTimestamp = _oplogReadTimestamp.load();
        if (currentVisibleTimestamp >= commitTimestamp.asULL()) {
            return;
        }

        // Fetch the all_durable timestamp from the storage engine, which is guaranteed not to have
        // any holes behind it in-memory.
        const uint64_t newTimestamp = engine->getAllDurableTimestamp().asULL();

        // The newTimestamp may actually go backward during secondary batch application,
        // where we commit data file changes separately from oplog changes, so ignore
        // a non-incrementing timestamp.
        if (newTimestamp <= currentVisibleTimestamp) {
            LOGV2_DEBUG(22373,
                        2,
                        "No new oplog entries became visible.",
                        "aNoHolesOplogTimestamp"_attr = Timestamp(newTimestamp));
            return;
        }

        // Publish the new timestamp value. Avoid going backward.
        _setOplogReadTimestamp(lk, newTimestamp);
    }

    if (_oplogRecordStore) {
        _oplogRecordStore->capped()->notifyWaitersIfNeeded();
    }
}

void WiredTigerOplogManager::waitForAllEarlierOplogWritesToBeVisible(
    const RecordStore* oplogRecordStore, OperationContext* opCtx) {
    invariant(!shard_role_details::getRecoveryUnit(opCtx)->inUnitOfWork());

    // In order to reliably detect rollback situations, we need to fetch the latestVisibleTimestamp
    // prior to querying the end of the oplog.
    auto currentLatestVisibleTimestamp = getOplogReadTimestamp();

    // Use a reverse oplog cursor that is not subject to the oplog visibility rules to see the
    // latest oplog entry timestamp. Then we will wait for that timestamp to become visible.
    //
    std::unique_ptr<SeekableRecordCursor> cursor =
        oplogRecordStore->getCursor(opCtx, false /* select a reverse cursor */);
    auto lastOplogRecord = cursor->next();
    if (!lastOplogRecord) {
        LOGV2_DEBUG(22369, 2, "The oplog does not exist. Not going to wait for oplog visibility.");
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
        return;
    }
    const auto& waitingFor = lastOplogRecord->id;

    // Close transaction before we wait.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    stdx::unique_lock<stdx::mutex> lk(_oplogVisibilityStateMutex);

    // Out of order writes to the oplog always call triggerOplogVisibilityUpdate() on commit to
    // run and update the oplog visibility. We simply need to wait until all of the writes behind
    // and including 'waitingFor' commit so there are no oplog holes.
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

std::uint64_t WiredTigerOplogManager::getOplogReadTimestamp() const {
    return _oplogReadTimestamp.load();
}

void WiredTigerOplogManager::setOplogReadTimestamp(Timestamp ts) {
    stdx::lock_guard<stdx::mutex> lk(_oplogVisibilityStateMutex);
    _setOplogReadTimestamp(lk, ts.asULL());
}

void WiredTigerOplogManager::_setOplogReadTimestamp(WithLock, uint64_t newTimestamp) {
    _oplogReadTimestamp.store(newTimestamp);
    _oplogEntriesBecameVisibleCV.notify_all();
    LOGV2_DEBUG(22374,
                2,
                "Updating the oplogReadTimestamp.",
                "newOplogReadTimestamp"_attr = Timestamp(newTimestamp));
}

}  // namespace mongo

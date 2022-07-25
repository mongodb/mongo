/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_snapshot_manager.h"

#include "mongo/db/server_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeMajorityReadTransactionStarted);
}

void WiredTigerSnapshotManager::setCommittedSnapshot(const Timestamp& timestamp) {
    stdx::lock_guard<Latch> lock(_committedSnapshotMutex);

    invariant(!_committedSnapshot || *_committedSnapshot <= timestamp);
    _committedSnapshot = timestamp;
}

void WiredTigerSnapshotManager::setLastApplied(const Timestamp& timestamp) {
    stdx::lock_guard<Latch> lock(_lastAppliedMutex);
    if (timestamp.isNull())
        _lastApplied = boost::none;
    else
        _lastApplied = timestamp;
}

boost::optional<Timestamp> WiredTigerSnapshotManager::getLastApplied() {
    stdx::lock_guard<Latch> lock(_lastAppliedMutex);
    return _lastApplied;
}

void WiredTigerSnapshotManager::clearCommittedSnapshot() {
    stdx::lock_guard<Latch> lock(_committedSnapshotMutex);
    _committedSnapshot = boost::none;
}

boost::optional<Timestamp> WiredTigerSnapshotManager::getMinSnapshotForNextCommittedRead() const {
    if (!serverGlobalParams.enableMajorityReadConcern) {
        return boost::none;
    }

    stdx::lock_guard<Latch> lock(_committedSnapshotMutex);
    return _committedSnapshot;
}

Timestamp WiredTigerSnapshotManager::beginTransactionOnCommittedSnapshot(
    WT_SESSION* session,
    PrepareConflictBehavior prepareConflictBehavior,
    RoundUpPreparedTimestamps roundUpPreparedTimestamps,
    WiredTigerBeginTxnBlock::UntimestampedWriteAssertion untimestampedWriteAssertion) const {

    auto committedSnapshot = [this]() {
        stdx::lock_guard<Latch> lock(_committedSnapshotMutex);
        uassert(ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Committed view disappeared while running operation",
                _committedSnapshot);
        return _committedSnapshot.get();
    }();

    if (MONGO_unlikely(hangBeforeMajorityReadTransactionStarted.shouldFail())) {
        sleepmillis(100);
    }

    // We need to round up our read timestamp in case the oldest timestamp has advanced past the
    // committedSnapshot we just read.
    WiredTigerBeginTxnBlock txnOpen(session,
                                    prepareConflictBehavior,
                                    roundUpPreparedTimestamps,
                                    RoundUpReadTimestamp::kRound,
                                    untimestampedWriteAssertion);
    auto status = txnOpen.setReadSnapshot(committedSnapshot);
    fassert(30635, status);

    txnOpen.done();
    return committedSnapshot;
}

}  // namespace mongo

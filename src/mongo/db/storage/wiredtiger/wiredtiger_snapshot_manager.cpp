// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/wiredtiger/wiredtiger_snapshot_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeMajorityReadTransactionStarted);
}

void WiredTigerSnapshotManager::setCommittedSnapshot(const Timestamp& timestamp) {
    std::lock_guard<std::mutex> lock(_committedSnapshotMutex);

    invariant(!_committedSnapshot || *_committedSnapshot <= timestamp);
    _committedSnapshot = timestamp;
}

void WiredTigerSnapshotManager::setLastApplied(const Timestamp& timestamp) {
    std::lock_guard<std::mutex> lock(_lastAppliedMutex);
    if (timestamp.isNull())
        _lastApplied = boost::none;
    else
        _lastApplied = timestamp;
}

boost::optional<Timestamp> WiredTigerSnapshotManager::getLastApplied() {
    std::lock_guard<std::mutex> lock(_lastAppliedMutex);
    return _lastApplied;
}

void WiredTigerSnapshotManager::clearCommittedSnapshot() {
    std::lock_guard<std::mutex> lock(_committedSnapshotMutex);
    _committedSnapshot = boost::none;
}

boost::optional<Timestamp> WiredTigerSnapshotManager::getMinSnapshotForNextCommittedRead() const {
    std::lock_guard<std::mutex> lock(_committedSnapshotMutex);
    return _committedSnapshot;
}

Timestamp WiredTigerSnapshotManager::beginTransactionOnCommittedSnapshot(
    WiredTigerSession* session,
    PrepareConflictBehavior prepareConflictBehavior,
    bool roundUpPreparedTimestamps,
    RecoveryUnit::UntimestampedWriteAssertionLevel untimestampedWriteAssertion) const {

    auto committedSnapshot = [this]() {
        std::lock_guard<std::mutex> lock(_committedSnapshotMutex);
        uassert(ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Committed view disappeared while running operation",
                _committedSnapshot);
        return _committedSnapshot.value();
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

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

#pragma once

#include <boost/optional.hpp>
#include <wiredtiger.h>

#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/platform/mutex.h"

namespace mongo {

using RoundUpPreparedTimestamps = WiredTigerBeginTxnBlock::RoundUpPreparedTimestamps;

class WiredTigerOplogManager;

class WiredTigerSnapshotManager final : public SnapshotManager {
    WiredTigerSnapshotManager(const WiredTigerSnapshotManager&) = delete;
    WiredTigerSnapshotManager& operator=(const WiredTigerSnapshotManager&) = delete;

public:
    WiredTigerSnapshotManager() = default;

    void setCommittedSnapshot(const Timestamp& timestamp) final;
    void setLastApplied(const Timestamp& timestamp) final;
    boost::optional<Timestamp> getLastApplied() final;
    void clearCommittedSnapshot() final;

    //
    // WT-specific methods
    //

    /**
     * Starts a transaction and returns the SnapshotName used.
     *
     * Throws if there is currently no committed snapshot.
     */
    Timestamp beginTransactionOnCommittedSnapshot(
        WT_SESSION* session,
        PrepareConflictBehavior prepareConflictBehavior,
        RoundUpPreparedTimestamps roundUpPreparedTimestamps,
        WiredTigerBeginTxnBlock::UntimestampedWriteAssertion untimestampedWriteAssertion) const;

    /**
     * Returns lowest SnapshotName that could possibly be used by a future call to
     * beginTransactionOnCommittedSnapshot, or boost::none if there is currently no committed
     * snapshot.
     *
     * This should not be used for starting a transaction on this SnapshotName since the named
     * snapshot may be deleted by the time you start the transaction.
     */
    boost::optional<Timestamp> getMinSnapshotForNextCommittedRead() const;

private:
    // Snapshot to use for reads at a commit timestamp.
    mutable Mutex _committedSnapshotMutex =  // Guards _committedSnapshot.
        MONGO_MAKE_LATCH("WiredTigerSnapshotManager::_committedSnapshotMutex");
    boost::optional<Timestamp> _committedSnapshot;

    // Timestamp to use for reads at a the lastApplied timestamp.
    mutable Mutex _lastAppliedMutex =  // Guards _lastApplied.
        MONGO_MAKE_LATCH("WiredTigerSnapshotManager::_lastAppliedMutex");
    boost::optional<Timestamp> _lastApplied;
};
}  // namespace mongo

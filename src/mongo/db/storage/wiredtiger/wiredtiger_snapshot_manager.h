// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/util/modules.h"

#include <mutex>

#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class WiredTigerSession;
using RoundUpPreparedTimestamps = WiredTigerBeginTxnBlock::RoundUpPreparedTimestamps;

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
        WiredTigerSession* session,
        PrepareConflictBehavior prepareConflictBehavior,
        bool roundUpPreparedTimestamps,
        RecoveryUnit::UntimestampedWriteAssertionLevel untimestampedWriteAssertion,
        boost::optional<int64_t> operationTimeoutMs = boost::none) const;

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
    mutable std::mutex _committedSnapshotMutex;  // Guards _committedSnapshot.
    boost::optional<Timestamp> _committedSnapshot;

    // Timestamp to use for reads at a the lastApplied timestamp.
    mutable std::mutex _lastAppliedMutex;  // Guards _lastApplied.
    boost::optional<Timestamp> _lastApplied;
};
}  // namespace mongo

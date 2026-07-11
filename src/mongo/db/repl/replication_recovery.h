// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;

namespace repl {

class StorageInterface;
class ReplicationConsistencyMarkers;

/**
 * This class is used by the replication system to recover after an unclean shutdown, a rollback or
 * during initial sync.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] ReplicationRecovery {
public:
    ReplicationRecovery() = default;
    virtual ~ReplicationRecovery() = default;

    /**
     * Recovers the data on disk from the oplog. If the provided stable timestamp is not "none",
     * this function assumes the data reflects that timestamp.
     * Returns the provided stable timestamp. If the provided stable timestamp is "none", this
     * function might try to ask storage for the last stable timestamp if it exists before doing
     * recovery which will be returned after performing successful recovery.
     */
    virtual boost::optional<Timestamp> recoverFromOplog(
        OperationContext* opCtx, boost::optional<Timestamp> stableTimestamp) = 0;

    /**
     *  Recovers the data on disk from the oplog and puts the node in readOnly mode. If
     *  'takeUnstableCheckpointOnShutdown' is specified and an unstable checkpoint is present,
     *  ensures that recovery can be skipped safely.
     */
    virtual void recoverFromOplogAsStandalone(OperationContext* opCtx,
                                              bool duringInitialSync = false) = 0;

    /**
     * Recovers the data on disk from the oplog up to and including the given timestamp.
     */
    virtual void recoverFromOplogUpTo(OperationContext* opCtx, Timestamp endPoint) = 0;

    /**
     * Truncates the oplog after the entry with the 'truncateAfterTimestamp'.
     */
    virtual void truncateOplogToTimestamp(OperationContext* opCtx,
                                          Timestamp truncateAfterTimestamp) = 0;

    /**
     * Performs oplog application for magic restore. This function expects the caller to correctly
     * truncate oplog the oplog application start point. Callers must be using a storage engine that
     * supports recover to stable timestamp.
     */
    virtual void applyOplogEntriesForRestore(OperationContext* opCtx,
                                             Timestamp stableTimestamp) = 0;
};

class [[MONGO_MOD_PUBLIC]] ReplicationRecoveryImpl : public ReplicationRecovery {
    ReplicationRecoveryImpl(const ReplicationRecoveryImpl&) = delete;
    ReplicationRecoveryImpl& operator=(const ReplicationRecoveryImpl&) = delete;

public:
    ReplicationRecoveryImpl(StorageInterface* storageInterface,
                            ReplicationConsistencyMarkers* consistencyMarkers);

    boost::optional<Timestamp> recoverFromOplog(
        OperationContext* opCtx, boost::optional<Timestamp> stableTimestamp) override;

    void recoverFromOplogAsStandalone(OperationContext* opCtx,
                                      bool duringInitialSync = false) override;

    void recoverFromOplogUpTo(OperationContext* opCtx, Timestamp endPoint) override;

    void truncateOplogToTimestamp(OperationContext* opCtx,
                                  Timestamp truncateAfterTimestamp) override;

    void applyOplogEntriesForRestore(OperationContext* opCtx, Timestamp stableTimestamp) override;

private:
    enum class RecoveryMode {
        kStartupFromStableTimestamp,
        kStartupFromUnstableCheckpoint,
        kRollbackFromStableTimestamp,
        // There is no RecoveryMode::kRollbackFromUnstableCheckpoint, rollback can only recover from
        // a stable timestamp.
    };

    /**
     * Confirms that the data and oplog all indicate that the nodes has an unstable checkpoint
     * that is fully up to date.
     */
    void _assertNoRecoveryNeededOnUnstableCheckpoint(OperationContext* opCtx);

    /**
     * After truncating the oplog, completes recovery if we're recovering from a stable timestamp
     * or a stable checkpoint.
     */
    void _recoverFromStableTimestamp(OperationContext* opCtx,
                                     Timestamp stableTimestamp,
                                     OpTime topOfOplog,
                                     RecoveryMode recoveryMode);

    /**
     * After truncating the oplog, completes recovery if we're recovering from an unstable
     * checkpoint.
     */
    void _recoverFromUnstableCheckpoint(OperationContext* opCtx,
                                        OpTime appliedThrough,
                                        OpTime topOfOplog);

    /**
     * Applies all oplog entries from oplogApplicationStartPoint (exclusive) to topOfOplog
     * (inclusive). This fasserts if oplogApplicationStartPoint is not in the oplog.
     */
    void _applyToEndOfOplog(OperationContext* opCtx,
                            const Timestamp& oplogApplicationStartPoint,
                            const Timestamp& topOfOplog,
                            RecoveryMode recoveryMode);

    /**
     * Applies all oplog entries from startPoint (exclusive) to endPoint (inclusive). Returns the
     * Timestamp of the last applied operation.
     */
    Timestamp _applyOplogOperations(OperationContext* opCtx,
                                    const Timestamp& startPoint,
                                    const Timestamp& endPoint,
                                    RecoveryMode recoveryMode);

    /**
     * Gets the last applied OpTime from the end of the oplog. Returns CollectionIsEmpty if there is
     * no oplog.
     */
    StatusWith<OpTime> _getTopOfOplog(OperationContext* opCtx) const;

    /**
     * Truncates the oplog after the "truncateAfterTimestamp" entry.
     * If the stableTimestamp is set, may move it backwards to the new top of oplog.
     */
    void _truncateOplogTo(OperationContext* opCtx,
                          Timestamp truncateAfterTimestamp,
                          boost::optional<Timestamp>* stableTimestamp);

    /**
     * Uses the oplogTruncateAfterPoint, accessed via '_consistencyMarkers', to decide whether to
     * truncate part of the oplog. If oplogTruncateAfterPoint has been set, then there may be holes
     * in the oplog after that point. In that case, we will truncate the oplog entries starting at
     * and including the entry associated with the oplogTruncateAfterPoint timestamp.
     *
     * If the oplogTruncateAfterPoint is earlier in time than or equal to the stable timestamp, we
     * will truncate the oplog after the stable timestamp instead. There cannot be holes before a
     * stable timestamp. The oplogTruncateAfterPoint can lag behind the stable timestamp because the
     * oplogTruncateAfterPoint is updated on primaries by an asynchronously looping thread that can
     * potentially be starved.
     *
     * If the stable timestamp is at a hole, this will move the stable timestamp back to the new
     * top of oplog.  This can happen on primaries in single-node replica sets.
     */
    void _truncateOplogIfNeededAndThenClearOplogTruncateAfterPoint(
        OperationContext* opCtx, boost::optional<Timestamp>* stableTimestamp);

    /**
     * Checks if the proposed oplog application start point (which is typically derived from the
     * stable timestamp) exists in the oplog. If it does, this returns that same start point
     * unchanged. If that point is not in the oplog, this function returns an entry before
     * that start point.
     * It is safe to do as as we make sure that we always keep an oplog entry that is less than
     * or equal to the stable timestamp so such a correction always pushes the start point back and
     * never forward. Applying entries from an earlier point is permissible due to oplog entry
     * idempotency (and also due to the order being preserved.)
     */
    Timestamp _adjustStartPointIfNecessary(OperationContext* opCtx, Timestamp startPoint);

    StorageInterface* _storageInterface;
    ReplicationConsistencyMarkers* _consistencyMarkers;

    // Flag to indicate whether the recovery is being done during initial sync or not.
    bool _duringInitialSync = false;
};

}  // namespace repl
}  // namespace mongo

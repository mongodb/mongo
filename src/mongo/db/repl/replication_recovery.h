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

#include <boost/optional/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime.h"

namespace mongo {

class OperationContext;

namespace repl {

class StorageInterface;
class ReplicationConsistencyMarkers;

/**
 * This class is used by the replication system to recover after an unclean shutdown, a rollback or
 * during initial sync.
 */
class ReplicationRecovery {
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

class ReplicationRecoveryImpl : public ReplicationRecovery {
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
     * top of oplog.  This can happen on primaries when EMRC=false or in single-node replica sets.
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

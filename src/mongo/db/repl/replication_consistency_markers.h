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

#include "mongo/db/namespace_string.h"

namespace mongo {

class BSONObj;
class OperationContext;
class Timestamp;

namespace repl {

class OpTime;
class StorageInterface;

/**
 * This interface provides helper functions for maintaining the documents used for
 * maintaining data consistency.
 *
 * The minValid document, in 'local.replset.minvalid', is used for indicating whether or not the
 * data on disk is consistent and for getting to a consistent point after unclean shutdown.
 *
 * Example of all fields:
 * {
 *      _id: <ObjectId>,                    // not used, but auto-generated
 *      ts: <Timestamp>,
 *      t: <long long>,                     // timestamp and term of minValid OpTime
 *      doingInitialSync: <bool>,
 *      begin: {
 *                  ts: <Timestamp>,
 *                  t: <long long>
 *             },                           // field for 'appliedThrough'
 * }
 *
 * The oplogTruncateAfterPoint document, in 'local.replset.oplogTruncateAfterPoint', is used to
 * indicate how much of the oplog is consistent and where the oplog should be truncated when
 * entering recovery.
 *
 * Example of all fields:
 * {
 *      _id: 'oplogTruncateAfterPoint',
 *      oplogTruncateAfterPoint: <Timestamp>
 * }
 *
 * See below for explanations of each field.
 *
 * The initialSyncId document, in 'local.replset.initialSyncId', is used to detect when nodes have
 * been resynced. It is set at the end of initial sync, or whenever replication is started when the
 * document does not exist.
 *
 * Example of all fields:
 * {
 *      _id: <UUID>,
 *      wallTime: <Date_t>
 * }
 */
class ReplicationConsistencyMarkers {
    ReplicationConsistencyMarkers(const ReplicationConsistencyMarkers&) = delete;
    ReplicationConsistencyMarkers& operator=(const ReplicationConsistencyMarkers&) = delete;

public:
    // Constructor and Destructor.
    ReplicationConsistencyMarkers();
    virtual ~ReplicationConsistencyMarkers();

    /**
     * Initializes the minValid document with the required fields. This is safe to call on an
     * already initialized minValid document and will add any required fields that do not exist.
     */
    virtual void initializeMinValidDocument(OperationContext* opCtx) = 0;

    // -------- Initial Sync Flag ----------

    /**
     * Returns true if initial sync was started but has not completed. If we start up and this is
     * set to true, we know that we must do a resync.
     */
    virtual bool getInitialSyncFlag(OperationContext* opCtx) const = 0;

    /**
     * Sets the initial sync flag to record that initial sync has not completed.
     *
     * This operation is durable and waits for durable writes (which will block on
     * journaling/checkpointing).
     */
    virtual void setInitialSyncFlag(OperationContext* opCtx) = 0;

    /**
     * Clears the initial sync flag to record that initial sync has completed.
     *
     * This operation is durable and waits for durable writes (which will block on
     * journaling/checkpointing).
     */
    virtual void clearInitialSyncFlag(OperationContext* opCtx) = 0;

    // -------- MinValid ----------

    /**
     * The minValid value is the earliest (minimum) OpTime that must be applied in order to
     * consider the dataset consistent.
     *
     * Returns the minValid OpTime.
     */
    virtual OpTime getMinValid(OperationContext* opCtx) const = 0;

    /**
     * Sets the minValid OpTime to 'minValid'. This can set minValid backwards, which is necessary
     * in rollback when the OpTimes in the oplog may move backwards. We usually only call this
     * function in rollback via refetch, so we need to check the storage engine's rollback method to
     * enforce that via an invariant. However, there are exceptions where we need to set the
     * minValid document outside of rollback with an untimestamped write. In that case, we can
     * ignore the storage engine's rollback method by setting the 'alwaysAllowUntimestampedWrite'
     * parameter to true.
     */
    virtual void setMinValid(OperationContext* opCtx,
                             const OpTime& minValid,
                             bool alwaysAllowUntimestampedWrite = false) = 0;

    // -------- Oplog Truncate After Point ----------

    /**
     * Ensures that the fast-count counter for the oplogTruncateAfterPoint collection is properly
     * set. An unclean shutdown can result in a miscount, if the persisted size store is not updated
     * before the crash. Rollback usually handles this for user collections, but local, unreplicated
     * collections are not adjusted.
     */
    virtual void ensureFastCountOnOplogTruncateAfterPoint(OperationContext* opCtx) = 0;

    /**
     * On startup all oplog entries with a ts field >= the oplog truncate after point will be
     * deleted. If the truncate point is null, no oplog entries are truncated. A null truncate point
     * can be found on startup if the server was certain at the time of shutdown that there were no
     * parallel writes running.
     *
     * Write operations are done in parallel, creating momentary oplog 'holes' where writes at an
     * earlier timestamp are not yet committed. Secondaries can read an oplog entry from a
     * sync-source as soon as there are no holes behind the oplog entry in-memory, but before there
     * are no holes behind the oplog entry on disk. Therefore, after a crash, the oplog is truncated
     * back to its on-disk no holes point that is guaranteed to be consistent with the rest of the
     * replica set.
     *
     * A primary will update the oplog truncate after point before every journal flush to disk with
     * the storage engine tracked in-memory no holes point.
     *
     * For other replication states than PRIMARY, the oplog truncate after point is updated
     * directly. For batch application, the oplog truncate after point is set to the current
     * lastApplied timestamp prior to writing a batch of oplog entries into the oplog, and reset to
     * null once the parallel oplog entry writes are complete.
     *
     * Concurrency control and serialization is the responsibility of the caller.
     *
     * This fasserts on write failure.
     */
    virtual void setOplogTruncateAfterPoint(OperationContext* opCtx,
                                            const Timestamp& timestamp) = 0;
    virtual Timestamp getOplogTruncateAfterPoint(OperationContext* opCtx) const = 0;

    /**
     * Turns updating the OplogTruncateAfterPoint in refreshOplogTruncateAfterPointIfPrimary on/off.
     *
     * Any already running calls to refreshOplogTruncateAfterPointIfPrimary must be interrupted to
     * ensure that the updates to the truncate point via that function have stopped.
     */
    virtual void startUsingOplogTruncateAfterPointForPrimary() = 0;
    virtual void stopUsingOplogTruncateAfterPointForPrimary() = 0;

    /**
     * Indicates whether the oplog truncate after point is currently in use (being periodically
     * refreshed), which is only done while in state PRIMARY.
     *
     * This class stores its own relevant replication state knowledge to avoid potential deadlocks
     * in accessing the replication coordinator's mutex to check; and will remain false for
     * standalones that do not use timestamps.
     */
    virtual bool isOplogTruncateAfterPointBeingUsedForPrimary() const = 0;

    /**
     * Initializes the oplog truncate after point with the timestamp of the latest oplog entry.
     *
     * On stepup to primary, the truncate point must be initialized to protect the window of time
     * between completion of stepup and the first periodic flush to disk that prompts a truncate
     * point update. Otherwise, in-memory writes (with no holes) can replicate while the on-disk
     * writes still have holes, at which point we could crash, leaving this node with unknown data
     * holes that other nodes do not have (they have the data).
     */
    virtual void setOplogTruncateAfterPointToTopOfOplog(OperationContext* opCtx) = 0;

    /**
     * Updates the OplogTruncateAfterPoint with the latest no-holes oplog timestamp.
     *
     * If primary, returns the OpTime and WallTime of the oplog entry associated with the updated
     * oplog truncate after point.
     * Returns boost::none if isOplogTruncateAfterPointBeingUsedForPrimary returns false.
     *
     * stopUsingOplogTruncateAfterPointForPrimary() will cause new calls to this function to do
     * nothing, but any already running callers of this function will need to be interrupted to
     * ensure the state change is in effect (that an update will not racily go ahead).
     *
     * Throws write interruption errors up to the caller.
     */
    virtual boost::optional<OpTimeAndWallTime> refreshOplogTruncateAfterPointIfPrimary(
        OperationContext* opCtx) = 0;

    // -------- Applied Through ----------

    /**
     * The applied through point is a persistent record of which oplog entries we've applied.
     * If we crash while applying a batch of oplog entries, this OpTime tells us where to start
     * applying operations on startup.
     */
    virtual void setAppliedThrough(OperationContext* opCtx, const OpTime& optime) = 0;

    /**
     * Unsets the applied through OpTime.
     * Once cleared, the applied through point is the top of the oplog.
     */
    virtual void clearAppliedThrough(OperationContext* opCtx) = 0;

    /**
     * You should probably be calling ReplicationCoordinator::getLastAppliedOpTime() instead.
     *
     * This reads the value from storage which isn't always updated when the ReplicationCoordinator
     * is. This is safe because it will only ever be stale and reapplying oplog operations is
     * always safe.
     */
    virtual OpTime getAppliedThrough(OperationContext* opCtx) const = 0;

    /**
     * Create the set of collections required for steady-state replication to work. E.g: `minvalid`
     * or `oplogTruncateAfterPoint`.
     */
    virtual Status createInternalCollections(OperationContext* opCtx) = 0;

    /**
     * Set the initial sync ID and wall time if it is not already set.  This will create the
     * collection if necessary.
     */
    virtual void setInitialSyncIdIfNotSet(OperationContext* opCtx) = 0;

    /**
     * Clears the initial sync ID by dropping the collection.
     */
    virtual void clearInitialSyncId(OperationContext* opCtx) = 0;

    /**
     * Returns the initial sync id object, or an empty object if none.
     */
    virtual BSONObj getInitialSyncId(OperationContext* opCtx) = 0;
};

}  // namespace repl
}  // namespace mongo

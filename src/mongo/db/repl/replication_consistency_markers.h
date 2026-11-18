// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class BSONObj;

class OperationContext;
class Timestamp;

namespace repl {

class OpTime;
class StorageInterface;

/**
 * The _id value of the singleton document in 'local.replset.recordIdsGroupId'.
 */
inline constexpr std::string_view kRecordIdsGroupIdDocId = "recordIdsGroupId";

/**
 * A node's membership in a set-wide Replicated RecordIds group: the group it belongs to and the
 * OpTime of the fencing entry that founded that group.
 */
struct RecordIdsGroupMembership {
    UUID groupId;
    OpTime foundingOpTime;

    bool operator==(const RecordIdsGroupMembership&) const = default;
};

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
 *
 * The recordIdsGroupId document, in 'local.replset.recordIdsGroupId', is the per-node membership
 * marker for set-wide Replicated RecordIds. A node honors recordIds carried in the oplog if and
 * only if this document is present. Membership is only ever acquired by a primary minting the
 * groupId (fenced by the new-primary noop or the setFCV upgrade-complete write) or by initial
 * sync inheriting the sync source's marker, and is only ever lost by dropping the collection,
 * fenced at or before the first op applied not honoring recordIds.
 *
 * Example of all fields:
 * {
 *      _id: 'recordIdsGroupId',
 *      groupId: <UUID>,
 *      foundingOpTime: <OpTime>      // the OpTime of the fencing entry that minted the groupId
 * }
 */
class [[MONGO_MOD_OPEN]] ReplicationConsistencyMarkers {
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

    // -------- Replicated RecordIds Group Id ----------

    /**
     * Creates the 'local.replset.recordIdsGroupId' collection if it does not already exist.
     *
     * This is an untimestamped catalog operation that manages its own WriteUnitOfWork, so it must
     * be called outside of any active WriteUnitOfWork, before opening the fencing entry's
     * WriteUnitOfWork that setRecordIdsGroupId is called within.
     *
     * This fasserts on failure.
     */
    virtual void createRecordIdsGroupIdCollection(OperationContext* opCtx) = 0;

    /**
     * Inserts the recordIdsGroupId document {_id: 'recordIdsGroupId', groupId, foundingOpTime},
     * where 'foundingOpTime' is the OpTime of the fencing entry that minted the groupId.
     *
     * Document writes to this collection must always be timestamped so that rollback-to-stable
     * reverts them together with the data. To that end, this must be called within an active
     * WriteUnitOfWork whose commit timestamp is the fencing entry's (the new-primary noop's
     * WriteUnitOfWork on a minting primary, or the setFCV upgrade-complete WriteUnitOfWork), and
     * the collection must already exist (see createRecordIdsGroupIdCollection). This shares the
     * fencing entry's WriteUnitOfWork so that mints are atomic with their fencing entries.
     * Secondaries never acquire a groupId during oplog application. The announced fields in the
     * new-primary noop are read-only for them and only feed the membership comparison, and
     * initial sync adoption writes at completion, outside the application window.
     *
     * A groupId is only ever written while groupless: minted by a groupless primary, or adopted at
     * initial sync completion onto an empty (or cleared) collection. A node that already holds a
     * groupId re-announces it without rewriting it, so writing over an existing document is a
     * logic error and invariants rather than silently overwriting the membership marker.
     *
     * An insert failure from the storage layer fasserts.
     */
    virtual void setRecordIdsGroupId(OperationContext* opCtx,
                                     const UUID& groupId,
                                     const OpTime& foundingOpTime) = 0;

    /**
     * Returns the recordIdsGroupId document, or boost::none if the node holds no groupId (the
     * collection or the document is absent). This reads from storage, so it is safe to call at
     * startup before replication starts.
     *
     * This reads from storage on every call and must not be used per-op. Hot paths (oplog rid
     * emission on the primary, rid honoring in the applier) must instead consult
     * getEffectiveRecordIdsGroupId, the cached membership view that is loaded at startup and
     * updated transactionally at each mint and clear.
     */
    virtual boost::optional<RecordIdsGroupIdDocument> getRecordIdsGroupId(
        OperationContext* opCtx) = 0;

    /**
     * Returns this node's effective Replicated RecordIds group membership, or boost::none if the
     * node is not a member of any group. This is the membership predicate that every consumer
     * (oplog rid emission, rid honoring in the applier, the rid-inclusive dbhash, and the
     * hello/replSetGetStatus reporting surface) must consult. Unlike getRecordIdsGroupId, this
     * never reads directly from storage and is safe to call per op.
     *
     * If the persistence provider requires replicated RecordIds, this returns the sentinel
     * membership: the all-zeros groupId with a null founding OpTime, since no fencing entry
     * founded the group. It can never equal a minted groupId, because UUID::gen() always sets
     * the RFC 4122 version 4 bits.
     *
     * Otherwise, this returns an in-memory cache of the stored marker. The cache must have been
     * loaded by reloadEffectiveRecordIdsGroupId; this tasserts if it has not.
     * setRecordIdsGroupId and clearRecordIdsGroupId update the cache when, and only when, their
     * WriteUnitOfWork commits.
     */
    virtual boost::optional<RecordIdsGroupMembership> getEffectiveRecordIdsGroupId(
        OperationContext* opCtx) const = 0;

    /**
     * Reloads the in-memory effective recordIdsGroupId cache from storage. This reads from storage,
     * except when the persistence provider requires replicated RecordIds: then
     * getEffectiveRecordIdsGroupId always returns the sentinel membership and never consults the
     * cache, so this does nothing.
     *
     * setRecordIdsGroupId and clearRecordIdsGroupId keep the cache up to date on commit. Any other
     * change to the stored marker leaves the cache stale, so callers must reload after it.
     *
     * The storage read and the cache write are not atomic. Callers must therefore guarantee that
     * no mint or clear can commit concurrently with the reload. Otherwise, the reload can
     * overwrite that commit's cache update with the older value it read, silently losing the mint
     * or clear. The current callers, and why each is safe:
     *  - startup, before replication recovery or oplog application (the cache starts unloaded).
     *    Replication has not started, so nothing can mint or clear.
     *  - rollback, after recover-to-stable, which by design rolls the unlogged marker back
     *    along with the data. The node is in ROLLBACK, so it is not primary and cannot mint,
     *    and the oplog buffer has drained, so the applier cannot apply a clear.
     *  - file copy based initial sync, which replaces the marker with the sync source's. Every
     *    storage switch holds the global lock in MODE_X, which excludes any mint or clear.
     */
    virtual void reloadEffectiveRecordIdsGroupId(OperationContext* opCtx) = 0;

    /**
     * Clears the recordIdsGroupId by dropping the collection. Clears are always collection drops,
     * never untimestamped document writes: untimestamped writes to this timestamped document would
     * rewrite its history and break snapshot isolation.
     *
     * Must be called within an active WriteUnitOfWork, and for fenced clears (a mismatched
     * new-primary noop or a setFCV downgrade) that WriteUnitOfWork must commit at the fencing
     * entry's timestamp. On the primary that means riding the fencing write's own WriteUnitOfWork
     * (the setFCV downgrade write). On secondaries the applier wraps the entry in a
     * WriteUnitOfWork committing at the entry's timestamp, so the drop lands at the fencing
     * timestamp even though the oplog insert itself commits on a separate writer thread. The
     * timestamp atomicity is load-bearing: the table is not logged, so rollback-to-stable reverts
     * it to the last stable checkpoint together with the data. If the drop committed in a later
     * WriteUnitOfWork than the fencing entry, a checkpoint covering the entry but not the drop
     * followed by a crash would leave the marker present while oplog replay resumes past the
     * fencing entry, and the node would carry a stale groupId into the new term. Callers without
     * a fencing entry (the flag-off startup clear and the standalone first-write clear) open their
     * own WriteUnitOfWork around the call.
     *
     * It is idempotent. Storage errors fassert. A WriteConflictException escapes to the caller so
     * that the whole fencing WriteUnitOfWork retries; this method must not retry internally
     * because a write conflict requires aborting the fencing entry's other writes as well.
     */
    virtual void clearRecordIdsGroupId(OperationContext* opCtx) = 0;
};

}  // namespace repl
}  // namespace mongo

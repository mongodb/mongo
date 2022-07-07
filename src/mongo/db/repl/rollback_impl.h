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

#include <functional>

#include "mongo/base/status_with.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/repl/rollback.h"
#include "mongo/db/repl/storage_interface.h"

namespace mongo {

class OperationContext;

namespace repl {

class OplogInterface;
class ReplicationCoordinator;
class ReplicationProcess;

/**
 * Tracks statistics about rollback, and is used to generate a summary about what has occurred.
 * Because it is possible for rollback to exit early, fields are initialized to boost::none and are
 * populated with actual values during the rollback process.
 */
struct RollbackStats {
    /**
     * The wall clock time when rollback started.
     */
    Date_t startTime;

    /**
     * The wall clock time when rollback completed, either successfully or unsuccessfully.
     */
    Date_t endTime;

    /**
     * The id number generated for this rollback event.
     */
    boost::optional<int> rollbackId;

    /**
     * The last optime on the branch of history being rolled back.
     */
    boost::optional<OpTime> lastLocalOptime;

    /**
     * The optime of the latest shared oplog entry between this node and the sync source.
     */
    boost::optional<OpTime> commonPoint;

    /**
     * The value of the oplog truncate timestamp. This is the timestamp of the entry immediately
     * after the common point on the local oplog (that is, on the branch of history being rolled
     * back).
     */
    boost::optional<Timestamp> truncateTimestamp;

    /**
     * The value of the stable timestamp to which rollback recovered.
     */
    boost::optional<Timestamp> stableTimestamp;

    /**
     * The directory containing rollback data files, if any were written.
     */
    boost::optional<std::string> rollbackDataFileDirectory;

    /**
     * The last wall clock time on the branch of history being rolled back, if known.
     */
    boost::optional<Date_t> lastLocalWallClockTime;

    /**
     * The wall clock time of the first operation after the common point, if known.
     */
    boost::optional<Date_t> firstOpWallClockTimeAfterCommonPoint;
};

/**
 * During steady state replication, it is possible to find the local server in a state where it
 * cannot replicate from a sync source. This can happen if the local server has gone offline and
 * comes back to find a new primary with an inconsistent set of operations in its oplog from the
 * local server. For example:

 *     F = node that is on the wrong branch of history
 *     SS = sync source (usually primary)
 *
 *     F  : a b c d e f g
 *     SS : a b c d h
 *
 * In the example 'e', 'f', and 'g' are getting rolled back, 'h' is what's getting rolled forward.
 *
 * This class performs 'rollback' via the storage engine's 'recover to a stable timestamp'
 * machinery. This class runs synchronously on the caller's thread.
 *
 * Order of actions:
 *   1. Transition to ROLLBACK.
 *   2. Await background index completion.
 *   3. Find the common point between the local and remote oplogs.
 *       a. Keep track of what is rolled back to provide a summary to the user and to write
 *          rollback files.
 *       b. Maintain a map of how the counts of each collection change during the rollback relative
 *          to the common point.
 *   4. Retrieve the sizes of each collection whose size will change and calculate the
 *      post-rollback size.
 *   5. Increment the Rollback ID (RBID).
 *   6. Write rolled back documents to 'Rollback Files'.
 *   7. Tell the storage engine to recover to the last stable timestamp.
 *   8. Write the oplog entry after the common point as the 'OplogTruncateAfterPoint'.
 *   9. Clear drop pending state.
 *   10. Call recovery code.
 *       a. Truncate the oplog at the common point.
 *       b. Apply all oplog entries to the end of oplog.
 *   11. Correct the counts of any collections whose counts changed.
 *   12. Reset last optimes from the oplog.
 *   13. Trigger the on-rollback op observer.
 *   14. Transition to SECONDARY.
 *
 * If the node crashes while in rollback and the storage engine has not recovered to the last
 * stable timestamp yet, then rollback will simply restart against the new sync source upon restart.
 * If the node crashes after the storage engine has recovered to the last stable timestamp,
 * then the normal startup recovery code will run and finish the rollback process.
 *
 * If the sync source rolls back while we're searching for a common point, the connection should
 * get closed and finding the common point should fail.
 *
 */
class RollbackImpl : public Rollback {
public:
    /**
     * Used to indicate that the files we create with deleted documents are from rollback.
     */
    static constexpr auto kRollbackRemoveSaverType = "rollback";
    static constexpr auto kRollbackRemoveSaverWhy = "removed";

    /**
     * A class with functions that get called throughout rollback. These can be overridden to
     * instrument this class for diagnostics and testing.
     */
    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         * Function called after we transition to ROLLBACK.
         */
        virtual void onTransitionToRollback() noexcept {}

        /**
         * Function called after all background index builds have completed.
         */
        virtual void onBgIndexesComplete() noexcept {}

        /**
         * Function called after we find the common point.
         */
        virtual void onCommonPointFound(Timestamp commonPoint) noexcept {}

        /**
         * Function called after we have incremented the rollback ID.
         */
        virtual void onRollbackIDIncremented() noexcept {}

        /**
         * Function called after a rollback file has been written for each namespace with inserts or
         * updates that are being rolled back.
         */
        virtual void onRollbackFileWrittenForNamespace(UUID, NamespaceString) noexcept {}

        /**
         * Function called after we recover to the stable timestamp.
         * NOTE: This may throw, for testing purposes.
         */
        virtual void onRecoverToStableTimestamp(Timestamp stableTimestamp) {}

        /**
         * Function called after we set the oplog truncate after point.
         */
        virtual void onSetOplogTruncateAfterPoint(Timestamp truncatePoint) noexcept {}

        /**
         * Function called after we recover from the oplog.
         */
        virtual void onRecoverFromOplog() noexcept {}

        /**
         * Function called after we reconstruct prepared transactions.
         */
        virtual void onPreparedTransactionsReconstructed() noexcept {}

        /**
         * Function called after we have triggered the 'onRollback' OpObserver method.
         */
        virtual void onRollbackOpObserver(const OpObserver::RollbackObserverInfo& rbInfo) noexcept {
        }
    };

    /**
     * Creates a RollbackImpl instance that will run the entire rollback algorithm. This is
     * called during steady state replication when we determine that we have to roll back after
     * processing the first batch of oplog entries from the sync source.
     */
    RollbackImpl(OplogInterface* localOplog,
                 OplogInterface* remoteOplog,
                 StorageInterface* storageInterface,
                 ReplicationProcess* replicationProcess,
                 ReplicationCoordinator* replicationCoordinator,
                 Listener* listener);

    /**
     * Constructs RollbackImpl with a default noop listener.
     */
    RollbackImpl(OplogInterface* localOplog,
                 OplogInterface* remoteOplog,
                 StorageInterface* storageInterface,
                 ReplicationProcess* replicationProcess,
                 ReplicationCoordinator* replicationCoordinator);

    virtual ~RollbackImpl();

    /**
     * Runs the rollback algorithm.
     *
     * This method transitions to the ROLLBACK state and then performs the steps of the rollback
     * process. It is required for this method to transition back to SECONDARY before returning,
     * even if rollback did not complete successfully.
     */
    Status runRollback(OperationContext* opCtx);

    /**
     * Cancels all outstanding work.
     */
    void shutdown();

    /**
     * Wrappers to expose private methods for testing.
     */
    StatusWith<std::set<NamespaceString>> _namespacesForOp_forTest(const OplogEntry& oplogEntry) {
        return _namespacesForOp(oplogEntry);
    }

    /**
     * Returns true if the rollback system should write out data files containing documents that
     * will be deleted by rollback.
     */
    static bool shouldCreateDataFiles();

    /**
     * Returns a structure containing all of the documents that would have been written to a
     * rollback data file for the namespace represented by 'uuid'.
     *
     * Only exposed for testing. It is invalid to call this function on a real RollbackImpl.
     */
    virtual const std::vector<BSONObj>& docsDeletedForNamespace_forTest(UUID uuid) const& {
        MONGO_UNREACHABLE;
    }
    void docsDeletedForNamespace_forTest(UUID) && = delete;

protected:
    /**
     * Returns the document with _id 'id' in the namespace 'nss', or boost::none if that document
     * no longer exists in 'nss'. This function is used to write documents to rollback data files,
     * and this function will terminate the server if an unexpected error is returned by the storage
     * interface.
     *
     * This function is protected so that subclasses can access this method for test purposes.
     */
    boost::optional<BSONObj> _findDocumentById(OperationContext* opCtx,
                                               UUID uuid,
                                               NamespaceString nss,
                                               BSONElement id);

    /**
     * Writes a rollback file for the namespace 'nss' containing all of the documents whose _ids are
     * listed in 'idSet'.
     *
     * This function is protected so that subclasses can override it for test purposes.
     */
    virtual void _writeRollbackFileForNamespace(OperationContext* opCtx,
                                                UUID uuid,
                                                NamespaceString nss,
                                                const SimpleBSONObjUnorderedSet& idSet);

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (M)  Reads and writes guarded by _mutex.
    // (N)  Should only ever be accessed by a single thread; no synchronization required.

    // A listener that's called at various points throughout rollback.
    Listener* _listener;  // (R)

private:
    /**
     * Returns if shutdown was called on this rollback process.
     */
    bool _isInShutdown() const;

    /**
     * Finds the common point between the local and remote oplogs.
     */
    StatusWith<RollBackLocalOperations::RollbackCommonPoint> _findCommonPoint(
        OperationContext* opCtx);

    /**
     * Determines whether or not we are trying to roll back too much data. Returns an
     * UnrecoverableRollbackError if we have exceeded the limit.
     */
    Status _checkAgainstTimeLimit(RollBackLocalOperations::RollbackCommonPoint commonPoint);

    /**
     * Kills all user operations currently being performed. Since this node is a secondary, these
     * operations are all reads.
     */
    void _killAllUserOperations(OperationContext* opCtx);

    /**
     * Uses the ReplicationCoordinator to transition the current member state to ROLLBACK.
     * If the transition to ROLLBACK fails, this could mean that we have been elected PRIMARY. In
     * this case, we return a NotSecondary error.
     *
     * 'opCtx' cannot be null.
     */
    Status _transitionToRollback(OperationContext* opCtx);

    /**
     * Stops any active index builds and waits for them to complete. We do this before beginning
     * the rollback process to prevent any issues surrounding index builds pausing/resuming around a
     * call to 'recoverToStableTimestamp'. It's not clear that an index build, resumed in this way,
     * that continues until completion, would be consistent with the collection data. Waiting for
     * all background index builds to complete is a conservative approach, to avoid any of these
     * potential issues.
     */
    void _stopAndWaitForIndexBuilds(OperationContext* opCtx);

    /**
     * Performs a forward scan of the oplog starting at 'stableTimestamp', exclusive. For every
     * retryable write oplog entry that has a 'prevOpTime' <= 'stableTimestamp', update the
     * transactions table with the appropriate information to detail the last executed operation. We
     * do this because derived updates to the transactions table can be coalesced into one
     * operation, and so certain session entry updates may not exist when restoring to the
     * 'stableTimestamp'.
     */
    void _restoreTxnsTableEntryFromRetryableWrites(OperationContext* opCtx,
                                                   Timestamp stableTimestamp);

    /**
     * Recovers to the stable timestamp while holding the global exclusive lock.
     * Returns the stable timestamp that the storage engine recovered to.
     */
    Timestamp _recoverToStableTimestamp(OperationContext* opCtx);

    /**
     * Process a single oplog entry that is getting rolled back and update the necessary rollback
     * info structures. This function assumes that oplog entries are processed in descending
     * timestamp order (that is, starting from the newest oplog entry, going backwards).
     */
    Status _processRollbackOp(OperationContext* opCtx, const OplogEntry& oplogEntry);

    /**
     * Process a single applyOps oplog entry that is getting rolled back.
     * This function processes each sub-operation using _processRollbackOp().
     */
    Status _processRollbackOpForApplyOps(OperationContext* opCtx, const OplogEntry& oplogEntry);

    /**
     * Iterates through the _countDiff map and retrieves the count of the record store pointed to
     * by each UUID. It then saves the post-rollback counts to the _newCounts map.
     */
    Status _findRecordStoreCounts(OperationContext* opCtx);

    /**
     * Executes the phase of rollback between aborting and reconstructing prepared transactions. We
     * cannot safely recover if we fail during this phase.
     */
    void _runPhaseFromAbortToReconstructPreparedTxns(
        OperationContext* opCtx, RollBackLocalOperations::RollbackCommonPoint commonPoint) noexcept;

    /**
     * Sets the record store counts to be the values stored in _newCounts.
     */
    void _correctRecordStoreCounts(OperationContext* opCtx);

    /**
     * Called after we have successfully recovered to the stable timestamp and recovered from the
     * oplog. Triggers the replication rollback OpObserver method, notifying other server subsystems
     * that a rollback has occurred.
     */
    Status _triggerOpObserver(OperationContext* opCtx);

    /**
     * Transitions the current member state from ROLLBACK to SECONDARY.
     * This operation must succeed. Otherwise, we will shut down the server.
     *
     * 'opCtx' cannot be null.
     */
    void _transitionFromRollbackToSecondary(OperationContext* opCtx);

    /**
     * Returns a set of all collection namespaces affected by the given oplog operation. Does not
     * handle 'applyOps' oplog entries, since it assumes their sub operations have already been
     * extracted at a higher layer.
     */
    StatusWith<std::set<NamespaceString>> _namespacesForOp(const OplogEntry& oplogEntry);

    /**
     * Persists rollback files to disk for each namespace that contains documents inserted or
     * updated after the common point, as these changes will be gone after rollback completes.
     * Before each namespace is examined, we check for interrupt and return a non-OK status if
     * shutdown is in progress.
     *
     * This function causes the server to terminate if an error occurs while fetching documents from
     * disk or while writing documents to the rollback file. It must be called before marking the
     * oplog truncate point, and before the storage engine recovers to the stable timestamp.
     */
    Status _writeRollbackFiles(OperationContext* opCtx);

    /**
     * Logs a summary of what has occurred so far during rollback to the server log.
     */
    void _summarizeRollback(OperationContext* opCtx) const;

    /**
     * Aligns the drop pending reaper's state with the catalog.
     */
    void _resetDropPendingState(OperationContext* opCtx);

    // Guards access to member variables.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("RollbackImpl::_mutex");  // (S)

    // Set to true when RollbackImpl should shut down.
    bool _inShutdown = false;  // (M)

    // This is used to read oplog entries from the local oplog that will be rolled back.
    OplogInterface* const _localOplog;  // (R)

    // This is used to read oplog entries from the remote oplog to find the common point.
    OplogInterface* const _remoteOplog;  // (R)

    // The StorageInterface associated with this Rollback instance. Used to execute operations
    // at the storage layer e.g. recovering to a timestamp.
    StorageInterface* _storageInterface;  // (R)

    // The ReplicationProcess associated with this Rollback instance. Used to update and persist
    // various pieces of replication state related to the rollback process.
    ReplicationProcess* _replicationProcess;  // (R)

    // This is used to read and update global replication settings. This includes:
    // - update transition member states;
    ReplicationCoordinator* const _replicationCoordinator;  // (R)

    // Contains information about the rollback that will be passed along to the rollback OpObserver
    // method.
    OpObserver::RollbackObserverInfo _observerInfo = {};  // (N)

    // Holds information about this rollback event.
    RollbackStats _rollbackStats;  // (N)

    // Maintains a count of the difference between the count of the record store pointed to by the
    // UUID before recover to a stable timestamp is called and the count after we recover from the
    // oplog. This only must keep track of inserts and deletes. Rolling back drops is just a rename
    // and rolling back creates means that the UUID does not exist post rollback.
    stdx::unordered_map<UUID, long long, UUID::Hash> _countDiffs;  // (N)

    // Maintains counts and namespaces of drop-pending collections.
    struct PendingDropInfo {
        long long count = 0;
        NamespaceString nss;
    };
    stdx::unordered_map<UUID, PendingDropInfo, UUID::Hash> _pendingDrops;  // (N)

    // Maintains the count of the record store pointed to by the UUID after we recover from the
    // oplog.
    stdx::unordered_map<UUID, long long, UUID::Hash> _newCounts;  // (N)
};

}  // namespace repl
}  // namespace mongo

/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/repl/rollback.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class OperationContext;

namespace repl {

class OplogInterface;
class ReplicationCoordinator;
class ReplicationProcess;

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
 *   1. Transition to ROLLBACK
 *   2. Find the common point between the local and remote oplogs.
 *       a. Keep track of what is rolled back to provide a summary to the user
 *       b. Write rolled back documents to 'Rollback Files'
 *   3. Increment the Rollback ID (RBID)
 *   4. Write the common point as the 'OplogTruncateAfterPoint'
 *   5. Tell the storage engine to recover to the last stable timestamp
 *   6. Call recovery code
 *       a. Truncate the oplog at the common point
 *       b. Apply all oplog entries to the end of oplog.
 *   7. Check the shard identity document for roll back
 *   8. Clear the in-memory transaction table
 *   9. Transition to SECONDARY
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
         * Function called after we recover to the stable timestamp.
         */
        virtual void onRecoverToStableTimestamp(Timestamp stableTimestamp) noexcept {}

        /**
         * Function called after we recover from the oplog.
         */
        virtual void onRecoverFromOplog() noexcept {}

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
     * Finds the timestamp of the record after the common point to put into the oplog truncate
     * after point.
     */
    Timestamp _findTruncateTimestamp(
        OperationContext* opCtx, RollBackLocalOperations::RollbackCommonPoint commonPoint) const;

    /**
     * Uses the ReplicationCoordinator to transition the current member state to ROLLBACK.
     * If the transition to ROLLBACK fails, this could mean that we have been elected PRIMARY. In
     * this case, we return a NotSecondary error.
     *
     * 'opCtx' cannot be null.
     */
    Status _transitionToRollback(OperationContext* opCtx);

    /**
     * Waits for any in-progress background index builds to complete. We do this before beginning
     * the rollback process to prevent any issues surrounding index builds pausing/resuming around a
     * call to 'recoverToStableTimestamp'. It's not clear that an index build, resumed in this way,
     * that continues until completion, would be consistent with the collection data. Waiting for
     * all background index builds to complete is a conservative approach, to avoid any of these
     * potential issues.
     */
    Status _awaitBgIndexCompletion(OperationContext* opCtx);

    /**
     * Recovers to the stable timestamp while holding the global exclusive lock.
     * Returns the stable timestamp that the storage engine recovered to.
     */
    StatusWith<Timestamp> _recoverToStableTimestamp(OperationContext* opCtx);

    /**
     * Runs the oplog recovery logic. This involves applying oplog operations between the stable
     * timestamp and the common point.
     */
    Status _oplogRecovery(OperationContext* opCtx, Timestamp stableTimestamp);

    /**
     * Process a single oplog entry that is getting rolled back and update the necessary rollback
     * info structures.
     */
    Status _processRollbackOp(const OplogEntry& oplogEntry);

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

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (M)  Reads and writes guarded by _mutex.
    // (N)  Should only ever be accessed by a single thread; no synchronization required.

    // Guards access to member variables.
    mutable stdx::mutex _mutex;  // (S)

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

    // A listener that's called at various points throughout rollback.
    Listener* _listener;  // (R)

    // Contains information about the rollback that will be passed along to the rollback OpObserver
    // method.
    OpObserver::RollbackObserverInfo _observerInfo = {};  // (N)
};

}  // namespace repl
}  // namespace mongo

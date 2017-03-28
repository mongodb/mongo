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
#include "mongo/db/repl/abstract_async_component.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/rollback.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class OperationContext;

namespace repl {

class OplogInterface;
class ReplicationCoordinator;
class StorageInterface;

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
 * This class models the logic necessary to perform this 'rollback' procedure in two phases:
 *
 * 1) Information needed to perform the rollback is first read from the sync source and stored
 *    locally. The user-visible server state (all databases except for the 'local' database) is not
 *    changed during this information gathering phase.
 *
 * 2) After the first phase is completed, we read the persisted rollback information during the
 *    second phase and logically undo the local operations to the common point before apply the
 *    remote operations ('h' in the example above) forward until we reach a consistent point
 *    (relative to the sync source).
 */
class RollbackImpl : public AbstractAsyncComponent, public Rollback {
public:
    /**
     * This constructor is used to create a RollbackImpl instance that will run the entire
     * rollback algorithm. This is called during steady state replication when we determine that we
     * have to roll back after processing the first batch of oplog entries from the sync source.
     *
     * To resume an interrupted rollback process at startup, the caller has to use the
     * ReplicationCoordinator to transition the member state to ROLLBACK and invoke
     * readLocalRollbackInfoAndApplyUntilConsistentWithSyncSource() directly.
     */
    RollbackImpl(executor::TaskExecutor* executor,
                 OplogInterface* localOplog,
                 const HostAndPort& syncSource,
                 int requiredRollbackId,
                 ReplicationCoordinator* replicationCoordinator,
                 StorageInterface* storageInterface,
                 const OnCompletionFn& onCompletion);

    virtual ~RollbackImpl();

    /**
     * This is a static function that will run the remaining work in the rollback algorithm
     * (Phase 2) after persisting (in Phase 1) all the information we need from the sync source.
     * This part of the rollback algorithm runs synchronously and may be invoked in one of two
     * contexts:
     * 1) as part of an rollback process started by an in-progress RollbackImpl; or
     * 2) to resume a previously interrupted rollback process at server startup while initializing
     *    the replication subsystem. In this case, RollbackImpl will transition the member state
     *    to ROLLBACK before executing the rest of the rollback procedure.
     *
     * This function does not need to communicate with the sync source.
     *
     * Returns the last optime applied.
     */
    static StatusWith<OpTime> readLocalRollbackInfoAndApplyUntilConsistentWithSyncSource(
        ReplicationCoordinator* replicationCoordinator, StorageInterface* storageInterface);

private:
    /**
     * Schedules the first task of the rollback algorithm, which is to transition to ROLLBACK.
     * Returns ShutdownInProgress if the first task cannot be scheduled because the task executor is
     * shutting down.
     *
     * Called by AbstractAsyncComponent::startup().
     */
    Status _doStartup_inlock() noexcept override;

    /**
     * Cancels all outstanding work.
     *
     * Called by AbstractAsyncComponent::shutdown().
     */
    void _doShutdown_inlock() noexcept override;

    /**
     * Returns mutex to guard this component's state variable.
     *
     * Used by AbstractAyncComponent to protect access to the component state stored in '_state'.
     */
    stdx::mutex* _getMutex() noexcept override;

    /**
     * Rollback flowchart (Incomplete):
     *
     *     _doStartup_inlock()
     *         |
     *         |
     *         V
     *     _transitionToRollbackCallback()
     *         |
     *         |
     *         V
     *    _tearDown()
     *         |
     *         |
     *         V
     *    _finishCallback()
     */

    /**
     * Uses the ReplicationCoordinator to transition the current member state to ROLLBACK.
     * If the transition to ROLLBACK fails, this could mean that we have been elected PRIMARY. In
     * this case, we invoke the completion function with a NotSecondary error.
     *
     * This callback is scheduled by _doStartup_inlock().
     */
    void _transitionToRollbackCallback(const executor::TaskExecutor::CallbackArgs& callbackArgs);

    /**
     * If we detected that we rolled back the shardIdentity document as part of this rollback
     * then we must shut down the server to clear the in-memory ShardingState associated with the
     * shardIdentity document.
     *
     * 'opCtx' cannot be null.
     *
     * Called by _tearDown().
     */
    void _checkShardIdentityRollback(OperationContext* opCtx);

    /**
     * Transitions the current member state from ROLLBACK to SECONDARY.
     * This operation must succeed. Otherwise, we will shut down the server.
     *
     * 'opCtx' cannot be null.
     *
     * Called by _tearDown().
     */
    void _transitionFromRollbackToSecondary(OperationContext* opCtx);

    /**
     * Performs tear down steps before caller is notified of completion status.
     *
     * 'opCtx' cannot be null.
     *
     * Called by _finishCallback() before the completion callback '_onCompletion' is invoked.
     */
    void _tearDown(OperationContext* opCtx);

    /**
     * Invokes completion callback and transitions state to State::kComplete.
     * _finishCallback() may require an OperationContext to perform certain tear down functions.
     * It will create its own OperationContext unless the caller provides one in 'opCtx'.
     *
     * Calls _tearDown() to perform tear down steps before invoking completion callback.
     *
     * Finally, this function transitions the component state to Complete by invoking
     * AbstractAsyncComponent::_transitionToComplete().
     */
    void _finishCallback(OperationContext* opCtx, StatusWith<OpTime> lastApplied);

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (M)  Reads and writes guarded by _mutex.

    // Guards access to member variables.
    mutable stdx::mutex _mutex;  // (S)

    // This is used to read oplog entries from the local oplog that will be rolled back.
    OplogInterface* const _localOplog;  // (R)

    // Host and port of the sync source we are rolling back against.
    const HostAndPort _syncSource;  // (R)

    // This is the current rollback ID on the sync source that we are rolling back against.
    // It is an error if the rollback ID on the sync source changes before rollback is complete.
    const int _requiredRollbackId;  // (R)

    // This is used to read and update global replication settings. This includes:
    // - update transition member states;
    // - update current applied and durable optimes; and
    // - update global rollback ID (that will be returned by the command replSetGetRBID).
    ReplicationCoordinator* const _replicationCoordinator;  // (R)

    // This is used to read and update the global minValid settings and to access the storage layer.
    StorageInterface* const _storageInterface;  // (R)

    // This is invoked with the final status of the rollback. If startup() fails, this callback
    // is never invoked. The caller gets the last applied optime when the rollback completes
    // successfully or an error status.
    // '_onCompletion' is cleared on completion (in _finishCallback()) in order to release any
    // resources that might be held by the callback function object.
    OnCompletionFn _onCompletion;  // (M)

    // Handle to currently scheduled _transitionToRollbackCallback() task.
    executor::TaskExecutor::CallbackHandle _transitionToRollbackHandle;  // (M)
};

}  // namespace repl
}  // namespace mongo

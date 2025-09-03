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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/commit_quorum_options.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

// Indicates which protocol an index build is using.
enum class IndexBuildProtocol {
    /**
     * Refers to the legacy index build protocol for building indexes in replica sets. Index builds
     * must complete on the primary before replicating, and are not resumable in any scenario.
     */
    kSinglePhase,
    /**
     * Refers to the two-phase index build protocol for building indexes in replica sets. Indexes
     * are built simultaneously on all nodes.
     */
    kTwoPhase
};

// Indicates the type of abort or commit signal that will be received by primary and secondaries.
enum class IndexBuildAction {
    /**
     * Does nothing. And, we set on shutdown.
     */
    kNoAction,
    /**
     * Commit signal set by oplog applier.
     */
    kOplogCommit,
    /**
     * Abort signal set by oplog applier.
     */
    kOplogAbort,
    /**
     * Abort signal set on rollback.
     */
    kRollbackAbort,
    /**
     * Abort signal set on initial sync.
     */
    kInitialSyncAbort,
    /**
     * Abort signal set by createIndexes cmd or by drop databases/collections/indexes cmds
     */
    kPrimaryAbort,
    /**
     * Commit signal set by an index build for a single-phase build.
     */
    kSinglePhaseCommit,
    /**
     * Commit signal set by "voteCommitIndexBuild" cmd and step up.
     */
    kCommitQuorumSatisfied
};

/**
 * Returns string representation of IndexBuildAction.
 */
std::string indexBuildActionToString(IndexBuildAction action);

/**
 * Represents the index build state. See _checkIfValidTransition() for valid state transitions.
 */
class IndexBuildState {
public:
    enum State {
        /**
         * Initial state, the index build is registered, but still not completely setup. Setup
         * implies instantiating all the required in-memory state for the index builder thread. For
         * primaries, it also implies persisting the index build entry to
         * 'config.system.indexBuilds' and replicating the 'startIndexBuild' oplog entry.
         */
        kSetup,
        /**
         * This state indicates all in-memory and durable state is prepared, but the index build is
         * not yet in progress. Some additional verification and configuration is performed, during
         * which we might end up with a killed index build thread. Transitioning to this state
         * immediately after setup is crucial to know when it is actually required to perform
         * teardown. An index build in kPostSetup or later is elegible for being aborted by an
         * external thread.
         */
        kPostSetup,
        /**
         * Once an index build is in-progress it is eligible for transition to any of the commit
         * states. It is also abortable.
         */
        kInProgress,
        /**
         * Below state indicates that IndexBuildsCoordinator thread was externally asked to commit.
         * For kApplyCommitOplogEntry, this can come from an oplog entry.
         */
        kApplyCommitOplogEntry,
        /**
         * Below state indicates that index build was successfully able to commit. For kCommitted,
         * the state is set immediately before it commits the index build.
         */
        kCommitted,
        /**
         * Below state indicates that index build was successfully able to abort. For kAborted, this
         * state is set after the build is cleaned up and the abort oplog entry is replicated.
         */
        kAborted,
        /**
         * Indicates that an internal error caused the index build to fail, or that an internal
         * operation or user killOp forced the index build abort by itself. In this state,
         * concurrent external aborts are not allowed. The index builder thread is responsible for
         * handling clean up. If it is determined that voting for abort is allowed, transitions to
         * kAwaitPrimaryAbort. Otherwise it attemps to cleanup directly.
         */
        kFailureCleanUp,
        /**
         * Below state indicates that the index build thread has voted for an abort to the current
         * primary, and is waiting for the index build to actually be aborted either because the
         * command is a loopback to itself (vote issuer is primary itself) or due to
         * 'abortIndexBuild' oplog entry being replicated by the primary. Concurrent external aborts
         * are allowed again (after being disallowed in kFailureCleanUp), as both loopback and
         * 'abortIndexBuild' are external aborts.
         */
        kAwaitPrimaryAbort,
        /**
         * Indicates that an external abort is ongoing. It is the responsibility of the external
         * aborter to clean up the resources.
         */
        kExternalAbort
    };

    /**
     * Transitions this index build to new 'state'.
     * Invariants if the requested transition is not valid and 'skipCheck' is true.
     * 'timestamp', 'abortStatus' may be provided for certain states such as 'commit' and
     * 'abort'.
     */
    void setState(State state,
                  bool skipCheck,
                  boost::optional<Timestamp> timestamp = boost::none,
                  boost::optional<Status> abortStatus = boost::none);

    bool isApplyingCommitOplogEntry() const {
        return _state == kApplyCommitOplogEntry;
    }

    bool isCommitted() const {
        return _state == kCommitted;
    }

    bool isAborted() const {
        return _state == kAborted;
    }

    bool isSettingUp() const {
        return _state == kSetup;
    }

    bool isPostSetup() const {
        return _state == kPostSetup;
    }

    bool isAwaitingPrimaryAbort() const {
        return _state == kAwaitPrimaryAbort;
    }

    bool isFailureCleanUp() const {
        return _state == kFailureCleanUp;
    }

    bool isExternalAbort() const {
        return _state == kExternalAbort;
    }

    bool isAborting() const {
        return _state == kAwaitPrimaryAbort || _state == kFailureCleanUp ||
            _state == kExternalAbort;
    }

    boost::optional<Timestamp> getTimestamp() const {
        return _timestamp;
    }

    boost::optional<std::string> getAbortReason() const {
        return boost::make_optional(!_abortStatus.isOK(), _abortStatus.reason());
    }

    Status getAbortStatus() const {
        return _abortStatus;
    }

    std::string toString() const {
        return toString(_state);
    }

    static std::string toString(State state) {
        switch (state) {
            case kSetup:
                return "Setting up";
            case kPostSetup:
                return "Post setup";
            case kInProgress:
                return "In progress";
            case kApplyCommitOplogEntry:
                return "Applying commit oplog entry";
            case kCommitted:
                return "Committed";
            case kAborted:
                return "Aborted";
            case kAwaitPrimaryAbort:
                return "Await primary abort oplog entry";
            case kFailureCleanUp:
                return "Cleaning up";
            case kExternalAbort:
                return "External abort";
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Appends the current state information of the index build to the builder.
     */
    void appendBuildInfo(BSONObjBuilder* builder) const;

private:
    /**
     * Returns true if the requested IndexBuildState transition is allowed.
     */
    bool _checkIfValidTransition(IndexBuildState::State currentState,
                                 IndexBuildState::State newState) const;

    // Represents the index build state.
    State _state = kSetup;
    // Timestamp will be populated only if the node is secondary.
    // It represents the commit or abort timestamp communicated via
    // commitIndexBuild and abortIndexBuild oplog entry.
    boost::optional<Timestamp> _timestamp;
    // Reason for abort, if any.
    Status _abortStatus = Status::OK();
};

/**
 * Tracks the cross replica set progress of a particular index build identified by a build UUID.
 *
 * This is intended to only be used by the IndexBuildsCoordinator class.
 *
 * TODO: pass in commit quorum setting.
 */
class ReplIndexBuildState {
    ReplIndexBuildState(const ReplIndexBuildState&) = delete;
    ReplIndexBuildState& operator=(const ReplIndexBuildState&) = delete;

public:
    ReplIndexBuildState(const UUID& indexBuildUUID,
                        const UUID& collUUID,
                        const DatabaseName& dbName,
                        std::vector<IndexBuildInfo> indexes,
                        IndexBuildProtocol protocol);

    const std::vector<IndexBuildInfo>& getIndexes() const {
        return _indexes;
    }

    /**
     * The index build thread has been scheduled, from now on it should be possible to interrupt the
     * index build by its opId.
     */
    void onThreadScheduled(OperationContext* opCtx);

    /**
     * The index build setup is complete, but not yet in progress. From now onwards, teardown of
     * index build state must be performed. This makes it eligible to be aborted in 'tryAbort'. Use
     * the current OperationContext's opId as the means for interrupting the index build.
     */
    void completeSetup();

    /**
     * Try to set the index build to in-progress state, does an interrupt check and throws if the
     * build is already killed.
     */
    void setInProgress(OperationContext* opCtx);

    /**
     * Sets '_generateTableWrites' to the specified value.
     */
    void setGenerateTableWrites(bool generateTableWrites);

    /**
     * Returns the value of '_generateTableWrites'.
     */
    bool getGenerateTableWrites() const;

    /**
     * Transition the index build to kFailureCleanUp state if the build isn't already in kAborted,
     * kExternalAbort, or kFailureCleanUp state. In case it already is in an abort state, does
     * nothing and preserves the previous status.
     */
    void setPostFailureState(const Status& status);

    /**
     * Indicate that the index build has attempted to vote for commit readiness. After calling this,
     * the index build cannot vote for abort. Performs an interrupt check, in case the build was
     * concurrently forced to self abort or received a killop, in which case the vote for abort is
     * necessary.
     */
    void setVotedForCommitReadiness(OperationContext* opCtx);

    /**
     * Returns true if this index build can still vote for abort. Voting for abort is not possible
     * after the index build has voted for commit.
     */
    bool canVoteForAbort() const;

    /**
     * This index build has completed successfully and there is no further work to be done.
     */
    void commit(OperationContext* opCtx);

    /**
     * Only for two-phase index builds. Requests the primary to abort the build, and transitions
     * into kAwaitPrimaryAbort state.
     */
    void requestAbortFromPrimary();

    /**
     * Returns timestamp for committing this index build.
     * Returns null timestamp if not set.
     */
    Timestamp getCommitTimestamp() const;

    /**
     * Called when handling a commitIndexIndexBuild oplog entry.
     * This signal can be received during primary (drain phase), secondary,
     * startup (startup recovery) and startup2 (initial sync).
     */
    void onOplogCommit(bool isPrimary) const;

    /**
     * This index build has failed while running in the builder thread due to a non-shutdown reason.
     */
    void completeAbort(OperationContext* opCtx);

    /**
     * This index build was interrupted because the server is shutting down.
     */
    void abortForShutdown(OperationContext* opCtx);

    /**
     * Called when handling an abortIndexIndexBuild oplog entry.
     * This signal can be received during primary (drain phase), secondary,
     * startup (startup recovery) and startup2 (initial sync).
     */
    void onOplogAbort(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Returns true if the index build requires reverting the setup after an abort.
     */
    bool isAbortCleanUpRequired() const;

    /**
     * Returns true if this index build has been aborted.
     */
    bool isAborted() const;

    /**
     * Returns true if this index is in the process of aborting.
     */
    bool isAborting() const;

    /**
     * Returns true if this index build has been committed.
     */
    bool isCommitted() const;

    /**
     * Returns true if this index build is being set up.
     */
    bool isSettingUp() const;

    /**
     * Returns true if this index build is being externally aborted.
     */
    bool isExternalAbort() const;

    /**
     * Returns true if this index build is performing self cleanup.
     */
    bool isFailureCleanUp() const;

    /**
     * Returns abort reason.
     */
    std::string getAbortReason() const;

    /**
     * Returns abort status. Returns Status::OK() if not in aborted state.
     */
    Status getAbortStatus() const;

    /**
     * Called when commit quorum is satisfied.
     */
    void setCommitQuorumSatisfied(OperationContext* opCtx);

    /**
     * Called when we are about to complete a single-phased index build.
     * Single-phase builds don't support commit quorum, but they must go through the process of
     * updating their state to synchronize with concurrent abort operations
     */
    void setSinglePhaseCommit(OperationContext* opCtx);

    /**
     * Attempt to signal the index build to commit and advance the index build to the
     * kApplyCommitOplogEntry state. Returns true if successful and false if the attempt was
     * unsuccessful and the caller should retry.
     */
    bool tryCommit(OperationContext* opCtx);

    /**
     * Attempt to abort an index build. Returns a flag indicating how the caller should proceed.
     */
    enum class TryAbortResult { kRetry, kAlreadyAborted, kNotAborted, kContinueAbort };
    TryAbortResult tryAbort(OperationContext* opCtx,
                            IndexBuildAction signalAction,
                            Status abortStatus);

    /**
     * Force an index build to abort on its own. Will return after signalling the index build or if
     * the index build is already in progress of aborting. Does not wait.
     *
     * Returns true if we signalled the index build. Returns false if we did not, like when the
     * index build is past a point of no return, like committing.
     */
    bool forceSelfAbort(OperationContext* opCtx, const Status& error);

    /**
     * Called when the vote request command is scheduled by the task executor.
     * Skips voting if we have already received commit or abort signal.
     */
    void onVoteRequestScheduled(OperationContext* opCtx,
                                executor::TaskExecutor::CallbackHandle handle);

    /**
     * Clears vote request callback handle set in onVoteRequestScheduled().
     */
    void clearVoteRequestCbk();

    /**
     * (Re-)initializes promise for next action.
     */
    void resetNextActionPromise();


    /**
     * Returns a future that can be used to wait on 'waitForNextAction' for the next action to be
     * available.
     */
    SharedSemiFuture<IndexBuildAction> getNextActionFuture() const;

    /**
     * Gets next action from future if available.
     * Returns boost::none if future is not ready.
     */
    boost::optional<IndexBuildAction> getNextActionNoWait() const;

    /**
     * Called when we are trying to add a new index build 'other' that conflicts with this one.
     * Returns a status that reflects whether this index build has been aborted or still active.
     */
    Status onConflictWithNewIndexBuild(const ReplIndexBuildState& otherIndexBuild,
                                       const std::string& otherIndexName) const;

    /**
     * Accessor and mutator for last optime in the oplog before the interceptors were installed.
     * This supports resumable index builds.
     */
    bool isResumable() const;
    repl::OpTime getLastOpTimeBeforeInterceptors() const;
    void setLastOpTimeBeforeInterceptors(repl::OpTime opTime);
    void clearLastOpTimeBeforeInterceptors();

    /**
     * Appends index build info to builder.
     */
    void appendBuildInfo(BSONObjBuilder* builder) const;

    // Uniquely identifies this index build across replica set members.
    const UUID buildUUID;

    // Identifies the collection for which the index is being built. Collections can be renamed, so
    // the collection UUID is used to maintain correct association.
    const UUID collectionUUID;

    // Identifies the database containing the index being built. Unlike collections, databases
    // cannot be renamed.
    const DatabaseName dbName;

    // Whether to do a two phase index build or a single phase index build like in v4.0. The FCV
    // at the start of the index build will determine this setting.
    const IndexBuildProtocol protocol;

    /*
     * Readers who read the commit quorum value from "config.system.indexBuilds" collection
     * to decide if the commit quorum got satisfied for an index build, should take this lock in
     * shared mode.
     *
     * Writers (setCommitQuorum) who update the commit quorum value of an existing index build
     * entry in "config.system.indexBuilds" collection should take this lock in exclusive mode.
     *
     * Resource mutex will be initialized only for 2 phase index protocol.
     * stdx::mutex lock order:
     * commitQuorumLock -> mutex.
     */
    boost::optional<Lock::ResourceMutex> commitQuorumLock;

    struct IndexCatalogStats {
        int numIndexesBefore = 0;
        int numIndexesAfter = 0;
    };

    // Tracks the index build stats that are returned to the caller upon success.
    // Used only by the thread pool task for the index build. No synchronization necessary.
    IndexCatalogStats stats;

    // Communicates the final outcome of the index build to any callers waiting upon the associated
    // SharedSemiFuture(s).
    SharedPromise<IndexCatalogStats> sharedPromise;

private:
    /*
     * Determines whether to skip the index build state transition check.
     * Index builder not using ReplIndexBuildState::waitForNextAction to signal primary and
     * secondaries to commit or abort signal will violate index build state transition. So, we
     * should skip state transition verification. Otherwise, we would invariant.
     */
    bool _shouldSkipIndexBuildStateTransitionCheck(OperationContext* opCtx) const;

    /**
     * Updates the next action signal and cancels the vote request under lock.
     * Used by IndexBuildsCoordinatorMongod only.
     */
    void _setSignalAndCancelVoteRequestCbkIfActive(WithLock lk,
                                                   OperationContext* opCtx,
                                                   IndexBuildAction signal);

    /**
     * Clears vote request callback handle set in onVoteRequestScheduled().
     */
    void _clearVoteRequestCbk(WithLock);

    /**
     * Cancels the vote request if valid and clears its callback handle.
     */
    void _cancelAndClearVoteRequestCbk(WithLock, OperationContext* opCtx);

    // Tracks all the indexes being built.
    const std::vector<IndexBuildInfo> _indexes;

    // Protects the state below.
    mutable stdx::mutex _mutex;

    // Primary and secondaries gets their commit or abort signal via this promise future pair.
    std::unique_ptr<SharedPromise<IndexBuildAction>> _waitForNextAction;

    // Maintains the state of the index build.
    IndexBuildState _indexBuildState;

    // TODO(SERVER-110172): Remove this field.
    // Indicates whether this node should produce any table writes during the index build. When
    // this is false, it means that this node is a secondary and is only applying writes received
    // from the primary via the oplog.
    bool _generateTableWrites{true};

    // Represents the callback handle for scheduled remote command "voteCommitIndexBuild".
    executor::TaskExecutor::CallbackHandle _voteCmdCbkHandle;

    // The OperationId of the index build. This allows external callers to interrupt the index build
    // thread. Initialized in start() as we transition from setup to in-progress.
    boost::optional<OperationId> _opId;

    // The last optime in the oplog before the interceptors were installed. If this is a single
    // phase index build, isn't running a hybrid index build, or isn't running during oplog
    // application, this will be null.
    repl::OpTime _lastOpTimeBeforeInterceptors;

    // Set once setup is complete, indicating that a clean up is required in case of abort.
    bool _cleanUpRequired = false;

    // Set once before attempting to vote for commit readiness.
    bool _votedForCommitReadiness = false;
};

}  // namespace mongo

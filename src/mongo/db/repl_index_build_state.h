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

#include <algorithm>
#include <list>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

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
 * Represents the index build state.
 * Valid State transition for primary:
 * ===================================
 * kSetup -> kInProgress
 * kInProgress -> kAborted    // An index build failed due to an indexing error or was aborted
 *                               externally.
 * kInProgress -> kCommitted  // An index build was committed

 *
 * Valid State transition for secondaries:
 * =======================================
 * kSetup -> kInProgress
 * kInProgress -> kAborted    // An index build received an abort oplog entry
 * kInProgress -> kPrepareCommit -> kCommitted // An index build received a commit oplog entry
 */
class IndexBuildState {
public:
    enum StateFlag {
        kSetup = 1 << 0,
        /**
         * Once an index build is in-progress it is eligible for being aborted by an external
         * thread. The kSetup state prevents other threads from observing an inconsistent state of
         * a build until it transitions to kInProgress.
         */
        kInProgress = 1 << 1,
        /**
         * Below state indicates that IndexBuildsCoordinator thread was externally asked to commit.
         * For kPrepareCommit, this can come from an oplog entry.
         */
        kPrepareCommit = 1 << 2,
        /**
         * Below state indicates that index build was successfully able to commit or abort. For
         * kCommitted, the state is set immediately before it commits the index build. For
         * kAborted, this state is set after the build is cleaned up and the abort oplog entry is
         * replicated.
         */
        kCommitted = 1 << 3,
        kAborted = 1 << 4,
    };

    /**
     * Transitions this index build to new 'state'.
     * Invariants if the requested transition is not valid and 'skipCheck' is true.
     * 'timestamp' and 'abortReason' may be provided for certain states such as 'commit' and
     * 'abort'.
     */
    void setState(StateFlag state,
                  bool skipCheck,
                  boost::optional<Timestamp> timestamp = boost::none,
                  boost::optional<std::string> abortReason = boost::none);

    bool isCommitPrepared() const {
        return _state == kPrepareCommit;
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

    boost::optional<Timestamp> getTimestamp() const {
        return _timestamp;
    }

    boost::optional<std::string> getAbortReason() const {
        return _abortReason;
    }

    std::string toString() const {
        return toString(_state);
    }

    static std::string toString(StateFlag state) {
        switch (state) {
            case kSetup:
                return "Setting up";
            case kInProgress:
                return "In progress";
            case kPrepareCommit:
                return "Prepare commit";
            case kCommitted:
                return "Committed";
            case kAborted:
                return "Aborted";
        }
        MONGO_UNREACHABLE;
    }

private:
    // Represents the index build state.
    StateFlag _state = kSetup;
    // Timestamp will be populated only if the node is secondary.
    // It represents the commit or abort timestamp communicated via
    // commitIndexBuild and abortIndexBuild oplog entry.
    boost::optional<Timestamp> _timestamp;
    // Reason for abort reason.
    boost::optional<std::string> _abortReason;
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
                        const std::string& dbName,
                        const std::vector<BSONObj>& specs,
                        IndexBuildProtocol protocol);

    /**
     * The index build is now past the setup stage and in progress. This makes it eligible to be
     * aborted. Use the current OperationContext's opId as the means for interrupting the index
     * build.
     */
    void start(OperationContext* opCtx);

    /**
     * This index build has completed successfully and there is no further work to be done.
     */
    void commit(OperationContext* opCtx);

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
    void abortSelf(OperationContext* opCtx);

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
     * Returns true if this index build has been aborted.
     */
    bool isAborted() const;

    /**
     * Returns abort reason. Invariants if not in aborted state.
     */
    std::string getAbortReason() const;

    /**
     * Called when commit quorum is satisfied.
     * Invokes 'onCommitQuorumSatisfied' if state is successfully transitioned to commit quorum
     * satisfied.
     */
    void setCommitQuorumSatisfied(OperationContext* opCtx);

    /**
     * Called when we are about to complete a single-phased index build.
     * Single-phase builds don't support commit quorum, but they must go through the process of
     * updating their state to synchronize with concurrent abort operations
     */
    void setSinglePhaseCommit(OperationContext* opCtx);

    /**
     * Attempt to signal the index build to commit and advance the index build to the kPrepareCommit
     * state.
     * Returns true if successful and false if the attempt was unnecessful and the caller should
     * retry.
     */
    bool tryCommit(OperationContext* opCtx);

    /**
     * Attempt to abort an index build. Returns a flag indicating how the caller should proceed.
     */
    enum class TryAbortResult { kRetry, kAlreadyAborted, kNotAborted, kContinueAbort };
    TryAbortResult tryAbort(OperationContext* opCtx,
                            IndexBuildAction signalAction,
                            std::string reason);

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

    // Uniquely identifies this index build across replica set members.
    const UUID buildUUID;

    // Identifies the collection for which the index is being built. Collections can be renamed, so
    // the collection UUID is used to maintain correct association.
    const UUID collectionUUID;

    // Identifies the database containing the index being built. Unlike collections, databases
    // cannot be renamed.
    const std::string dbName;

    // The names of the indexes being built.
    const std::vector<std::string> indexNames;

    // The specs of the index(es) being built. Facilitates new callers joining an active index
    // build.
    const std::vector<BSONObj> indexSpecs;

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
     * Mutex lock order:
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

    // Protects the state below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ReplIndexBuildState::_mutex");

    // Primary and secondaries gets their commit or abort signal via this promise future pair.
    std::unique_ptr<SharedPromise<IndexBuildAction>> _waitForNextAction;

    // Maintains the state of the index build.
    IndexBuildState _indexBuildState;

    // Represents the callback handle for scheduled remote command "voteCommitIndexBuild".
    executor::TaskExecutor::CallbackHandle _voteCmdCbkHandle;

    // The OperationId of the index build. This allows external callers to interrupt the index build
    // thread. Initialized in start() as we transition from setup to in-progress.
    boost::optional<OperationId> _opId;

    // The last optime in the oplog before the interceptors were installed. If this is a single
    // phase index build, isn't running a hybrid index build, or isn't running during oplog
    // application, this will be null.
    repl::OpTime _lastOpTimeBeforeInterceptors;
};

}  // namespace mongo

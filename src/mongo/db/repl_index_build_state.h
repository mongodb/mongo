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

    using StateSet = int;
    bool isSet(StateSet stateSet) const {
        return _state & stateSet;
    }

    bool checkIfValidTransition(StateFlag newState) {
        if ((_state == kSetup && newState == kInProgress) ||
            (_state == kInProgress && newState != kSetup) ||
            (_state == kPrepareCommit && newState == kCommitted)) {
            return true;
        }
        return false;
    }

    void setState(StateFlag state,
                  bool skipCheck,
                  boost::optional<Timestamp> timestamp = boost::none,
                  boost::optional<std::string> abortReason = boost::none) {
        if (!skipCheck) {
            invariant(checkIfValidTransition(state),
                      str::stream() << "current state :" << toString(_state)
                                    << ", new state: " << toString(state));
        }
        _state = state;
        if (timestamp)
            _timestamp = timestamp;
        if (abortReason) {
            invariant(_state == kAborted);
            _abortReason = abortReason;
        }
    }

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
struct ReplIndexBuildState {
    ReplIndexBuildState(const UUID& indexBuildUUID,
                        const UUID& collUUID,
                        const std::string& dbName,
                        const std::vector<BSONObj>& specs,
                        IndexBuildProtocol protocol)
        : buildUUID(indexBuildUUID),
          collectionUUID(collUUID),
          dbName(dbName),
          indexNames(extractIndexNames(specs)),
          indexSpecs(specs),
          protocol(protocol) {
        waitForNextAction = std::make_unique<SharedPromise<IndexBuildAction>>();
        if (protocol == IndexBuildProtocol::kTwoPhase)
            commitQuorumLock.emplace(indexBuildUUID.toString());
    }

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

    // Protects the state below.
    mutable Mutex mutex = MONGO_MAKE_LATCH("ReplIndexBuildState::mutex");

    // The OperationId of the index build. This allows external callers to interrupt the index build
    // thread.
    OperationId opId;

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

    using IndexCatalogStats = struct {
        int numIndexesBefore = 0;
        int numIndexesAfter = 0;
    };

    // Tracks the index build stats that are returned to the caller upon success.
    IndexCatalogStats stats;

    // Communicates the final outcome of the index build to any callers waiting upon the associated
    // SharedSemiFuture(s).
    SharedPromise<IndexCatalogStats> sharedPromise;

    // Primary and secondaries gets their commit or abort signal via this promise future pair.
    std::unique_ptr<SharedPromise<IndexBuildAction>> waitForNextAction;

    // Maintains the state of the index build.
    IndexBuildState indexBuildState;

    // Represents the callback handle for scheduled remote command "voteCommitIndexBuild".
    executor::TaskExecutor::CallbackHandle voteCmdCbkHandle;

private:
    std::vector<std::string> extractIndexNames(const std::vector<BSONObj>& specs) {
        std::vector<std::string> indexNames;
        for (const auto& spec : specs) {
            std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
            invariant(!name.empty(),
                      str::stream()
                          << "Bad spec passed into ReplIndexBuildState constructor, missing '"
                          << IndexDescriptor::kIndexNameFieldName << "' field: " << spec);
            indexNames.push_back(name);
        }
        return indexNames;
    }
};

}  // namespace mongo

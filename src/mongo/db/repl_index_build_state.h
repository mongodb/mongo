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
 * kNone ---> kAborted.
 * kNone ---> kPrepareAbort ---> kAborted.
 *
 * Valid State transition for secondaries:
 * =======================================
 * kNone ---> kPrepareCommit ---> kCommitted.
 * kNone ---> kPrepareAbort ---> kAborted.
 */
class IndexBuildState {
public:
    enum StateFlag {
        kNone = 1 << 0,
        /**
         * Below state indicates that indexBuildscoordinator thread was externally asked either to
         * commit or abort. Oplog applier, rollback, createIndexes command and drop
         * databases/collections/indexes cmds can change this state to kPrepareCommit or
         * kPrepareAbort.
         */
        kPrepareCommit = 1 << 1,
        kPrepareAbort = 1 << 2,
        /**
         * Below state indicates that index build was successfully able to commit or abort. And,
         * it's yet to generate the commitIndexBuild or abortIndexBuild oplog entry respectively.
         */
        kCommitted = 1 << 3,
        kAborted = 1 << 4
    };

    using StateSet = int;
    bool isSet(StateSet stateSet) const {
        return _state & stateSet;
    }

    bool checkIfValidTransition(StateFlag newState) {
        if (_state == kNone || (_state == kPrepareCommit && newState == kCommitted) ||
            (_state == kPrepareAbort && newState == kAborted)) {
            return true;
        }
        return false;
    }

    void setState(StateFlag state,
                  bool skipCheck,
                  boost::optional<Timestamp> timestamp = boost::none,
                  boost::optional<std::string> abortReason = boost::none) {
        // TODO SERVER-46560: Should remove the hard-coded value skipCheck 'true'.
        skipCheck = true;
        if (!skipCheck) {
            invariant(checkIfValidTransition(state),
                      str::stream() << "current state :" << toString(_state)
                                    << ", new state: " << toString(state));
        }
        _state = state;
        if (timestamp)
            _timestamp = timestamp;
        if (abortReason) {
            invariant(_state == kPrepareAbort);
            _abortReason = abortReason;
        }
    }

    bool isCommitPrepared() const {
        return _state == kPrepareCommit;
    }

    bool isCommitted() const {
        return _state == kCommitted;
    }

    bool isAbortPrepared() const {
        return _state == kPrepareAbort;
    }

    bool isAborted() const {
        return _state == kAborted;
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
            case kNone:
                return "in-progress";
            case kPrepareCommit:
                return "Prepare commit";
            case kCommitted:
                return "Committed";
            case kPrepareAbort:
                return "Prepare abort";
            case kAborted:
                return "Aborted";
        }
        MONGO_UNREACHABLE;
    }

private:
    // Represents the index build state.
    StateFlag _state = kNone;
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
                        IndexBuildProtocol protocol,
                        boost::optional<CommitQuorumOptions> commitQuorum)
        : buildUUID(indexBuildUUID),
          collectionUUID(collUUID),
          dbName(dbName),
          indexNames(extractIndexNames(specs)),
          indexSpecs(specs),
          protocol(protocol),
          commitQuorum(commitQuorum) {
        waitForNextAction = std::make_unique<SharedPromise<IndexBuildAction>>();
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

    // Secondaries do not set this information, so it is only set on primaries or on
    // transition to primary.
    boost::optional<CommitQuorumOptions> commitQuorum;

    // Tracks the members of the replica set that have finished building the index(es) and are ready
    // to commit the index(es).
    std::vector<HostAndPort> commitReadyMembers;

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

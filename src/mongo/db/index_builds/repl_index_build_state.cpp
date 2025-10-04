/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/index_builds/repl_index_build_state.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

/**
 * Parses index specs to generate list of index names for ReplIndexBuildState initialization.
 */
std::vector<std::string> extractIndexNames(const std::vector<BSONObj>& specs) {
    std::vector<std::string> indexNames;
    for (const auto& spec : specs) {
        std::string name = std::string{spec.getStringField(IndexDescriptor::kIndexNameFieldName)};
        invariant(!name.empty(),
                  str::stream() << "Bad spec passed into ReplIndexBuildState constructor, missing '"
                                << IndexDescriptor::kIndexNameFieldName << "' field: " << spec);
        indexNames.push_back(name);
    }
    return indexNames;
}

}  // namespace

std::string indexBuildActionToString(IndexBuildAction action) {
    switch (action) {
        case IndexBuildAction::kNoAction:
            return "No action";
        case IndexBuildAction::kOplogCommit:
            return "Oplog commit";
        case IndexBuildAction::kOplogAbort:
            return "Oplog abort";
        case IndexBuildAction::kInitialSyncAbort:
            return "Initial sync abort";
        case IndexBuildAction::kRollbackAbort:
            return "Rollback abort";
        case IndexBuildAction::kPrimaryAbort:
            return "Primary abort";
        case IndexBuildAction::kSinglePhaseCommit:
            return "Single-phase commit";
        case IndexBuildAction::kCommitQuorumSatisfied:
            return "Commit quorum Satisfied";
    }
    MONGO_UNREACHABLE;
}

void IndexBuildState::setState(State state,
                               bool skipCheck,
                               boost::optional<Timestamp> timestamp,
                               boost::optional<Status> abortStatus) {
    if (!skipCheck) {
        invariant(_checkIfValidTransition(_state, state),
                  str::stream() << "current state :" << toString(_state)
                                << ", new state: " << toString(state));
    }
    LOGV2_DEBUG(6826201,
                1,
                "Index build: transitioning state",
                "current"_attr = toString(_state),
                "new"_attr = toString(state));
    _state = state;
    if (timestamp)
        _timestamp = timestamp;
    if (abortStatus) {
        invariant(_state == kAborted || _state == kFailureCleanUp || _state == kExternalAbort);
        _abortStatus = *abortStatus;
    }
}

bool IndexBuildState::_checkIfValidTransition(IndexBuildState::State currentState,
                                              IndexBuildState::State newState) const {
    switch (currentState) {
        case IndexBuildState::State::kSetup:
            return
                // Normal case.
                newState == IndexBuildState::State::kPostSetup ||
                // Setup failed on a primary, proceed to cleanup. At this point cleanup only
                // requires unregistering.
                newState == IndexBuildState::State::kFailureCleanUp;

        case IndexBuildState::State::kPostSetup:
            return
                // Normal case.
                newState == IndexBuildState::State::kInProgress ||
                // The index build was aborted, and the caller took responsibility for cleanup.
                newState == IndexBuildState::State::kExternalAbort ||
                // Internal failure or interrupted (user killOp, disk space monitor, or shutdown).
                newState == IndexBuildState::State::kFailureCleanUp;

        case IndexBuildState::State::kInProgress:
            return
                // Index build was successfully committed by the primary.
                newState == IndexBuildState::State::kCommitted ||
                // As a secondary, we received a commit oplog entry.
                newState == IndexBuildState::State::kApplyCommitOplogEntry ||
                // The index build was aborted, and the caller took responsibility for cleanup.
                newState == IndexBuildState::State::kExternalAbort ||
                // Internal failure or interrupted (user killOp, disk space monitor, or shutdown).
                newState == IndexBuildState::State::kFailureCleanUp;

        case IndexBuildState::State::kApplyCommitOplogEntry:
            return
                // We successfully committed the index build as a secondary.
                newState == IndexBuildState::State::kCommitted;

        case IndexBuildState::State::kAwaitPrimaryAbort:
            return
                // Abort for shutdown.
                newState == IndexBuildState::State::kAborted ||
                // The oplog applier is externally aborting the index build while applying
                // 'abortIndexBuild'.
                newState == IndexBuildState::State::kExternalAbort;

        case IndexBuildState::State::kFailureCleanUp:
            return
                // A primary node completed self-abort or abort for shutdown.
                newState == IndexBuildState::State::kAborted ||
                // We are waiting for the current primary to abort the index build.
                newState == IndexBuildState::State::kAwaitPrimaryAbort;

        case IndexBuildState::State::kExternalAbort:
            // The external aborter has finished cleaned up the index build.
            return newState == IndexBuildState::State::kAborted;

        case IndexBuildState::State::kAborted:
            return false;

        case IndexBuildState::State::kCommitted:
            return false;
    }
    MONGO_UNREACHABLE;
}


void IndexBuildState::appendBuildInfo(BSONObjBuilder* builder) const {
    BSONObjBuilder stateBuilder;
    stateBuilder.append("state", toString());
    if (auto ts = getTimestamp()) {
        stateBuilder.append("timestamp", *ts);
    }
    if (auto abortStatus = getAbortStatus(); !abortStatus.isOK()) {
        abortStatus.serializeErrorToBSON(&stateBuilder);
    }
    builder->append("replicationState", stateBuilder.obj());
}

ReplIndexBuildState::ReplIndexBuildState(const UUID& indexBuildUUID,
                                         const UUID& collUUID,
                                         const DatabaseName& dbName,
                                         std::vector<IndexBuildInfo> indexes,
                                         IndexBuildProtocol protocol)
    : buildUUID(indexBuildUUID),
      collectionUUID(collUUID),
      dbName(dbName),
      protocol(protocol),
      _indexes(std::move(indexes)) {
    _waitForNextAction = std::make_unique<SharedPromise<IndexBuildAction>>();
    if (protocol == IndexBuildProtocol::kTwoPhase)
        commitQuorumLock.emplace(indexBuildUUID.toString());
}

void ReplIndexBuildState::onThreadScheduled(OperationContext* opCtx) {
    stdx::lock_guard lk(_mutex);
    _opId = opCtx->getOpID();
}

void ReplIndexBuildState::completeSetup() {
    stdx::lock_guard lk(_mutex);
    _indexBuildState.setState(IndexBuildState::kPostSetup, false /* skipCheck */);
    _cleanUpRequired = true;
}

void ReplIndexBuildState::setInProgress(OperationContext* opCtx) {
    stdx::lock_guard lk(_mutex);
    // The index build might have been aborted/interrupted before reaching this point. Trying to
    // transtion to kInProgress would be an error.
    opCtx->checkForInterrupt();
    _indexBuildState.setState(IndexBuildState::kInProgress, false /* skipCheck */);
}

void ReplIndexBuildState::setGenerateTableWrites(bool generateTableWrites) {
    stdx::lock_guard lk(_mutex);
    _generateTableWrites = generateTableWrites;
}

bool ReplIndexBuildState::getGenerateTableWrites() const {
    stdx::lock_guard lk(_mutex);
    return _generateTableWrites;
}

void ReplIndexBuildState::setPostFailureState(const Status& status) {
    stdx::lock_guard lk(_mutex);
    if (_indexBuildState.isFailureCleanUp() || _indexBuildState.isExternalAbort() ||
        _indexBuildState.isAborted()) {
        LOGV2_DEBUG(7693500,
                    1,
                    "Index build: already in an abort handling state",
                    "state"_attr = _indexBuildState.toString());
        return;
    }

    // It is possible that a 'commitIndexBuild' oplog entry is applied while the index builder is
    // transitioning to an abort, or even to have been in a state where the oplog applier is already
    // waiting for the index build to finish.
    if (_indexBuildState.isApplyingCommitOplogEntry()) {
        LOGV2_FATAL(7329407,
                    "Trying to abort an index build while a 'commitIndexBuild' oplog entry is "
                    "being applied. The primary has already committed the build, but this node is "
                    "trying to abort it. This is an inconsistent state we cannot recover from.",
                    "buildUUID"_attr = buildUUID);
    }

    _indexBuildState.setState(
        IndexBuildState::kFailureCleanUp, false /* skipCheck */, boost::none, status);
}

void ReplIndexBuildState::setVotedForCommitReadiness(OperationContext* opCtx) {
    stdx::lock_guard lk(_mutex);
    invariant(!_votedForCommitReadiness);
    opCtx->checkForInterrupt();
    _votedForCommitReadiness = true;
}

bool ReplIndexBuildState::canVoteForAbort() const {
    stdx::lock_guard lk(_mutex);
    return !_votedForCommitReadiness;
}

void ReplIndexBuildState::commit(OperationContext* opCtx) {
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [this, skipCheck](OperationContext*, boost::optional<Timestamp>) {
            stdx::lock_guard lk(_mutex);
            _indexBuildState.setState(IndexBuildState::kCommitted, skipCheck);
        });
}

void ReplIndexBuildState::requestAbortFromPrimary() {
    invariant(protocol == IndexBuildProtocol::kTwoPhase);
    stdx::lock_guard lk(_mutex);
    _indexBuildState.setState(IndexBuildState::kAwaitPrimaryAbort, false /* skipCheck */);
}

Timestamp ReplIndexBuildState::getCommitTimestamp() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.getTimestamp().value_or(Timestamp());
}

void ReplIndexBuildState::onOplogCommit(bool isPrimary) const {
    stdx::lock_guard lk(_mutex);
    invariant(!isPrimary && _indexBuildState.isApplyingCommitOplogEntry(),
              str::stream() << "Index build: " << buildUUID
                            << ",  index build state: " << _indexBuildState.toString());
}

void ReplIndexBuildState::completeAbort(OperationContext* opCtx) {
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);
    stdx::lock_guard lk(_mutex);
    _indexBuildState.setState(IndexBuildState::kAborted, skipCheck);
}

void ReplIndexBuildState::abortForShutdown(OperationContext* opCtx) {
    // Promise should be set at least once before it's getting destroyed.
    stdx::lock_guard lk(_mutex);
    if (!_waitForNextAction->getFuture().isReady()) {
        _waitForNextAction->emplaceValue(IndexBuildAction::kNoAction);
    }
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);
    _indexBuildState.setState(IndexBuildState::kAborted, skipCheck);
}

void ReplIndexBuildState::onOplogAbort(OperationContext* opCtx, const NamespaceString& nss) const {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isPrimary = replCoord->canAcceptWritesFor(opCtx, nss) &&
        !replCoord->getSettings().shouldRecoverFromOplogAsStandalone() &&
        !storageGlobalParams.magicRestore;
    invariant(!isPrimary, str::stream() << "Index build: " << buildUUID);

    stdx::lock_guard lk(_mutex);
    invariant(_indexBuildState.isExternalAbort(),
              str::stream() << "Index build: " << buildUUID
                            << ",  index build state: " << _indexBuildState.toString());
    invariant(_indexBuildState.getTimestamp() && _indexBuildState.getAbortReason(),
              buildUUID.toString());
    LOGV2(3856206,
          "Aborting index build from oplog entry",
          "buildUUID"_attr = buildUUID,
          "abortTimestamp"_attr = _indexBuildState.getTimestamp().value(),
          "abortReason"_attr = _indexBuildState.getAbortReason().value(),
          "collectionUUID"_attr = collectionUUID);
}

bool ReplIndexBuildState::isAbortCleanUpRequired() const {
    stdx::lock_guard lk(_mutex);
    // Cleanup is required for external aborts if setup stage completed at some point in the past.
    return _cleanUpRequired;
}

bool ReplIndexBuildState::isAborted() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.isAborted();
}

bool ReplIndexBuildState::isAborting() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.isAborting();
}

bool ReplIndexBuildState::isCommitted() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.isCommitted();
}

bool ReplIndexBuildState::isSettingUp() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.isSettingUp();
}

bool ReplIndexBuildState::isExternalAbort() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.isExternalAbort();
}

bool ReplIndexBuildState::isFailureCleanUp() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.isFailureCleanUp();
}

std::string ReplIndexBuildState::getAbortReason() const {
    stdx::lock_guard lk(_mutex);
    invariant(_indexBuildState.isAborted() || _indexBuildState.isAborting(),
              str::stream() << "Index build: " << buildUUID
                            << ",  index build state: " << _indexBuildState.toString());
    auto reason = _indexBuildState.getAbortReason();
    invariant(reason, str::stream() << buildUUID);
    return *reason;
}

Status ReplIndexBuildState::getAbortStatus() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.getAbortStatus();
}

void ReplIndexBuildState::setCommitQuorumSatisfied(OperationContext* opCtx) {
    stdx::lock_guard lk(_mutex);
    if (!_waitForNextAction->getFuture().isReady()) {
        _setSignalAndCancelVoteRequestCbkIfActive(
            lk, opCtx, IndexBuildAction::kCommitQuorumSatisfied);
    } else {
        // This implies we already got a commit or abort signal by other ways. This might have
        // been signaled earlier with kPrimaryAbort or kCommitQuorumSatisfied. Or, it's also
        // possible the node got stepped down and received kOplogCommit/koplogAbort or got
        // kRollbackAbort. So, it's ok to skip signaling.
        auto action = _waitForNextAction->getFuture().get(opCtx);

        LOGV2(3856200,
              "Skipping signaling as it was previously signaled for index build",
              "skippedAction"_attr =
                  indexBuildActionToString(IndexBuildAction::kCommitQuorumSatisfied),
              "previousAction"_attr = indexBuildActionToString(action),
              "buildUUID"_attr = buildUUID);
    }
}

void ReplIndexBuildState::setSinglePhaseCommit(OperationContext* opCtx) {
    stdx::lock_guard lk(_mutex);
    if (_waitForNextAction->getFuture().isReady()) {
        // If the signal action has been set, it should only be because a concurrent operation
        // already aborted the index build.
        auto action = _waitForNextAction->getFuture().get(opCtx);
        invariant(action == IndexBuildAction::kPrimaryAbort,
                  str::stream() << "action: " << indexBuildActionToString(action)
                                << ", buildUUID: " << buildUUID);
        LOGV2(4639700,
              "Not committing single-phase build because it has already been aborted",
              "buildUUID"_attr = buildUUID);
        return;
    }
    _waitForNextAction->emplaceValue(IndexBuildAction::kSinglePhaseCommit);
}

bool ReplIndexBuildState::tryCommit(OperationContext* opCtx) {
    stdx::lock_guard lk(_mutex);
    if (_indexBuildState.isSettingUp() || _indexBuildState.isPostSetup()) {
        // It's possible that the index build thread has not reached the point where it can be
        // committed yet.
        return false;
    }

    // If the node is secondary, and awaiting a primary abort, the transition is invalid, and the
    // node should crash.
    if (_indexBuildState.isAwaitingPrimaryAbort() || _indexBuildState.isFailureCleanUp()) {
        LOGV2_FATAL(7329403,
                    "Received an index build commit from the primary for an index build that we "
                    "were unable to build successfully and was waiting for an abort",
                    "buildUUID"_attr = buildUUID);
    }

    if (_waitForNextAction->getFuture().isReady()) {
        // If the future wait were uninterruptible, then shutdown could hang. If the
        // IndexBuildsCoordinator thread gets interrupted on shutdown, the oplog applier will hang
        // waiting for the promise applying the commitIndexBuild oplog entry.
        const auto nextAction = _waitForNextAction->getFuture().get(opCtx);
        invariant(nextAction == IndexBuildAction::kCommitQuorumSatisfied);
        // Retry until the current promise result is consumed by the index builder thread and
        // a new empty promise got created by the indexBuildscoordinator thread.
        return false;
    }
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);

    _indexBuildState.setState(IndexBuildState::kApplyCommitOplogEntry,
                              skipCheck,
                              shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp());
    // Promise can be set only once.
    // We can't skip signaling here if a signal is already set because the previous commit or
    // abort signal might have been sent to handle for primary case.
    _setSignalAndCancelVoteRequestCbkIfActive(lk, opCtx, IndexBuildAction::kOplogCommit);
    return true;
}

ReplIndexBuildState::TryAbortResult ReplIndexBuildState::tryAbort(OperationContext* opCtx,
                                                                  IndexBuildAction signalAction,
                                                                  Status abortStatus) {
    stdx::lock_guard lk(_mutex);
    // It is not possible for the index build to be in kExternalAbort state, as the collection
    // MODE_X lock is held and there cannot be concurrent external aborters.
    auto nssOptional = CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, collectionUUID);
    invariant(!_indexBuildState.isExternalAbort());
    invariant(
        nssOptional &&
        shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nssOptional.get(), MODE_X));

    // Wait until the build is done setting up. This indicates that all required state is
    // initialized to attempt an abort.
    if (_indexBuildState.isSettingUp()) {
        LOGV2_DEBUG(465605,
                    2,
                    "waiting until index build is done setting up before attempting to abort",
                    "buildUUID"_attr = buildUUID);
        return TryAbortResult::kRetry;
    }
    // Wait until an earlier self-abort finishes. The kAwaitPrimaryAbort state must be allowed, in
    // case the voteAbortIndexBuild command ends up in a loopback or 'abortIndexBuild' is being
    // applied. We retry here instead of returning kAlreadyAborted to ensure that by the time the
    // external aborter receives TryAbortResult::kAlreadyAborted, the build is actually aborted and
    // not in the process of aborting.
    if (_indexBuildState.isFailureCleanUp()) {
        LOGV2_DEBUG(7693501,
                    2,
                    "waiting until index build is finishes abort",
                    "buildUUID"_attr = buildUUID);
        return TryAbortResult::kRetry;
    }
    if (_indexBuildState.isAborted()) {
        // Returns if a concurrent operation already aborted the index build.
        return TryAbortResult::kAlreadyAborted;
    }
    if (_waitForNextAction->getFuture().isReady()) {
        const auto nextAction = _waitForNextAction->getFuture().get(opCtx);
        invariant(nextAction == IndexBuildAction::kSinglePhaseCommit ||
                  nextAction == IndexBuildAction::kCommitQuorumSatisfied ||
                  nextAction == IndexBuildAction::kPrimaryAbort);

        // Index build coordinator already received a signal to commit or abort. So, it's ok
        // to return and wait for the index build to complete if we are trying to signal
        // 'kPrimaryAbort'. The index build coordinator will not perform the signaled action
        // (i.e, will not commit or abort the index build) only when the node steps down.
        // When the node steps down, the caller of this function, dropIndexes/createIndexes
        // command (user operation) will also get interrupted. So, we no longer need to
        // abort the index build on step down.
        if (signalAction == IndexBuildAction::kPrimaryAbort) {
            // Indicate if the index build is already being committed or aborted.
            if (nextAction == IndexBuildAction::kPrimaryAbort) {
                return TryAbortResult::kAlreadyAborted;
            } else {
                return TryAbortResult::kNotAborted;
            }
        }

        // Retry until the current promise result is consumed by the index builder thread
        // and a new empty promise got created by the indexBuildscoordinator thread. Or,
        // until the index build got torn down after index build commit.
        return TryAbortResult::kRetry;
    }

    LOGV2(
        4656003, "Aborting index build", "buildUUID"_attr = buildUUID, "error"_attr = abortStatus);

    // Set the state on replState. Once set, the calling thread must complete the abort process.
    auto abortTimestamp = boost::make_optional<Timestamp>(
        !shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp().isNull(),
        shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp());
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);
    invariant(!abortStatus.isOK());
    _indexBuildState.setState(
        IndexBuildState::kExternalAbort, skipCheck, abortTimestamp, abortStatus);

    // Interrupt the builder thread so that it can no longer acquire locks or make progress.
    // It is possible that the index build thread may have completed its operation and removed
    // itself from the ServiceContext. This may happen in the case of an explicit db.killOp()
    // operation or during shutdown.
    // During normal operation, the abort logic, initiated through external means such as
    // dropIndexes or internally through an indexing error, should have set the state in
    // ReplIndexBuildState so that this code would not be reachable as it is no longer necessary
    // to interrupt the builder thread here.
    auto serviceContext = opCtx->getServiceContext();
    if (auto target = serviceContext->getLockedClient(*_opId)) {
        auto targetOpCtx = target->getOperationContext();
        serviceContext->killOperation(target, targetOpCtx);
    }

    // Set the signal. Because we have already interrupted the index build, it will not observe
    // this signal. We do this so that other observers do not also try to abort the index build.
    _setSignalAndCancelVoteRequestCbkIfActive(lk, opCtx, signalAction);

    return TryAbortResult::kContinueAbort;
}

bool ReplIndexBuildState::forceSelfAbort(OperationContext* opCtx, const Status& error) {
    stdx::lock_guard lk(_mutex);
    if (_indexBuildState.isSettingUp() || _indexBuildState.isAborted() ||
        _indexBuildState.isCommitted() || _indexBuildState.isAborting() ||
        _indexBuildState.isApplyingCommitOplogEntry() || _votedForCommitReadiness) {
        // If the build is setting up, it is not yet abortable. If the index build has already
        // passed a point of no return, interrupting will not be productive. If the index build is
        // already in the process of aborting, it cannot be aborted again.
        LOGV2(7617000,
              "Index build: cannot force abort",
              "buildUUID"_attr = buildUUID,
              "state"_attr = _indexBuildState,
              "votedForCommit"_attr = _votedForCommitReadiness);
        return false;
    }

    _indexBuildState.setState(IndexBuildState::kFailureCleanUp,
                              false /* skipCheck */,
                              boost::none /* timestamp */,
                              error);

    auto serviceContext = opCtx->getServiceContext();
    invariant(_opId);
    if (auto target = serviceContext->getLockedClient(*_opId)) {
        auto targetOpCtx = target->getOperationContext();

        LOGV2(7419400, "Forcefully aborting index build", "buildUUID"_attr = buildUUID);

        // The index builder thread is responsible for cleaning up, as indicated by the
        // kFailureCleanUp state.
        serviceContext->killOperation(target, targetOpCtx);
    }
    return true;
}

void ReplIndexBuildState::onVoteRequestScheduled(OperationContext* opCtx,
                                                 executor::TaskExecutor::CallbackHandle handle) {
    stdx::lock_guard lk(_mutex);
    if (_waitForNextAction->getFuture().isReady()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        replCoord->cancelCbkHandle(handle);
    } else {
        invariant(!_voteCmdCbkHandle.isValid(), str::stream() << buildUUID);
        _voteCmdCbkHandle = handle;
    }
}

void ReplIndexBuildState::_clearVoteRequestCbk(WithLock) {
    _voteCmdCbkHandle = executor::TaskExecutor::CallbackHandle();
}

void ReplIndexBuildState::clearVoteRequestCbk() {
    stdx::lock_guard lk(_mutex);
    _clearVoteRequestCbk(lk);
}

void ReplIndexBuildState::_cancelAndClearVoteRequestCbk(WithLock lk, OperationContext* opCtx) {
    if (_voteCmdCbkHandle.isValid()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        replCoord->cancelCbkHandle(_voteCmdCbkHandle);
    }
    _clearVoteRequestCbk(lk);
}

void ReplIndexBuildState::resetNextActionPromise() {
    stdx::lock_guard lk(_mutex);
    _waitForNextAction = std::make_unique<SharedPromise<IndexBuildAction>>();
}

SharedSemiFuture<IndexBuildAction> ReplIndexBuildState::getNextActionFuture() const {
    stdx::lock_guard lk(_mutex);
    invariant(_waitForNextAction, str::stream() << buildUUID);
    return _waitForNextAction->getFuture();
}

boost::optional<IndexBuildAction> ReplIndexBuildState::getNextActionNoWait() const {
    stdx::lock_guard lk(_mutex);
    auto future = _waitForNextAction->getFuture();
    if (!future.isReady()) {
        return boost::none;
    }
    return future.get();
}

Status ReplIndexBuildState::onConflictWithNewIndexBuild(const ReplIndexBuildState& otherIndexBuild,
                                                        const std::string& otherIndexName) const {
    str::stream ss;
    ss << "Index build conflict: " << otherIndexBuild.buildUUID
       << ": There's already an index with name '" << otherIndexName
       << "' being built on the collection "
       << " ( " << otherIndexBuild.collectionUUID
       << " ) under an existing index build: " << buildUUID;
    auto aborted = false;
    IndexBuildState existingIndexBuildState;
    {
        // We have to lock the mutex in order to read the committed/aborted state.
        stdx::lock_guard lk(_mutex);
        existingIndexBuildState = _indexBuildState;
    }
    ss << " index build state: " << existingIndexBuildState.toString();
    if (auto ts = existingIndexBuildState.getTimestamp()) {
        ss << ", timestamp: " << ts->toString();
    }
    if (existingIndexBuildState.isAborted() || existingIndexBuildState.isAwaitingPrimaryAbort()) {
        if (auto abortReason = existingIndexBuildState.getAbortReason()) {
            ss << ", abort reason: " << abortReason.value();
        }
        aborted = true;
    }
    std::string msg = ss;
    LOGV2(20661,
          "Index build conflict. There's already an index with the same name being "
          "built under an existing index build",
          "buildUUID"_attr = otherIndexBuild.buildUUID,
          "existingBuildUUID"_attr = buildUUID,
          "index"_attr = otherIndexName,
          "collectionUUID"_attr = otherIndexBuild.collectionUUID);
    if (aborted) {
        return {ErrorCodes::IndexBuildAborted, msg};
    }
    return Status(ErrorCodes::IndexBuildAlreadyInProgress, msg);
}

bool ReplIndexBuildState::isResumable() const {
    stdx::lock_guard lk(_mutex);
    return !_lastOpTimeBeforeInterceptors.isNull();
}

repl::OpTime ReplIndexBuildState::getLastOpTimeBeforeInterceptors() const {
    stdx::lock_guard lk(_mutex);
    return _lastOpTimeBeforeInterceptors;
}

void ReplIndexBuildState::setLastOpTimeBeforeInterceptors(repl::OpTime opTime) {
    stdx::lock_guard lk(_mutex);
    _lastOpTimeBeforeInterceptors = std::move(opTime);
}

void ReplIndexBuildState::clearLastOpTimeBeforeInterceptors() {
    stdx::lock_guard lk(_mutex);
    _lastOpTimeBeforeInterceptors = {};
}

void ReplIndexBuildState::appendBuildInfo(BSONObjBuilder* builder) const {
    stdx::lock_guard lk(_mutex);

    // This allows listIndexes callers to identify how to kill the index build.
    // Previously, users have to locate the index build in the currentOp command output
    // for this information.
    if (_opId) {
        builder->append("opid", static_cast<int>(*_opId));
    }

    builder->append("resumable", !_lastOpTimeBeforeInterceptors.isNull());

    _indexBuildState.appendBuildInfo(builder);
}

bool ReplIndexBuildState::_shouldSkipIndexBuildStateTransitionCheck(OperationContext* opCtx) const {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet() && protocol == IndexBuildProtocol::kTwoPhase) {
        if (replCoord->getMemberState() == repl::MemberState::RS_STARTUP2 &&
            !serverGlobalParams.featureCompatibility.acquireFCVSnapshot().isVersionInitialized()) {
            // We're likely at the initial stages of a new logical initial sync attempt, and we
            // haven't yet replicated the FCV from the sync source. Skip the index build state
            // transition checks because they rely on the FCV.
            LOGV2_DEBUG(6826202,
                        2,
                        "Index build: skipping index build state transition checks because the FCV "
                        "isn't known yet");
            return true;
        }
        return false;
    }
    return true;
}

void ReplIndexBuildState::_setSignalAndCancelVoteRequestCbkIfActive(WithLock lk,
                                                                    OperationContext* opCtx,
                                                                    IndexBuildAction signal) {
    // set the signal
    _waitForNextAction->emplaceValue(signal);
    // Cancel the callback, as we are checking if it is valid, this should work even if it is a
    // loopback command.
    _cancelAndClearVoteRequestCbk(lk, opCtx);
}

}  // namespace mongo

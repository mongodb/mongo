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

#include "mongo/platform/basic.h"

#include "mongo/db/repl_index_build_state.h"

#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

/**
 * Parses index specs to generate list of index names for ReplIndexBuildState initialization.
 */
std::vector<std::string> extractIndexNames(const std::vector<BSONObj>& specs) {
    std::vector<std::string> indexNames;
    for (const auto& spec : specs) {
        std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName).toString();
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
        case IndexBuildAction::kTenantMigrationAbort:
            return "Tenant migration abort";
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
    _state = state;
    if (timestamp)
        _timestamp = timestamp;
    if (abortStatus) {
        invariant(_state == kAborted || _state == kAwaitPrimaryAbort || _state == kForceSelfAbort);
        _abortStatus = *abortStatus;
    }
}

bool IndexBuildState::_checkIfValidTransition(IndexBuildState::State currentState,
                                              IndexBuildState::State newState) const {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    const auto graceful =
        feature_flags::gIndexBuildGracefulErrorHandling.isEnabledAndIgnoreFCVUnsafe();
    switch (currentState) {
        case IndexBuildState::State::kSetup:
            return
                // Normal case.
                newState == IndexBuildState::State::kPostSetup ||
                // Setup failed on a primary.
                newState == IndexBuildState::State::kAborted ||
                // Setup failed and we signalled the current primary to abort.
                (graceful && newState == IndexBuildState::State::kAwaitPrimaryAbort);

        case IndexBuildState::State::kPostSetup:
            return
                // Normal case.
                newState == IndexBuildState::State::kInProgress ||
                // After setup, the primary aborted the index build.
                newState == IndexBuildState::State::kAborted ||
                // After setup, we signalled the current primary to abort the index build.
                (graceful && newState == IndexBuildState::State::kAwaitPrimaryAbort) ||
                // We were forced to abort ourselves externally.
                (graceful && newState == IndexBuildState::State::kForceSelfAbort);

        case IndexBuildState::State::kInProgress:
            return
                // Index build was successfully committed by the primary.
                newState == IndexBuildState::State::kCommitted ||
                // As a secondary, we received a commit oplog entry.
                newState == IndexBuildState::State::kApplyCommitOplogEntry ||
                // The index build was aborted, and the caller took responsibility for cleanup.
                newState == IndexBuildState::State::kAborted ||
                // The index build failed and we are waiting for the primary to send an abort oplog
                // entry.
                (graceful && newState == IndexBuildState::State::kAwaitPrimaryAbort) ||
                // We were forced to abort ourselves externally and cleanup is required.
                (graceful && newState == IndexBuildState::State::kForceSelfAbort);

        case IndexBuildState::State::kApplyCommitOplogEntry:
            return
                // We successfully committed the index build as a secondary.
                newState == IndexBuildState::State::kCommitted;

        case IndexBuildState::State::kAwaitPrimaryAbort:
            return
                // We successfully aborted the index build as a primary or secondary.
                (graceful && newState == IndexBuildState::State::kAborted);

        case IndexBuildState::State::kForceSelfAbort:
            return
                // After being signalled to self-abort a second caller explicitly aborted the index
                // build. The second caller has taken responsibility of cleanup.
                (graceful && newState == IndexBuildState::State::kAborted) ||
                // We are waiting for the current primary to abort the index build.
                (graceful && newState == IndexBuildState::State::kAwaitPrimaryAbort);

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
                                         const std::vector<BSONObj>& specs,
                                         IndexBuildProtocol protocol)
    : buildUUID(indexBuildUUID),
      collectionUUID(collUUID),
      dbName(dbName),
      indexNames(extractIndexNames(specs)),
      indexSpecs(specs),
      protocol(protocol) {
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

Status ReplIndexBuildState::tryStart(OperationContext* opCtx) {
    stdx::lock_guard lk(_mutex);
    // The index build might have been aborted/interrupted before reaching this point. Trying to
    // transtion to kInProgress would be an error.
    auto interruptCheck = opCtx->checkForInterruptNoAssert();
    if (interruptCheck.isOK()) {
        _indexBuildState.setState(IndexBuildState::kInProgress, false /* skipCheck */);
    }
    return interruptCheck;
}

void ReplIndexBuildState::commit(OperationContext* opCtx) {
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);
    opCtx->recoveryUnit()->onCommit(
        [this, skipCheck](OperationContext*, boost::optional<Timestamp>) {
            stdx::lock_guard lk(_mutex);
            _indexBuildState.setState(IndexBuildState::kCommitted, skipCheck);
        });
}

bool ReplIndexBuildState::requestAbortFromPrimary(const Status& abortStatus) {
    invariant(protocol == IndexBuildProtocol::kTwoPhase);
    stdx::lock_guard lk(_mutex);

    // It is possible that a 'commitIndexBuild' oplog entry is applied while the index builder is
    // transitioning to an abort, or even to have been in a state where the oplog applier is already
    // waiting for the index build to finish. In such instances, the node cannot try to recover by
    // requesting an abort from the primary, as the commitQuorum already decided to commit.
    if (_indexBuildState.isApplyingCommitOplogEntry()) {
        LOGV2_FATAL(7329407,
                    "Trying to abort an index build while a 'commitIndexBuild' oplog entry is "
                    "being applied. The primary has already committed the build, but this node is "
                    "trying to abort it. This is an inconsistent state we cannot recover from.",
                    "buildUUID"_attr = buildUUID);
    }

    if (_indexBuildState.isAborted()) {
        return false;
    }

    _indexBuildState.setState(
        IndexBuildState::kAwaitPrimaryAbort, false /* skipCheck */, boost::none, abortStatus);

    return true;
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

void ReplIndexBuildState::abortSelf(OperationContext* opCtx) {
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
        !replCoord->getSettings().shouldRecoverFromOplogAsStandalone();
    invariant(!isPrimary, str::stream() << "Index build: " << buildUUID);

    stdx::lock_guard lk(_mutex);
    invariant(_indexBuildState.isAborted(),
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
    return _indexBuildState.isAwaitingPrimaryAbort() || _indexBuildState.isForceSelfAbort();
}

bool ReplIndexBuildState::isCommitted() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.isCommitted();
}

bool ReplIndexBuildState::isSettingUp() const {
    stdx::lock_guard lk(_mutex);
    return _indexBuildState.isSettingUp();
}

std::string ReplIndexBuildState::getAbortReason() const {
    stdx::lock_guard lk(_mutex);
    invariant(_indexBuildState.isAborted() || _indexBuildState.isAwaitingPrimaryAbort(),
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
              "Not signaling \"{skippedAction}\" as it was previously signaled with "
              "\"{previousAction}\" for index build: {buildUUID}",
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
    if (_indexBuildState.isAwaitingPrimaryAbort() || _indexBuildState.isForceSelfAbort()) {
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
                              opCtx->recoveryUnit()->getCommitTimestamp());
    // Promise can be set only once.
    // We can't skip signaling here if a signal is already set because the previous commit or
    // abort signal might have been sent to handle for primary case.
    _setSignalAndCancelVoteRequestCbkIfActive(lk, opCtx, IndexBuildAction::kOplogCommit);
    return true;
}

ReplIndexBuildState::TryAbortResult ReplIndexBuildState::tryAbort(OperationContext* opCtx,
                                                                  IndexBuildAction signalAction,
                                                                  std::string reason) {
    stdx::lock_guard lk(_mutex);
    // Wait until the build is done setting up. This indicates that all required state is
    // initialized to attempt an abort.
    if (_indexBuildState.isSettingUp()) {
        LOGV2_DEBUG(465605,
                    2,
                    "waiting until index build is done setting up before attempting to abort",
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
        if (signalAction == IndexBuildAction::kPrimaryAbort ||
            signalAction == IndexBuildAction::kTenantMigrationAbort) {
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

    LOGV2(4656003, "Aborting index build", "buildUUID"_attr = buildUUID, "error"_attr = reason);

    // Set the state on replState. Once set, the calling thread must complete the abort process.
    auto abortTimestamp =
        boost::make_optional<Timestamp>(!opCtx->recoveryUnit()->getCommitTimestamp().isNull(),
                                        opCtx->recoveryUnit()->getCommitTimestamp());
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);
    Status abortStatus = signalAction == IndexBuildAction::kTenantMigrationAbort
        ? tenant_migration_access_blocker::checkIfCanBuildIndex(opCtx, dbName)
        : Status(ErrorCodes::IndexBuildAborted, reason);
    invariant(!abortStatus.isOK());
    _indexBuildState.setState(IndexBuildState::kAborted, skipCheck, abortTimestamp, abortStatus);

    // Aside from setting the tenantMigrationAbortStatus, tenant migration aborts are identical to
    // primary aborts.
    if (signalAction == IndexBuildAction::kTenantMigrationAbort)
        signalAction = IndexBuildAction::kPrimaryAbort;
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
        serviceContext->killOperation(target, targetOpCtx, ErrorCodes::IndexBuildAborted);
    }

    // Set the signal. Because we have already interrupted the index build, it will not observe
    // this signal. We do this so that other observers do not also try to abort the index build.
    _setSignalAndCancelVoteRequestCbkIfActive(lk, opCtx, signalAction);

    return TryAbortResult::kContinueAbort;
}

bool ReplIndexBuildState::forceSelfAbort(OperationContext* opCtx, const Status& error) {
    stdx::lock_guard lk(_mutex);
    if (_indexBuildState.isSettingUp() || _indexBuildState.isAborted() ||
        _indexBuildState.isCommitted() || _indexBuildState.isAwaitingPrimaryAbort() ||
        _indexBuildState.isApplyingCommitOplogEntry()) {
        // If the build is setting up, it is not yet abortable. If the index build has already
        // passed a point of no return, interrupting will not be productive.
        return false;
    }

    _indexBuildState.setState(IndexBuildState::kForceSelfAbort,
                              false /* skipCheck */,
                              boost::none /* timestamp */,
                              error);

    auto serviceContext = opCtx->getServiceContext();
    invariant(_opId);
    if (auto target = serviceContext->getLockedClient(*_opId)) {
        auto targetOpCtx = target->getOperationContext();

        LOGV2(7419400, "Forcefully aborting index build", "buildUUID"_attr = buildUUID);

        // We don't pass IndexBuildAborted as the interruption error code because that would imply
        // that we are taking responsibility for cleaning up the index build, when in fact the index
        // builder thread is responsible.
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

void ReplIndexBuildState::clearVoteRequestCbk() {
    stdx::lock_guard lk(_mutex);
    _voteCmdCbkHandle = executor::TaskExecutor::CallbackHandle();
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
    if (replCoord->isReplEnabled() && protocol == IndexBuildProtocol::kTwoPhase) {
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
    if (_voteCmdCbkHandle.isValid()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        replCoord->cancelCbkHandle(_voteCmdCbkHandle);
    }
}

}  // namespace mongo

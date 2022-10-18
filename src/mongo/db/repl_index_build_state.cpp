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

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/logv2/log.h"

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

/**
 * Returns true if the requested IndexBuildState transition is allowed.
 */
bool checkIfValidTransition(IndexBuildState::StateFlag currentState,
                            IndexBuildState::StateFlag newState) {
    if ((currentState == IndexBuildState::StateFlag::kSetup &&
         (newState == IndexBuildState::StateFlag::kInProgress ||
          newState == IndexBuildState::StateFlag::kAborted)) ||
        (currentState == IndexBuildState::StateFlag::kInProgress &&
         newState != IndexBuildState::StateFlag::kSetup) ||
        (currentState == IndexBuildState::StateFlag::kPrepareCommit &&
         newState == IndexBuildState::StateFlag::kCommitted)) {
        return true;
    }
    return false;
}

}  // namespace

std::string indexBuildActionToString(IndexBuildAction action) {
    if (action == IndexBuildAction::kNoAction) {
        return "No action";
    } else if (action == IndexBuildAction::kOplogCommit) {
        return "Oplog commit";
    } else if (action == IndexBuildAction::kOplogAbort) {
        return "Oplog abort";
    } else if (action == IndexBuildAction::kInitialSyncAbort) {
        return "Initial sync abort";
    } else if (action == IndexBuildAction::kRollbackAbort) {
        return "Rollback abort";
    } else if (action == IndexBuildAction::kTenantMigrationAbort) {
        return "Tenant migration abort";
    } else if (action == IndexBuildAction::kPrimaryAbort) {
        return "Primary abort";
    } else if (action == IndexBuildAction::kSinglePhaseCommit) {
        return "Single-phase commit";
    } else if (action == IndexBuildAction::kCommitQuorumSatisfied) {
        return "Commit quorum Satisfied";
    }
    MONGO_UNREACHABLE;
}

void IndexBuildState::setState(StateFlag state,
                               bool skipCheck,
                               boost::optional<Timestamp> timestamp,
                               boost::optional<Status> abortStatus) {
    if (!skipCheck) {
        invariant(checkIfValidTransition(_state, state),
                  str::stream() << "current state :" << toString(_state)
                                << ", new state: " << toString(state));
    }
    _state = state;
    if (timestamp)
        _timestamp = timestamp;
    if (abortStatus) {
        invariant(_state == kAborted);
        _abortStatus = *abortStatus;
    }
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

void ReplIndexBuildState::start(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_mutex);
    _opId = opCtx->getOpID();
    _indexBuildState.setState(IndexBuildState::kInProgress, false /* skipCheck */);
}

void ReplIndexBuildState::commit(OperationContext* opCtx) {
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);
    opCtx->recoveryUnit()->onCommit([this, skipCheck](boost::optional<Timestamp> commitTime) {
        stdx::unique_lock<Latch> lk(_mutex);
        _indexBuildState.setState(IndexBuildState::kCommitted, skipCheck);
    });
}

Timestamp ReplIndexBuildState::getCommitTimestamp() const {
    stdx::unique_lock<Latch> lk(_mutex);
    return _indexBuildState.getTimestamp().value_or(Timestamp());
}

void ReplIndexBuildState::onOplogCommit(bool isPrimary) const {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(!isPrimary && _indexBuildState.isCommitPrepared(),
              str::stream() << "Index build: " << buildUUID
                            << ",  index build state: " << _indexBuildState.toString());
}

void ReplIndexBuildState::abortSelf(OperationContext* opCtx) {
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);
    stdx::unique_lock<Latch> lk(_mutex);
    _indexBuildState.setState(IndexBuildState::kAborted, skipCheck);
}

void ReplIndexBuildState::abortForShutdown(OperationContext* opCtx) {
    // Promise should be set at least once before it's getting destroyed.
    stdx::unique_lock<Latch> lk(_mutex);
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

    stdx::unique_lock<Latch> lk(_mutex);
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

bool ReplIndexBuildState::isAborted() const {
    stdx::unique_lock<Latch> lk(_mutex);
    return _indexBuildState.isAborted();
}

bool ReplIndexBuildState::isSettingUp() const {
    stdx::unique_lock<Latch> lk(_mutex);
    return _indexBuildState.isSettingUp();
}

std::string ReplIndexBuildState::getAbortReason() const {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(_indexBuildState.isAborted(),
              str::stream() << "Index build: " << buildUUID
                            << ",  index build state: " << _indexBuildState.toString());
    auto reason = _indexBuildState.getAbortReason();
    invariant(reason, str::stream() << buildUUID);
    return *reason;
}

Status ReplIndexBuildState::getAbortStatus() const {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(_indexBuildState.isAborted(),
              str::stream() << "Index build: " << buildUUID
                            << ",  index build state: " << _indexBuildState.toString());
    Status abortStatus = _indexBuildState.getAbortStatus();
    return abortStatus;
}

void ReplIndexBuildState::setCommitQuorumSatisfied(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_mutex);
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
    stdx::unique_lock<Latch> lk(_mutex);
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
    stdx::unique_lock<Latch> lk(_mutex);
    if (_indexBuildState.isSettingUp()) {
        // It's possible that the index build thread has not reached the point where it can be
        // committed yet.
        return false;
    }
    if (_waitForNextAction->getFuture().isReady()) {
        // If the future wait were uninterruptible, then shutdown could hang.  If the
        // IndexBuildsCoordinator thread gets interrupted on shutdown, the oplog applier will hang
        // waiting for the promise applying the commitIndexBuild oplog entry.
        const auto nextAction = _waitForNextAction->getFuture().get(opCtx);
        invariant(nextAction == IndexBuildAction::kCommitQuorumSatisfied);
        // Retry until the current promise result is consumed by the index builder thread and
        // a new empty promise got created by the indexBuildscoordinator thread.
        return false;
    }
    auto skipCheck = _shouldSkipIndexBuildStateTransitionCheck(opCtx);
    _indexBuildState.setState(
        IndexBuildState::kPrepareCommit, skipCheck, opCtx->recoveryUnit()->getCommitTimestamp());
    // Promise can be set only once.
    // We can't skip signaling here if a signal is already set because the previous commit or
    // abort signal might have been sent to handle for primary case.
    _setSignalAndCancelVoteRequestCbkIfActive(lk, opCtx, IndexBuildAction::kOplogCommit);
    return true;
}

ReplIndexBuildState::TryAbortResult ReplIndexBuildState::tryAbort(OperationContext* opCtx,
                                                                  IndexBuildAction signalAction,
                                                                  std::string reason) {
    stdx::unique_lock<Latch> lk(_mutex);
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
        // Returns if a concurrent operation already aborts the index build.
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
        ? tenant_migration_access_blocker::checkIfCanBuildIndex(opCtx,
                                                                dbName.toStringWithTenantId())
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

void ReplIndexBuildState::onVoteRequestScheduled(OperationContext* opCtx,
                                                 executor::TaskExecutor::CallbackHandle handle) {
    stdx::unique_lock<Latch> lk(_mutex);
    if (_waitForNextAction->getFuture().isReady()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        replCoord->cancelCbkHandle(handle);
    } else {
        invariant(!_voteCmdCbkHandle.isValid(), str::stream() << buildUUID);
        _voteCmdCbkHandle = handle;
    }
}

void ReplIndexBuildState::clearVoteRequestCbk() {
    stdx::unique_lock<Latch> lk(_mutex);
    _voteCmdCbkHandle = executor::TaskExecutor::CallbackHandle();
}

void ReplIndexBuildState::resetNextActionPromise() {
    stdx::unique_lock<Latch> lk(_mutex);
    _waitForNextAction = std::make_unique<SharedPromise<IndexBuildAction>>();
}

SharedSemiFuture<IndexBuildAction> ReplIndexBuildState::getNextActionFuture() const {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(_waitForNextAction, str::stream() << buildUUID);
    return _waitForNextAction->getFuture();
}

boost::optional<IndexBuildAction> ReplIndexBuildState::getNextActionNoWait() const {
    stdx::unique_lock<Latch> lk(_mutex);
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
        stdx::unique_lock<Latch> lk(_mutex);
        existingIndexBuildState = _indexBuildState;
    }
    ss << " index build state: " << existingIndexBuildState.toString();
    if (auto ts = existingIndexBuildState.getTimestamp()) {
        ss << ", timestamp: " << ts->toString();
    }
    if (existingIndexBuildState.isAborted()) {
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
    stdx::unique_lock<Latch> lk(_mutex);
    return !_lastOpTimeBeforeInterceptors.isNull();
}

repl::OpTime ReplIndexBuildState::getLastOpTimeBeforeInterceptors() const {
    stdx::unique_lock<Latch> lk(_mutex);
    return _lastOpTimeBeforeInterceptors;
}

void ReplIndexBuildState::setLastOpTimeBeforeInterceptors(repl::OpTime opTime) {
    stdx::unique_lock<Latch> lk(_mutex);
    _lastOpTimeBeforeInterceptors = std::move(opTime);
}

void ReplIndexBuildState::clearLastOpTimeBeforeInterceptors() {
    stdx::unique_lock<Latch> lk(_mutex);
    _lastOpTimeBeforeInterceptors = {};
}

void ReplIndexBuildState::appendBuildInfo(BSONObjBuilder* builder) const {
    stdx::unique_lock<Latch> lk(_mutex);

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
    // Cancel the callback.
    if (_voteCmdCbkHandle.isValid()) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        replCoord->cancelCbkHandle(_voteCmdCbkHandle);
    }
}

}  // namespace mongo

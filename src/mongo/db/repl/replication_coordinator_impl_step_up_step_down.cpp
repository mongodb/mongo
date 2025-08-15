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


#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/local_catalog/lock_manager/dump_lock_manager.h"
#include "mongo/db/repl/auto_get_rstl_for_stepup_stepdown.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_impl_gen.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/prepare_conflict_tracker.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/time_support.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(stepdownHangBeforePerformingPostMemberStateUpdateActions);
MONGO_FAIL_POINT_DEFINE(stepdownHangBeforeRSTLEnqueue);
// Hang after grabbing the RSTL but before we start rejecting writes.
MONGO_FAIL_POINT_DEFINE(stepdownHangAfterGrabbingRSTL);


void ReplicationCoordinatorImpl::waitForStepDownAttempt_forTest() {
    auto isSteppingDown = [&]() {
        stdx::unique_lock lk(_mutex);
        // If true, we know that a stepdown is underway.
        return (_topCoord->isSteppingDown());
    };

    while (!isSteppingDown()) {
        sleepFor(Milliseconds{10});
    }
}

void ReplicationCoordinatorImpl::autoGetRstlEnterStepDown() {
    stdx::lock_guard lk(_mutex);
    // This makes us tell the 'hello' command we can't accept writes (though in fact we can,
    // it is not valid to disable writes until we actually acquire the RSTL).
    if (_stepDownPending++ == 0)
        _fulfillTopologyChangePromise(lk);
}

void ReplicationCoordinatorImpl::autoGetRstlExitStepDown() {
    stdx::lock_guard lk(_mutex);
    // Once we release the RSTL, we announce either that we can accept writes or that we're now
    // a real secondary.
    invariant(_stepDownPending > 0);
    if (--_stepDownPending == 0)
        _fulfillTopologyChangePromise(lk);
}


void ReplicationCoordinatorImpl::stepDown(OperationContext* opCtx,
                                          const bool force,
                                          const Milliseconds& waitTime,
                                          const Milliseconds& stepdownTime) {
    const Date_t startTime = _replExecutor->now();
    const Date_t stepDownUntil = startTime + stepdownTime;
    const Date_t waitUntil = startTime + waitTime;

    // Note this check is inherently racy - it's always possible for the node to stepdown from some
    // other path before we acquire the global exclusive lock.  This check is just to try to save us
    // from acquiring the global X lock unnecessarily.
    uassert(ErrorCodes::NotWritablePrimary,
            "not primary so can't step down",
            getMemberState().primary());
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &stepdownHangBeforeRSTLEnqueue, opCtx, "stepdownHangBeforeRSTLEnqueue");

    boost::optional<rss::consensus::ReplicationStateTransitionGuard> rstg;
    boost::optional<AutoGetRstlForStepUpStepDown> arsd;

    // Using 'force' sets the default for the wait time to zero, which means the stepdown will
    // fail if it does not acquire the lock immediately. In such a scenario, we use the
    // stepDownUntil deadline instead.
    auto deadline = force ? stepDownUntil : waitUntil;

    const Date_t startTimeKillConflictingOperations = _replExecutor->now();
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        rstg.emplace(_killConflictingOperations(
            rss::consensus::IntentRegistry::InterruptionType::StepDown, opCtx));
    }
    const Date_t endTimeKillConflictingOperations = _replExecutor->now();
    LOGV2(962666,
          "killConflictingOperations in stepDown completed",
          "totalTime"_attr =
              (endTimeKillConflictingOperations - startTimeKillConflictingOperations));

    const Date_t startTimeAcquireRSTL = _replExecutor->now();
    arsd.emplace(
        this, opCtx, ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown, deadline);
    const Date_t endTimeAcquireRSTL = _replExecutor->now();
    LOGV2(962661,
          "Acquired RSTL for stepDown",
          "totalTimeToAcquire"_attr = (endTimeAcquireRSTL - startTimeAcquireRSTL));

    stepdownHangAfterGrabbingRSTL.pauseWhileSet();

    stdx::unique_lock lk(_mutex);

    opCtx->checkForInterrupt();

    const long long termAtStart = _topCoord->getTerm();

    // This will cause us to fail if we're already in the process of stepping down, or if we've
    // already successfully stepped down via another path.
    auto abortFn = uassertStatusOK(_topCoord->prepareForStepDownAttempt());

    // Update _canAcceptNonLocalWrites from the TopologyCoordinator now that we're in the middle
    // of a stepdown attempt.  This will prevent us from accepting writes so that if our stepdown
    // attempt fails later we can release the RSTL and go to sleep to allow secondaries to
    // catch up without allowing new writes in.
    _updateWriteAbilityFromTopologyCoordinator(lk, opCtx);
    invariant(!_readWriteAbility->canAcceptNonLocalWrites(lk));

    bool interruptedBeforeReacquireRSTL = false;
    const Date_t startTimeUpdateMemberState = _replExecutor->now();
    auto updateMemberState = [this, &lk](OperationContext* opCtx) {
        invariant(lk.owns_lock());
        invariant(shard_role_details::getLocker(opCtx)->isRSTLExclusive() ||
                  gFeatureFlagIntentRegistration.isEnabled());

        // Make sure that we leave _canAcceptNonLocalWrites in the proper state.
        _updateWriteAbilityFromTopologyCoordinator(lk, opCtx);
        auto action = _updateMemberStateFromTopologyCoordinator(lk);

        lk.unlock();

        if (MONGO_unlikely(stepdownHangBeforePerformingPostMemberStateUpdateActions.shouldFail())) {
            LOGV2(21345,
                  "stepping down from primary - "
                  "stepdownHangBeforePerformingPostMemberStateUpdateActions fail point enabled. "
                  "Blocking until fail point is disabled");
            while (MONGO_unlikely(
                stepdownHangBeforePerformingPostMemberStateUpdateActions.shouldFail())) {
                mongo::sleepsecs(1);
                {
                    stdx::lock_guard lock(_mutex);
                    if (_inShutdown) {
                        break;
                    }
                }
            }
        }

        _performPostMemberStateUpdateAction(action);
    };
    const Date_t endTimeUpdateMemberState = _replExecutor->now();
    ScopeGuard onExitGuard([&] {
        if (interruptedBeforeReacquireRSTL) {
            // We should only enter this branch when we get interrupted during reacquiring RSTL, so
            // we should not holding the RSTL and _mutex. We need to create a new client and opCtx
            // for the cleanup since the cleanup is a must do.
            invariant(!lk.owns_lock());
            invariant(!shard_role_details::getLocker(opCtx)->isRSTLExclusive() ||
                      gFeatureFlagIntentRegistration.isEnabled());
            while (true) {
                try {
                    auto newClient = opCtx->getServiceContext()
                                         ->getService(ClusterRole::ShardServer)
                                         ->makeClient("StepdownCleaner");
                    AlternativeClientRegion acr(newClient);
                    auto newOpCtx = cc().makeOperationContext();
                    // We wait RSTL at no timeout because we have to get it to update WriteAbility
                    // and MemberState anyway. If it gets interrupted again, keep retrying.
                    AutoGetRstlForStepUpStepDown arsdForCleanup(
                        this,
                        newOpCtx.get(),
                        ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown,
                        Date_t::max());
                    lk.lock();
                    abortFn();
                    updateMemberState(newOpCtx.get());
                    break;
                } catch (const DBException& ex) {
                    LOGV2(9080201,
                          "Reacquiring RSTL on cleanup gets interrupted",
                          "error"_attr = ex.toStatus());
                }
            }
        } else {
            abortFn();
            updateMemberState(opCtx);
        }
    });

    auto waitTimeout = std::min(waitTime, stepdownTime);

    // Set up a waiter which will be signaled when we process a heartbeat or updatePosition
    // and have a majority of nodes at our optime.
    const WriteConcernOptions waiterWriteConcern(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::NONE, waitTimeout);

    // If attemptStepDown() succeeds, we are guaranteed that no concurrent step up or
    // step down can happen afterwards. So, it's safe to release the mutex before
    // yieldLocksForPreparedTransactions().
    while (!_topCoord->tryToStartStepDown(
        termAtStart, _replExecutor->now(), waitUntil, stepDownUntil, force)) {
        // The stepdown attempt failed. We now release the RSTL to allow secondaries to read the
        // oplog, then wait until enough secondaries are caught up for us to finish stepdown.
        const Date_t startTimeReleaseRSTL = _replExecutor->now();
        if (arsd) {
            arsd->rstlRelease();
        }
        const Date_t endTimeReleaseRSTL = _replExecutor->now();
        LOGV2(962662,
              "Released RSTL during stepDown",
              "timeToRelease"_attr = (endTimeReleaseRSTL - startTimeReleaseRSTL));
        rstg = boost::none;
        invariant(!shard_role_details::getLocker(opCtx)->isLocked());

        auto lastAppliedOpTime = _getMyLastAppliedOpTime(lk);
        auto currentTerm = _topCoord->getTerm();
        // If termAtStart != currentTerm, tryToStartStepDown would have thrown.
        invariant(termAtStart == currentTerm);
        // As we should not wait for secondaries to catch up if this node has not yet written in
        // this term, invariant that the lastAppliedOpTime we will wait for has the same term as the
        // current term. Also see TopologyCoordinator::isSafeToStepDown.
        invariant(lastAppliedOpTime.getTerm() == currentTerm);

        auto [future, waiter] =
            _replicationWaiterList.add(lk, lastAppliedOpTime, waiterWriteConcern);
        lk.unlock();

        // Operations that can be interrupted through opCtx should be executed inside this try/catch
        // block in case that the operation get interrupted, the stepdown thread doesn't hold RSTL
        // and _mutex so we can safely reacquire those locks in the onExitGuard to do the cleanup.
        try {
            auto status = _futureGetNoThrowWithDeadline(
                opCtx, future, std::min(stepDownUntil, waitUntil), ErrorCodes::ExceededTimeLimit);

            // Remove the waiter from the list if it times out before the future is ready.
            // The replicationWaiterList does not support delayed removal with waiter->givenUp.
            if (!status.isOK() && !future.isReady()) {
                lk.lock();
                invariant(waiter);
                _replicationWaiterList.remove(lk, lastAppliedOpTime, waiter);
                lk.unlock();
            }

            // We ignore the case where runWithDeadline returns timeoutError because in that case
            // coming back around the loop and calling tryToStartStepDown again will cause
            // tryToStartStepDown to return ExceededTimeLimit with the proper error message.
            if (!status.isOK() && status.code() != ErrorCodes::ExceededTimeLimit) {
                opCtx->checkForInterrupt();
            }

            // Since we have released the RSTL lock at this point, there can be some read
            // operations sneaked in here, that might hold global lock in S mode or blocked on
            // prepare conflict. We need to kill those operations to avoid 3-way deadlock
            // between read, prepared transaction and step down thread. And, any write
            // operations that gets sneaked in here will fail as we have updated
            // _canAcceptNonLocalWrites to false after our first successful RSTL lock
            // acquisition. So, we won't get into problems like SERVER-27534.
            const Date_t startTimeKillConflictingOps = _replExecutor->now();
            if (gFeatureFlagIntentRegistration.isEnabled()) {
                rstg.emplace(_killConflictingOperations(
                    rss::consensus::IntentRegistry::InterruptionType::StepDown, opCtx));
            }
            const Date_t endTimeKillConflictingOps = _replExecutor->now();
            LOGV2(962667,
                  "killConflictingOperations for stepDown",
                  "totalTime"_attr = (endTimeKillConflictingOps - startTimeKillConflictingOps));

            const Date_t startTimeReacquireRSTL = _replExecutor->now();
            if (arsd) {
                arsd->rstlReacquire();
            }
            const Date_t endTimeReacquireRSTL = _replExecutor->now();
            LOGV2(962663,
                  "Reacquired RSTL during stepDown",
                  "timeToReacquire"_attr = (endTimeReacquireRSTL - startTimeReacquireRSTL));
            lk.lock();
        } catch (const DBException& ex) {
            // We can get interrupted when reacquiring the RSTL. If that happens, the
            // onExitGuard will create a new client and opCtx to perform the WriteAbility and
            // MemberState cleanup after failed stepdown.
            LOGV2(9080200, "Reacquiring RSTL gets interrupted", "error"_attr = ex.toStatus());
            interruptedBeforeReacquireRSTL = true;
            throw;
        }
    }

    // Prepare for unconditional stepdown success!
    // We need to release the mutex before yielding locks for prepared transactions, which might
    // check out sessions, to avoid deadlocks with checked-out sessions accessing this mutex.
    lk.unlock();
    const Date_t startTimeYieldLocksInvalidateSessions = _replExecutor->now();
    yieldLocksForPreparedTransactions(opCtx);
    invalidateSessionsForStepdown(opCtx);
    const Date_t endTimeYieldLocksInvalidateSessions = _replExecutor->now();
    lk.lock();

    // Clear the node's election candidate metrics since it is no longer primary.
    ReplicationMetrics::get(opCtx).clearElectionCandidateMetrics();

    _topCoord->finishUnconditionalStepDown();

    onExitGuard.dismiss();
    updateMemberState(opCtx);

    // Schedule work to (potentially) step back up once the stepdown period has ended.
    _scheduleWorkAt(stepDownUntil, [=, this](const executor::TaskExecutor::CallbackArgs& cbData) {
        _handleTimePassing(cbData);
    });

    // If election handoff is enabled, schedule a step-up immediately instead of waiting for the
    // election timeout to expire.
    if (!force && enableElectionHandoff.load()) {
        _performElectionHandoff();
    }
    const Date_t endTime = _replExecutor->now();
    LOGV2(962660,
          "Stepdown succeeded",
          "totalTime"_attr = (endTime - startTime),
          "updateMemberStateTime"_attr = (endTimeUpdateMemberState - startTimeUpdateMemberState),
          "yieldLocksTime"_attr =
              (endTimeYieldLocksInvalidateSessions - startTimeYieldLocksInvalidateSessions));
}

Status ReplicationCoordinatorImpl::stepUpIfEligible(bool skipDryRun) {

    auto reason = skipDryRun ? StartElectionReasonEnum::kStepUpRequestSkipDryRun
                             : StartElectionReasonEnum::kStepUpRequest;
    _startElectSelfIfEligibleV1(reason);

    EventHandle finishEvent;
    {
        stdx::lock_guard lk(_mutex);
        // A null _electionState indicates that the election has already completed.
        if (_electionState) {
            finishEvent = _electionState->getElectionFinishedEvent(lk);
        }
    }
    if (finishEvent.isValid()) {
        LOGV2(6015303, "Waiting for in-progress election to complete before finishing stepup");
        _replExecutor->waitForEvent(finishEvent);
    }
    {
        // Step up is considered successful only if we are currently a primary and we are not in the
        // process of stepping down. If we know we are going to step down, we should fail the
        // replSetStepUp command so caller can retry if necessary.
        stdx::lock_guard lk(_mutex);
        if (!_getMemberState(lk).primary())
            return Status(ErrorCodes::CommandFailed, "Election failed.");
        else if (_topCoord->isSteppingDown())
            return Status(ErrorCodes::CommandFailed, "Election failed due to concurrent stepdown.");
    }
    return Status::OK();
}


}  // namespace repl
}  // namespace mongo

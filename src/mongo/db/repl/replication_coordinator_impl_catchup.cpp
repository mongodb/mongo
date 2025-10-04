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


#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/time_support.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {


boost::optional<Date_t> ReplicationCoordinatorImpl::getCatchupTakeover_forTest() const {
    stdx::lock_guard lk(_mutex);
    if (!_catchupTakeoverCbh.isValid()) {
        return boost::none;
    }
    return _catchupTakeoverWhen;
}

executor::TaskExecutor::CallbackHandle ReplicationCoordinatorImpl::getCatchupTakeoverCbh_forTest()
    const {
    return _catchupTakeoverCbh;
}

void ReplicationCoordinatorImpl::CatchupState::start(WithLock lk) {
    LOGV2(21359, "Entering primary catch-up mode");

    // Reset the number of catchup operations performed before starting catchup.
    _numCatchUpOps = 0;

    // No catchup in single node replica set.
    if (_repl->_rsConfig.unsafePeek().getNumMembers() == 1) {
        LOGV2(6015304, "Skipping primary catchup since we are the only node in the replica set.");
        abort(lk, PrimaryCatchUpConclusionReason::kSkipped);
        return;
    }

    auto catchupTimeout = _repl->_rsConfig.unsafePeek().getCatchUpTimeoutPeriod();

    // When catchUpTimeoutMillis is 0, we skip doing catchup entirely.
    if (catchupTimeout == ReplSetConfig::kCatchUpDisabled) {
        LOGV2(21360, "Skipping primary catchup since the catchup timeout is 0");
        abort(lk, PrimaryCatchUpConclusionReason::kSkipped);
        return;
    }

    auto mutex = &_repl->_mutex;
    auto timeoutCB = [this, mutex](const executor::TaskExecutor::CallbackArgs& cbData) {
        if (!cbData.status.isOK()) {
            return;
        }
        stdx::lock_guard lk(*mutex);
        // Check whether the callback has been cancelled while holding mutex.
        if (cbData.myHandle.isCanceled()) {
            return;
        }
        LOGV2(21361, "Catchup timed out after becoming primary");
        abort(lk, PrimaryCatchUpConclusionReason::kTimedOut);
    };

    // Deal with infinity and overflow - no timeout.
    if (catchupTimeout == ReplSetConfig::kInfiniteCatchUpTimeout ||
        Date_t::max() - _repl->_replExecutor->now() <= catchupTimeout) {
        return;
    }
    // Schedule timeout callback.
    auto timeoutDate = _repl->_replExecutor->now() + catchupTimeout;
    auto status = _repl->_replExecutor->scheduleWorkAt(timeoutDate, std::move(timeoutCB));
    if (!status.isOK()) {
        LOGV2(21362, "Failed to schedule catchup timeout work");
        abort(lk, PrimaryCatchUpConclusionReason::kFailedWithError);
        return;
    }
    _timeoutCbh = status.getValue();
}

void ReplicationCoordinatorImpl::CatchupState::abort(WithLock lk,
                                                     PrimaryCatchUpConclusionReason reason) {
    invariant(_repl->_getMemberState(lk).primary());

    ReplicationMetrics::get(getGlobalServiceContext())
        .incrementNumCatchUpsConcludedForReason(reason);

    LOGV2(21363, "Exited primary catch-up mode");
    // Clean up its own members.
    if (_timeoutCbh) {
        _repl->_replExecutor->cancel(_timeoutCbh);
    }
    if (reason != PrimaryCatchUpConclusionReason::kSucceeded && _waiter) {
        _repl->_lastAppliedOpTimeWaiterList.remove(lk, _targetOpTime, _waiter);
        _waiter.reset();
    }

    // Enter primary drain mode.
    _repl->_enterDrainMode(lk);
    // Destroy the state itself.
    _repl->_catchupState.reset();
}

void ReplicationCoordinatorImpl::CatchupState::signalHeartbeatUpdate(WithLock lk) {
    auto targetOpTime = _repl->_topCoord->latestKnownOpTimeSinceHeartbeatRestart();
    // Haven't collected all heartbeat responses.
    if (!targetOpTime) {
        LOGV2_DEBUG(
            6015305,
            1,
            "Not updating target optime for catchup, we haven't collected all heartbeat responses");
        return;
    }

    // We've caught up.
    const auto myLastApplied = _repl->_getMyLastAppliedOpTime(lk);
    if (*targetOpTime <= myLastApplied) {
        LOGV2(21364,
              "Caught up to the latest optime known via heartbeats after becoming primary",
              "targetOpTime"_attr = *targetOpTime,
              "myLastApplied"_attr = myLastApplied);
        // Report the number of ops applied during catchup in replSetGetStatus once the primary is
        // caught up.
        ReplicationMetrics::get(getGlobalServiceContext()).setNumCatchUpOps(_numCatchUpOps);
        abort(lk, PrimaryCatchUpConclusionReason::kAlreadyCaughtUp);
        return;
    }

    // Reset the target optime if it has changed.
    if (_waiter && _targetOpTime == *targetOpTime) {
        return;
    }

    if (_waiter) {
        _repl->_lastAppliedOpTimeWaiterList.remove(lk, _targetOpTime, _waiter);
        _waiter.reset();
    } else {
        // Only increment the 'numCatchUps' election metric the first time we add a waiter, so that
        // we only increment it once each time a primary has to catch up. If there is already an
        // existing waiter, then the node is catching up and has already been counted.
        ReplicationMetrics::get(getGlobalServiceContext()).incrementNumCatchUps();
    }

    _targetOpTime = *targetOpTime;

    ReplicationMetrics::get(getGlobalServiceContext()).setTargetCatchupOpTime(_targetOpTime);

    LOGV2(21365, "Heartbeats updated catchup target optime", "targetOpTime"_attr = _targetOpTime);
    LOGV2(21366, "Latest known optime per replica set member");
    auto opTimesPerMember = _repl->_topCoord->latestKnownOpTimeSinceHeartbeatRestartPerMember();
    for (auto&& pair : opTimesPerMember) {
        LOGV2(21367,
              "Latest known optime",
              "memberId"_attr = pair.first,
              "latestKnownOpTime"_attr = (pair.second ? (*pair.second).toString() : "unknown"));
    }

    auto targetOpTimeCB = [this](Status status) {
        // Double check the target time since stepdown may signal us too.
        const auto myLastApplied = _repl->_getMyLastAppliedOpTime(
            WithLock::withoutLock() /* We hold _mutex when we execute this. */);
        if (_targetOpTime <= myLastApplied) {
            LOGV2(21368,
                  "Caught up to the latest known optime successfully after becoming primary",
                  "targetOpTime"_attr = _targetOpTime,
                  "myLastApplied"_attr = myLastApplied);
            // Report the number of ops applied during catchup in replSetGetStatus once the primary
            // is caught up.
            ReplicationMetrics::get(getGlobalServiceContext()).setNumCatchUpOps(_numCatchUpOps);
            abort(WithLock::withoutLock() /* We hold _mutex when we execute this. */,
                  PrimaryCatchUpConclusionReason::kSucceeded);
        }
    };
    auto pf = makePromiseFuture<void>();
    _waiter = std::make_shared<Waiter>(std::move(pf.promise));
    auto future = std::move(pf.future).onCompletion(targetOpTimeCB);
    _repl->_lastAppliedOpTimeWaiterList.add(lk, _targetOpTime, _waiter);
}

void ReplicationCoordinatorImpl::CatchupState::incrementNumCatchUpOps(WithLock, long numOps) {
    _numCatchUpOps += numOps;
}

Status ReplicationCoordinatorImpl::abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) {
    stdx::lock_guard lk(_mutex);
    if (_catchupState) {
        _catchupState->abort(lk, reason);
        return Status::OK();
    }
    return Status(ErrorCodes::IllegalOperation, "The node is not in catch-up mode.");
}

void ReplicationCoordinatorImpl::incrementNumCatchUpOpsIfCatchingUp(long numOps) {
    stdx::lock_guard lk(_mutex);
    if (_catchupState) {
        _catchupState->incrementNumCatchUpOps(lk, numOps);
    }
}

ReplicationCoordinatorImpl::PostMemberStateUpdateAction
ReplicationCoordinatorImpl::_updateMemberStateFromTopologyCoordinator(WithLock lk) {
    // We want to respond to any waiting hellos even if our current and target state are the
    // same as it is possible writes have been disabled during a stepDown but the primary has yet
    // to transition to SECONDARY state.  We do not do so when _stepDownPending is true
    // because in that case we have already said we cannot accept writes in the hello response
    // and explictly incremented the toplogy version.
    ON_BLOCK_EXIT([&] {
        if (_rsConfig.unsafePeek().isInitialized() && !_stepDownPending) {
            _fulfillTopologyChangePromise(lk);
        }
    });

    const MemberState newState = _topCoord->getMemberState();

    if (newState == _memberState) {
        return kActionNone;
    }

    PostMemberStateUpdateAction result;
    if (_memberState.primary() || newState.removed() || newState.rollback()) {
        // Wake up any threads blocked in awaitReplication, close connections, etc.
        _replicationWaiterList.setErrorAll(
            lk,
            {ErrorCodes::PrimarySteppedDown, "Primary stepped down while waiting for replication"});

        if (_memberState.primary()) {
            // We may have already disallowed primary majority reads via failing a replication
            // waiter in the previous line. However, it's possible we are stepping down here before
            // we managed to create a waiter, so in that case we should explicitly indicate that we
            // have stepped down.
            _primaryMajorityReadsAvailability.onBecomeNonPrimary();
        }

        // Wake up the optime waiter that is waiting for primary catch-up to finish.
        _lastAppliedOpTimeWaiterList.setErrorAll(
            lk,
            {ErrorCodes::PrimarySteppedDown, "Primary stepped down while waiting for replication"});
        // Wake up the optime waiter that is waiting for oplog to be written
        _lastWrittenOpTimeWaiterList.setErrorAll(
            lk,
            {ErrorCodes::PrimarySteppedDown, "Primary stepped down while waiting for replication"});

        // _canAcceptNonLocalWrites should already be set.
        invariant(!_readWriteAbility->canAcceptNonLocalWrites(lk));

        serverGlobalParams.validateFeaturesAsPrimary.store(false);
        result = (newState.removed() || newState.rollback()) ? kActionRollbackOrRemoved
                                                             : kActionSteppedDown;
    } else {
        result = kActionFollowerModeStateChange;
    }

    // Exit catchup mode if we're in it and enable replication producer and applier on stepdown.
    if (_memberState.primary()) {
        if (_catchupState) {
            // _pendingTermUpdateDuringStepDown is set before stepping down due to hearing about a
            // higher term, so that we can remember the term we heard and update our term as part of
            // finishing stepdown. It is then unset toward the end of stepdown, after the function
            // we are in is called. Thus we must be stepping down due to seeing a higher term if and
            // only if _pendingTermUpdateDuringStepDown is set here.
            if (_pendingTermUpdateDuringStepDown) {
                _catchupState->abort(lk, PrimaryCatchUpConclusionReason::kFailedWithNewTerm);
            } else {
                _catchupState->abort(lk, PrimaryCatchUpConclusionReason::kFailedWithError);
            }
        }
        _oplogSyncState = OplogSyncState::Running;
        _externalState->startProducerIfStopped();
    }

    if (_memberState.secondary() && !newState.primary() && !newState.rollback()) {
        // Switching out of SECONDARY, but not to PRIMARY or ROLLBACK. Note that ROLLBACK case is
        // handled separately and requires RSTL lock held, see setFollowerModeRollback.
        _readWriteAbility->setCanServeNonLocalReads_UNSAFE(0U);
    } else if (!_memberState.primary() && newState.secondary()) {
        // Switching into SECONDARY, but not from PRIMARY.
        _readWriteAbility->setCanServeNonLocalReads_UNSAFE(1U);
    }

    if (newState.secondary() && result != kActionSteppedDown &&
        _topCoord->isElectableNodeInSingleNodeReplicaSet()) {
        // When transitioning from other follower states to SECONDARY, run for election on a
        // single-node replica set.
        result = kActionStartSingleNodeElection;
    }

    // If we are transitioning from secondary, cancel any scheduled takeovers.
    if (_memberState.secondary()) {
        _cancelCatchupTakeover(lk);
        _cancelPriorityTakeover(lk);
    }

    // Ensure replication is running if we are no longer REMOVED.
    if (_memberState.removed() && !newState.arbiter()) {
        LOGV2(5268000, "Scheduling a task to begin or continue replication");
        _scheduleWorkAt(_replExecutor->now(),
                        [=, this](const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
                            _externalState->startThreads();
                            auto opCtx = cc().makeOperationContext();
                            _startDataReplication(opCtx.get());
                        });
    }


    if (newState.primary()) {
        // Since we are stepping up, we need to reset the primary majority read availability state
        // to track availabiltity of those reads in our new term.
        _primaryMajorityReadsAvailability.onBecomePrimary();
    }

    LOGV2(21358,
          "Replica set state transition",
          "newState"_attr = newState,
          "oldState"_attr = _memberState);

    // Initializes the featureCompatibilityVersion to the latest value, because arbiters do not
    // receive the replicated version. This is to avoid bugs like SERVER-32639.
    if (newState.arbiter()) {
        // (Generic FCV reference): This FCV check should exist across LTS binary versions.
        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().logFCVWithContext(
            "arbiter"_sd);
    }

    _memberState = newState;

    _cancelAndRescheduleElectionTimeout(lk);

    // Notifies waiters blocked in waitForMemberState().
    // For testing only.
    _memberStateChange.notify_all();

    return result;
}

void ReplicationCoordinatorImpl::_postWonElectionUpdateMemberState(WithLock lk) {
    invariant(_topCoord->getTerm() != OpTime::kUninitializedTerm);

    auto ts = VectorClockMutable::get(getServiceContext())->tickClusterTime(1).asTimestamp();
    _topCoord->processWinElection(ts);

    // Get the term from the topology coordinator, which we use to generate the election ID.
    // We intentionally wait until the end of this function to store the term in the
    // atomic shadow variable.
    auto electionIdTerm = _topCoord->getElectionIdTerm();

    ON_BLOCK_EXIT([&] { _electionIdTermShadow.store(electionIdTerm); });

    const PostMemberStateUpdateAction nextAction = _updateMemberStateFromTopologyCoordinator(lk);

    invariant(nextAction == kActionFollowerModeStateChange,
              str::stream() << "nextAction == " << static_cast<int>(nextAction));
    invariant(_getMemberState(lk).primary());
    // Clear the sync source.
    _onFollowerModeStateChange();

    // Notify all secondaries of the election win by cancelling all current heartbeats and sending
    // new heartbeat requests to all nodes. We must cancel and start instead of restarting scheduled
    // heartbeats because all heartbeats must be restarted upon election succeeding.
    _cancelHeartbeats(lk);
    _startHeartbeats(lk);

    invariant(!_catchupState);
    _catchupState = std::make_unique<CatchupState>(this);
    _catchupState->start(lk);
}

void ReplicationCoordinatorImpl::_setMyLastAppliedOpTimeAndWallTime(
    WithLock lk, const OpTimeAndWallTime& opTimeAndWallTime, bool isRollbackAllowed) {
    const auto opTime = opTimeAndWallTime.opTime;

    // The last applied opTime should never advance beyond the global timestamp (i.e. the latest
    // cluster time). Not enforced if the logical clock is disabled, e.g. for arbiters.
    dassert(!VectorClock::get(getServiceContext())->isEnabled() ||
            _externalState->getGlobalTimestamp(getServiceContext()) >= opTime.getTimestamp());

    _topCoord->setMyLastAppliedOpTimeAndWallTime(
        opTimeAndWallTime, _replExecutor->now(), isRollbackAllowed);

    // No need to wake up replication waiters because there should not be any replication waiters
    // waiting on our own lastApplied.

    // Update the storage engine's lastApplied snapshot before updating the stable timestamp on the
    // storage engine. New transactions reading from the lastApplied snapshot should start before
    // the oldest timestamp is advanced to avoid races. Additionally, update this snapshot before
    // signaling optime waiters. This avoids a race that would allow optime waiters to open
    // transactions on stale lastApplied values because they do not hold or reacquire the
    // replication coordinator mutex when signaled.
    _externalState->updateLastAppliedSnapshot(opTime);

    // Signal anyone waiting on optime changes.
    _lastAppliedOpTimeWaiterList.setValueIf(
        lk,
        [opTime](WithLock lk, const OpTime& waitOpTime, const SharedWaiterHandle& waiter) {
            return waitOpTime <= opTime;
        },
        opTime);

    if (opTime.isNull()) {
        return;
    }

    // Advance the stable timestamp if necessary. Stable timestamps are used to determine the latest
    // timestamp that it is safe to revert the database to, in the event of a rollback via the
    // 'recover to timestamp' method.
    invariant(opTime.getTimestamp().getInc() > 0,
              str::stream() << "Impossible optime received: " << opTime.toString());
    // If we are lagged behind the commit optime, set a new stable timestamp here.
    if (opTime <= _topCoord->getLastCommittedOpTime()) {
        _setStableTimestampForStorage(lk);
    }
}


}  // namespace repl
}  // namespace mongo

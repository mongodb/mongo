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

#define LOGV2_FOR_ELECTION(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                             \
        ID, DLEVEL, {logv2::LogComponent::kReplicationElection}, MESSAGE, ##__VA_ARGS__)
#define LOGV2_FOR_HEARTBEATS(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                               \
        ID, DLEVEL, {logv2::LogComponent::kReplicationHeartbeats}, MESSAGE, ##__VA_ARGS__)
#define LOGV2_FOR_SHARD_SPLIT(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kTenantMigration}, MESSAGE, ##__VA_ARGS__)

#include "mongo/platform/basic.h"

#include <algorithm>
#include <functional>

#include "mongo/base/status.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config_checks.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {

MONGO_FAIL_POINT_DEFINE(blockHeartbeatStepdown);
MONGO_FAIL_POINT_DEFINE(blockHeartbeatReconfigFinish);
MONGO_FAIL_POINT_DEFINE(hangAfterTrackingNewHandleInHandleHeartbeatResponseForTest);
MONGO_FAIL_POINT_DEFINE(waitForPostActionCompleteInHbReconfig);

}  // namespace

using executor::RemoteCommandRequest;

long long ReplicationCoordinatorImpl::_getElectionOffsetUpperBound_inlock() {
    long long electionTimeout = durationCount<Milliseconds>(_rsConfig.getElectionTimeoutPeriod());
    return electionTimeout * _externalState->getElectionTimeoutOffsetLimitFraction();
}

Milliseconds ReplicationCoordinatorImpl::_getRandomizedElectionOffset_inlock() {
    long long randomOffsetUpperBound = _getElectionOffsetUpperBound_inlock();
    // Avoid divide by zero error in random number generator.
    if (randomOffsetUpperBound == 0) {
        return Milliseconds(0);
    }

    return Milliseconds{_nextRandomInt64_inlock(randomOffsetUpperBound)};
}

void ReplicationCoordinatorImpl::_doMemberHeartbeat(executor::TaskExecutor::CallbackArgs cbData,
                                                    const HostAndPort& target,
                                                    const std::string& replSetName) {
    stdx::lock_guard<Latch> lk(_mutex);

    _untrackHeartbeatHandle_inlock(cbData.myHandle);
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }

    const Date_t now = _replExecutor->now();
    BSONObj heartbeatObj;
    Milliseconds timeout(0);
    const std::pair<ReplSetHeartbeatArgsV1, Milliseconds> hbRequest =
        _topCoord->prepareHeartbeatRequestV1(now, replSetName, target);
    heartbeatObj = hbRequest.first.toBSON();
    timeout = hbRequest.second;

    const RemoteCommandRequest request(
        target, "admin", heartbeatObj, BSON(rpc::kReplSetMetadataFieldName << 1), nullptr, timeout);
    const executor::TaskExecutor::RemoteCommandCallbackFn callback =
        [=](const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {
            return _handleHeartbeatResponse(cbData, replSetName);
        };

    LOGV2_FOR_HEARTBEATS(4615670,
                         2,
                         "Sending heartbeat (requestId: {requestId}) to {target} {heartbeatObj}",
                         "Sending heartbeat",
                         "requestId"_attr = request.id,
                         "target"_attr = target,
                         "heartbeatObj"_attr = heartbeatObj);
    _trackHeartbeatHandle_inlock(
        _replExecutor->scheduleRemoteCommand(request, callback), HeartbeatState::kSent, target);
}

void ReplicationCoordinatorImpl::_scheduleHeartbeatToTarget_inlock(const HostAndPort& target,
                                                                   Date_t when,
                                                                   std::string replSetName) {
    LOGV2_FOR_HEARTBEATS(4615618,
                         2,
                         "Scheduling heartbeat to {target} at {when}",
                         "Scheduling heartbeat",
                         "target"_attr = target,
                         "when"_attr = when);
    _trackHeartbeatHandle_inlock(
        _replExecutor->scheduleWorkAt(when,
                                      [=, replSetName = std::move(replSetName)](
                                          const executor::TaskExecutor::CallbackArgs& cbData) {
                                          _doMemberHeartbeat(cbData, target, replSetName);
                                      }),
        HeartbeatState::kScheduled,
        target);
}

void ReplicationCoordinatorImpl::handleHeartbeatResponse_forTest(BSONObj response,
                                                                 int targetIndex,
                                                                 Milliseconds ping) {
    // Make a handle to a valid no-op.
    CallbackHandle handle = uassertStatusOK(
        _replExecutor->scheduleWork([=](const executor::TaskExecutor::CallbackArgs& args) {}));
    RemoteCommandRequest request;

    {
        stdx::unique_lock<Latch> lk(_mutex);

        request.target = _rsConfig.getMemberAt(targetIndex).getHostAndPort();

        // Simulate preparing a heartbeat request so that the target's ping stats are initialized.
        _topCoord->prepareHeartbeatRequestV1(
            _replExecutor->now(), _rsConfig.getReplSetName(), request.target);

        // Pretend we sent a request so that _untrackHeartbeatHandle_inlock succeeds.
        _trackHeartbeatHandle_inlock(handle, HeartbeatState::kSent, request.target);
    }

    executor::TaskExecutor::ResponseStatus status(response, ping);
    executor::TaskExecutor::RemoteCommandCallbackArgs cbData(
        _replExecutor.get(), handle, request, status);

    hangAfterTrackingNewHandleInHandleHeartbeatResponseForTest.pauseWhileSet();

    _handleHeartbeatResponse(cbData, _rsConfig.getReplSetName().toString());
}

void ReplicationCoordinatorImpl::_handleHeartbeatResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData, const std::string& setName) {
    stdx::unique_lock<Latch> lk(_mutex);

    // remove handle from queued heartbeats
    _untrackHeartbeatHandle_inlock(cbData.myHandle);

    // Parse and validate the response.  At the end of this step, if responseStatus is OK then
    // hbResponse is valid.
    Status responseStatus = cbData.response.status;
    const HostAndPort& target = cbData.request.target;

    if (responseStatus == ErrorCodes::CallbackCanceled) {
        LOGV2_FOR_HEARTBEATS(4615619,
                             2,
                             "Received response to heartbeat (requestId: {requestId}) from "
                             "{target} but the heartbeat was cancelled.",
                             "Received response to heartbeat, but the heartbeat was cancelled",
                             "requestId"_attr = cbData.request.id,
                             "target"_attr = target);
        return;
    }

    ReplSetHeartbeatResponse hbResponse;
    BSONObj resp;
    if (responseStatus.isOK()) {
        resp = cbData.response.data;
        responseStatus = hbResponse.initialize(resp, _topCoord->getTerm());
        StatusWith<rpc::ReplSetMetadata> replMetadata =
            rpc::ReplSetMetadata::readFromMetadata(cbData.response.data);

        LOGV2_FOR_HEARTBEATS(
            4615620,
            2,
            "Received response to heartbeat (requestId: {requestId}) from {target}, {response}",
            "Received response to heartbeat",
            "requestId"_attr = cbData.request.id,
            "target"_attr = target,
            "response"_attr = resp);

        if (responseStatus.isOK() && _rsConfig.isInitialized() &&
            _rsConfig.getReplSetName() != hbResponse.getReplicaSetName()) {
            responseStatus =
                Status(ErrorCodes::InconsistentReplicaSetNames,
                       str::stream()
                           << "replica set names do not match, ours: " << _rsConfig.getReplSetName()
                           << "; remote node's: " << hbResponse.getReplicaSetName());
            // Ignore metadata.
            replMetadata = responseStatus;
        }

        // Reject heartbeat responses (and metadata) from nodes with mismatched replica set IDs.
        // It is problematic to perform this check in the heartbeat reconfiguring logic because it
        // is possible for two mismatched replica sets to have the same replica set name and
        // configuration version. A heartbeat reconfiguration would not take place in that case.
        // Additionally, this is where we would stop further processing of the metadata from an
        // unknown replica set.
        if (replMetadata.isOK() && _rsConfig.isInitialized() && _rsConfig.hasReplicaSetId() &&
            replMetadata.getValue().getReplicaSetId().isSet() &&
            _rsConfig.getReplicaSetId() != replMetadata.getValue().getReplicaSetId()) {
            responseStatus =
                Status(ErrorCodes::InvalidReplicaSetConfig,
                       str::stream()
                           << "replica set IDs do not match, ours: " << _rsConfig.getReplicaSetId()
                           << "; remote node's: " << replMetadata.getValue().getReplicaSetId());
            // Ignore metadata.
            replMetadata = responseStatus;
        }
        if (replMetadata.isOK()) {
            // It is safe to update our commit point via heartbeat propagation as long as the
            // the new commit point we learned of is on the same branch of history as our own
            // oplog.
            if (_getMemberState_inlock().arbiter() ||
                (!_getMemberState_inlock().startup() && !_getMemberState_inlock().startup2() &&
                 !_getMemberState_inlock().rollback())) {
                // The node that sent the heartbeat is not guaranteed to be our sync source.
                const bool fromSyncSource = false;
                _advanceCommitPoint(
                    lk, replMetadata.getValue().getLastOpCommitted(), fromSyncSource);
            }

            // Asynchronous stepdown could happen, but it will wait for _mutex and execute
            // after this function, so we cannot and don't need to wait for it to finish.
            _processReplSetMetadata_inlock(replMetadata.getValue());
        }

        // Arbiters are always expected to report null durable optimes (and wall times).
        // If that is not the case here, make sure to correct these times before ingesting them.
        auto memberInConfig = _rsConfig.findMemberByHostAndPort(target);
        if ((hbResponse.hasState() && hbResponse.getState().arbiter()) ||
            (_rsConfig.isInitialized() && memberInConfig && memberInConfig->isArbiter())) {
            if (hbResponse.hasDurableOpTime() &&
                (!hbResponse.getDurableOpTime().isNull() ||
                 hbResponse.getDurableOpTimeAndWallTime().wallTime != Date_t())) {
                LOGV2_FOR_HEARTBEATS(
                    5662000,
                    1,
                    "Received non-null durable optime/walltime for arbiter from heartbeat. "
                    "Ignoring value(s).",
                    "target"_attr = target,
                    "durableOpTimeAndWallTime"_attr = hbResponse.getDurableOpTimeAndWallTime());
                hbResponse.unsetDurableOpTimeAndWallTime();
            }
        }
    }
    const Date_t now = _replExecutor->now();
    Microseconds networkTime(0);
    StatusWith<ReplSetHeartbeatResponse> hbStatusResponse(hbResponse);

    if (responseStatus.isOK()) {
        networkTime = cbData.response.elapsed.value_or(Microseconds{0});
        // TODO(sz) Because the term is duplicated in ReplSetMetaData, we can get rid of this
        // and update tests.
        const auto& hbResponse = hbStatusResponse.getValue();
        _updateTerm_inlock(hbResponse.getTerm());
        // Postpone election timeout if we have a successful heartbeat response from the primary.
        if (hbResponse.hasState() && hbResponse.getState().primary() &&
            hbResponse.getTerm() == _topCoord->getTerm()) {
            LOGV2_FOR_ELECTION(
                4615659, 4, "Postponing election timeout due to heartbeat from primary");
            _cancelAndRescheduleElectionTimeout_inlock();
        }
    } else {
        LOGV2_FOR_HEARTBEATS(4615621,
                             2,
                             "Error in heartbeat (requestId: {requestId}) to {target}, "
                             "response status: {error}",
                             "Error in heartbeat",
                             "requestId"_attr = cbData.request.id,
                             "target"_attr = target,
                             "error"_attr = responseStatus);

        hbStatusResponse = StatusWith<ReplSetHeartbeatResponse>(responseStatus);
    }

    // Leaving networkTime units as ms since the average ping calulation may be affected.
    HeartbeatResponseAction action = _topCoord->processHeartbeatResponse(
        now, duration_cast<Milliseconds>(networkTime), target, hbStatusResponse);

    if (action.getAction() == HeartbeatResponseAction::NoAction && hbStatusResponse.isOK() &&
        hbStatusResponse.getValue().hasState() &&
        hbStatusResponse.getValue().getState() != MemberState::RS_PRIMARY) {
        if (action.getAdvancedOpTimeOrUpdatedConfig()) {
            // If a member's opTime has moved forward or config is newer, try to update the
            // lastCommitted. Even if we've only updated the config, this is still safe.
            _updateLastCommittedOpTimeAndWallTime(lk);
            // Wake up replication waiters on optime changes or updated configs.
            _wakeReadyWaiters(lk);
        } else if (action.getBecameElectable() && _topCoord->isSteppingDown()) {
            // Try to wake up the stepDown waiter when a new node becomes electable.
            _wakeReadyWaiters(lk);
        }
    }

    // When receiving a heartbeat response indicating that the remote is in a state past
    // STARTUP_2, the primary will initiate a reconfig to remove the 'newlyAdded' field for that
    // node (if present). This field is normally set when we add new members with votes:1 to the
    // set.
    if (_getMemberState_inlock().primary() && hbStatusResponse.isOK() &&
        hbStatusResponse.getValue().hasState()) {
        auto remoteState = hbStatusResponse.getValue().getState();
        if (remoteState == MemberState::RS_SECONDARY || remoteState == MemberState::RS_RECOVERING ||
            remoteState == MemberState::RS_ROLLBACK) {
            const auto mem = _rsConfig.findMemberByHostAndPort(target);
            if (mem && mem->isNewlyAdded()) {
                const auto memId = mem->getId();
                auto status = _replExecutor->scheduleWork(
                    [=](const executor::TaskExecutor::CallbackArgs& cbData) {
                        _reconfigToRemoveNewlyAddedField(
                            cbData, memId, _rsConfig.getConfigVersionAndTerm());
                    });

                if (!status.isOK()) {
                    LOGV2_DEBUG(4634500,
                                1,
                                "Failed to schedule work for removing 'newlyAdded' field.",
                                "memberId"_attr = memId.getData(),
                                "error"_attr = status.getStatus());
                } else {
                    LOGV2_DEBUG(4634501,
                                1,
                                "Scheduled automatic reconfig to remove 'newlyAdded' field.",
                                "memberId"_attr = memId.getData());
                }
            }
        }
    }

    // Abort catchup if we have caught up to the latest known optime after heartbeat refreshing.
    if (_catchupState) {
        _catchupState->signalHeartbeatUpdate_inlock();
    }

    // Cancel catchup takeover if the last applied write by any node in the replica set was made
    // in the current term, which implies that the primary has caught up.
    bool catchupTakeoverScheduled = _catchupTakeoverCbh.isValid();
    if (responseStatus.isOK() && catchupTakeoverScheduled && hbResponse.hasAppliedOpTime()) {
        const auto& hbLastAppliedOpTime = hbResponse.getAppliedOpTime();
        if (hbLastAppliedOpTime.getTerm() == _topCoord->getTerm()) {
            _cancelCatchupTakeover_inlock();
        }
    }

    _scheduleHeartbeatToTarget_inlock(
        target, std::max(now, action.getNextHeartbeatStartDate()), setName);

    _handleHeartbeatResponseAction_inlock(action, hbStatusResponse, std::move(lk));
}

stdx::unique_lock<Latch> ReplicationCoordinatorImpl::_handleHeartbeatResponseAction_inlock(
    const HeartbeatResponseAction& action,
    const StatusWith<ReplSetHeartbeatResponse>& responseStatus,
    stdx::unique_lock<Latch> lock) {
    invariant(lock.owns_lock());
    switch (action.getAction()) {
        case HeartbeatResponseAction::NoAction:
            // Update the cached member state if different than the current topology member state
            if (_memberState != _topCoord->getMemberState()) {
                const PostMemberStateUpdateAction postUpdateAction =
                    _updateMemberStateFromTopologyCoordinator(lock);
                lock.unlock();
                _performPostMemberStateUpdateAction(postUpdateAction);
                lock.lock();
            }
            break;
        case HeartbeatResponseAction::Reconfig:
            invariant(responseStatus.isOK());
            _scheduleHeartbeatReconfig(lock, responseStatus.getValue().getConfig());
            break;
        case HeartbeatResponseAction::RetryReconfig:
            _scheduleHeartbeatReconfig(lock, _rsConfig);
            break;
        case HeartbeatResponseAction::StepDownSelf:
            invariant(action.getPrimaryConfigIndex() == _selfIndex);
            if (_topCoord->prepareForUnconditionalStepDown()) {
                LOGV2(21475, "Stepping down from primary in response to heartbeat");
                _stepDownStart();
            } else {
                LOGV2_DEBUG(21476,
                            2,
                            "Heartbeat would have triggered a stepdown, but we're already in the "
                            "process of stepping down");
            }
            break;
        case HeartbeatResponseAction::PriorityTakeover: {
            // Don't schedule a priority takeover if any takeover is already scheduled.
            if (!_priorityTakeoverCbh.isValid() && !_catchupTakeoverCbh.isValid()) {

                // Add randomized offset to calculated priority takeover delay.
                Milliseconds priorityTakeoverDelay = _rsConfig.getPriorityTakeoverDelay(_selfIndex);
                Milliseconds randomOffset = _getRandomizedElectionOffset_inlock();
                _priorityTakeoverWhen = _replExecutor->now() + priorityTakeoverDelay + randomOffset;
                LOGV2_FOR_ELECTION(4615601,
                                   0,
                                   "Scheduling priority takeover at {when}",
                                   "Scheduling priority takeover",
                                   "when"_attr = _priorityTakeoverWhen);
                _priorityTakeoverCbh = _scheduleWorkAt(
                    _priorityTakeoverWhen, [=](const mongo::executor::TaskExecutor::CallbackArgs&) {
                        _startElectSelfIfEligibleV1(StartElectionReasonEnum::kPriorityTakeover);
                    });
            }
            break;
        }
        case HeartbeatResponseAction::CatchupTakeover: {
            // Don't schedule a catchup takeover if any takeover is already scheduled.
            if (!_catchupTakeoverCbh.isValid() && !_priorityTakeoverCbh.isValid()) {
                Milliseconds catchupTakeoverDelay = _rsConfig.getCatchUpTakeoverDelay();
                _catchupTakeoverWhen = _replExecutor->now() + catchupTakeoverDelay;
                LOGV2_FOR_ELECTION(4615648,
                                   0,
                                   "Scheduling catchup takeover at {when}",
                                   "Scheduling catchup takeover",
                                   "when"_attr = _catchupTakeoverWhen);
                _catchupTakeoverCbh = _scheduleWorkAt(
                    _catchupTakeoverWhen, [=](const mongo::executor::TaskExecutor::CallbackArgs&) {
                        _startElectSelfIfEligibleV1(StartElectionReasonEnum::kCatchupTakeover);
                    });
            }
            break;
        }
    }
    if (action.getChangedSignificantly()) {
        lock.unlock();
        _externalState->notifyOtherMemberDataChanged();
        lock.lock();
    }
    return lock;
}

namespace {
/**
 * This callback is purely for logging and has no effect on any other operations
 */
void remoteStepdownCallback(const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {
    const Status status = cbData.response.status;
    if (status == ErrorCodes::CallbackCanceled) {
        return;
    }

    if (status.isOK()) {
        LOGV2_DEBUG(21477,
                    1,
                    "stepdown of primary({primary}) succeeded with response -- "
                    "{response}",
                    "Stepdown of primary succeeded",
                    "primary"_attr = cbData.request.target,
                    "response"_attr = cbData.response.data);
    } else {
        LOGV2_WARNING(21486,
                      "stepdown of primary({primary}) failed due to {error}",
                      "Stepdown of primary failed",
                      "primary"_attr = cbData.request.target,
                      "error"_attr = cbData.response.status);
    }
}
}  // namespace

executor::TaskExecutor::EventHandle ReplicationCoordinatorImpl::_stepDownStart() {
    auto finishEvent = _makeEvent();
    if (!finishEvent) {
        return finishEvent;
    }

    _replExecutor
        ->scheduleWork([=](const executor::TaskExecutor::CallbackArgs& cbData) {
            _stepDownFinish(cbData, finishEvent);
        })
        .status_with_transitional_ignore();
    return finishEvent;
}

void ReplicationCoordinatorImpl::_stepDownFinish(
    const executor::TaskExecutor::CallbackArgs& cbData,
    const executor::TaskExecutor::EventHandle& finishedEvent) {

    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }

    if (MONGO_unlikely(blockHeartbeatStepdown.shouldFail())) {
        // This log output is used in js tests so please leave it.
        LOGV2(21479,
              "stepDown - blockHeartbeatStepdown fail point enabled. "
              "Blocking until fail point is disabled.");

        auto inShutdown = [&] {
            stdx::lock_guard<Latch> lk(_mutex);
            return _inShutdown;
        };

        while (MONGO_unlikely(blockHeartbeatStepdown.shouldFail()) && !inShutdown()) {
            mongo::sleepsecs(1);
        }
    }

    auto opCtx = cc().makeOperationContext();

    // kill all write operations which are no longer safe to run on step down. Also, operations that
    // have taken global lock in S mode and operations blocked on prepare conflict will be killed to
    // avoid 3-way deadlock between read, prepared transaction and step down thread.
    AutoGetRstlForStepUpStepDown arsd(
        this, opCtx.get(), ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown);
    stdx::unique_lock<Latch> lk(_mutex);

    // This node has already stepped down due to reconfig. So, signal anyone who is waiting on the
    // step down event.
    if (!_topCoord->isSteppingDownUnconditionally()) {
        _replExecutor->signalEvent(finishedEvent);
        return;
    }

    // We need to release the mutex before yielding locks for prepared transactions, which might
    // check out sessions, to avoid deadlocks with checked-out sessions accessing this mutex.
    lk.unlock();

    yieldLocksForPreparedTransactions(opCtx.get());
    invalidateSessionsForStepdown(opCtx.get());

    lk.lock();

    // Clear the node's election candidate metrics since it is no longer primary.
    ReplicationMetrics::get(opCtx.get()).clearElectionCandidateMetrics();

    _topCoord->finishUnconditionalStepDown();

    // Update _canAcceptNonLocalWrites.
    _updateWriteAbilityFromTopologyCoordinator(lk, opCtx.get());

    const auto action = _updateMemberStateFromTopologyCoordinator(lk);
    if (_pendingTermUpdateDuringStepDown) {
        TopologyCoordinator::UpdateTermResult result;
        _updateTerm_inlock(*_pendingTermUpdateDuringStepDown, &result);
        // We've just stepped down due to the "term", so it's impossible to step down again
        // for the same term.
        invariant(result != TopologyCoordinator::UpdateTermResult::kTriggerStepDown);
        _pendingTermUpdateDuringStepDown = boost::none;
    }
    lk.unlock();
    _performPostMemberStateUpdateAction(action);
    _replExecutor->signalEvent(finishedEvent);
}

bool ReplicationCoordinatorImpl::_shouldStepDownOnReconfig(WithLock,
                                                           const ReplSetConfig& newConfig,
                                                           StatusWith<int> myIndex) {
    return _memberState.primary() &&
        !(myIndex.isOK() && newConfig.getMemberAt(myIndex.getValue()).isElectable());
}

void ReplicationCoordinatorImpl::_scheduleHeartbeatReconfig(WithLock lk,
                                                            const ReplSetConfig& newConfig) {
    if (_inShutdown) {
        return;
    }

    switch (_rsConfigState) {
        case kConfigUninitialized:
        case kConfigSteady:
            LOGV2_FOR_HEARTBEATS(4615622,
                                 1,
                                 "Received new config via heartbeat with {newConfigVersionAndTerm}",
                                 "Received new config via heartbeat",
                                 "newConfigVersionAndTerm"_attr =
                                     newConfig.getConfigVersionAndTerm());
            break;
        case kConfigInitiating:
        case kConfigReconfiguring:
        case kConfigHBReconfiguring:
            LOGV2_FOR_HEARTBEATS(
                4615623,
                1,
                "Ignoring new configuration with {newConfigVersionAndTerm} because "
                "already in the midst of a configuration process.",
                "Ignoring new configuration because we are already in the midst of a configuration "
                "process",
                "newConfigVersionAndTerm"_attr = newConfig.getConfigVersionAndTerm());
            return;
        case kConfigPreStart:
        case kConfigStartingUp:
        case kConfigReplicationDisabled:
            LOGV2_FATAL(18807,
                        "Reconfiguration request occurred while _rsConfigState == "
                        "{_rsConfigState}; aborting.",
                        "Aborting reconfiguration request",
                        "_rsConfigState"_attr = int(_rsConfigState));
    }

    // Allow force reconfigs to proceed even if we are not a writable primary yet.
    if (_memberState.primary() && !_readWriteAbility->canAcceptNonLocalWrites(lk) &&
        newConfig.getConfigTerm() != OpTime::kUninitializedTerm) {
        LOGV2_FOR_HEARTBEATS(
            4794900,
            1,
            "Not scheduling a heartbeat reconfig since we are in PRIMARY state but "
            "cannot accept writes yet.");
        return;
    }

    // Prevent heartbeat reconfigs from running concurrently with an election.
    if (_topCoord->getRole() == TopologyCoordinator::Role::kCandidate) {
        LOGV2_FOR_HEARTBEATS(
            482570, 1, "Not scheduling a heartbeat reconfig when running for election");
        return;
    }

    _setConfigState_inlock(kConfigHBReconfiguring);
    invariant(!_rsConfig.isInitialized() ||
              _rsConfig.getConfigVersionAndTerm() < newConfig.getConfigVersionAndTerm() ||
              _selfIndex < 0);
    _replExecutor
        ->scheduleWork([=](const executor::TaskExecutor::CallbackArgs& cbData) {
            _heartbeatReconfigStore(cbData, newConfig);
        })
        .status_with_transitional_ignore();
}

std::tuple<StatusWith<ReplSetConfig>, boost::optional<OpTime>>
ReplicationCoordinatorImpl::_resolveConfigToApply(const ReplSetConfig& config) {
    if (!_settings.isServerless() || !config.isSplitConfig()) {
        return {config, boost::none};
    }

    stdx::unique_lock<Latch> lk(_mutex);
    if (!_rsConfig.isInitialized()) {
        // Unlock the lock because isSelf performs network I/O.
        lk.unlock();

        // If this node is listed in the members of incoming config, accept the config.
        const auto foundSelfInMembers = std::any_of(
            config.membersBegin(),
            config.membersEnd(),
            [externalState = _externalState.get()](const MemberConfig& config) {
                return externalState->isSelf(config.getHostAndPort(), getGlobalServiceContext());
            });

        if (foundSelfInMembers) {
            return {config, boost::none};
        }

        return {Status(ErrorCodes::NotYetInitialized,
                       "Cannot apply a split config if the current config is uninitialized"),
                boost::none};
    }

    auto recipientConfig = config.getRecipientConfig();
    const auto& selfMember = _rsConfig.getMemberAt(_selfIndex);
    if (recipientConfig->findMemberByHostAndPort(selfMember.getHostAndPort())) {
        if (selfMember.getNumVotes() > 0) {
            return {Status(ErrorCodes::BadValue, "Cannot apply recipient config to a voting node"),
                    boost::none};
        }

        if (_rsConfig.getReplSetName() == recipientConfig->getReplSetName()) {
            return {Status(ErrorCodes::InvalidReplicaSetConfig,
                           "Cannot apply recipient config since current config and recipient "
                           "config have the same set name."),
                    boost::none};
        }

        invariant(recipientConfig->getShardSplitBlockOpTime());
        auto shardSplitBlockOpTime = *recipientConfig->getShardSplitBlockOpTime();
        auto mutableConfig = recipientConfig->getMutable();
        mutableConfig.removeShardSplitBlockOpTime();

        return {ReplSetConfig(std::move(mutableConfig)), shardSplitBlockOpTime};
    }

    return {config, boost::none};
}

void ReplicationCoordinatorImpl::_heartbeatReconfigStore(
    const executor::TaskExecutor::CallbackArgs& cbd, const ReplSetConfig& newConfig) {

    if (cbd.status.code() == ErrorCodes::CallbackCanceled) {
        LOGV2(21480,
              "The callback to persist the replica set configuration was canceled - the "
              "configuration was not persisted but was used: {newConfig}",
              "The callback to persist the replica set configuration was canceled - the "
              "configuration was not persisted but was used",
              "newConfig"_attr = newConfig.toBSON());
        return;
    }

    const auto [swConfig, shardSplitBlockOpTime] = _resolveConfigToApply(newConfig);
    if (!swConfig.isOK()) {
        LOGV2_WARNING(6234600,
                      "Ignoring new configuration in heartbeat response because it is invalid",
                      "status"_attr = swConfig.getStatus());

        stdx::lock_guard<Latch> lg(_mutex);
        invariant(_rsConfigState == kConfigHBReconfiguring);
        _setConfigState_inlock(!_rsConfig.isInitialized() ? kConfigUninitialized : kConfigSteady);
        return;
    }

    const auto configToApply = swConfig.getValue();
    if (shardSplitBlockOpTime) {
        LOGV2(6309200,
              "Applying a recipient config for a shard split operation.",
              "config"_attr = configToApply);
    }

    const auto myIndex = [&]() -> StatusWith<int> {
        // We always check the config when _selfIndex is not valid, in order to be able to
        // recover from transient DNS errors.
        {
            stdx::lock_guard<Latch> lk(_mutex);
            if (_selfIndex >= 0 && sameConfigContents(_rsConfig, configToApply)) {
                LOGV2_FOR_HEARTBEATS(6351200,
                                     2,
                                     "New heartbeat config is only a version/term change, skipping "
                                     "validation checks",
                                     "oldConfig"_attr = _rsConfig,
                                     "newConfig"_attr = configToApply);
                // If the configs are the same, so is our index.
                return _selfIndex;
            }
        }
        return validateConfigForHeartbeatReconfig(
            _externalState.get(), configToApply, getGlobalServiceContext());
    }();

    if (myIndex.getStatus() == ErrorCodes::NodeNotFound) {
        stdx::lock_guard<Latch> lk(_mutex);
        // If this node absent in configToApply, and this node was not previously initialized,
        // return to kConfigUninitialized immediately, rather than storing the config and
        // transitioning into the RS_REMOVED state.  See SERVER-15740.
        if (!_rsConfig.isInitialized()) {
            invariant(_rsConfigState == kConfigHBReconfiguring);
            LOGV2_FOR_HEARTBEATS(4615625,
                                 1,
                                 "Ignoring new configuration in heartbeat response because we "
                                 "are uninitialized and not a member of the new configuration");
            _setConfigState_inlock(kConfigUninitialized);
            return;
        }
    }

    bool shouldStartDataReplication = false;
    if (!myIndex.getStatus().isOK() && myIndex.getStatus() != ErrorCodes::NodeNotFound) {
        LOGV2_WARNING(21487,
                      "Not persisting new configuration in heartbeat response to disk because "
                      "it is invalid: {error}",
                      "Not persisting new configuration in heartbeat response to disk because "
                      "it is invalid",
                      "error"_attr = myIndex.getStatus());
    } else {
        LOGV2_FOR_HEARTBEATS(4615626,
                             2,
                             "Config with {configToApplyVersionAndTerm} validated for "
                             "reconfig; persisting to disk.",
                             "Config validated for reconfig; persisting to disk",
                             "configToApplyVersionAndTerm"_attr =
                                 configToApply.getConfigVersionAndTerm());

        auto opCtx = cc().makeOperationContext();
        // Don't write the no-op for config learned via heartbeats.
        auto status = [&, isRecipientConfig = shardSplitBlockOpTime.has_value()]() {
            if (isRecipientConfig) {
                return _externalState->replaceLocalConfigDocument(opCtx.get(),
                                                                  configToApply.toBSON());
            } else {
                return _externalState->storeLocalConfigDocument(
                    opCtx.get(), configToApply.toBSON(), false /* writeOplog */);
            }
        }();

        // Wait for durability of the new config document.
        try {
            JournalFlusher::get(opCtx.get())->waitForJournalFlush();
        } catch (const ExceptionFor<ErrorCodes::InterruptedDueToStorageChange>& e) {
            // Anyone changing the storage engine is responsible for copying the on-disk
            // configuration between the old engine and the new.
            LOGV2_DEBUG(6121300,
                        1,
                        "Storage engine changed while waiting for new config to become durable.",
                        "error"_attr = e.toStatus());
        }

        bool isFirstConfig;
        {
            stdx::lock_guard<Latch> lk(_mutex);
            isFirstConfig = !_rsConfig.isInitialized();
            if (!status.isOK()) {
                LOGV2_ERROR(21488,
                            "Ignoring new configuration in heartbeat response because we failed to"
                            " write it to stable storage; {error}",
                            "Ignoring new configuration in heartbeat response because we failed to"
                            " write it to stable storage",
                            "error"_attr = status);
                invariant(_rsConfigState == kConfigHBReconfiguring);
                if (isFirstConfig) {
                    _setConfigState_inlock(kConfigUninitialized);
                } else {
                    _setConfigState_inlock(kConfigSteady);
                }
                return;
            }
        }

        bool isArbiter = myIndex.isOK() && myIndex.getValue() != -1 &&
            configToApply.getMemberAt(myIndex.getValue()).isArbiter();

        if (isArbiter) {
            ReplicaSetAwareServiceRegistry::get(_service).onBecomeArbiter();
        }

        if (!isArbiter && myIndex.isOK() && myIndex.getValue() != -1) {
            shouldStartDataReplication = true;
        }

        if (shardSplitBlockOpTime) {
            // Donor access blockers are removed from donor nodes via the shard split op observer.
            // Donor access blockers are removed from recipient nodes when the node applies the
            // recipient config. When the recipient primary steps up it will delete its state
            // document, the call to remove access blockers there will be a no-op.

            LOGV2_FOR_SHARD_SPLIT(8423354, 1, "Removing donor access blockers on recipient node.");
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAll(TenantMigrationAccessBlocker::BlockerType::kDonor);
        }

        LOGV2_FOR_HEARTBEATS(
            4615627,
            2,
            "New configuration with {configToApplyVersionAndTerm} persisted "
            "to local storage; installing new config in memory",
            "New configuration persisted to local storage; installing new config in memory",
            "configToApplyVersionAndTerm"_attr = configToApply.getConfigVersionAndTerm());
    }

    _heartbeatReconfigFinish(cbd, configToApply, myIndex, shardSplitBlockOpTime);

    // Start data replication after the config has been installed.
    if (shouldStartDataReplication) {
        while (true) {
            try {
                auto opCtx = cc().makeOperationContext();
                // Initializing minvalid is not allowed to be interrupted.  Make sure it
                // can't be interrupted by a storage change by taking the global lock first.
                {
                    Lock::GlobalLock lk(opCtx.get(), MODE_IX);
                    _replicationProcess->getConsistencyMarkers()->initializeMinValidDocument(
                        opCtx.get());
                }
                _externalState->startThreads();
                _startDataReplication(opCtx.get());
                break;
            } catch (const ExceptionFor<ErrorCodes::InterruptedDueToStorageChange>& e) {
                LOGV2_DEBUG(6137701,
                            1,
                            "Interrupted while trying to start data replication",
                            "error"_attr = e.toStatus());
            }
        }
    }
}

void ReplicationCoordinatorImpl::_heartbeatReconfigFinish(
    const executor::TaskExecutor::CallbackArgs& cbData,
    const ReplSetConfig& newConfig,
    StatusWith<int> myIndex,
    boost::optional<OpTime> shardSplitBlockOpTime) {
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }

    if (MONGO_unlikely(blockHeartbeatReconfigFinish.shouldFail())) {
        LOGV2_FOR_HEARTBEATS(4615628,
                             0,
                             "blockHeartbeatReconfigFinish fail point enabled. Rescheduling "
                             "_heartbeatReconfigFinish until fail point is disabled");
        _replExecutor
            ->scheduleWorkAt(_replExecutor->now() + Milliseconds{10},
                             [=](const executor::TaskExecutor::CallbackArgs& cbData) {
                                 _heartbeatReconfigFinish(
                                     cbData, newConfig, myIndex, shardSplitBlockOpTime);
                             })
            .status_with_transitional_ignore();
        return;
    }

    // Do not conduct an election during a reconfig, as the node may not be electable post-reconfig.
    // If there is an election in-progress, there can be at most one. No new election can happen as
    // we have already set our ReplicationCoordinatorImpl::_rsConfigState state to
    // "kConfigReconfiguring" which prevents new elections from happening.
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (auto electionFinishedEvent = _cancelElectionIfNeeded(lk)) {
            LOGV2_FOR_HEARTBEATS(4615629,
                                 0,
                                 "Waiting for election to complete before finishing reconfig to "
                                 "config with {newConfigVersionAndTerm}",
                                 "Waiting for election to complete before finishing reconfig",
                                 "newConfigVersionAndTerm"_attr =
                                     newConfig.getConfigVersionAndTerm());
            // Wait for the election to complete and the node's Role to be set to follower.
            _replExecutor
                ->onEvent(electionFinishedEvent,
                          [=](const executor::TaskExecutor::CallbackArgs& cbData) {
                              _heartbeatReconfigFinish(
                                  cbData, newConfig, myIndex, shardSplitBlockOpTime);
                          })
                .status_with_transitional_ignore();
            return;
        }
    }

    auto opCtx = cc().makeOperationContext();

    boost::optional<AutoGetRstlForStepUpStepDown> arsd;
    stdx::unique_lock<Latch> lk(_mutex);
    if (_shouldStepDownOnReconfig(lk, newConfig, myIndex)) {
        _topCoord->prepareForUnconditionalStepDown();
        lk.unlock();

        // Primary node will be either unelectable or removed after the configuration change.
        // So, finish the reconfig under RSTL, so that the step down occurs safely.
        arsd.emplace(
            this, opCtx.get(), ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown);

        lk.lock();
        if (_topCoord->isSteppingDownUnconditionally()) {
            invariant(opCtx->lockState()->isRSTLExclusive());
            LOGV2(21481,
                  "Stepping down from primary, because we received a new config via heartbeat");
            // We need to release the mutex before yielding locks for prepared transactions, which
            // might check out sessions, to avoid deadlocks with checked-out sessions accessing
            // this mutex.
            lk.unlock();

            yieldLocksForPreparedTransactions(opCtx.get());
            invalidateSessionsForStepdown(opCtx.get());

            lk.lock();

            // Clear the node's election candidate metrics since it is no longer primary.
            ReplicationMetrics::get(opCtx.get()).clearElectionCandidateMetrics();

            // Update _canAcceptNonLocalWrites.
            _updateWriteAbilityFromTopologyCoordinator(lk, opCtx.get());
        } else {
            // Release the rstl lock as the node might have stepped down due to
            // other unconditional step down code paths like learning new term via heartbeat &
            // liveness timeout. And, no new election can happen as we have already set our
            // ReplicationCoordinatorImpl::_rsConfigState state to "kConfigReconfiguring" which
            // prevents new elections from happening. So, its safe to release the RSTL lock.
            arsd.reset();
        }
    }

    invariant(_rsConfigState == kConfigHBReconfiguring);
    invariant(!_rsConfig.isInitialized() ||
              _rsConfig.getConfigVersionAndTerm() < newConfig.getConfigVersionAndTerm() ||
              _selfIndex < 0 || shardSplitBlockOpTime);

    if (!myIndex.isOK()) {
        switch (myIndex.getStatus().code()) {
            case ErrorCodes::NodeNotFound:
                LOGV2(21482,
                      "Cannot find self in new replica set configuration; I must be removed; "
                      "{error}",
                      "Cannot find self in new replica set configuration; I must be removed",
                      "error"_attr = myIndex.getStatus());
                break;
            case ErrorCodes::InvalidReplicaSetConfig:
                LOGV2_ERROR(21489,
                            "Several entries in new config represent this node; "
                            "Removing self until an acceptable configuration arrives; {error}",
                            "Several entries in new config represent this node; "
                            "Removing self until an acceptable configuration arrives",
                            "error"_attr = myIndex.getStatus());
                break;
            default:
                LOGV2_ERROR(21490,
                            "Could not validate configuration received from remote node; "
                            "Removing self until an acceptable configuration arrives; {error}",
                            "Could not validate configuration received from remote node; "
                            "Removing self until an acceptable configuration arrives",
                            "error"_attr = myIndex.getStatus());
                break;
        }
        myIndex = StatusWith<int>(-1);
    }
    const bool contentChanged = !sameConfigContents(_rsConfig, newConfig);
    // If we do not have an index, we should pass -1 as our index to avoid falsely adding ourself to
    // the data structures inside of the TopologyCoordinator.
    const int myIndexValue = myIndex.getStatus().isOK() ? myIndex.getValue() : -1;

    const PostMemberStateUpdateAction action =
        _setCurrentRSConfig(lk, opCtx.get(), newConfig, myIndexValue);

    if (shardSplitBlockOpTime) {
        _topCoord->resetLastCommittedOpTime(*shardSplitBlockOpTime);
    }

    lk.unlock();
    if (contentChanged) {
        _externalState->notifyOtherMemberDataChanged();
    }
    _performPostMemberStateUpdateAction(action);
    if (MONGO_unlikely(waitForPostActionCompleteInHbReconfig.shouldFail())) {
        // Used in tests that wait for the post member state update action to complete.
        // eg. Closing connections upon being removed.
        LOGV2(5286701, "waitForPostActionCompleteInHbReconfig failpoint enabled");
    }
}

void ReplicationCoordinatorImpl::_trackHeartbeatHandle_inlock(
    const StatusWith<executor::TaskExecutor::CallbackHandle>& handle,
    HeartbeatState hbState,
    const HostAndPort& target) {
    if (handle.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(18912, handle.getStatus());

    // The target's HostAndPort should be safe to store, because it cannot change without a
    // reconfig. On reconfig, all current heartbeats get cancelled and new requests are sent out, so
    // there should not be a situation where the target node's HostAndPort changes but this
    // heartbeat handle remains active.
    _heartbeatHandles.push_back({handle.getValue(), hbState, target});
}

void ReplicationCoordinatorImpl::_untrackHeartbeatHandle_inlock(
    const executor::TaskExecutor::CallbackHandle& handle) {
    const auto newEnd = std::remove_if(_heartbeatHandles.begin(),
                                       _heartbeatHandles.end(),
                                       [&](auto& hbHandle) { return hbHandle.handle == handle; });
    invariant(newEnd != _heartbeatHandles.end());
    _heartbeatHandles.erase(newEnd, _heartbeatHandles.end());
}

void ReplicationCoordinatorImpl::_cancelHeartbeats_inlock() {
    LOGV2_FOR_HEARTBEATS(4615630, 2, "Cancelling all heartbeats");

    for (const auto& hbHandle : _heartbeatHandles) {
        _replExecutor->cancel(hbHandle.handle);
    }
    // Heartbeat callbacks will remove themselves from _heartbeatHandles when they execute with
    // CallbackCanceled status, so it's better to leave the handles in the list, for now.

    _handleLivenessTimeoutCallback.cancel();
}

void ReplicationCoordinatorImpl::restartScheduledHeartbeats_forTest() {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(getTestCommandsEnabled());
    _restartScheduledHeartbeats_inlock(_rsConfig.getReplSetName().toString());
};

void ReplicationCoordinatorImpl::_restartScheduledHeartbeats_inlock(
    const std::string& replSetName) {
    LOGV2_FOR_HEARTBEATS(5031800, 2, "Restarting all scheduled heartbeats");

    const Date_t now = _replExecutor->now();
    stdx::unordered_set<HostAndPort> restartedTargets;

    for (auto& hbHandle : _heartbeatHandles) {
        // Only cancel heartbeats that are scheduled. If a heartbeat request has already been
        // sent, we should wait for the response instead.
        if (hbHandle.hbState != HeartbeatState::kScheduled) {
            continue;
        }

        LOGV2_FOR_HEARTBEATS(5031802, 2, "Restarting heartbeat", "target"_attr = hbHandle.target);
        _replExecutor->cancel(hbHandle.handle);

        // Track the members that we have cancelled heartbeats.
        restartedTargets.insert(hbHandle.target);
    }

    for (auto target : restartedTargets) {
        _scheduleHeartbeatToTarget_inlock(target, now, replSetName);
        _topCoord->restartHeartbeat(now, target);
    }
}

void ReplicationCoordinatorImpl::_startHeartbeats_inlock() {
    const Date_t now = _replExecutor->now();
    _seedList.clear();

    for (int i = 0; i < _rsConfig.getNumMembers(); ++i) {
        if (i == _selfIndex) {
            continue;
        }
        auto target = _rsConfig.getMemberAt(i).getHostAndPort();
        _scheduleHeartbeatToTarget_inlock(target, now, _rsConfig.getReplSetName().toString());
        _topCoord->restartHeartbeat(now, target);
    }

    _scheduleNextLivenessUpdate_inlock(/* reschedule = */ false);
}

void ReplicationCoordinatorImpl::_handleLivenessTimeout(
    const executor::TaskExecutor::CallbackArgs& cbData) {
    stdx::unique_lock<Latch> lk(_mutex);
    if (!cbData.status.isOK()) {
        return;
    }

    // Scan liveness table for problems and mark nodes as down by calling into topocoord.
    HeartbeatResponseAction action = _topCoord->checkMemberTimeouts(_replExecutor->now());
    // Don't mind potential asynchronous stepdown as this is the last step of
    // liveness check.
    lk = _handleHeartbeatResponseAction_inlock(
        action, StatusWith(ReplSetHeartbeatResponse()), std::move(lk));

    _scheduleNextLivenessUpdate_inlock(/* reschedule = */ false);
}

void ReplicationCoordinatorImpl::_scheduleNextLivenessUpdate_inlock(bool reschedule) {
    // Scan liveness table for earliest date; schedule a run at (that date plus election
    // timeout).
    Date_t earliestDate;
    MemberId earliestMemberId;
    std::tie(earliestMemberId, earliestDate) = _topCoord->getStalestLiveMember();

    if (!earliestMemberId || earliestDate == Date_t::max()) {
        _earliestMemberId = -1;
        // Nobody here but us.
        return;
    }

    if (!reschedule && _handleLivenessTimeoutCallback.isActive()) {
        // don't bother to schedule; one is already scheduled and pending.
        return;
    }

    auto nextTimeout = earliestDate + _rsConfig.getElectionTimeoutPeriod();
    LOGV2_DEBUG(21483,
                3,
                "scheduling next check at {nextTimeout}",
                "Scheduling next check",
                "nextTimeout"_attr = nextTimeout);

    // It is possible we will schedule the next timeout in the past.
    // DelayableTimeoutCallback schedules its work immediately if it's given a time <= now().
    // If we missed the timeout, it means that on our last check the earliest live member was
    // just barely fresh and it has become stale since then. We must schedule another liveness
    // check to continue conducting liveness checks and be able to step down from primary if we
    // lose contact with a majority of nodes.
    // We ignore shutdown errors; any other error triggers an fassert.
    _handleLivenessTimeoutCallback.delayUntil(nextTimeout).ignore();
    _earliestMemberId = earliestMemberId.getData();
}

void ReplicationCoordinatorImpl::_rescheduleLivenessUpdate_inlock(int updatedMemberId) {
    if ((_earliestMemberId != -1) && (_earliestMemberId != updatedMemberId)) {
        return;
    }
    _scheduleNextLivenessUpdate_inlock(/* reschedule = */ true);
}

void ReplicationCoordinatorImpl::_cancelPriorityTakeover_inlock() {
    if (_priorityTakeoverCbh.isValid()) {
        LOGV2(21484, "Canceling priority takeover callback");
        _replExecutor->cancel(_priorityTakeoverCbh);
        _priorityTakeoverCbh = CallbackHandle();
        _priorityTakeoverWhen = Date_t();
    }
}

void ReplicationCoordinatorImpl::_cancelCatchupTakeover_inlock() {
    if (_catchupTakeoverCbh.isValid()) {
        LOGV2(21485, "Canceling catchup takeover callback");
        _replExecutor->cancel(_catchupTakeoverCbh);
        _catchupTakeoverCbh = CallbackHandle();
        _catchupTakeoverWhen = Date_t();
    }
}

void ReplicationCoordinatorImpl::_cancelAndRescheduleElectionTimeout_inlock() {
    // We log at level 5 except when:
    // * This is the first time we're scheduling after becoming an electable secondary.
    // * We are not going to reschedule the election timeout because we are shutting down or
    //   no longer an electable secondary.
    // * It has been at least a second since we last logged at level 4.
    //
    // In those instances we log at level 4.  This routine is called on every replication batch,
    // which would produce many log lines per second, so this logging strategy provides a
    // compromise which allows us to see the election timeout being rescheduled without spamming
    // the logs.
    int cancelAndRescheduleLogLevel = 5;
    static auto logThrottleTime = _replExecutor->now();
    auto oldWhen = _handleElectionTimeoutCallback.getNextCall();
    const bool wasActive = oldWhen != Date_t();
    auto now = _replExecutor->now();
    const bool doNotReschedule = _inShutdown || !_memberState.secondary() || _selfIndex < 0 ||
        !_rsConfig.getMemberAt(_selfIndex).isElectable();

    if (doNotReschedule || !wasActive || (now - logThrottleTime) >= Seconds(1)) {
        cancelAndRescheduleLogLevel = 4;
        logThrottleTime = now;
    }
    if (wasActive && doNotReschedule) {
        LOGV2_FOR_ELECTION(4615649,
                           cancelAndRescheduleLogLevel,
                           "Canceling election timeout callback at {when}",
                           "Canceling election timeout callback",
                           "when"_attr = oldWhen);
        _handleElectionTimeoutCallback.cancel();
    }

    if (doNotReschedule)
        return;

    Milliseconds upperBound = Milliseconds(_getElectionOffsetUpperBound_inlock());
    auto requestedWhen = now + _rsConfig.getElectionTimeoutPeriod();
    invariant(requestedWhen > now);
    Status delayStatus =
        _handleElectionTimeoutCallback.delayUntilWithJitter(requestedWhen, upperBound);
    Date_t when = _handleElectionTimeoutCallback.getNextCall();
    if (wasActive) {
        // The log level here is 4 once per second, otherwise 5.
        LOGV2_FOR_ELECTION(4615650,
                           cancelAndRescheduleLogLevel,
                           "Rescheduled election timeout callback",
                           "when"_attr = when,
                           "requestedWhen"_attr = requestedWhen,
                           "error"_attr = delayStatus);
    } else {
        LOGV2_FOR_ELECTION(4615651,
                           4,
                           "Scheduled election timeout callback",
                           "when"_attr = when,
                           "requestedWhen"_attr = requestedWhen,
                           "error"_attr = delayStatus);
    }
}

void ReplicationCoordinatorImpl::_startElectSelfIfEligibleV1(StartElectionReasonEnum reason) {
    stdx::lock_guard<Latch> lock(_mutex);
    _startElectSelfIfEligibleV1(lock, reason);
}

void ReplicationCoordinatorImpl::_startElectSelfIfEligibleV1(WithLock lk,
                                                             StartElectionReasonEnum reason) {
    // If it is not a single node replica set, no need to start an election after stepdown timeout.
    if (reason == StartElectionReasonEnum::kSingleNodePromptElection &&
        !_topCoord->isElectableNodeInSingleNodeReplicaSet()) {
        LOGV2_FOR_ELECTION(
            4764800, 0, "Not starting an election, since we are not an electable single node");
        return;
    }

    // We should always reschedule this callback even if we do not make it to the election
    // process.
    {
        _cancelCatchupTakeover_inlock();
        _cancelPriorityTakeover_inlock();
        _cancelAndRescheduleElectionTimeout_inlock();
        if (_inShutdown) {
            LOGV2_FOR_ELECTION(4615654, 0, "Not starting an election, since we are shutting down");
            return;
        }
    }

    const auto status = _topCoord->becomeCandidateIfElectable(_replExecutor->now(), reason);
    if (!status.isOK()) {
        switch (reason) {
            case StartElectionReasonEnum::kElectionTimeout:
                LOGV2_FOR_ELECTION(
                    4615655,
                    0,
                    "Not starting an election, since we are not electable due to: {reason}",
                    "Not starting an election, since we are not electable",
                    "reason"_attr = status.reason());
                break;
            case StartElectionReasonEnum::kPriorityTakeover:
                LOGV2_FOR_ELECTION(4615656,
                                   0,
                                   "Not starting an election for a priority takeover, since we are "
                                   "not electable due to: {reason}",
                                   "Not starting an election for a priority takeover, since we are "
                                   "not electable",
                                   "reason"_attr = status.reason());
                break;
            case StartElectionReasonEnum::kStepUpRequest:
            case StartElectionReasonEnum::kStepUpRequestSkipDryRun:
                LOGV2_FOR_ELECTION(4615657,
                                   0,
                                   "Not starting an election for a replSetStepUp request, since we "
                                   "are not electable due to: {reason}",
                                   "Not starting an election for a replSetStepUp request, since we "
                                   "are not electable",
                                   "reason"_attr = status.reason());
                break;
            case StartElectionReasonEnum::kCatchupTakeover:
                LOGV2_FOR_ELECTION(4615658,
                                   0,
                                   "Not starting an election for a catchup takeover, since we are "
                                   "not electable due to: {reason}",
                                   "Not starting an election for a catchup takeover, since we are "
                                   "not electable",
                                   "reason"_attr = status.reason());
                break;
            case StartElectionReasonEnum::kSingleNodePromptElection:
                LOGV2_FOR_ELECTION(4615653,
                                   0,
                                   "Not starting an election for a single node replica set prompt "
                                   "election, since we are not electable due to: {reason}",
                                   "Not starting an election for a single node replica set prompt "
                                   "election, since we are not electable",
                                   "reason"_attr = status.reason());
                break;
            default:
                MONGO_UNREACHABLE;
        }
        return;
    }

    switch (reason) {
        case StartElectionReasonEnum::kElectionTimeout:
            LOGV2_FOR_ELECTION(
                4615652,
                0,
                "Starting an election, since we've seen no PRIMARY in the past "
                "{electionTimeoutPeriod}",
                "Starting an election, since we've seen no PRIMARY in election timeout period",
                "electionTimeoutPeriod"_attr = _rsConfig.getElectionTimeoutPeriod());
            break;
        case StartElectionReasonEnum::kPriorityTakeover:
            LOGV2_FOR_ELECTION(4615660, 0, "Starting an election for a priority takeover");
            break;
        case StartElectionReasonEnum::kStepUpRequest:
        case StartElectionReasonEnum::kStepUpRequestSkipDryRun:
            LOGV2_FOR_ELECTION(4615661, 0, "Starting an election due to step up request");
            break;
        case StartElectionReasonEnum::kCatchupTakeover:
            LOGV2_FOR_ELECTION(4615662, 0, "Starting an election for a catchup takeover");
            break;
        case StartElectionReasonEnum::kSingleNodePromptElection:
            LOGV2_FOR_ELECTION(
                4615663, 0, "Starting an election due to single node replica set prompt election");
            break;
        default:
            MONGO_UNREACHABLE;
    }

    invariant(!_electionState);

    _electionState = std::make_unique<ElectionState>(this);
    _electionState->start(lk, reason);
}

}  // namespace repl
}  // namespace mongo

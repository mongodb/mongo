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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication
#define LOGV2_FOR_ELECTION(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                             \
        ID, DLEVEL, {logv2::LogComponent::kReplicationElection}, MESSAGE, ##__VA_ARGS__)
#define LOGV2_FOR_HEARTBEATS(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                               \
        ID, DLEVEL, {logv2::LogComponent::kReplicationHeartbeats}, MESSAGE, ##__VA_ARGS__)

#include "mongo/platform/basic.h"

#include <algorithm>
#include <functional>

#include "mongo/base/status.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config_checks.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

namespace {

MONGO_FAIL_POINT_DEFINE(blockHeartbeatStepdown);
MONGO_FAIL_POINT_DEFINE(blockHeartbeatReconfigFinish);

}  // namespace

using executor::RemoteCommandRequest;

Milliseconds ReplicationCoordinatorImpl::_getRandomizedElectionOffset_inlock() {
    long long electionTimeout = durationCount<Milliseconds>(_rsConfig.getElectionTimeoutPeriod());
    long long randomOffsetUpperBound =
        electionTimeout * _externalState->getElectionTimeoutOffsetLimitFraction();

    // Avoid divide by zero error in random number generator.
    if (randomOffsetUpperBound == 0) {
        return Milliseconds(0);
    }

    return Milliseconds{_nextRandomInt64_inlock(randomOffsetUpperBound)};
}

void ReplicationCoordinatorImpl::_doMemberHeartbeat(executor::TaskExecutor::CallbackArgs cbData,
                                                    const HostAndPort& target,
                                                    int targetIndex) {
    stdx::lock_guard<Latch> lk(_mutex);

    _untrackHeartbeatHandle_inlock(cbData.myHandle);
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }

    const Date_t now = _replExecutor->now();
    BSONObj heartbeatObj;
    Milliseconds timeout(0);
    const std::pair<ReplSetHeartbeatArgsV1, Milliseconds> hbRequest =
        _topCoord->prepareHeartbeatRequestV1(now, _settings.ourSetName(), target);
    heartbeatObj = hbRequest.first.toBSON();
    timeout = hbRequest.second;

    const RemoteCommandRequest request(
        target, "admin", heartbeatObj, BSON(rpc::kReplSetMetadataFieldName << 1), nullptr, timeout);
    const executor::TaskExecutor::RemoteCommandCallbackFn callback =
        [=](const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {
            return _handleHeartbeatResponse(cbData, targetIndex);
        };

    LOGV2_FOR_HEARTBEATS(4615670,
                         2,
                         "Sending heartbeat (requestId: {requestId}) to {target} {heartbeatObj}",
                         "Sending heartbeat",
                         "requestId"_attr = request.id,
                         "target"_attr = target,
                         "heartbeatObj"_attr = heartbeatObj);
    _trackHeartbeatHandle_inlock(_replExecutor->scheduleRemoteCommand(request, callback));
}

void ReplicationCoordinatorImpl::_scheduleHeartbeatToTarget_inlock(const HostAndPort& target,
                                                                   int targetIndex,
                                                                   Date_t when) {
    LOGV2_FOR_HEARTBEATS(4615618,
                         2,
                         "Scheduling heartbeat to {target} at {when}",
                         "Scheduling heartbeat",
                         "target"_attr = target,
                         "when"_attr = when);
    _trackHeartbeatHandle_inlock(_replExecutor->scheduleWorkAt(
        when, [=](const executor::TaskExecutor::CallbackArgs& cbData) {
            _doMemberHeartbeat(cbData, target, targetIndex);
        }));
}

void ReplicationCoordinatorImpl::handleHeartbeatResponse_forTest(BSONObj response,
                                                                 int targetIndex) {
    CallbackHandle handle;
    RemoteCommandRequest request;
    request.target = _rsConfig.getMemberAt(targetIndex).getHostAndPort();
    executor::TaskExecutor::ResponseStatus status(response, Milliseconds(100));
    executor::TaskExecutor::RemoteCommandCallbackArgs cbData(
        _replExecutor.get(), handle, request, status);

    {
        // Pretend we sent a request so that _untrackHeartbeatHandle_inlock succeeds.
        stdx::unique_lock<Latch> lk(_mutex);
        _trackHeartbeatHandle_inlock(handle);
    }

    _handleHeartbeatResponse(cbData, targetIndex);
}

void ReplicationCoordinatorImpl::_handleHeartbeatResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData, int targetIndex) {
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
                (!_getMemberState_inlock().startup() && !_getMemberState_inlock().startup2())) {
                // The node that sent the heartbeat is not guaranteed to be our sync source.
                const bool fromSyncSource = false;
                _advanceCommitPoint(
                    lk, replMetadata.getValue().getLastOpCommitted(), fromSyncSource);
            }

            // Asynchronous stepdown could happen, but it will wait for _mutex and execute
            // after this function, so we cannot and don't need to wait for it to finish.
            _processReplSetMetadata_inlock(replMetadata.getValue());
        }
    }
    const Date_t now = _replExecutor->now();
    Milliseconds networkTime(0);
    StatusWith<ReplSetHeartbeatResponse> hbStatusResponse(hbResponse);

    if (responseStatus.isOK()) {
        networkTime = cbData.response.elapsedMillis.value_or(Milliseconds{0});
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

    HeartbeatResponseAction action =
        _topCoord->processHeartbeatResponse(now, networkTime, target, hbStatusResponse);

    if (action.getAction() == HeartbeatResponseAction::NoAction && hbStatusResponse.isOK() &&
        hbStatusResponse.getValue().hasState() &&
        hbStatusResponse.getValue().getState() != MemberState::RS_PRIMARY &&
        action.getAdvancedOpTimeOrUpdatedConfig()) {
        // If a member's opTime has moved forward or config is newer, try to update the
        // lastCommitted. Even if we've only updated the config, this is still safe.
        _updateLastCommittedOpTimeAndWallTime(lk);

        // Wake up replication waiters on optime changes or updated configs.
        _wakeReadyWaiters(lk);
    }

    if (enableAutomaticReconfig) {
        // When receiving a heartbeat response indicating that the remote is in a state past
        // STARTUP_2, the primary will initiate a reconfig to remove the 'newlyAdded' field for that
        // node (if present). This field is normally set when we add new members with votes:1 to the
        // set.
        if (_getMemberState_inlock().primary() && hbStatusResponse.isOK() &&
            hbStatusResponse.getValue().hasState()) {
            auto remoteState = hbStatusResponse.getValue().getState();
            if (remoteState == MemberState::RS_SECONDARY ||
                remoteState == MemberState::RS_RECOVERING ||
                remoteState == MemberState::RS_ROLLBACK) {
                const auto mem = _rsConfig.getMemberAt(targetIndex);
                const auto memId = mem.getId();
                if (mem.isNewlyAdded()) {
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
        target, targetIndex, std::max(now, action.getNextHeartbeatStartDate()));

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
            _scheduleHeartbeatReconfig_inlock(responseStatus.getValue().getConfig());
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

void ReplicationCoordinatorImpl::_scheduleHeartbeatReconfig_inlock(const ReplSetConfig& newConfig) {
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
    _setConfigState_inlock(kConfigHBReconfiguring);
    invariant(!_rsConfig.isInitialized() ||
              _rsConfig.getConfigVersionAndTerm() < newConfig.getConfigVersionAndTerm());
    if (auto electionFinishedEvent = _cancelElectionIfNeeded_inlock()) {
        LOGV2_FOR_HEARTBEATS(
            4615624,
            2,
            "Rescheduling heartbeat reconfig to config with {newConfigVersionAndTerm} to "
            "be processed after election is cancelled.",
            "Rescheduling heartbeat reconfig to be processed after election is cancelled",
            "newConfigVersionAndTerm"_attr = newConfig.getConfigVersionAndTerm());

        _replExecutor
            ->onEvent(electionFinishedEvent,
                      [=](const executor::TaskExecutor::CallbackArgs& cbData) {
                          _heartbeatReconfigStore(cbData, newConfig);
                      })
            .status_with_transitional_ignore();
        return;
    }
    _replExecutor
        ->scheduleWork([=](const executor::TaskExecutor::CallbackArgs& cbData) {
            _heartbeatReconfigStore(cbData, newConfig);
        })
        .status_with_transitional_ignore();
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

    const StatusWith<int> myIndex = validateConfigForHeartbeatReconfig(
        _externalState.get(), newConfig, getGlobalServiceContext());

    if (myIndex.getStatus() == ErrorCodes::NodeNotFound) {
        stdx::lock_guard<Latch> lk(_mutex);
        // If this node absent in newConfig, and this node was not previously initialized,
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
                             "Config with {newConfigVersionAndTerm} validated for "
                             "reconfig; persisting to disk.",
                             "Config validated for reconfig; persisting to disk",
                             "newConfigVersionAndTerm"_attr = newConfig.getConfigVersionAndTerm());

        auto opCtx = cc().makeOperationContext();
        // Don't write the no-op for config learned via heartbeats.
        auto status = _externalState->storeLocalConfigDocument(
            opCtx.get(), newConfig.toBSON(), false /* writeOplog */);
        // Wait for durability of the new config document.
        opCtx->recoveryUnit()->waitUntilDurable(opCtx.get());

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
            newConfig.getMemberAt(myIndex.getValue()).isArbiter();

        if (isArbiter) {
            LogicalClock::get(getGlobalServiceContext())->disable();
            if (auto validator = LogicalTimeValidator::get(getGlobalServiceContext())) {
                validator->stopKeyManager();
            }
        }

        if (!isArbiter && isFirstConfig) {
            shouldStartDataReplication = true;
        }

        LOGV2_FOR_HEARTBEATS(
            4615627,
            2,
            "New configuration with {newConfigVersionAndTerm} persisted "
            "to local storage; installing new config in memory",
            "New configuration persisted to local storage; installing new config in memory",
            "newConfigVersionAndTerm"_attr = newConfig.getConfigVersionAndTerm());
    }

    _heartbeatReconfigFinish(cbd, newConfig, myIndex);

    // Start data replication after the config has been installed.
    if (shouldStartDataReplication) {
        auto opCtx = cc().makeOperationContext();
        _replicationProcess->getConsistencyMarkers()->initializeMinValidDocument(opCtx.get());
        _externalState->startThreads(_settings);
        _startDataReplication(opCtx.get());
    }
}

void ReplicationCoordinatorImpl::_heartbeatReconfigFinish(
    const executor::TaskExecutor::CallbackArgs& cbData,
    const ReplSetConfig& newConfig,
    StatusWith<int> myIndex) {
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
                                 _heartbeatReconfigFinish(cbData, newConfig, myIndex);
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
        if (auto electionFinishedEvent = _cancelElectionIfNeeded_inlock()) {
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
                              _heartbeatReconfigFinish(cbData, newConfig, myIndex);
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
              _rsConfig.getConfigVersionAndTerm() < newConfig.getConfigVersionAndTerm());

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
    const ReplSetConfig oldConfig = _rsConfig;
    // If we do not have an index, we should pass -1 as our index to avoid falsely adding ourself to
    // the data structures inside of the TopologyCoordinator.
    const int myIndexValue = myIndex.getStatus().isOK() ? myIndex.getValue() : -1;

    const PostMemberStateUpdateAction action =
        _setCurrentRSConfig(lk, opCtx.get(), newConfig, myIndexValue);

    lk.unlock();
    _performPostMemberStateUpdateAction(action);

    // Inform the index builds coordinator of the replica set reconfig.
    IndexBuildsCoordinator::get(opCtx.get())->onReplicaSetReconfig();
}

void ReplicationCoordinatorImpl::_trackHeartbeatHandle_inlock(
    const StatusWith<executor::TaskExecutor::CallbackHandle>& handle) {
    if (handle.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(18912, handle.getStatus());
    _heartbeatHandles.push_back(handle.getValue());
}

void ReplicationCoordinatorImpl::_untrackHeartbeatHandle_inlock(
    const executor::TaskExecutor::CallbackHandle& handle) {
    const HeartbeatHandles::iterator newEnd =
        std::remove(_heartbeatHandles.begin(), _heartbeatHandles.end(), handle);
    invariant(newEnd != _heartbeatHandles.end());
    _heartbeatHandles.erase(newEnd, _heartbeatHandles.end());
}

void ReplicationCoordinatorImpl::_cancelHeartbeats_inlock() {
    LOGV2_FOR_HEARTBEATS(4615630, 2, "Cancelling all heartbeats");

    for (const auto& handle : _heartbeatHandles) {
        _replExecutor->cancel(handle);
    }
    // Heartbeat callbacks will remove themselves from _heartbeatHandles when they execute with
    // CallbackCanceled status, so it's better to leave the handles in the list, for now.

    if (_handleLivenessTimeoutCbh.isValid()) {
        _replExecutor->cancel(_handleLivenessTimeoutCbh);
    }
}

void ReplicationCoordinatorImpl::restartHeartbeats_forTest() {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(getTestCommandsEnabled());
    LOGV2_FOR_HEARTBEATS(4406800, 0, "Restarting heartbeats");
    _restartHeartbeats_inlock();
};

void ReplicationCoordinatorImpl::_restartHeartbeats_inlock() {
    _cancelHeartbeats_inlock();
    _startHeartbeats_inlock();
}

void ReplicationCoordinatorImpl::_startHeartbeats_inlock() {
    const Date_t now = _replExecutor->now();
    _seedList.clear();
    for (int i = 0; i < _rsConfig.getNumMembers(); ++i) {
        if (i == _selfIndex) {
            continue;
        }
        _scheduleHeartbeatToTarget_inlock(_rsConfig.getMemberAt(i).getHostAndPort(), i, now);
    }

    _topCoord->restartHeartbeats();

    _topCoord->resetAllMemberTimeouts(_replExecutor->now());
    _scheduleNextLivenessUpdate_inlock();
}

void ReplicationCoordinatorImpl::_handleLivenessTimeout(
    const executor::TaskExecutor::CallbackArgs& cbData) {
    stdx::unique_lock<Latch> lk(_mutex);
    // Only reset the callback handle if it matches, otherwise more will be coming through
    if (cbData.myHandle == _handleLivenessTimeoutCbh) {
        _handleLivenessTimeoutCbh = CallbackHandle();
    }
    if (!cbData.status.isOK()) {
        return;
    }

    // Scan liveness table for problems and mark nodes as down by calling into topocoord.
    HeartbeatResponseAction action = _topCoord->checkMemberTimeouts(_replExecutor->now());
    // Don't mind potential asynchronous stepdown as this is the last step of
    // liveness check.
    lk = _handleHeartbeatResponseAction_inlock(
        action, makeStatusWith<ReplSetHeartbeatResponse>(), std::move(lk));

    _scheduleNextLivenessUpdate_inlock();
}

void ReplicationCoordinatorImpl::_scheduleNextLivenessUpdate_inlock() {
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

    if (_handleLivenessTimeoutCbh.isValid() && !_handleLivenessTimeoutCbh.isCanceled()) {
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
    // ThreadPoolTaskExecutor::_scheduleWorkAt() schedules its work immediately if it's given a
    // time <= now().
    // If we missed the timeout, it means that on our last check the earliest live member was
    // just barely fresh and it has become stale since then. We must schedule another liveness
    // check to continue conducting liveness checks and be able to step down from primary if we
    // lose contact with a majority of nodes.
    auto cbh =
        _scheduleWorkAt(nextTimeout, [=](const executor::TaskExecutor::CallbackArgs& cbData) {
            _handleLivenessTimeout(cbData);
        });
    if (!cbh) {
        return;
    }
    _handleLivenessTimeoutCbh = cbh;
    _earliestMemberId = earliestMemberId.getData();
}

void ReplicationCoordinatorImpl::_cancelAndRescheduleLivenessUpdate_inlock(int updatedMemberId) {
    if ((_earliestMemberId != -1) && (_earliestMemberId != updatedMemberId)) {
        return;
    }
    if (_handleLivenessTimeoutCbh.isValid()) {
        _replExecutor->cancel(_handleLivenessTimeoutCbh);
    }
    _scheduleNextLivenessUpdate_inlock();
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
    const bool wasActive = _handleElectionTimeoutCbh.isValid();
    auto now = _replExecutor->now();
    const bool doNotReschedule = _inShutdown || !_memberState.secondary() || _selfIndex < 0 ||
        !_rsConfig.getMemberAt(_selfIndex).isElectable();

    if (doNotReschedule || !wasActive || (now - logThrottleTime) >= Seconds(1)) {
        cancelAndRescheduleLogLevel = 4;
        logThrottleTime = now;
    }
    if (wasActive) {
        LOGV2_FOR_ELECTION(4615649,
                           cancelAndRescheduleLogLevel,
                           "Canceling election timeout callback at {when}",
                           "Canceling election timeout callback",
                           "when"_attr = _handleElectionTimeoutWhen);
        _replExecutor->cancel(_handleElectionTimeoutCbh);
        _handleElectionTimeoutCbh = CallbackHandle();
        _handleElectionTimeoutWhen = Date_t();
    }

    if (doNotReschedule)
        return;

    Milliseconds randomOffset = _getRandomizedElectionOffset_inlock();
    auto when = now + _rsConfig.getElectionTimeoutPeriod() + randomOffset;
    invariant(when > now);
    if (wasActive) {
        // The log level here is 4 once per second, otherwise 5.
        LOGV2_FOR_ELECTION(4615650,
                           cancelAndRescheduleLogLevel,
                           "Rescheduling election timeout callback at {when}",
                           "Rescheduling election timeout callback",
                           "when"_attr = when);
    } else {
        LOGV2_FOR_ELECTION(4615651,
                           4,
                           "Scheduling election timeout callback at {when}",
                           "Scheduling election timeout callback",
                           "when"_attr = when);
    }
    _handleElectionTimeoutWhen = when;
    _handleElectionTimeoutCbh =
        _scheduleWorkAt(when, [=](const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
            stdx::lock_guard<Latch> lk(_mutex);
            if (_handleElectionTimeoutCbh == cbData.myHandle) {
                // This lets _cancelAndRescheduleElectionTimeout_inlock know the callback
                // has happened.
                _handleElectionTimeoutCbh = CallbackHandle();
            }
            _startElectSelfIfEligibleV1(lk, StartElectionReasonEnum::kElectionTimeout);
        });
}

void ReplicationCoordinatorImpl::_startElectSelfIfEligibleV1(StartElectionReasonEnum reason) {
    stdx::lock_guard<Latch> lock(_mutex);
    _startElectSelfIfEligibleV1(lock, reason);
}

void ReplicationCoordinatorImpl::_startElectSelfIfEligibleV1(WithLock,
                                                             StartElectionReasonEnum reason) {
    // If it is not a single node replica set, no need to start an election after stepdown timeout.
    if (reason == StartElectionReasonEnum::kSingleNodePromptElection &&
        _rsConfig.getNumMembers() != 1) {
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

    _startElectSelfV1_inlock(reason);
}

}  // namespace repl
}  // namespace mongo

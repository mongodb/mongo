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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/repl/delayable_timeout_callback.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_config_checks.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/repl/replication_metrics_gen.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {

MONGO_FAIL_POINT_DEFINE(blockHeartbeatStepdown);
MONGO_FAIL_POINT_DEFINE(blockHeartbeatReconfigFinish);
MONGO_FAIL_POINT_DEFINE(hangAfterTrackingNewHandleInHandleHeartbeatResponseForTest);
MONGO_FAIL_POINT_DEFINE(waitForPostActionCompleteInHbReconfig);
MONGO_FAIL_POINT_DEFINE(pauseInHandleHeartbeatResponse);
MONGO_FAIL_POINT_DEFINE(hangHeartbeatReconfigStore);

}  // namespace

using executor::RemoteCommandRequest;

auto& heartBeatHandleQueueSize = *MetricBuilder<Counter64>("repl.heartBeat.handleQueueSize");
auto& heartBeatHandleMaxSeenQueueSize =
    *MetricBuilder<Atomic64Metric>("repl.heartBeat.maxSeenHandleQueueSize");


long long ReplicationCoordinatorImpl::_getElectionOffsetUpperBound(WithLock lk) {
    long long electionTimeout =
        durationCount<Milliseconds>(_rsConfig.unsafePeek().getElectionTimeoutPeriod());
    return electionTimeout * _externalState->getElectionTimeoutOffsetLimitFraction();
}

Milliseconds ReplicationCoordinatorImpl::_getRandomizedElectionOffset(WithLock lk) {
    long long randomOffsetUpperBound = _getElectionOffsetUpperBound(lk);
    // Avoid divide by zero error in random number generator.
    if (randomOffsetUpperBound == 0) {
        return Milliseconds(0);
    }

    return Milliseconds{_nextRandomInt64(lk, randomOffsetUpperBound)};
}

void ReplicationCoordinatorImpl::_doMemberHeartbeat(executor::TaskExecutor::CallbackArgs cbData,
                                                    const HostAndPort& target,
                                                    const std::string& replSetName) {
    stdx::lock_guard lk(_mutex);

    _untrackHeartbeatHandle(lk, cbData.myHandle);
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

    const RemoteCommandRequest request(target,
                                       DatabaseName::kAdmin,
                                       heartbeatObj,
                                       BSON(rpc::kReplSetMetadataFieldName << 1),
                                       nullptr,
                                       timeout);
    const executor::TaskExecutor::RemoteCommandCallbackFn callback =
        [=, this](const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {
            return _handleHeartbeatResponse(cbData, replSetName);
        };

    LOGV2_FOR_HEARTBEATS(4615670,
                         2,
                         "Sending heartbeat",
                         "requestId"_attr = request.id,
                         "target"_attr = target,
                         "heartbeatObj"_attr = heartbeatObj);
    _trackHeartbeatHandle(
        lk, _replExecutor->scheduleRemoteCommand(request, callback), HeartbeatState::kSent, target);
}

void ReplicationCoordinatorImpl::_scheduleHeartbeatToTarget(WithLock lk,
                                                            const HostAndPort& target,
                                                            Date_t when,
                                                            std::string replSetName) {
    LOGV2_FOR_HEARTBEATS(
        4615618, 4, "Scheduling heartbeat", "target"_attr = target, "when"_attr = when);
    _trackHeartbeatHandle(
        lk,
        _replExecutor->scheduleWorkAt(when,
                                      [=, this, replSetName = std::move(replSetName)](
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

    std::string replSetNameString;
    {
        stdx::unique_lock lk(_mutex);

        ReplSetConfig rsc = _rsConfig.unsafePeek();
        request.target = rsc.getMemberAt(targetIndex).getHostAndPort();

        StringData replSetName = rsc.getReplSetName();
        // Simulate preparing a heartbeat request so that the target's ping stats are initialized.
        _topCoord->prepareHeartbeatRequestV1(_replExecutor->now(), replSetName, request.target);

        // Pretend we sent a request so that _untrackHeartbeatHandle succeeds.
        _trackHeartbeatHandle(lk, handle, HeartbeatState::kSent, request.target);
        invariant(_heartbeatHandles.contains(handle));
        replSetNameString = std::string{replSetName};
    }

    executor::TaskExecutor::ResponseStatus status(request.target, response, ping);
    executor::TaskExecutor::RemoteCommandCallbackArgs cbData(
        _replExecutor.get(), handle, request, status);

    hangAfterTrackingNewHandleInHandleHeartbeatResponseForTest.pauseWhileSet();

    _handleHeartbeatResponse(cbData, replSetNameString);
    {
        stdx::unique_lock lk(_mutex);
        invariant(!_heartbeatHandles.contains(handle));
    }
}

void ReplicationCoordinatorImpl::_handleHeartbeatResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData, const std::string& setName) {
    pauseInHandleHeartbeatResponse.executeIf(
        [](const BSONObj& data) { pauseInHandleHeartbeatResponse.pauseWhileSet(); },
        [&cbData](const BSONObj& data) -> bool {
            StringData dtarget = data["target"].valueStringDataSafe();
            return dtarget == cbData.request.target.toString();
        });
    stdx::unique_lock lk(_mutex);

    // remove handle from queued heartbeats
    _untrackHeartbeatHandle(lk, cbData.myHandle);

    // Parse and validate the response.  At the end of this step, if responseStatus is OK then
    // hbResponse is valid.
    Status responseStatus = cbData.response.status;
    const HostAndPort& target = cbData.request.target;

    // It is possible that the callback was canceled after handleHeartbeatResponse was called but
    // before it got the lock above.
    //
    // In this case, the responseStatus will be OK and we can process the heartbeat.  However, if
    // we do so, cancelling heartbeats no longer establishes a barrier after which all heartbeats
    // processed are "new" (sent subsequent to the cancel), which is something we care about for
    // catchup takeover.  So if we detect this situation (by checking if the handle was canceled)
    // we will NOT process the 'stale' heartbeat.
    if (responseStatus == ErrorCodes::CallbackCanceled || cbData.myHandle.isCanceled()) {
        LOGV2_FOR_HEARTBEATS(4615619,
                             2,
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

        LOGV2_FOR_HEARTBEATS(4615620,
                             2,
                             "Received response to heartbeat",
                             "requestId"_attr = cbData.request.id,
                             "target"_attr = target,
                             "response"_attr = resp);

        if (!responseStatus.isOK() && responseStatus == ErrorCodes::BadValue) {
            LOGV2_FOR_HEARTBEATS(
                10456300,
                2,
                "Received response to heartbeat, but the heartbeat was from ourselves",
                "requestId"_attr = cbData.request.id,
                "errorMessage"_attr = responseStatus.reason());
            return;
        }

        if (responseStatus.isOK() && _rsConfig.unsafePeek().isInitialized() &&
            _rsConfig.unsafePeek().getReplSetName() != hbResponse.getReplicaSetName()) {
            responseStatus =
                Status(ErrorCodes::InconsistentReplicaSetNames,
                       str::stream() << "replica set names do not match, ours: "
                                     << _rsConfig.unsafePeek().getReplSetName()
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
        if (replMetadata.isOK() && _rsConfig.unsafePeek().isInitialized() &&
            _rsConfig.unsafePeek().hasReplicaSetId() &&
            replMetadata.getValue().getReplicaSetId().isSet() &&
            _rsConfig.unsafePeek().getReplicaSetId() != replMetadata.getValue().getReplicaSetId()) {
            responseStatus = Status(ErrorCodes::InvalidReplicaSetConfig,
                                    str::stream() << "replica set IDs do not match, ours: "
                                                  << _rsConfig.unsafePeek().getReplicaSetId()
                                                  << "; remote node's: "
                                                  << replMetadata.getValue().getReplicaSetId());
            // Ignore metadata.
            replMetadata = responseStatus;
        }
        if (replMetadata.isOK()) {
            // It is safe to update our commit point via heartbeat propagation as long as the
            // the new commit point we learned of is on the same branch of history as our own
            // oplog.
            if (_getMemberState(lk).arbiter() ||
                (!_getMemberState(lk).startup() && !_getMemberState(lk).startup2() &&
                 !_getMemberState(lk).rollback())) {
                // The node that sent the heartbeat is not guaranteed to be our sync source.
                const bool fromSyncSource = false;
                _advanceCommitPoint(
                    lk, replMetadata.getValue().getLastOpCommitted(), fromSyncSource);
            }

            // Asynchronous stepdown could happen, but it will wait for _mutex and execute
            // after this function, so we cannot and don't need to wait for it to finish.
            _processReplSetMetadata(lk, replMetadata.getValue());
        }

        // Arbiters are always expected to report null durable optimes (and wall times).
        // If that is not the case here, make sure to correct these times before ingesting them.
        auto memberInConfig = _rsConfig.unsafePeek().findMemberByHostAndPort(target);
        if ((hbResponse.hasState() && hbResponse.getState().arbiter()) ||
            (_rsConfig.unsafePeek().isInitialized() && memberInConfig &&
             memberInConfig->isArbiter())) {
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
        _updateTerm(lk, hbResponse.getTerm());
        // Postpone election timeout if we have a successful heartbeat response from the primary.
        if (hbResponse.hasState() && hbResponse.getState().primary() &&
            hbResponse.getTerm() == _topCoord->getTerm()) {
            LOGV2_FOR_ELECTION(
                4615659, 4, "Postponing election timeout due to heartbeat from primary");
            _cancelAndRescheduleElectionTimeout(lk);
        }
    } else {
        LOGV2_FOR_HEARTBEATS(4615621,
                             2,
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
    if (_getMemberState(lk).primary() && hbStatusResponse.isOK() &&
        hbStatusResponse.getValue().hasState()) {
        auto remoteState = hbStatusResponse.getValue().getState();
        if (remoteState == MemberState::RS_SECONDARY || remoteState == MemberState::RS_RECOVERING ||
            remoteState == MemberState::RS_ROLLBACK) {
            const auto mem = _rsConfig.unsafePeek().findMemberByHostAndPort(target);
            if (mem && mem->isNewlyAdded()) {
                const auto memId = mem->getId();
                const auto configVersion = _rsConfig.unsafePeek().getConfigVersionAndTerm();
                auto status = _replExecutor->scheduleWork(
                    [=, this](const executor::TaskExecutor::CallbackArgs& cbData) {
                        _reconfigToRemoveNewlyAddedField(cbData, memId, configVersion);
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
        _catchupState->signalHeartbeatUpdate(lk);
    }

    // Cancel catchup takeover if the last applied write by any node in the replica set was made
    // in the current term, which implies that the primary has caught up.
    bool catchupTakeoverScheduled = _catchupTakeoverCbh.isValid();
    if (responseStatus.isOK() && catchupTakeoverScheduled && hbResponse.hasAppliedOpTime()) {
        const auto& hbLastAppliedOpTime = hbResponse.getAppliedOpTime();
        if (hbLastAppliedOpTime.getTerm() == _topCoord->getTerm()) {
            _cancelCatchupTakeover(lk);
        }
    }

    _scheduleHeartbeatToTarget(
        lk, target, std::max(now, action.getNextHeartbeatStartDate()), setName);

    _handleHeartbeatResponseAction(action, hbStatusResponse, std::move(lk));
}

stdx::unique_lock<ObservableMutex<stdx::mutex>>
ReplicationCoordinatorImpl::_handleHeartbeatResponseAction(
    const HeartbeatResponseAction& action,
    const StatusWith<ReplSetHeartbeatResponse>& responseStatus,
    stdx::unique_lock<ObservableMutex<stdx::mutex>> lock) {
    invariant(lock.owns_lock());
    auto rsc = _rsConfig.unsafePeek();
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
            invariant(responseStatus.getStatus());
            _scheduleHeartbeatReconfig(lock, responseStatus.getValue().getConfig());
            break;
        case HeartbeatResponseAction::RetryReconfig:
            _scheduleHeartbeatReconfig(lock, rsc);
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
                Milliseconds priorityTakeoverDelay = rsc.getPriorityTakeoverDelay(_selfIndex);
                Milliseconds randomOffset = _getRandomizedElectionOffset(lock);
                _priorityTakeoverWhen = _replExecutor->now() + priorityTakeoverDelay + randomOffset;
                LOGV2_FOR_ELECTION(4615601,
                                   0,
                                   "Scheduling priority takeover",
                                   "when"_attr = _priorityTakeoverWhen);
                _priorityTakeoverCbh = _scheduleWorkAt(
                    _priorityTakeoverWhen,
                    [=, this](const mongo::executor::TaskExecutor::CallbackArgs&) {
                        _startElectSelfIfEligibleV1(StartElectionReasonEnum::kPriorityTakeover);
                    });
            }
            break;
        }
        case HeartbeatResponseAction::CatchupTakeover: {
            // Don't schedule a catchup takeover if any takeover is already scheduled.
            if (!_catchupTakeoverCbh.isValid() && !_priorityTakeoverCbh.isValid()) {
                Milliseconds catchupTakeoverDelay = rsc.getCatchUpTakeoverDelay();
                _catchupTakeoverWhen = _replExecutor->now() + catchupTakeoverDelay;
                LOGV2_FOR_ELECTION(
                    4615648, 0, "Scheduling catchup takeover", "when"_attr = _catchupTakeoverWhen);
                _catchupTakeoverCbh = _scheduleWorkAt(
                    _catchupTakeoverWhen,
                    [=, this](const mongo::executor::TaskExecutor::CallbackArgs&) {
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

executor::TaskExecutor::EventHandle ReplicationCoordinatorImpl::_stepDownStart() {
    auto finishEvent = _makeEvent();
    if (!finishEvent) {
        return finishEvent;
    }

    _replExecutor
        ->scheduleWork([=, this](const executor::TaskExecutor::CallbackArgs& cbData) {
            _stepDownFinish(cbData, finishEvent);
        })
        .status_with_transitional_ignore();
    return finishEvent;
}

void ReplicationCoordinatorImpl::_stepDownFinish(
    const executor::TaskExecutor::CallbackArgs& cbData,
    const executor::TaskExecutor::EventHandle& finishedEvent) {
    const Date_t startStepDownTime = _replExecutor->now();
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }

    if (MONGO_unlikely(blockHeartbeatStepdown.shouldFail())) {
        // This log output is used in js tests so please leave it.
        LOGV2(21479,
              "stepDown - blockHeartbeatStepdown fail point enabled. "
              "Blocking until fail point is disabled.");

        auto inShutdown = [&] {
            stdx::lock_guard lk(_mutex);
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
    boost::optional<rss::consensus::ReplicationStateTransitionGuard> rstg;
    const Date_t startTimeKillConflictingOperations = _replExecutor->now();
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        rstg.emplace(_killConflictingOperations(
            rss::consensus::IntentRegistry::InterruptionType::StepDown, opCtx.get()));
    }
    const Date_t endTimeKillConflictingOperations = _replExecutor->now();

    const Date_t startTimeAcquireRSTL = _replExecutor->now();
    AutoGetRstlForStepUpStepDown arsd(
        this, opCtx.get(), ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown);
    const Date_t endTimeAcquireRSTL = _replExecutor->now();
    LOGV2(962665,
          "Acquired RSTL during stepDown",
          "timeToAcquire"_attr = (endTimeAcquireRSTL - startTimeAcquireRSTL));
    stdx::unique_lock lk(_mutex);

    // This node has already stepped down due to reconfig. So, signal anyone who is waiting on the
    // step down event.
    if (!_topCoord->isSteppingDownUnconditionally()) {
        _replExecutor->signalEvent(finishedEvent);
        return;
    }

    // We need to release the mutex before yielding locks for prepared transactions, which might
    // check out sessions, to avoid deadlocks with checked-out sessions accessing this mutex.
    lk.unlock();
    const Date_t startTimeYieldLocksInvalidateSessions = _replExecutor->now();
    yieldLocksForPreparedTransactions(opCtx.get());
    invalidateSessionsForStepdown(opCtx.get());
    const Date_t endTimeYieldLocksInvalidateSessions = _replExecutor->now();
    lk.lock();

    // Clear the node's election candidate metrics since it is no longer primary.
    ReplicationMetrics::get(opCtx.get()).clearElectionCandidateMetrics();

    _topCoord->finishUnconditionalStepDown();

    // Update _canAcceptNonLocalWrites.
    _updateWriteAbilityFromTopologyCoordinator(lk, opCtx.get());
    const Date_t startTimeUpdateMemberState = _replExecutor->now();
    const auto action = _updateMemberStateFromTopologyCoordinator(lk);
    if (_pendingTermUpdateDuringStepDown) {
        TopologyCoordinator::UpdateTermResult result;
        _updateTerm(lk, *_pendingTermUpdateDuringStepDown, &result);
        // We've just stepped down due to the "term", so it's impossible to step down again
        // for the same term.
        invariant(result != TopologyCoordinator::UpdateTermResult::kTriggerStepDown);
        _pendingTermUpdateDuringStepDown = boost::none;
    }
    lk.unlock();
    _performPostMemberStateUpdateAction(action);
    const Date_t endTimeUpdateMemberState = _replExecutor->now();
    _replExecutor->signalEvent(finishedEvent);
    const Date_t endStepDownTime = _replExecutor->now();
    LOGV2(962664,
          "Stepdown succeeded",
          "totalTime"_attr = (endStepDownTime - startStepDownTime),
          "killOpsTime"_attr =
              (endTimeKillConflictingOperations - startTimeKillConflictingOperations),
          "updateMemberStateTime"_attr = (endTimeUpdateMemberState - startTimeUpdateMemberState),
          "yieldLocksTime"_attr =
              (endTimeYieldLocksInvalidateSessions - startTimeYieldLocksInvalidateSessions));
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
                "Ignoring new configuration because we are already in the midst of a configuration "
                "process",
                "newConfigVersionAndTerm"_attr = newConfig.getConfigVersionAndTerm());
            return;
        case kConfigPreStart:
        case kConfigStartingUp:
        case kConfigReplicationDisabled:
            LOGV2_FATAL(18807,
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

    _setConfigState(lk, kConfigHBReconfiguring);
    auto rsc = _rsConfig.unsafePeek();
    invariant(
        !rsc.isInitialized() ||
            rsc.getConfigVersionAndTerm() < newConfig.getConfigVersionAndTerm() || _selfIndex < 0,
        str::stream() << "initialized: " << rsc.isInitialized() << ", old config version and term: "
                      << rsc.getConfigVersionAndTerm().toString()
                      << ", new config version and term: "
                      << newConfig.getConfigVersionAndTerm().toString()
                      << ", selfIndex: " << _selfIndex);
    _replExecutor
        ->scheduleWork([=, this](const executor::TaskExecutor::CallbackArgs& cbData) {
            _heartbeatReconfigStore(cbData, newConfig);
            return;
        })
        .status_with_transitional_ignore();
}

void ReplicationCoordinatorImpl::_heartbeatReconfigStore(
    const executor::TaskExecutor::CallbackArgs& cbd, const ReplSetConfig& newConfig) {

    if (cbd.status.code() == ErrorCodes::CallbackCanceled) {
        LOGV2(21480,
              "The callback to persist the replica set configuration was canceled - the "
              "configuration was not persisted but was used",
              "newConfig"_attr = newConfig.toBSON());
        return;
    }

    if (MONGO_unlikely(hangHeartbeatReconfigStore.shouldFail())) {
        LOGV2(10142900,
              "hangHeartbeatReconfigStore failpoint set, hanging while failpoint is active");
        hangHeartbeatReconfigStore.pauseWhileSet();
    }

    auto rsc = _getReplSetConfig();

    const auto myIndex = [&]() -> StatusWith<int> {
        // We always check the config when _selfIndex is not valid, in order to be able to
        // recover from transient DNS errors.
        {
            stdx::lock_guard lk(_mutex);
            if (_selfIndex >= 0 && sameConfigContents(rsc, newConfig)) {
                LOGV2_FOR_HEARTBEATS(6351200,
                                     2,
                                     "New heartbeat config is only a version/term change, skipping "
                                     "validation checks",
                                     "oldConfig"_attr = rsc,
                                     "newConfig"_attr = newConfig);
                // If the configs are the same, so is our index.
                return _selfIndex;
            }
        }
        return validateConfigForHeartbeatReconfig(
            _externalState.get(), newConfig, getMyHostAndPort(), cc().getServiceContext());
    }();

    if (myIndex.getStatus() == ErrorCodes::NodeNotFound) {
        stdx::lock_guard lk(_mutex);
        // If this node absent in newConfig, and this node was not previously initialized,
        // return to kConfigUninitialized immediately, rather than storing the config and
        // transitioning into the RS_REMOVED state.  See SERVER-15740.
        if (!rsc.isInitialized()) {
            invariant(_rsConfigState == kConfigHBReconfiguring);
            LOGV2_FOR_HEARTBEATS(4615625,
                                 1,
                                 "Ignoring new configuration in heartbeat response because we "
                                 "are uninitialized and not a member of the new configuration");
            _setConfigState(lk, kConfigUninitialized);
            return;
        }
    }

    bool shouldStartDataReplication = false;
    if (!myIndex.getStatus().isOK() && myIndex.getStatus() != ErrorCodes::NodeNotFound) {
        LOGV2_WARNING(21487,
                      "Not persisting new configuration in heartbeat response to disk because "
                      "it is invalid",
                      "error"_attr = myIndex.getStatus());
    } else {
        LOGV2_FOR_HEARTBEATS(4615626,
                             2,
                             "Config validated for reconfig; persisting to disk",
                             "newConfigVersionAndTerm"_attr = newConfig.getConfigVersionAndTerm());

        auto opCtx = cc().makeOperationContext();
        auto status = [this, opCtx = opCtx.get(), newConfig]() {
            // Don't write the no-op for config learned via heartbeats.
            return _externalState->storeLocalConfigDocument(
                opCtx, newConfig.toBSON(), false /* writeOplog */);
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
            stdx::lock_guard lk(_mutex);
            isFirstConfig = !rsc.isInitialized();
            if (!status.isOK()) {
                LOGV2_ERROR(21488,
                            "Ignoring new configuration in heartbeat response because we failed to"
                            " write it to stable storage",
                            "error"_attr = status);
                invariant(_rsConfigState == kConfigHBReconfiguring);
                if (isFirstConfig) {
                    _setConfigState(lk, kConfigUninitialized);
                } else {
                    _setConfigState(lk, kConfigSteady);
                }
                return;
            }
        }

        bool isArbiter = myIndex.isOK() && myIndex.getValue() != -1 &&
            newConfig.getMemberAt(myIndex.getValue()).isArbiter();

        if (isArbiter) {
            ReplicaSetAwareServiceRegistry::get(_service).onBecomeArbiter();
        }

        if (!isArbiter && myIndex.isOK() && myIndex.getValue() != -1) {
            shouldStartDataReplication = true;
        }

        LOGV2_FOR_HEARTBEATS(
            4615627,
            2,
            "New configuration persisted to local storage; installing new config in memory",
            "newConfigVersionAndTerm"_attr = newConfig.getConfigVersionAndTerm());
    }

    _heartbeatReconfigFinish(cbd, newConfig, myIndex);

    // Start data replication after the config has been installed.
    if (shouldStartDataReplication) {
        while (true) {
            try {
                auto opCtx = cc().makeOperationContext();
                // Initializing minvalid is not allowed to be interrupted.  Make sure it
                // can't be interrupted by a storage change by taking the global lock first.
                {
                    Lock::GlobalLock lk(
                        opCtx.get(),
                        MODE_IX,
                        {false, false, false, rss::consensus::IntentRegistry::Intent::LocalWrite});
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
                             [=, this](const executor::TaskExecutor::CallbackArgs& cbData) {
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
        stdx::lock_guard lk(_mutex);
        if (auto electionFinishedEvent = _cancelElectionIfNeeded(lk)) {
            LOGV2_FOR_HEARTBEATS(4615629,
                                 0,
                                 "Waiting for election to complete before finishing reconfig",
                                 "newConfigVersionAndTerm"_attr =
                                     newConfig.getConfigVersionAndTerm());
            // Wait for the election to complete and the node's Role to be set to follower.
            _replExecutor
                ->onEvent(electionFinishedEvent,
                          [=, this](const executor::TaskExecutor::CallbackArgs& cbData) {
                              _heartbeatReconfigFinish(cbData, newConfig, myIndex);
                          })
                .status_with_transitional_ignore();
            return;
        }
    }
    const Date_t startTime = _replExecutor->now();
    auto opCtx = cc().makeOperationContext();
    boost::optional<rss::consensus::ReplicationStateTransitionGuard> rstg;
    boost::optional<AutoGetRstlForStepUpStepDown> arsd;
    stdx::unique_lock lk(_mutex);
    auto rsc = _rsConfig.unsafePeek();
    if (_shouldStepDownOnReconfig(lk, newConfig, myIndex)) {
        _topCoord->prepareForUnconditionalStepDown();
        lk.unlock();
        // Primary node will be either unelectable or removed after the configuration change.
        // So, finish the reconfig under RSTL, so that the step down occurs safely.
        const Date_t startTimeKillConflictingOperations = _replExecutor->now();
        if (gFeatureFlagIntentRegistration.isEnabled()) {
            rstg.emplace(_killConflictingOperations(
                rss::consensus::IntentRegistry::InterruptionType::StepDown, opCtx.get()));
        }
        const Date_t endTimeKillConflictingOperations = _replExecutor->now();
        LOGV2(962669,
              "killConflictingOperations in stepDown completed",
              "totalTime"_attr =
                  (endTimeKillConflictingOperations - startTimeKillConflictingOperations));

        const Date_t startTimeAcquireRSTL = _replExecutor->now();
        arsd.emplace(
            this, opCtx.get(), ReplicationCoordinator::OpsKillingStateTransitionEnum::kStepDown);
        const Date_t endTimeAcquireRSTL = _replExecutor->now();
        LOGV2(962668,
              "Acquired RSTL for stepDown",
              "totalTimeToAcquire"_attr = (endTimeAcquireRSTL - startTimeAcquireRSTL));

        lk.lock();
        if (_topCoord->isSteppingDownUnconditionally()) {
            invariant(shard_role_details::getLocker(opCtx.get())->isRSTLExclusive() ||
                      gFeatureFlagIntentRegistration.isEnabled());
            LOGV2(21481,
                  "Stepping down from primary, because we received a new config via heartbeat");
            // We need to release the mutex before yielding locks for prepared transactions, which
            // might check out sessions, to avoid deadlocks with checked-out sessions accessing
            // this mutex.
            lk.unlock();
            const Date_t startTimeYieldLocksInvalidateSessions = _replExecutor->now();
            yieldLocksForPreparedTransactions(opCtx.get());
            invalidateSessionsForStepdown(opCtx.get());
            const Date_t endTimeYieldLocksInvalidateSessions = _replExecutor->now();
            LOGV2(9626610,
                  "Yielding locks for prepared transactions and invalidating sessions for stepDown "
                  "completed",
                  "totalTime"_attr = (endTimeYieldLocksInvalidateSessions -
                                      startTimeYieldLocksInvalidateSessions));
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
            const Date_t startTimeReleaseRSTL = _replExecutor->now();
            arsd.reset();
            const Date_t endTimeReleaseRSTL = _replExecutor->now();
            LOGV2(9626617,
                  "Released RSTL during stepDown",
                  "timeToRelease"_attr = (endTimeReleaseRSTL - startTimeReleaseRSTL));
            rstg = boost::none;
        }
    }

    invariant(_rsConfigState == kConfigHBReconfiguring);
    invariant(
        !rsc.isInitialized() ||
            rsc.getConfigVersionAndTerm() < newConfig.getConfigVersionAndTerm() || _selfIndex < 0,
        str::stream() << "initialized: " << rsc.isInitialized() << ", old config version and term: "
                      << rsc.getConfigVersionAndTerm().toString()
                      << ", new config version and term: "
                      << newConfig.getConfigVersionAndTerm().toString()
                      << ", selfIndex: " << _selfIndex);

    if (!myIndex.isOK()) {
        switch (myIndex.getStatus().code()) {
            case ErrorCodes::NodeNotFound:
                LOGV2(21482,
                      "Cannot find self in new replica set configuration; I must be removed",
                      "error"_attr = myIndex.getStatus());
                break;
            case ErrorCodes::InvalidReplicaSetConfig:
                LOGV2_ERROR(21489,
                            "Several entries in new config represent this node; "
                            "Removing self until an acceptable configuration arrives",
                            "error"_attr = myIndex.getStatus());
                break;
            default:
                LOGV2_ERROR(21490,
                            "Could not validate configuration received from remote node; "
                            "Removing self until an acceptable configuration arrives",
                            "error"_attr = myIndex.getStatus());
                break;
        }
        myIndex = StatusWith<int>(-1);
    }
    const bool contentChanged = !sameConfigContents(rsc, newConfig);
    // If we do not have an index, we should pass -1 as our index to avoid falsely adding ourself to
    // the data structures inside of the TopologyCoordinator.
    const int myIndexValue = myIndex.getStatus().isOK() ? myIndex.getValue() : -1;
    const Date_t startTimeUpdateMemberState = _replExecutor->now();
    const PostMemberStateUpdateAction action =
        _setCurrentRSConfig(lk, opCtx.get(), newConfig, myIndexValue);

    lk.unlock();
    if (contentChanged) {
        _externalState->notifyOtherMemberDataChanged();
    }
    ReplicaSetAwareServiceRegistry::get(_service).onSetCurrentConfig(opCtx.get());
    _performPostMemberStateUpdateAction(action);
    const Date_t endTimeUpdateMemberState = _replExecutor->now();
    if (MONGO_unlikely(waitForPostActionCompleteInHbReconfig.shouldFail())) {
        // Used in tests that wait for the post member state update action to complete.
        // eg. Closing connections upon being removed.
        LOGV2(5286701, "waitForPostActionCompleteInHbReconfig failpoint enabled");
    }
    const Date_t endTime = _replExecutor->now();
    LOGV2(9626611,
          "Heartbeat Reconfig succeeded",
          "totalTime"_attr = (endTime - startTime),
          "updateMemberStateTime"_attr = (endTimeUpdateMemberState - startTimeUpdateMemberState));
}

void ReplicationCoordinatorImpl::_trackHeartbeatHandle(
    WithLock,
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
    _heartbeatHandles.insert({handle.getValue(), {hbState, target}});
    if (_maxSeenHeartbeatQSize < _heartbeatHandles.size()) {
        _maxSeenHeartbeatQSize = _heartbeatHandles.size();
        heartBeatHandleMaxSeenQueueSize.set(_maxSeenHeartbeatQSize);
    }
    heartBeatHandleQueueSize.increment();
}

void ReplicationCoordinatorImpl::_untrackHeartbeatHandle(
    WithLock, const executor::TaskExecutor::CallbackHandle& handle) {
    auto erased = _heartbeatHandles.erase(handle);
    invariant(erased == 1);
    heartBeatHandleQueueSize.decrement(erased);
}

void ReplicationCoordinatorImpl::_cancelHeartbeats(WithLock) {
    LOGV2_FOR_HEARTBEATS(4615630, 2, "Cancelling all heartbeats");

    for (const auto& [handle, mdata] : _heartbeatHandles) {
        _replExecutor->cancel(handle);
    }
    // Heartbeat callbacks will remove themselves from _heartbeatHandles when they execute with
    // CallbackCanceled status, so it's better to leave the handles in the list, for now.

    _handleLivenessTimeoutCallback.cancel();
}

void ReplicationCoordinatorImpl::restartScheduledHeartbeats_forTest() {
    stdx::unique_lock lk(_mutex);
    invariant(getTestCommandsEnabled());
    _restartScheduledHeartbeats(lk, std::string{_rsConfig.unsafePeek().getReplSetName()});
};

void ReplicationCoordinatorImpl::_restartScheduledHeartbeats(WithLock lk,
                                                             const std::string& replSetName) {
    LOGV2_FOR_HEARTBEATS(5031800, 2, "Restarting all scheduled heartbeats");

    const Date_t now = _replExecutor->now();
    stdx::unordered_set<HostAndPort> restartedTargets;

    for (auto& [handle, mdata] : _heartbeatHandles) {
        // Only cancel heartbeats that are scheduled. If a heartbeat request has already been
        // sent, we should wait for the response instead.
        if (mdata.hbState != HeartbeatState::kScheduled) {
            continue;
        }

        _replExecutor->cancel(handle);

        // Track the members that we have cancelled heartbeats.
        restartedTargets.insert(mdata.target);
    }

    for (const auto& target : restartedTargets) {
        LOGV2_FOR_HEARTBEATS(5031802, 2, "Restarting heartbeat", "target"_attr = target);
        _scheduleHeartbeatToTarget(lk, target, now, replSetName);
        _topCoord->restartHeartbeat(now, target);
    }
}

void ReplicationCoordinatorImpl::_startHeartbeats(WithLock lk) {
    const Date_t now = _replExecutor->now();
    _seedList.clear();

    auto rsc = _rsConfig.unsafePeek();
    for (int i = 0; i < rsc.getNumMembers(); ++i) {
        if (i == _selfIndex) {
            continue;
        }
        auto target = rsc.getMemberAt(i).getHostAndPort();
        _scheduleHeartbeatToTarget(lk, target, now, std::string{rsc.getReplSetName()});
        _topCoord->restartHeartbeat(now, target);
    }

    _scheduleNextLivenessUpdate(lk, /* reschedule = */ false);
}

void ReplicationCoordinatorImpl::_handleLivenessTimeout(
    const executor::TaskExecutor::CallbackArgs& cbData) {
    stdx::unique_lock lk(_mutex);
    if (!cbData.status.isOK()) {
        return;
    }

    // Scan liveness table for problems and mark nodes as down by calling into topocoord.
    HeartbeatResponseAction action = _topCoord->checkMemberTimeouts(_replExecutor->now());
    // Don't mind potential asynchronous stepdown as this is the last step of
    // liveness check.
    lk = _handleHeartbeatResponseAction(
        action, StatusWith(ReplSetHeartbeatResponse()), std::move(lk));

    _scheduleNextLivenessUpdate(lk, /* reschedule = */ false);
}

void ReplicationCoordinatorImpl::_scheduleNextLivenessUpdate(WithLock lk, bool reschedule) {
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

    auto nextTimeout = earliestDate + _rsConfig.unsafePeek().getElectionTimeoutPeriod();
    LOGV2_DEBUG(21483, 3, "Scheduling next check", "nextTimeout"_attr = nextTimeout);

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

void ReplicationCoordinatorImpl::_rescheduleLivenessUpdate(WithLock lk, int updatedMemberId) {
    if ((_earliestMemberId != -1) && (_earliestMemberId != updatedMemberId)) {
        return;
    }
    _scheduleNextLivenessUpdate(lk, /* reschedule = */ true);
}

void ReplicationCoordinatorImpl::_cancelPriorityTakeover(WithLock) {
    if (_priorityTakeoverCbh.isValid()) {
        LOGV2(21484, "Canceling priority takeover callback");
        _replExecutor->cancel(_priorityTakeoverCbh);
        _priorityTakeoverCbh = CallbackHandle();
        _priorityTakeoverWhen = Date_t();
    }
}

void ReplicationCoordinatorImpl::_cancelCatchupTakeover(WithLock) {
    if (_catchupTakeoverCbh.isValid()) {
        LOGV2(21485, "Canceling catchup takeover callback");
        _replExecutor->cancel(_catchupTakeoverCbh);
        _catchupTakeoverCbh = CallbackHandle();
        _catchupTakeoverWhen = Date_t();
    }
}

void ReplicationCoordinatorImpl::_cancelAndRescheduleElectionTimeout(WithLock lk) {
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
        !_rsConfig.unsafePeek().getMemberAt(_selfIndex).isElectable();

    if (doNotReschedule || !wasActive || (now - logThrottleTime) >= Seconds(1)) {
        cancelAndRescheduleLogLevel = 4;
        logThrottleTime = now;
    }
    if (wasActive && doNotReschedule) {
        LOGV2_FOR_ELECTION(4615649,
                           cancelAndRescheduleLogLevel,
                           "Canceling election timeout callback",
                           "when"_attr = oldWhen);
        _handleElectionTimeoutCallback.cancel();
    }

    if (doNotReschedule)
        return;

    Milliseconds upperBound = Milliseconds(_getElectionOffsetUpperBound(lk));
    auto requestedWhen = now + _rsConfig.unsafePeek().getElectionTimeoutPeriod();
    invariant(requestedWhen > now);
    Status delayStatus =
        _handleElectionTimeoutCallback.delayUntilWithJitter(lk, requestedWhen, upperBound);
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
    stdx::lock_guard lock(_mutex);
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
        _cancelCatchupTakeover(lk);
        _cancelPriorityTakeover(lk);
        _cancelAndRescheduleElectionTimeout(lk);
        if (_inShutdown || _inQuiesceMode) {
            LOGV2_FOR_ELECTION(4615654, 0, "Not starting an election, since we are shutting down");
            return;
        }
    }

    const auto status = _topCoord->becomeCandidateIfElectable(_replExecutor->now(), reason);
    if (!status.isOK()) {
        switch (reason) {
            case StartElectionReasonEnum::kElectionTimeout:
                LOGV2_FOR_ELECTION(4615655,
                                   0,
                                   "Not starting an election, since we are not electable",
                                   "reason"_attr = status.reason());
                break;
            case StartElectionReasonEnum::kPriorityTakeover:
                LOGV2_FOR_ELECTION(4615656,
                                   0,
                                   "Not starting an election for a priority takeover, since we are "
                                   "not electable",
                                   "reason"_attr = status.reason());
                break;
            case StartElectionReasonEnum::kStepUpRequest:
            case StartElectionReasonEnum::kStepUpRequestSkipDryRun:
                LOGV2_FOR_ELECTION(4615657,
                                   0,
                                   "Not starting an election for a replSetStepUp request, since we "
                                   "are not electable",
                                   "reason"_attr = status.reason());
                break;
            case StartElectionReasonEnum::kCatchupTakeover:
                LOGV2_FOR_ELECTION(4615658,
                                   0,
                                   "Not starting an election for a catchup takeover, since we are "
                                   "not electable",
                                   "reason"_attr = status.reason());
                break;
            case StartElectionReasonEnum::kSingleNodePromptElection:
                LOGV2_FOR_ELECTION(4615653,
                                   0,
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
                "Starting an election, since we've seen no PRIMARY in election timeout period",
                "electionTimeoutPeriod"_attr = _rsConfig.unsafePeek().getElectionTimeoutPeriod());
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

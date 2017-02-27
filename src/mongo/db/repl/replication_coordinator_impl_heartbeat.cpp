/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replica_set_config_checks.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

namespace {

using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using CBHandle = ReplicationExecutor::CallbackHandle;
using CBHStatus = StatusWith<CBHandle>;
using LockGuard = stdx::lock_guard<stdx::mutex>;

MONGO_FP_DECLARE(blockHeartbeatStepdown);

}  // namespace

using executor::RemoteCommandRequest;

Milliseconds ReplicationCoordinatorImpl::_getRandomizedElectionOffset() {
    long long electionTimeout = durationCount<Milliseconds>(_rsConfig.getElectionTimeoutPeriod());
    long long randomOffsetUpperBound =
        electionTimeout * _externalState->getElectionTimeoutOffsetLimitFraction();

    // Avoid divide by zero error in random number generator.
    if (randomOffsetUpperBound == 0) {
        return Milliseconds(0);
    }

    int64_t randomOffset = _replExecutor.nextRandomInt64(randomOffsetUpperBound);
    return Milliseconds(randomOffset);
}

void ReplicationCoordinatorImpl::_doMemberHeartbeat(ReplicationExecutor::CallbackArgs cbData,
                                                    const HostAndPort& target,
                                                    int targetIndex) {
    LockGuard topoLock(_topoMutex);

    _untrackHeartbeatHandle(cbData.myHandle);
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }

    const Date_t now = _replExecutor.now();
    BSONObj heartbeatObj;
    Milliseconds timeout(0);
    if (isV1ElectionProtocol()) {
        const std::pair<ReplSetHeartbeatArgsV1, Milliseconds> hbRequest =
            _topCoord->prepareHeartbeatRequestV1(now, _settings.ourSetName(), target);
        heartbeatObj = hbRequest.first.toBSON();
        timeout = hbRequest.second;
    } else {
        const std::pair<ReplSetHeartbeatArgs, Milliseconds> hbRequest =
            _topCoord->prepareHeartbeatRequest(now, _settings.ourSetName(), target);
        heartbeatObj = hbRequest.first.toBSON();
        timeout = hbRequest.second;
    }

    const RemoteCommandRequest request(
        target, "admin", heartbeatObj, BSON(rpc::kReplSetMetadataFieldName << 1), nullptr, timeout);
    const ReplicationExecutor::RemoteCommandCallbackFn callback =
        stdx::bind(&ReplicationCoordinatorImpl::_handleHeartbeatResponse,
                   this,
                   stdx::placeholders::_1,
                   targetIndex);

    _trackHeartbeatHandle(_replExecutor.scheduleRemoteCommand(request, callback));
}

void ReplicationCoordinatorImpl::_scheduleHeartbeatToTarget(const HostAndPort& target,
                                                            int targetIndex,
                                                            Date_t when) {
    LOG(2) << "Scheduling heartbeat to " << target << " at " << dateToISOStringUTC(when);
    _trackHeartbeatHandle(
        _replExecutor.scheduleWorkAt(when,
                                     stdx::bind(&ReplicationCoordinatorImpl::_doMemberHeartbeat,
                                                this,
                                                stdx::placeholders::_1,
                                                target,
                                                targetIndex)));
}

void ReplicationCoordinatorImpl::_handleHeartbeatResponse(
    const ReplicationExecutor::RemoteCommandCallbackArgs& cbData, int targetIndex) {
    LockGuard topoLock(_topoMutex);

    // remove handle from queued heartbeats
    _untrackHeartbeatHandle(cbData.myHandle);

    // Parse and validate the response.  At the end of this step, if responseStatus is OK then
    // hbResponse is valid.
    Status responseStatus = cbData.response.status;
    if (responseStatus == ErrorCodes::CallbackCanceled) {
        return;
    }

    const HostAndPort& target = cbData.request.target;
    ReplSetHeartbeatResponse hbResponse;
    BSONObj resp;
    if (responseStatus.isOK()) {
        resp = cbData.response.data;
        responseStatus = hbResponse.initialize(resp, _topCoord->getTerm());
        StatusWith<rpc::ReplSetMetadata> replMetadata =
            rpc::ReplSetMetadata::readFromMetadata(cbData.response.metadata);

        // Reject heartbeat responses (and metadata) from nodes with mismatched replica set IDs.
        // It is problematic to perform this check in the heartbeat reconfiguring logic because it
        // is possible for two mismatched replica sets to have the same replica set name and
        // configuration version. A heartbeat reconfiguration would not take place in that case.
        // Additionally, this is where we would stop further processing of the metadata from an
        // unknown replica set.
        if (replMetadata.isOK() && _rsConfig.isInitialized() && _rsConfig.hasReplicaSetId() &&
            replMetadata.getValue().getReplicaSetId().isSet() &&
            _rsConfig.getReplicaSetId() != replMetadata.getValue().getReplicaSetId()) {
            responseStatus = Status(ErrorCodes::InvalidReplicaSetConfig,
                                    str::stream() << "replica set IDs do not match, ours: "
                                                  << _rsConfig.getReplicaSetId()
                                                  << "; remote node's: "
                                                  << replMetadata.getValue().getReplicaSetId());
            // Ignore metadata.
            replMetadata = responseStatus;
        }
        if (replMetadata.isOK()) {
            // Arbiters are the only nodes allowed to advance their commit point via heartbeats.
            if (getMemberState().arbiter()) {
                advanceCommitPoint(replMetadata.getValue().getLastOpCommitted());
            }
            // Asynchronous stepdown could happen, but it will wait for _topoMutex and execute
            // after this function, so we cannot and don't need to wait for it to finish.
            _processReplSetMetadata_incallback(replMetadata.getValue());
        }
    }
    const Date_t now = _replExecutor.now();
    const OpTime lastApplied = getMyLastAppliedOpTime();  // Locks and unlocks _mutex.
    Milliseconds networkTime(0);
    StatusWith<ReplSetHeartbeatResponse> hbStatusResponse(hbResponse);

    if (responseStatus.isOK()) {
        networkTime = cbData.response.elapsedMillis.value_or(Milliseconds{0});
        // TODO(sz) Because the term is duplicated in ReplSetMetaData, we can get rid of this
        // and update tests.
        const auto& hbResponse = hbStatusResponse.getValue();
        _updateTerm_incallback(hbResponse.getTerm());
        // Postpone election timeout if we have a successful heartbeat response from the primary.
        if (hbResponse.hasState() && hbResponse.getState().primary() &&
            hbResponse.getTerm() == _topCoord->getTerm()) {
            cancelAndRescheduleElectionTimeout();
        }
    } else {
        log() << "Error in heartbeat request to " << target << "; " << responseStatus;
        if (!resp.isEmpty()) {
            LOG(3) << "heartbeat response: " << resp;
        }

        hbStatusResponse = StatusWith<ReplSetHeartbeatResponse>(responseStatus);
    }

    HeartbeatResponseAction action = _topCoord->processHeartbeatResponse(
        now, networkTime, target, hbStatusResponse, lastApplied);

    if (action.getAction() == HeartbeatResponseAction::NoAction && hbStatusResponse.isOK() &&
        targetIndex >= 0 && hbStatusResponse.getValue().hasState() &&
        hbStatusResponse.getValue().getState() != MemberState::RS_PRIMARY) {
        ReplSetHeartbeatResponse hbResp = hbStatusResponse.getValue();
        if (hbResp.hasAppliedOpTime()) {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            if (hbResp.getConfigVersion() == _rsConfig.getConfigVersion()) {
                _updateOpTimesFromHeartbeat_inlock(
                    targetIndex,
                    hbResp.hasDurableOpTime() ? hbResp.getDurableOpTime() : OpTime(),
                    hbResp.getAppliedOpTime());
            }
        }
    }

    // Wake the stepdown waiter when our updated OpTime allows it to finish stepping down.
    _signalStepDownWaiter_inlock();

    _scheduleHeartbeatToTarget(
        target, targetIndex, std::max(now, action.getNextHeartbeatStartDate()));

    _handleHeartbeatResponseAction(action, hbStatusResponse, false /*we're not holding _mutex*/);
}

void ReplicationCoordinatorImpl::_updateOpTimesFromHeartbeat_inlock(int targetIndex,
                                                                    const OpTime& durableOpTime,
                                                                    const OpTime& appliedOpTime) {
    invariant(_selfIndex >= 0);
    invariant(targetIndex >= 0);

    SlaveInfo& slaveInfo = _slaveInfo[targetIndex];
    if (appliedOpTime > slaveInfo.lastAppliedOpTime) {
        _updateSlaveInfoAppliedOpTime_inlock(&slaveInfo, appliedOpTime);
    }
    if (durableOpTime > slaveInfo.lastDurableOpTime) {
        _updateSlaveInfoDurableOpTime_inlock(&slaveInfo, durableOpTime);
    }
}

void ReplicationCoordinatorImpl::_handleHeartbeatResponseAction(
    const HeartbeatResponseAction& action,
    const StatusWith<ReplSetHeartbeatResponse>& responseStatus,
    bool hasMutex) {
    switch (action.getAction()) {
        case HeartbeatResponseAction::NoAction:
            // Update the cached member state if different than the current topology member state
            if (_memberState != _topCoord->getMemberState()) {
                invariant(!hasMutex);
                stdx::unique_lock<stdx::mutex> lk(_mutex);
                const PostMemberStateUpdateAction postUpdateAction =
                    _updateMemberStateFromTopologyCoordinator_inlock();
                lk.unlock();
                _performPostMemberStateUpdateAction(postUpdateAction);
            }
            break;
        case HeartbeatResponseAction::Reconfig:
            invariant(responseStatus.isOK());
            _scheduleHeartbeatReconfig(responseStatus.getValue().getConfig());
            break;
        case HeartbeatResponseAction::StartElection:
            _startElectSelf();
            break;
        case HeartbeatResponseAction::StepDownSelf:
            invariant(action.getPrimaryConfigIndex() == _selfIndex);
            log() << "Stepping down from primary in response to heartbeat";
            _topCoord->prepareForStepDown();
            // Don't need to wait for stepdown to finish.
            _stepDownStart(hasMutex);
            break;
        case HeartbeatResponseAction::StepDownRemotePrimary: {
            invariant(action.getPrimaryConfigIndex() != _selfIndex);
            _requestRemotePrimaryStepdown(
                _rsConfig.getMemberAt(action.getPrimaryConfigIndex()).getHostAndPort());
            break;
        }
        case HeartbeatResponseAction::PriorityTakeover: {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            // Don't schedule a takeover if one is already scheduled.
            if (!_priorityTakeoverCbh.isValid()) {

                // Add randomized offset to calculated priority takeover delay.
                Milliseconds priorityTakeoverDelay = _rsConfig.getPriorityTakeoverDelay(_selfIndex);
                Milliseconds randomOffset = _getRandomizedElectionOffset();
                _priorityTakeoverWhen = _replExecutor.now() + priorityTakeoverDelay + randomOffset;
                log() << "Scheduling priority takeover at " << _priorityTakeoverWhen;
                _priorityTakeoverCbh = _scheduleWorkAt(
                    _priorityTakeoverWhen,
                    stdx::bind(
                        &ReplicationCoordinatorImpl::_startElectSelfIfEligibleV1, this, true));
            }
            break;
        }
    }
}

namespace {
/**
 * This callback is purely for logging and has no effect on any other operations
 */
void remoteStepdownCallback(const ReplicationExecutor::RemoteCommandCallbackArgs& cbData) {
    const Status status = cbData.response.status;
    if (status == ErrorCodes::CallbackCanceled) {
        return;
    }

    if (status.isOK()) {
        LOG(1) << "stepdown of primary(" << cbData.request.target << ") succeeded with response -- "
               << cbData.response.data;
    } else {
        warning() << "stepdown of primary(" << cbData.request.target << ") failed due to "
                  << cbData.response.status;
    }
}
}  // namespace

void ReplicationCoordinatorImpl::_requestRemotePrimaryStepdown(const HostAndPort& target) {
    auto secondaryCatchUpPeriod(duration_cast<Seconds>(_rsConfig.getHeartbeatInterval() / 2));
    RemoteCommandRequest request(
        target,
        "admin",
        BSON("replSetStepDown" << 20 << "secondaryCatchUpPeriodSecs"
                               << std::min(static_cast<long long>(secondaryCatchUpPeriod.count()),
                                           20LL)),
        nullptr);

    log() << "Requesting " << target << " step down from primary";
    CBHStatus cbh = _replExecutor.scheduleRemoteCommand(request, remoteStepdownCallback);
    if (cbh.getStatus() != ErrorCodes::ShutdownInProgress) {
        fassert(18808, cbh.getStatus());
    }
}

ReplicationExecutor::EventHandle ReplicationCoordinatorImpl::_stepDownStart(bool hasMutex) {
    {
        boost::optional<stdx::lock_guard<stdx::mutex>> lk;
        if (!hasMutex) {
            lk.emplace(_mutex);
        }
        _stepDownPending = true;
    }
    auto finishEvent = _makeEvent();
    if (!finishEvent) {
        return finishEvent;
    }

    _replExecutor.scheduleWorkWithGlobalExclusiveLock(stdx::bind(
        &ReplicationCoordinatorImpl::_stepDownFinish, this, stdx::placeholders::_1, finishEvent));
    return finishEvent;
}

void ReplicationCoordinatorImpl::_stepDownFinish(
    const ReplicationExecutor::CallbackArgs& cbData,
    const ReplicationExecutor::EventHandle& finishedEvent) {
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }

    if (MONGO_FAIL_POINT(blockHeartbeatStepdown)) {
        // Must reschedule rather than block so we don't take up threads in the replication
        // executor.
        sleepmillis(10);
        _replExecutor.scheduleWorkWithGlobalExclusiveLock(
            stdx::bind(&ReplicationCoordinatorImpl::_stepDownFinish,
                       this,
                       stdx::placeholders::_1,
                       finishedEvent));

        return;
    }

    LockGuard topoLock(_topoMutex);

    invariant(cbData.txn);
    // TODO Add invariant that we've got global shared or global exclusive lock, when supported
    // by lock manager.
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_topCoord->stepDownIfPending()) {
        const auto action = _updateMemberStateFromTopologyCoordinator_inlock();
        lk.unlock();
        _performPostMemberStateUpdateAction(action);
    }
    _replExecutor.signalEvent(finishedEvent);
}

void ReplicationCoordinatorImpl::_scheduleHeartbeatReconfig(const ReplicaSetConfig& newConfig) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown) {
        return;
    }

    switch (_rsConfigState) {
        case kConfigUninitialized:
        case kConfigSteady:
            LOG(1) << "Received new config via heartbeat with version "
                   << newConfig.getConfigVersion();
            break;
        case kConfigInitiating:
        case kConfigReconfiguring:
        case kConfigHBReconfiguring:
            LOG(1) << "Ignoring new configuration with version " << newConfig.getConfigVersion()
                   << " because already in the midst of a configuration process";
            return;
        case kConfigPreStart:
        case kConfigStartingUp:
        case kConfigReplicationDisabled:
            severe() << "Reconfiguration request occurred while _rsConfigState == "
                     << int(_rsConfigState) << "; aborting.";
            fassertFailed(18807);
    }
    _setConfigState_inlock(kConfigHBReconfiguring);
    invariant(!_rsConfig.isInitialized() ||
              _rsConfig.getConfigVersion() < newConfig.getConfigVersion());
    if (_freshnessChecker) {
        _freshnessChecker->cancel();
        if (_electCmdRunner) {
            _electCmdRunner->cancel();
        }
        _replExecutor.onEvent(
            _electionFinishedEvent,
            stdx::bind(&ReplicationCoordinatorImpl::_heartbeatReconfigAfterElectionCanceled,
                       this,
                       stdx::placeholders::_1,
                       newConfig));
        return;
    }
    _replExecutor.scheduleDBWork(stdx::bind(&ReplicationCoordinatorImpl::_heartbeatReconfigStore,
                                            this,
                                            stdx::placeholders::_1,
                                            newConfig));
}

void ReplicationCoordinatorImpl::_heartbeatReconfigAfterElectionCanceled(
    const ReplicationExecutor::CallbackArgs& cbData, const ReplicaSetConfig& newConfig) {
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }

    LockGuard topoLock(_topoMutex);
    fassert(18911, cbData.status);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown) {
        return;
    }

    _replExecutor.scheduleDBWork(stdx::bind(&ReplicationCoordinatorImpl::_heartbeatReconfigStore,
                                            this,
                                            stdx::placeholders::_1,
                                            newConfig));
}

void ReplicationCoordinatorImpl::_heartbeatReconfigStore(
    const ReplicationExecutor::CallbackArgs& cbd, const ReplicaSetConfig& newConfig) {
    if (cbd.status.code() == ErrorCodes::CallbackCanceled) {
        log() << "The callback to persist the replica set configuration was canceled - "
              << "the configuration was not persisted but was used: " << newConfig.toBSON();
        return;
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex, stdx::defer_lock);

    const StatusWith<int> myIndex = validateConfigForHeartbeatReconfig(
        _externalState.get(), newConfig, getGlobalServiceContext());

    if (myIndex.getStatus() == ErrorCodes::NodeNotFound) {
        lk.lock();
        // If this node absent in newConfig, and this node was not previously initialized,
        // return to kConfigUninitialized immediately, rather than storing the config and
        // transitioning into the RS_REMOVED state.  See SERVER-15740.
        if (!_rsConfig.isInitialized()) {
            invariant(_rsConfigState == kConfigHBReconfiguring);
            LOG(1) << "Ignoring new configuration in heartbeat response because we are "
                      "uninitialized and not a member of the new configuration";
            _setConfigState_inlock(kConfigUninitialized);
            return;
        }
        lk.unlock();
    }

    if (!myIndex.getStatus().isOK() && myIndex.getStatus() != ErrorCodes::NodeNotFound) {
        warning() << "Not persisting new configuration in heartbeat response to disk because "
                     "it is invalid: "
                  << myIndex.getStatus();
    } else {
        Status status = _externalState->storeLocalConfigDocument(cbd.txn, newConfig.toBSON());

        lk.lock();
        if (!status.isOK()) {
            error() << "Ignoring new configuration in heartbeat response because we failed to"
                       " write it to stable storage; "
                    << status;
            invariant(_rsConfigState == kConfigHBReconfiguring);
            if (_rsConfig.isInitialized()) {
                _setConfigState_inlock(kConfigSteady);
            } else {
                _setConfigState_inlock(kConfigUninitialized);
            }
            return;
        }
        auto isFirstConfig = !_rsConfig.isInitialized();
        lk.unlock();

        bool isArbiter = myIndex.isOK() && myIndex.getValue() != -1 &&
            newConfig.getMemberAt(myIndex.getValue()).isArbiter();
        if (!isArbiter && isFirstConfig) {
            _externalState->startThreads(_settings);
            _startDataReplication(cbd.txn);
        }
    }

    const CallbackFn reconfigFinishFn(
        stdx::bind(&ReplicationCoordinatorImpl::_heartbeatReconfigFinish,
                   this,
                   stdx::placeholders::_1,
                   newConfig,
                   myIndex));

    // Make sure that the reconfigFinishFn doesn't finish until we've reset
    // _heartbeatReconfigThread.
    lk.lock();
    if (_memberState.primary()) {
        // If the primary is receiving a heartbeat reconfig, that strongly suggests
        // that there has been a force reconfiguration.  In any event, it might lead
        // to this node stepping down as primary, so we'd better do it with the global
        // lock.
        _replExecutor.scheduleWorkWithGlobalExclusiveLock(reconfigFinishFn);
    } else {
        _replExecutor.scheduleWork(reconfigFinishFn);
    }
}

void ReplicationCoordinatorImpl::_heartbeatReconfigFinish(
    const ReplicationExecutor::CallbackArgs& cbData,
    const ReplicaSetConfig& newConfig,
    StatusWith<int> myIndex) {
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }

    LockGuard topoLock(_topoMutex);

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_rsConfigState == kConfigHBReconfiguring);
    invariant(!_rsConfig.isInitialized() ||
              _rsConfig.getConfigVersion() < newConfig.getConfigVersion());

    if (_getMemberState_inlock().primary() && !cbData.txn) {
        // Not having an OperationContext in the CallbackData means we definitely aren't holding
        // the global lock.  Since we're primary and this reconfig could cause us to stepdown,
        // reschedule this work with the global exclusive lock so the stepdown is safe.
        // TODO(spencer): When we *do* have an OperationContext, consult it to confirm that
        // we are indeed holding the global lock.
        _replExecutor.scheduleWorkWithGlobalExclusiveLock(
            stdx::bind(&ReplicationCoordinatorImpl::_heartbeatReconfigFinish,
                       this,
                       stdx::placeholders::_1,
                       newConfig,
                       myIndex));
        return;
    }

    // Do not conduct an election during a reconfig, as the node may not be electable post-reconfig.
    if (_topCoord->getRole() == TopologyCoordinator::Role::candidate) {
        if (isV1ElectionProtocol()) {
            invariant(_voteRequester);
            _voteRequester->cancel();
        } else {
            invariant(_freshnessChecker);
            _freshnessChecker->cancel();
            if (_electCmdRunner) {
                _electCmdRunner->cancel();
            }
        }
        // Wait for the election to complete and the node's Role to be set to follower.
        _replExecutor.onEvent(_electionFinishedEvent,
                              stdx::bind(&ReplicationCoordinatorImpl::_heartbeatReconfigFinish,
                                         this,
                                         stdx::placeholders::_1,
                                         newConfig,
                                         myIndex));
        return;
    }

    if (!myIndex.isOK()) {
        switch (myIndex.getStatus().code()) {
            case ErrorCodes::NodeNotFound:
                log() << "Cannot find self in new replica set configuration; I must be removed; "
                      << myIndex.getStatus();
                break;
            case ErrorCodes::DuplicateKey:
                error() << "Several entries in new config represent this node; "
                           "Removing self until an acceptable configuration arrives; "
                        << myIndex.getStatus();
                break;
            default:
                error() << "Could not validate configuration received from remote node; "
                           "Removing self until an acceptable configuration arrives; "
                        << myIndex.getStatus();
                break;
        }
        myIndex = StatusWith<int>(-1);
    }
    const ReplicaSetConfig oldConfig = _rsConfig;
    // If we do not have an index, we should pass -1 as our index to avoid falsely adding ourself to
    // the data structures inside of the TopologyCoordinator.
    const int myIndexValue = myIndex.getStatus().isOK() ? myIndex.getValue() : -1;
    const PostMemberStateUpdateAction action = _setCurrentRSConfig_inlock(newConfig, myIndexValue);
    lk.unlock();
    auto evh = _resetElectionInfoOnProtocolVersionUpgrade(oldConfig, newConfig);
    if (evh) {
        _replExecutor.onEvent(evh, [this, action](const CallbackArgs& cbArgs) {
            LockGuard topoLock(_topoMutex);
            _performPostMemberStateUpdateAction(action);
        });
    } else {
        _performPostMemberStateUpdateAction(action);
    }
}

void ReplicationCoordinatorImpl::_trackHeartbeatHandle(const StatusWith<CBHandle>& handle) {
    if (handle.getStatus() == ErrorCodes::ShutdownInProgress) {
        return;
    }
    fassert(18912, handle.getStatus());
    _heartbeatHandles.push_back(handle.getValue());
}

void ReplicationCoordinatorImpl::_untrackHeartbeatHandle(const CBHandle& handle) {
    const HeartbeatHandles::iterator newEnd =
        std::remove(_heartbeatHandles.begin(), _heartbeatHandles.end(), handle);
    invariant(newEnd != _heartbeatHandles.end());
    _heartbeatHandles.erase(newEnd, _heartbeatHandles.end());
}

void ReplicationCoordinatorImpl::_cancelHeartbeats_inlock() {
    std::for_each(_heartbeatHandles.begin(),
                  _heartbeatHandles.end(),
                  stdx::bind(&ReplicationExecutor::cancel, &_replExecutor, stdx::placeholders::_1));
    // Heartbeat callbacks will remove themselves from _heartbeatHandles when they execute with
    // CallbackCanceled status, so it's better to leave the handles in the list, for now.

    if (_handleLivenessTimeoutCbh.isValid()) {
        _replExecutor.cancel(_handleLivenessTimeoutCbh);
    }
}

void ReplicationCoordinatorImpl::_restartHeartbeats_inlock() {
    _cancelHeartbeats_inlock();
    _startHeartbeats_inlock();
}

void ReplicationCoordinatorImpl::_startHeartbeats_inlock() {
    const Date_t now = _replExecutor.now();
    _seedList.clear();
    for (int i = 0; i < _rsConfig.getNumMembers(); ++i) {
        if (i == _selfIndex) {
            continue;
        }
        _scheduleHeartbeatToTarget(_rsConfig.getMemberAt(i).getHostAndPort(), i, now);
    }
    if (isV1ElectionProtocol()) {
        for (auto&& slaveInfo : _slaveInfo) {
            slaveInfo.lastUpdate = _replExecutor.now();
            slaveInfo.down = false;
        }
        _scheduleNextLivenessUpdate_inlock();
    }
}

void ReplicationCoordinatorImpl::_handleLivenessTimeout(
    const ReplicationExecutor::CallbackArgs& cbData) {
    LockGuard topoLock(_topoMutex);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // Only reset the callback handle if it matches, otherwise more will be coming through
    if (cbData.myHandle == _handleLivenessTimeoutCbh) {
        _handleLivenessTimeoutCbh = CallbackHandle();
    }
    if (!cbData.status.isOK()) {
        return;
    }
    if (!isV1ElectionProtocol()) {
        return;
    }

    // Scan liveness table for problems and mark nodes as down by calling into topocoord.
    auto now(_replExecutor.now());
    for (auto&& slaveInfo : _slaveInfo) {
        if (slaveInfo.self) {
            continue;
        }
        if (slaveInfo.down) {
            continue;
        }

        if (now - slaveInfo.lastUpdate >= _rsConfig.getElectionTimeoutPeriod()) {
            int memberIndex = _rsConfig.findMemberIndexByConfigId(slaveInfo.memberId);
            if (memberIndex == -1) {
                continue;
            }

            slaveInfo.down = true;

            if (_memberState.primary()) {
                // Only adjust hbdata if we are primary, since only the primary has a full view
                // of the entire cluster.
                // Secondaries might not see other secondaries in the cluster if they are not
                // downstream.
                HeartbeatResponseAction action =
                    _topCoord->setMemberAsDown(now, memberIndex, _getMyLastDurableOpTime_inlock());
                // Don't mind potential asynchronous stepdown as this is the last step of
                // liveness check.
                _handleHeartbeatResponseAction(action,
                                               makeStatusWith<ReplSetHeartbeatResponse>(),
                                               true /*we're holding _mutex*/);
            }
        }
    }
    _scheduleNextLivenessUpdate_inlock();
}

void ReplicationCoordinatorImpl::_scheduleNextLivenessUpdate_inlock() {
    if (!isV1ElectionProtocol()) {
        return;
    }
    // Scan liveness table for earliest date; schedule a run at (that date plus election
    // timeout).
    Date_t earliestDate = Date_t::max();
    int earliestMemberId = -1;
    for (auto&& slaveInfo : _slaveInfo) {
        if (slaveInfo.self) {
            continue;
        }
        if (slaveInfo.down) {
            // Already down.
            continue;
        }
        LOG(3) << "slaveinfo lastupdate is: " << slaveInfo.lastUpdate;
        if (earliestDate > slaveInfo.lastUpdate) {
            earliestDate = slaveInfo.lastUpdate;
            earliestMemberId = slaveInfo.memberId;
        }
    }
    LOG(3) << "earliest member " << earliestMemberId << " date: " << earliestDate;
    if (earliestMemberId == -1 || earliestDate == Date_t::max()) {
        _earliestMemberId = -1;
        // Nobody here but us.
        return;
    }

    if (_handleLivenessTimeoutCbh.isValid() && !_handleLivenessTimeoutCbh.isCanceled()) {
        // don't bother to schedule; one is already scheduled and pending.
        return;
    }

    auto nextTimeout = earliestDate + _rsConfig.getElectionTimeoutPeriod();
    if (nextTimeout > _replExecutor.now()) {
        LOG(3) << "scheduling next check at " << nextTimeout;
        auto cbh = _scheduleWorkAt(nextTimeout,
                                   stdx::bind(&ReplicationCoordinatorImpl::_handleLivenessTimeout,
                                              this,
                                              stdx::placeholders::_1));
        if (!cbh) {
            return;
        }
        _handleLivenessTimeoutCbh = cbh;
        _earliestMemberId = earliestMemberId;
    }
}

void ReplicationCoordinatorImpl::_cancelAndRescheduleLivenessUpdate_inlock(int updatedMemberId) {
    if ((_earliestMemberId != -1) && (_earliestMemberId != updatedMemberId)) {
        return;
    }
    if (_handleLivenessTimeoutCbh.isValid()) {
        _replExecutor.cancel(_handleLivenessTimeoutCbh);
    }
    _scheduleNextLivenessUpdate_inlock();
}

void ReplicationCoordinatorImpl::_cancelPriorityTakeover_inlock() {
    if (_priorityTakeoverCbh.isValid()) {
        log() << "Canceling priority takeover callback";
        _replExecutor.cancel(_priorityTakeoverCbh);
        _priorityTakeoverCbh = CallbackHandle();
        _priorityTakeoverWhen = Date_t();
    }
}

void ReplicationCoordinatorImpl::_cancelAndRescheduleElectionTimeout_inlock() {
    if (_handleElectionTimeoutCbh.isValid()) {
        LOG(4) << "Canceling election timeout callback at " << _handleElectionTimeoutWhen;
        _replExecutor.cancel(_handleElectionTimeoutCbh);
        _handleElectionTimeoutCbh = CallbackHandle();
        _handleElectionTimeoutWhen = Date_t();
    }

    if (_inShutdown) {
        return;
    }

    if (!isV1ElectionProtocol()) {
        return;
    }

    if (!_memberState.secondary()) {
        return;
    }

    if (_selfIndex < 0) {
        return;
    }

    if (!_rsConfig.getMemberAt(_selfIndex).isElectable()) {
        return;
    }

    Milliseconds randomOffset = _getRandomizedElectionOffset();
    auto now = _replExecutor.now();
    auto when = now + _rsConfig.getElectionTimeoutPeriod() + randomOffset;
    invariant(when > now);
    LOG(4) << "Scheduling election timeout callback at " << when;
    _handleElectionTimeoutWhen = when;
    _handleElectionTimeoutCbh = _scheduleWorkAt(
        when, stdx::bind(&ReplicationCoordinatorImpl::_startElectSelfIfEligibleV1, this, false));
}

void ReplicationCoordinatorImpl::_startElectSelfIfEligibleV1(bool isPriorityTakeover) {
    LockGuard topoLock(_topoMutex);

    if (!isV1ElectionProtocol()) {
        return;
    }

    // We should always reschedule this callback even if we do not make it to the election
    // process.
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _cancelPriorityTakeover_inlock();
        _cancelAndRescheduleElectionTimeout_inlock();
        if (_inShutdown) {
            log() << "Not starting an election, since we are shutting down";
            return;
        }
    }

    const auto status = _topCoord->becomeCandidateIfElectable(
        _replExecutor.now(), getMyLastAppliedOpTime(), isPriorityTakeover);
    if (!status.isOK()) {
        if (isPriorityTakeover) {
            log() << "Not starting an election for a priority takeover, "
                  << "since we are not electable due to: " << status.reason();
        } else {
            log() << "Not starting an election, since we are not electable due to: "
                  << status.reason();
        }
        return;
    }
    if (isPriorityTakeover) {
        log() << "Starting an election for a priority takeover";
    } else {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        log() << "Starting an election, since we've seen no PRIMARY in the past "
              << _rsConfig.getElectionTimeoutPeriod();
    }
    _startElectSelfV1();
}

}  // namespace repl
}  // namespace mongo

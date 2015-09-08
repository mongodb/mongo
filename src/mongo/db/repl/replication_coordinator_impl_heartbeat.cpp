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
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

namespace {

typedef ReplicationExecutor::CallbackHandle CBHandle;

}  // namespace

using executor::RemoteCommandRequest;

void ReplicationCoordinatorImpl::_doMemberHeartbeat(ReplicationExecutor::CallbackArgs cbData,
                                                    const HostAndPort& target,
                                                    int targetIndex) {
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
        target, "admin", heartbeatObj, BSON(rpc::kReplSetMetadataFieldName << 1), timeout);
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
    // remove handle from queued heartbeats
    _untrackHeartbeatHandle(cbData.myHandle);

    // Parse and validate the response.  At the end of this step, if responseStatus is OK then
    // hbResponse is valid.
    Status responseStatus = cbData.response.getStatus();
    if (responseStatus == ErrorCodes::CallbackCanceled) {
        return;
    }

    const HostAndPort& target = cbData.request.target;
    ReplSetHeartbeatResponse hbResponse;
    BSONObj resp;
    if (responseStatus.isOK()) {
        resp = cbData.response.getValue().data;
        responseStatus = hbResponse.initialize(resp, _topCoord->getTerm());
        StatusWith<rpc::ReplSetMetadata> replMetadata =
            rpc::ReplSetMetadata::readFromMetadata(cbData.response.getValue().metadata);
        if (replMetadata.isOK()) {
            _processReplSetMetadata_incallback(replMetadata.getValue());
        }
    }
    const Date_t now = _replExecutor.now();
    const OpTime lastApplied = getMyLastOptime();  // Locks and unlocks _mutex.
    Milliseconds networkTime(0);
    StatusWith<ReplSetHeartbeatResponse> hbStatusResponse(hbResponse);

    if (responseStatus.isOK()) {
        networkTime = cbData.response.getValue().elapsedMillis;
        _updateTerm_incallback(hbStatusResponse.getValue().getTerm());
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
        hbStatusResponse.getValue().hasOpTime() && targetIndex >= 0 &&
        hbStatusResponse.getValue().hasState() &&
        hbStatusResponse.getValue().getState() != MemberState::RS_PRIMARY) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        if (hbStatusResponse.getValue().getConfigVersion() == _rsConfig.getConfigVersion()) {
            _updateOpTimeFromHeartbeat_inlock(targetIndex, hbStatusResponse.getValue().getOpTime());
            // TODO: Enable with Data Replicator
            // lk.unlock();
            //_dr.slavesHaveProgressed();
        }
    }

    // In case our updated OpTime allows a waiter to finish stepping down, we wake all the waiters.
    _signalStepDownWaiters();

    _scheduleHeartbeatToTarget(
        target, targetIndex, std::max(now, action.getNextHeartbeatStartDate()));

    _handleHeartbeatResponseAction(action, hbStatusResponse);
}

void ReplicationCoordinatorImpl::_updateOpTimeFromHeartbeat_inlock(int targetIndex,
                                                                   const OpTime& optime) {
    invariant(_selfIndex >= 0);
    invariant(targetIndex >= 0);

    SlaveInfo& slaveInfo = _slaveInfo[targetIndex];
    if (optime > slaveInfo.opTime) {
        _updateSlaveInfoOptime_inlock(&slaveInfo, optime);
    }
}

void ReplicationCoordinatorImpl::_handleHeartbeatResponseAction(
    const HeartbeatResponseAction& action,
    const StatusWith<ReplSetHeartbeatResponse>& responseStatus) {
    switch (action.getAction()) {
        case HeartbeatResponseAction::NoAction:
            // Update the cached member state if different than the current topology member state
            if (_memberState != _topCoord->getMemberState()) {
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
            if (isV1ElectionProtocol()) {
                _startElectSelfV1();
            } else {
                _startElectSelf();
            }
            break;
        case HeartbeatResponseAction::StepDownSelf:
            invariant(action.getPrimaryConfigIndex() == _selfIndex);
            log() << "Stepping down from primary in response to heartbeat";
            _stepDownStart();
            break;
        case HeartbeatResponseAction::StepDownRemotePrimary: {
            invariant(action.getPrimaryConfigIndex() != _selfIndex);
            _requestRemotePrimaryStepdown(
                _rsConfig.getMemberAt(action.getPrimaryConfigIndex()).getHostAndPort());
            break;
        }
        case HeartbeatResponseAction::PriorityTakeover: {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            if (!_priorityTakeoverCbh.isValid()) {
                Milliseconds delay = _rsConfig.getPriorityTakeoverDelay(_selfIndex);
                auto cbh = _replExecutor.scheduleWorkAt(
                    _replExecutor.now() + delay,
                    stdx::bind(&ReplicationCoordinatorImpl::_priorityTakeover,
                               this,
                               stdx::placeholders::_1));
                if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
                    return;
                }
                fassert(28806, cbh.getStatus());
                _priorityTakeoverCbh = cbh.getValue();
            }
            break;
        }
        default:
            severe() << "Illegal heartbeat response action code " << int(action.getAction());
            invariant(false);
    }
}

namespace {
/**
 * This callback is purely for logging and has no effect on any other operations
 */
void remoteStepdownCallback(const ReplicationExecutor::RemoteCommandCallbackArgs& cbData) {
    const Status status = cbData.response.getStatus();
    if (status == ErrorCodes::CallbackCanceled) {
        return;
    }

    if (status.isOK()) {
        LOG(1) << "stepdown of primary(" << cbData.request.target << ") succeeded with response -- "
               << cbData.response.getValue().data;
    } else {
        warning() << "stepdown of primary(" << cbData.request.target << ") failed due to "
                  << cbData.response.getStatus();
    }
}
}  // namespace

void ReplicationCoordinatorImpl::_requestRemotePrimaryStepdown(const HostAndPort& target) {
    RemoteCommandRequest request(target, "admin", BSON("replSetStepDown" << 1));

    log() << "Requesting " << target << " step down from primary";
    CBHStatus cbh = _replExecutor.scheduleRemoteCommand(request, remoteStepdownCallback);
    if (cbh.getStatus() != ErrorCodes::ShutdownInProgress) {
        fassert(18808, cbh.getStatus());
    }
}

void ReplicationCoordinatorImpl::_stepDownStart() {
    const StatusWith<ReplicationExecutor::EventHandle> stepDownFinishEvh =
        _replExecutor.makeEvent();
    if (!stepDownFinishEvh.isOK()) {
        if (stepDownFinishEvh.getStatus() != ErrorCodes::ShutdownInProgress) {
            fassert(28672, stepDownFinishEvh.getStatus());
        }
        return;
    }
    _stepDownFinishedEvent = stepDownFinishEvh.getValue();
    _replExecutor.scheduleWorkWithGlobalExclusiveLock(
        stdx::bind(&ReplicationCoordinatorImpl::_stepDownFinish, this, stdx::placeholders::_1));
}

void ReplicationCoordinatorImpl::_stepDownFinish(const ReplicationExecutor::CallbackArgs& cbData) {
    if (cbData.status == ErrorCodes::CallbackCanceled) {
        return;
    }
    invariant(cbData.txn);
    // TODO Add invariant that we've got global shared or global exclusive lock, when supported
    // by lock manager.
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _topCoord->stepDownIfPending();
    const PostMemberStateUpdateAction action = _updateMemberStateFromTopologyCoordinator_inlock();
    lk.unlock();
    _performPostMemberStateUpdateAction(action);
    if (_stepDownFinishedEvent.isValid()) {
        _replExecutor.signalEvent(_stepDownFinishedEvent);
    }
}

void ReplicationCoordinatorImpl::_scheduleHeartbeatReconfig(const ReplicaSetConfig& newConfig) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown) {
        return;
    }

    switch (_rsConfigState) {
        case kConfigStartingUp:
            LOG(1) << "Ignoring new configuration with version " << newConfig.getConfigVersion()
                   << " because still attempting to load local configuration information";
            return;
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
        default:
            severe() << "Reconfiguration request occurred while _rsConfigState == "
                     << int(_rsConfigState) << "; aborting.";
            fassertFailed(18807);
    }
    _setConfigState_inlock(kConfigHBReconfiguring);
    invariant(!_rsConfig.isInitialized() ||
              _rsConfig.getConfigVersion() < newConfig.getConfigVersion());
    if (_freshnessChecker) {
        _freshnessChecker->cancel(&_replExecutor);
        if (_electCmdRunner) {
            _electCmdRunner->cancel(&_replExecutor);
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

    const StatusWith<int> myIndex =
        validateConfigForHeartbeatReconfig(_externalState.get(), newConfig);

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
                     "it is invalid: " << myIndex.getStatus();
    } else {
        Status status = _externalState->storeLocalConfigDocument(cbd.txn, newConfig.toBSON());

        lk.lock();
        if (!status.isOK()) {
            error() << "Ignoring new configuration in heartbeat response because we failed to"
                       " write it to stable storage; " << status;
            invariant(_rsConfigState == kConfigHBReconfiguring);
            if (_rsConfig.isInitialized()) {
                _setConfigState_inlock(kConfigSteady);
            } else {
                _setConfigState_inlock(kConfigUninitialized);
            }
            return;
        }

        lk.unlock();

        _externalState->startThreads();
    }

    const stdx::function<void(const ReplicationExecutor::CallbackArgs&)> reconfigFinishFn(
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
    const PostMemberStateUpdateAction action =
        _setCurrentRSConfig_inlock(cbData, newConfig, myIndex.getValue());
    lk.unlock();
    _resetElectionInfoOnProtocolVersionUpgrade(newConfig);
    _performPostMemberStateUpdateAction(action);
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

void ReplicationCoordinatorImpl::_cancelHeartbeats() {
    std::for_each(_heartbeatHandles.begin(),
                  _heartbeatHandles.end(),
                  stdx::bind(&ReplicationExecutor::cancel, &_replExecutor, stdx::placeholders::_1));
    // Heartbeat callbacks will remove themselves from _heartbeatHandles when they execute with
    // CallbackCanceled status, so it's better to leave the handles in the list, for now.
}

void ReplicationCoordinatorImpl::_startHeartbeats_inlock(
    const ReplicationExecutor::CallbackArgs& cbData) {
    const Date_t now = _replExecutor.now();
    _seedList.clear();
    for (int i = 0; i < _rsConfig.getNumMembers(); ++i) {
        if (i == _selfIndex) {
            continue;
        }
        _scheduleHeartbeatToTarget(_rsConfig.getMemberAt(i).getHostAndPort(), i, now);
    }
    if (isV1ElectionProtocol()) {
        _scheduleNextLivenessUpdate_inlock(cbData);
    }
}

void ReplicationCoordinatorImpl::_handleLivenessTimeout(
    const ReplicationExecutor::CallbackArgs& cbData) {
    if (!cbData.status.isOK()) {
        return;
    }
    if (!isV1ElectionProtocol()) {
        return;
    }
    stdx::lock_guard<stdx::mutex> lock(_mutex);
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
                // Only adjust hbdata if we are primary, since only the primary has a full view of
                // the entire cluster.
                // Secondaries might not see other secondaries in the cluster if they are not
                // downstream.
                HeartbeatResponseAction action =
                    _topCoord->setMemberAsDown(now, memberIndex, _getMyLastOptime_inlock());
                _handleHeartbeatResponseAction(action, makeStatusWith<ReplSetHeartbeatResponse>());
            }
        }
    }
    _scheduleNextLivenessUpdate_inlock(cbData);
}

void ReplicationCoordinatorImpl::_scheduleNextLivenessUpdate(
    const ReplicationExecutor::CallbackArgs& cbData) {
    if (cbData.status == ErrorCodes::CallbackCanceled)
        return;

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _scheduleNextLivenessUpdate_inlock(cbData);
}

void ReplicationCoordinatorImpl::_scheduleNextLivenessUpdate_inlock(
    const ReplicationExecutor::CallbackArgs& cbData) {
    if (!isV1ElectionProtocol()) {
        return;
    }
    // Scan liveness table for earliest date; schedule a run at (that date plus election timeout).
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

    auto nextTimeout = earliestDate + _rsConfig.getElectionTimeoutPeriod() +
        _topCoord->getTimeoutDelayForMember(earliestMemberId);
    if (nextTimeout > _replExecutor.now()) {
        LOG(3) << "scheduling next check at " << nextTimeout;
        auto cbh = _replExecutor.scheduleWorkAt(
            nextTimeout,
            stdx::bind(
                &ReplicationCoordinatorImpl::_handleLivenessTimeout, this, stdx::placeholders::_1));
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return;
        }
        fassert(22002, cbh.getStatus());
        _handleLivenessTimeoutCbh = cbh.getValue();
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
    _replExecutor.scheduleWork(stdx::bind(
        &ReplicationCoordinatorImpl::_scheduleNextLivenessUpdate, this, stdx::placeholders::_1));
}

void ReplicationCoordinatorImpl::_priorityTakeover(
    const ReplicationExecutor::CallbackArgs& cbData) {
    if (!cbData.status.isOK()) {
        return;
    }
    if (!isV1ElectionProtocol()) {
        return;
    }
    if (!_topCoord->stagePriorityTakeoverIfElectable(_replExecutor.now(), getMyLastOptime())) {
        return;
    }
    _startElectSelfV1();
}

}  // namespace repl
}  // namespace mongo

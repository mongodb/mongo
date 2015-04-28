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
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replica_set_config_checks.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

namespace {
    typedef StatusWith<ReplicationExecutor::CallbackHandle> CBHStatus;
    typedef ReplicationExecutor::RemoteCommandRequest CmdRequest;
    typedef ReplicationExecutor::CallbackHandle CBHandle;

}  //namespace

    void ReplicationCoordinatorImpl::_doMemberHeartbeat(ReplicationExecutor::CallbackData cbData,
                                                        const HostAndPort& target,
                                                        int targetIndex) {

        _untrackHeartbeatHandle(cbData.myHandle);
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }

        const Date_t now = _replExecutor.now();
        const std::pair<ReplSetHeartbeatArgs, Milliseconds> hbRequest =
            _topCoord->prepareHeartbeatRequest(
                    now,
                    _settings.ourSetName(),
                    target);

        const CmdRequest request(target, "admin", hbRequest.first.toBSON(), hbRequest.second);
        const ReplicationExecutor::RemoteCommandCallbackFn callback = stdx::bind(
                &ReplicationCoordinatorImpl::_handleHeartbeatResponse,
                this,
                stdx::placeholders::_1,
                targetIndex);

        _trackHeartbeatHandle(_replExecutor.scheduleRemoteCommand(request, callback));
    }

    void ReplicationCoordinatorImpl::_scheduleHeartbeatToTarget(
            const HostAndPort& target,
            int targetIndex,
            Date_t when) {

        LOG(2) << "Scheduling heartbeat to " << target << " at " << dateToISOStringUTC(when);
        _trackHeartbeatHandle(
                _replExecutor.scheduleWorkAt(
                        when,
                        stdx::bind(&ReplicationCoordinatorImpl::_doMemberHeartbeat,
                                   this,
                                   stdx::placeholders::_1,
                                   target,
                                   targetIndex)));
    }

    void ReplicationCoordinatorImpl::_handleHeartbeatResponse(
            const ReplicationExecutor::RemoteCommandCallbackData& cbData, int targetIndex) {

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
            responseStatus = hbResponse.initialize(resp);
        }
        const bool isUnauthorized = (responseStatus.code() == ErrorCodes::Unauthorized) ||
                                    (responseStatus.code() == ErrorCodes::AuthenticationFailed);
        const Date_t now = _replExecutor.now();
        const Timestamp lastApplied = getMyLastOptime();  // Locks and unlocks _mutex.
        Milliseconds networkTime(0);
        StatusWith<ReplSetHeartbeatResponse> hbStatusResponse(hbResponse);

        if (responseStatus.isOK()) {
            networkTime = cbData.response.getValue().elapsedMillis;
        }
        else {
            log() << "Error in heartbeat request to " << target << "; " << responseStatus;
            if (!resp.isEmpty()) {
                LOG(3) << "heartbeat response: " << resp;
            }

            if (isUnauthorized) {
                networkTime = cbData.response.getValue().elapsedMillis;
            }
            hbStatusResponse = StatusWith<ReplSetHeartbeatResponse>(responseStatus);
        }

        HeartbeatResponseAction action =
            _topCoord->processHeartbeatResponse(
                    now,
                    networkTime,
                    target,
                    hbStatusResponse,
                    lastApplied);

        if (action.getAction() == HeartbeatResponseAction::NoAction &&
                hbStatusResponse.isOK() &&
                hbStatusResponse.getValue().hasOpTime() &&
                targetIndex >= 0 &&
                hbStatusResponse.getValue().hasState() &&
                hbStatusResponse.getValue().getState() != MemberState::RS_PRIMARY) {
            boost::unique_lock<boost::mutex> lk(_mutex);
            if (hbStatusResponse.getValue().getVersion() == _rsConfig.getConfigVersion()) {
                _updateOpTimeFromHeartbeat_inlock(targetIndex,
                                                  hbStatusResponse.getValue().getOpTime());
                // TODO: Enable with Data Replicator
                //lk.unlock();
                //_dr.slavesHaveProgressed();
            }
        }

        _signalStepDownWaiters();

        _scheduleHeartbeatToTarget(
                target,
                targetIndex,
                std::max(now, action.getNextHeartbeatStartDate()));

        _handleHeartbeatResponseAction(action, hbStatusResponse);
    }

    void ReplicationCoordinatorImpl::_updateOpTimeFromHeartbeat_inlock(int targetIndex,
                                                                       Timestamp optime) {
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
                boost::unique_lock<boost::mutex> lk(_mutex);
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
            _heartbeatStepDownStart();
            break;
        case HeartbeatResponseAction::StepDownRemotePrimary: {
            invariant(action.getPrimaryConfigIndex() != _selfIndex);
            _requestRemotePrimaryStepdown(
                    _rsConfig.getMemberAt(action.getPrimaryConfigIndex()).getHostAndPort());
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
    void remoteStepdownCallback(const ReplicationExecutor::RemoteCommandCallbackData& cbData) {

        const Status status = cbData.response.getStatus();
        if (status == ErrorCodes::CallbackCanceled) {
            return;
        }

        if (status.isOK()) {
            LOG(1) << "stepdown of primary(" << cbData.request.target
                   << ") succeeded with response -- "
                   << cbData.response.getValue().data;
        }
        else {
            warning() << "stepdown of primary(" << cbData.request.target
                      << ") failed due to " << cbData.response.getStatus();
        }
    }
}  // namespace

    void ReplicationCoordinatorImpl::_requestRemotePrimaryStepdown(const HostAndPort& target) {
        CmdRequest request(target, "admin", BSON("replSetStepDown" << 1));

        log() << "Requesting " << target << " step down from primary";
        CBHStatus cbh = _replExecutor.scheduleRemoteCommand(
                request, remoteStepdownCallback);
        if (cbh.getStatus() != ErrorCodes::ShutdownInProgress) {
            fassert(18808, cbh.getStatus());
        }
    }

    void ReplicationCoordinatorImpl::_heartbeatStepDownStart() {
        log() << "Stepping down from primary in response to heartbeat";
        _replExecutor.scheduleWorkWithGlobalExclusiveLock(
                stdx::bind(&ReplicationCoordinatorImpl::_heartbeatStepDownFinish,
                           this,
                           stdx::placeholders::_1));
    }

    void ReplicationCoordinatorImpl::_heartbeatStepDownFinish(
            const ReplicationExecutor::CallbackData& cbData) {

        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        invariant(cbData.txn);
        // TODO Add invariant that we've got global shared or global exclusive lock, when supported
        // by lock manager.
        boost::unique_lock<boost::mutex> lk(_mutex);
        _topCoord->stepDownIfPending();
        const PostMemberStateUpdateAction action =
            _updateMemberStateFromTopologyCoordinator_inlock();
        lk.unlock();
        _performPostMemberStateUpdateAction(action);
    }

    void ReplicationCoordinatorImpl::_scheduleHeartbeatReconfig(const ReplicaSetConfig& newConfig) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        if (_inShutdown) {
            return;
        }

        switch (_rsConfigState) {
        case kConfigStartingUp:
            LOG(1) << "Ignoring new configuration with version " << newConfig.getConfigVersion() <<
                " because still attempting to load local configuration information";
            return;
        case kConfigUninitialized:
        case kConfigSteady:
            LOG(1) << "Received new config via heartbeat with version " <<
                newConfig.getConfigVersion();
            break;
        case kConfigInitiating:
        case kConfigReconfiguring:
        case kConfigHBReconfiguring:
            LOG(1) << "Ignoring new configuration with version " << newConfig.getConfigVersion() <<
                " because already in the midst of a configuration process";
            return;
        default:
            severe() << "Reconfiguration request occurred while _rsConfigState == " <<
                int(_rsConfigState) << "; aborting.";
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
        _replExecutor.scheduleDBWork(stdx::bind(
            &ReplicationCoordinatorImpl::_heartbeatReconfigStore,
            this,
            stdx::placeholders::_1,
            newConfig));
    }

    void ReplicationCoordinatorImpl::_heartbeatReconfigAfterElectionCanceled(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& newConfig) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        fassert(18911, cbData.status);
        boost::lock_guard<boost::mutex> lk(_mutex);
        if (_inShutdown) {
            return;
        }

        _replExecutor.scheduleDBWork(stdx::bind(
            &ReplicationCoordinatorImpl::_heartbeatReconfigStore,
            this,
            stdx::placeholders::_1,
            newConfig));
    }

    void ReplicationCoordinatorImpl::_heartbeatReconfigStore(
        const ReplicationExecutor::CallbackData& cbd,
        const ReplicaSetConfig& newConfig) {

        if (cbd.status.code() == ErrorCodes::CallbackCanceled) {
            log() << "The callback to persist the replica set configuration was canceled - "
                  << "the configuration was not persisted but was used: " << newConfig.toBSON();
            return;
        }

        boost::unique_lock<boost::mutex> lk(_mutex, boost::defer_lock_t());

        const StatusWith<int> myIndex = validateConfigForHeartbeatReconfig(
                _externalState.get(),
                newConfig);

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
                    "it is invalid: "<< myIndex.getStatus();
        }
        else {
            Status status = _externalState->storeLocalConfigDocument(cbd.txn, newConfig.toBSON());

            lk.lock();
            if (!status.isOK()) {
                error() << "Ignoring new configuration in heartbeat response because we failed to"
                    " write it to stable storage; " << status;
                invariant(_rsConfigState == kConfigHBReconfiguring);
                if (_rsConfig.isInitialized()) {
                    _setConfigState_inlock(kConfigSteady);
                }
                else {
                    _setConfigState_inlock(kConfigUninitialized);
                }
                return;
            }

            lk.unlock();

            _externalState->startThreads();
        }

        const stdx::function<void (const ReplicationExecutor::CallbackData&)> reconfigFinishFn(
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
        }
        else {
            _replExecutor.scheduleWork(reconfigFinishFn);
        }
    }

    void ReplicationCoordinatorImpl::_heartbeatReconfigFinish(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& newConfig,
            StatusWith<int> myIndex) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }

        boost::unique_lock<boost::mutex> lk(_mutex);
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
                log() << "Cannot find self in new replica set configuration; I must be removed; " <<
                    myIndex.getStatus();
                break;
            case ErrorCodes::DuplicateKey:
                error() << "Several entries in new config represent this node; "
                    "Removing self until an acceptable configuration arrives; " <<
                    myIndex.getStatus();
                break;
            default:
                error() << "Could not validate configuration received from remote node; "
                    "Removing self until an acceptable configuration arrives; " <<
                    myIndex.getStatus();
                break;
            }
            myIndex = StatusWith<int>(-1);
        }
        const PostMemberStateUpdateAction action =
            _setCurrentRSConfig_inlock(newConfig, myIndex.getValue());
        lk.unlock();
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
        const HeartbeatHandles::iterator newEnd = std::remove(
                _heartbeatHandles.begin(),
                _heartbeatHandles.end(),
                handle);
        invariant(newEnd != _heartbeatHandles.end());
        _heartbeatHandles.erase(newEnd, _heartbeatHandles.end());
    }

    void ReplicationCoordinatorImpl::_cancelHeartbeats() {
        std::for_each(_heartbeatHandles.begin(),
                      _heartbeatHandles.end(),
                      stdx::bind(&ReplicationExecutor::cancel,
                                 &_replExecutor,
                                 stdx::placeholders::_1));
        // Heartbeat callbacks will remove themselves from _heartbeatHandles when they execute with
        // CallbackCanceled status, so it's better to leave the handles in the list, for now.
    }

    void ReplicationCoordinatorImpl::_startHeartbeats() {
        const Date_t now = _replExecutor.now();
        _seedList.clear();
        for (int i = 0; i < _rsConfig.getNumMembers(); ++i) {
            if (i == _selfIndex) {
                continue;
            }
            _scheduleHeartbeatToTarget(_rsConfig.getMemberAt(i).getHostAndPort(), i, now);
        }
    }

} // namespace repl
} // namespace mongo

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
#include "mongo/db/get_status_from_command_result.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replica_set_config_checks.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/topology_coordinator.h"
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

    MONGO_FP_DECLARE(rsHeartbeatRequestNoopByMember);

    void ReplicationCoordinatorImpl::_doMemberHeartbeat(ReplicationExecutor::CallbackData cbData,
                                                        const HostAndPort& target) {

        _untrackHeartbeatHandle(cbData.myHandle);
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }

        // Are we blind, or do we have a failpoint setup to ignore this member?
        bool dontHeartbeatMember = false; // TODO: replSetBlind should be here as the default

        MONGO_FAIL_POINT_BLOCK(rsHeartbeatRequestNoopByMember, member) {
            const StringData& stopMember = member.getData()["member"].valueStringData();
            HostAndPort ignoreHAP;
            Status status = ignoreHAP.initialize(stopMember);
            // Ignore
            if (status.isOK()) {
                if (target == ignoreHAP) {
                    dontHeartbeatMember = true;
                }
            }
            else {
                log() << "replset: Bad member for rsHeartbeatRequestNoopByMember failpoint "
                       <<  member.getData() << ". 'member' failed to parse into HostAndPort -- "
                       << status;
            }
        }

        const Date_t now = _replExecutor.now();
        const std::pair<ReplSetHeartbeatArgs, Milliseconds> hbRequest =
            _topCoord->prepareHeartbeatRequest(
                    now,
                    _settings.ourSetName(),
                    target);
        if (dontHeartbeatMember) {
            // Don't issue real heartbeats, just call start again after the timeout.
            const StatusWith<ReplSetHeartbeatResponse> responseStatus(
                    ErrorCodes::UnknownError,
                    str::stream() << "Failure forced for heartbeat to " <<
                    target.toString() << " due to failpoint.");
            const HeartbeatResponseAction action =
                _topCoord->processHeartbeatResponse(
                        now,
                        Milliseconds(0),
                        target,
                        responseStatus,
                        _getLastOpApplied());
            _scheduleHeartbeatToTarget(
                    target,
                    action.getNextHeartbeatStartDate());
            _handleHeartbeatResponseAction(action, responseStatus);
            return;
        }

        const CmdRequest request(target, "admin", hbRequest.first.toBSON(), hbRequest.second);
        const ReplicationExecutor::RemoteCommandCallbackFn callback = stdx::bind(
                &ReplicationCoordinatorImpl::_handleHeartbeatResponse,
                this,
                stdx::placeholders::_1);

        _trackHeartbeatHandle(_replExecutor.scheduleRemoteCommand(request, callback));
    }

    void ReplicationCoordinatorImpl::_scheduleHeartbeatToTarget(
            const HostAndPort& host,
            Date_t when) {

        LOG(2) << "Scheduling heartbeat to " << host << " at " << dateToISOStringUTC(when);
        _trackHeartbeatHandle(
                _replExecutor.scheduleWorkAt(
                        when,
                        stdx::bind(&ReplicationCoordinatorImpl::_doMemberHeartbeat,
                                   this,
                                   stdx::placeholders::_1,
                                   host)));
    }

    void ReplicationCoordinatorImpl::_handleHeartbeatResponse(
            const ReplicationExecutor::RemoteCommandCallbackData& cbData) {

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
            responseStatus = getStatusFromCommandResult(resp);
        }
        if (responseStatus.isOK()) {
            responseStatus = hbResponse.initialize(resp);
        }
        if (!responseStatus.isOK()) {
            LOG(1) << "Error in heartbeat request to " << target << ";" << responseStatus;
            if (!resp.isEmpty()) {
                LOG(3) << "heartbeat response: " << resp;
            }
        }
        const Date_t now = _replExecutor.now();
        const OpTime lastApplied = _getLastOpApplied();  // Locks and unlocks _mutex.
        Milliseconds networkTime(0);
        StatusWith<ReplSetHeartbeatResponse> hbStatusResponse(hbResponse);
        if (cbData.response.isOK()) {
            networkTime = cbData.response.getValue().elapsedMillis;
        }
        else {
            hbStatusResponse = StatusWith<ReplSetHeartbeatResponse>(responseStatus);
        }
        HeartbeatResponseAction action =
            _topCoord->processHeartbeatResponse(
                    now,
                    networkTime,
                    target,
                    hbStatusResponse,
                    lastApplied);

        _scheduleHeartbeatToTarget(
                target,
                std::max(now, action.getNextHeartbeatStartDate()));

        _handleHeartbeatResponseAction(action, hbStatusResponse);
    }

    void ReplicationCoordinatorImpl::_handleHeartbeatResponseAction(
            const HeartbeatResponseAction& action,
            const StatusWith<ReplSetHeartbeatResponse>& responseStatus) {

        switch (action.getAction()) {
        case HeartbeatResponseAction::NoAction:
            break;
        case HeartbeatResponseAction::Reconfig:
            invariant(responseStatus.isOK());
            _scheduleHeartbeatReconfig(responseStatus.getValue().getConfig());
            break;
        case HeartbeatResponseAction::StartElection:
            _startElectSelf();
            break;
        case HeartbeatResponseAction::StepDownSelf:
            invariant(action.getPrimaryConfigIndex() == _thisMembersConfigIndex);
            _heartbeatStepDownStart();
            break;
        case HeartbeatResponseAction::StepDownRemotePrimary: {
            invariant(action.getPrimaryConfigIndex() != _thisMembersConfigIndex);
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
            LOG(1) << "replset: stepdown of primary(" << cbData.request.target
                   << ") succeeded with response -- "
                   << cbData.response.getValue().data;
        }
        else {
            warning() << "replset: stepdown of primary(" << cbData.request.target
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
        log() << "Stepping down from primary in repsonse to heartbeat";
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
        _updateCurrentMemberStateFromTopologyCoordinator_inlock();
        lk.unlock();
        _externalState->closeConnections();
    }

    void ReplicationCoordinatorImpl::_scheduleHeartbeatReconfig(const ReplicaSetConfig& newConfig) {
        boost::lock_guard<boost::mutex> lk(_mutex);
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
        boost::thread(stdx::bind(&ReplicationCoordinatorImpl::_heartbeatReconfigStore,
                                 this,
                                 newConfig)).detach();
    }

    void ReplicationCoordinatorImpl::_heartbeatReconfigAfterElectionCanceled(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& newConfig) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            return;
        }
        fassert(18911, cbData.status);
        boost::thread(stdx::bind(&ReplicationCoordinatorImpl::_heartbeatReconfigStore,
                                 this,
                                 newConfig)).detach();
    }

    void ReplicationCoordinatorImpl::_heartbeatReconfigStore(const ReplicaSetConfig& newConfig) {
        {
            boost::scoped_ptr<OperationContext> txn(_externalState->createOperationContext());
            Status status = _externalState->storeLocalConfigDocument(txn.get(), newConfig.toBSON());
            if (!status.isOK()) {
                error() << "Ignoring new configuration in heartbeat response because we failed to"
                    " write it to stable storage; " << status;
                boost::lock_guard<boost::mutex> lk(_mutex);
                invariant(_rsConfigState == kConfigHBReconfiguring);
                if (_rsConfig.isInitialized()) {
                    _setConfigState_inlock(kConfigSteady);
                }
                else {
                    // This is the _only_ case where we can return to kConfigUninitialized from
                    // kConfigHBReconfiguring.
                    _setConfigState_inlock(kConfigUninitialized);
                }
                return;
            }
        }
        const StatusWith<int> myIndex = validateConfigForHeartbeatReconfig(
                _externalState.get(),
                newConfig);
        _replExecutor.scheduleWork(stdx::bind(&ReplicationCoordinatorImpl::_heartbeatReconfigFinish,
                                              this,
                                              stdx::placeholders::_1,
                                              newConfig,
                                              myIndex));
    }

    void ReplicationCoordinatorImpl::_heartbeatReconfigFinish(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicaSetConfig& newConfig,
            StatusWith<int> myIndex) {

        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(_rsConfigState == kConfigHBReconfiguring);
        invariant(!_rsConfig.isInitialized() ||
                  _rsConfig.getConfigVersion() < newConfig.getConfigVersion());
        if (!myIndex.isOK()) {
            switch (myIndex.getStatus().code()) {
            case ErrorCodes::NoSuchKey:
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
        _setCurrentRSConfig_inlock(newConfig, myIndex.getValue());
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
            if (i == _thisMembersConfigIndex) {
                continue;
            }
            _scheduleHeartbeatToTarget(_rsConfig.getMemberAt(i).getHostAndPort(), now);
        }
    }

} // namespace repl
} // namespace mongo

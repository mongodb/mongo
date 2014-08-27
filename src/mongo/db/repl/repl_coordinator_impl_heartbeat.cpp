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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_coordinator_impl.h"

#include <algorithm>
#include <boost/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

    namespace {
        typedef StatusWith<ReplicationExecutor::CallbackHandle> CBHStatus;
    } //namespace

    MONGO_FP_DECLARE(rsHeartbeatRequestNoopByMember);

    namespace {
        // decide where these live, see TopologyCoordinator::HeartbeatOptions
        const int heartbeatFrequencyMillis = 2 * 1000; // 2 seconds
        const int heartbeatTimeoutDefaultMillis = 10 * 1000; // 10 seconds
        const int heartbeatRetries = 2;
    } //namespace


    void ReplicationCoordinatorImpl::doMemberHeartbeat(ReplicationExecutor::CallbackData cbData,
                                                       const HostAndPort& hap) {

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
                if (hap == ignoreHAP) {
                    dontHeartbeatMember = true;
                }
            }
            else {
                log() << "replset: Bad member for rsHeartbeatRequestNoopByMember failpoint "
                       <<  member.getData() << ". 'member' failed to parse into HostAndPort -- "
                       << status;
            }
        }

        if (dontHeartbeatMember) {
            // Don't issue real heartbeats, just call start again after the timeout.
            ReplicationExecutor::CallbackFn restartCB = stdx::bind(
                                                &ReplicationCoordinatorImpl::doMemberHeartbeat,
                                                this,
                                                stdx::placeholders::_1,
                                                hap);
            CBHStatus status = _replExecutor.scheduleWorkAt(
                    Date_t(_replExecutor.now() + heartbeatFrequencyMillis),
                    restartCB);
            if (!status.isOK()) {
                log() << "replset: aborting heartbeats for " << hap << " due to scheduling error"
                       << " -- "<< status;
                return;
             }
            _trackHeartbeatHandle(status.getValue());
            return;
        }

        // Compose heartbeat command message
        BSONObj hbCommandBSON;
        {
            // take lock to build request
            boost::lock_guard<boost::mutex> lock(_mutex);
            BSONObjBuilder cmdBuilder;
            const MemberConfig me = _rsConfig.getMemberAt(_thisMembersConfigIndex);
            cmdBuilder.append("replSetHeartbeat", _rsConfig.getReplSetName());
            cmdBuilder.append("v", _rsConfig.getConfigVersion());
            cmdBuilder.append("pv", 1);
            cmdBuilder.append("checkEmpty", false);
            cmdBuilder.append("from", me.getHostAndPort().toString());
            cmdBuilder.append("fromId", me.getId());
            hbCommandBSON = cmdBuilder.done();
        }
        const ReplicationExecutor::RemoteCommandRequest request(hap, "admin", hbCommandBSON);

        ReplicationExecutor::RemoteCommandCallbackFn callback = stdx::bind(
                                       &ReplicationCoordinatorImpl::_handleHeartbeatResponse,
                                       this,
                                       stdx::placeholders::_1,
                                       hap,
                                       _replExecutor.now(),
                                       heartbeatRetries);


        CBHStatus status = _replExecutor.scheduleRemoteCommand(request, callback);
        if (!status.isOK()) {
            log() << "replset: aborting heartbeats for " << hap << " due to scheduling error"
                   << status;
            return;
         }
        _trackHeartbeatHandle(status.getValue());
    }

    void ReplicationCoordinatorImpl::_handleHeartbeatResponse(
                                const ReplicationExecutor::RemoteCommandCallbackData& cbData,
                                const HostAndPort& hap,
                                Date_t firstCallDate,
                                int retriesLeft) {
        // TODO
    }

    void ReplicationCoordinatorImpl::_trackHeartbeatHandle(
                                            const ReplicationExecutor::CallbackHandle& handle) {
        // this mutex should not be needed because it is always used during a callback.
        // boost::mutex::scoped_lock lock(_mutex);
        _heartbeatHandles.push_back(handle);
    }

    void ReplicationCoordinatorImpl::_untrackHeartbeatHandle(
                                            const ReplicationExecutor::CallbackHandle& handle) {
        // this mutex should not be needed because it is always used during a callback.
        // boost::mutex::scoped_lock lock(_mutex);
        HeartbeatHandles::iterator it = std::find(_heartbeatHandles.begin(),
                                                  _heartbeatHandles.end(),
                                                  handle);
        invariant(it != _heartbeatHandles.end());

        _heartbeatHandles.erase(it);

    }

    void ReplicationCoordinatorImpl::cancelHeartbeats() {
        // this mutex should not be needed because it is always used during a callback.
        //boost::mutex::scoped_lock lock(_mutex);
        HeartbeatHandles::const_iterator it = _heartbeatHandles.begin();
        const HeartbeatHandles::const_iterator end = _heartbeatHandles.end();
        for( ; it != end; ++it ) {
            _replExecutor.cancel(*it);
        }

        _heartbeatHandles.clear();
    }

    void ReplicationCoordinatorImpl::_startHeartbeats() {
        ReplicaSetConfig::MemberIterator it = _rsConfig.membersBegin();
        ReplicaSetConfig::MemberIterator end = _rsConfig.membersBegin();

        for(;it != end; it++) {
            HostAndPort host = it->getHostAndPort();
            CBHStatus status = _replExecutor.scheduleWork(
                                    stdx::bind(
                                            &ReplicationCoordinatorImpl::doMemberHeartbeat,
                                            this,
                                            stdx::placeholders::_1,
                                            host));
            if (!status.isOK()) {
                log() << "replset: cannot start heartbeats for "
                      << host << " due to scheduling error -- "<< status;
                continue;
             }
            _trackHeartbeatHandle(status.getValue());
        }
    }

} // namespace repl
} // namespace mongo

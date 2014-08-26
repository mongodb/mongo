/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/db/repl/freshness_checker.h"

#include "mongo/base/status.h"
#include "mongo/bson/optime.h"
#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

    FreshnessChecker::FreshnessChecker() : _actualResponses(0), 
                                           _freshest(true), 
                                           _tied(false), 
                                           _originalConfigVersion(0) {
    }


    Status FreshnessChecker::start(
        ReplicationExecutor* executor,
        const ReplicationExecutor::EventHandle& evh,
        const OpTime& lastOpTimeApplied,
        const ReplicaSetConfig& currentConfig,
        int selfIndex,
        const std::vector<HostAndPort>& hosts) {

        _lastOpTimeApplied = lastOpTimeApplied;
        _freshest = true;
        _originalConfigVersion = currentConfig.getConfigVersion();
        _sufficientResponsesReceived = evh;

        // gather all not-down nodes, get their fullnames(or hostandport's)
        // schedule fresh command for each node
        BSONObj replSetFreshCmd = BSON("replSetFresh" << 1 <<
                                       "set" << currentConfig.getReplSetName() <<
                                       "opTime" << Date_t(lastOpTimeApplied.asDate()) <<
                                       "who" << currentConfig.getMemberAt(selfIndex)
                                       .getHostAndPort().toString() <<
                                       "cfgver" << currentConfig.getConfigVersion() <<
                                       "id" << currentConfig.getMemberAt(selfIndex).getId());
        for (std::vector<HostAndPort>::const_iterator it = hosts.begin(); it != hosts.end(); ++it) {
            const StatusWith<ReplicationExecutor::CallbackHandle> cbh =
                executor->scheduleRemoteCommand(
                    ReplicationExecutor::RemoteCommandRequest(
                        *it,
                        "admin",
                        replSetFreshCmd,
                        Milliseconds(30*1000)),   // trying to match current Socket timeout
                    stdx::bind(&FreshnessChecker::_onReplSetFreshResponse,
                               this,
                               stdx::placeholders::_1));
            if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
                return cbh.getStatus();
            }
            fassert(18682, cbh.getStatus());

            _responseCallbacks.push_back(cbh.getValue());
        }
        
        if (_responseCallbacks.size() == 0) {
            _signalSufficientResponsesReceived(executor);
        }

        return Status::OK();
    }

    void FreshnessChecker::_onReplSetFreshResponse(
        const ReplicationExecutor::RemoteCommandCallbackData& cbData) {
        ++_actualResponses;

        if (cbData.response.getStatus() == ErrorCodes::CallbackCanceled) {
            return;
        }

        if (!cbData.response.isOK()) {
            // command failed, so nothing further to do.
            if (_actualResponses == _responseCallbacks.size()) {
                _signalSufficientResponsesReceived(cbData.executor);
            }
            return;
        }

        ScopeGuard sufficientResponsesReceivedCaller =
            MakeObjGuard(*this,
                         &FreshnessChecker::_signalSufficientResponsesReceived,
                         cbData.executor);

        BSONObj res = cbData.response.getValue().data;

        if (res["fresher"].trueValue()) {
            log() << "not electing self, we are not freshest";
            _freshest = false;
            return;
        }
        
        if (res["opTime"].type() != mongo::Date) {
            error() << "wrong type for opTime argument in replSetFresh response: " << 
                typeName(res["opTime"].type());
            _freshest = false;
            if (_actualResponses != _responseCallbacks.size()) {
                // More responses are still pending.
                sufficientResponsesReceivedCaller.Dismiss();
            }
            return;
        }
        OpTime remoteTime(res["opTime"].date());
        if (remoteTime == _lastOpTimeApplied) {
            _tied = true;
        }
        if (remoteTime > _lastOpTimeApplied) {
            // something really wrong (rogue command?)
            _freshest = false;
            return;
        }
        
        if (res["veto"].trueValue()) {
            BSONElement msg = res["errmsg"];
            if (!msg.eoo()) {
                log() << "not electing self, " << cbData.request.target.toString() << 
                    " would veto with '" << msg << "'";
            }
            else {
                log() << "not electing self, " << cbData.request.target.toString() << 
                    " would veto";
            }
            _freshest = false;
            return;
        }

        if (_actualResponses != _responseCallbacks.size()) {
            // More responses are still pending.
            sufficientResponsesReceivedCaller.Dismiss();
        }
    }

    void FreshnessChecker::_signalSufficientResponsesReceived(ReplicationExecutor* executor) {
        if (_sufficientResponsesReceived.isValid()) {

            // Cancel all the command callbacks, 
            // so that they do not attempt to access FreshnessChecker
            // state after this callback completes.
            std::for_each(_responseCallbacks.begin(),
                          _responseCallbacks.end(),
                          stdx::bind(&ReplicationExecutor::cancel,
                                     executor,
                                     stdx::placeholders::_1));

            executor->signalEvent(_sufficientResponsesReceived);
            _sufficientResponsesReceived = ReplicationExecutor::EventHandle();
   
        }
    }

    void FreshnessChecker::getResults(bool* freshest, bool* tied) const {
        *freshest = _freshest;
        *tied = _tied;
    }

    long long FreshnessChecker::getOriginalConfigVersion() const {
        return _originalConfigVersion;
    }


} // namespace repl
} // namespace mongo

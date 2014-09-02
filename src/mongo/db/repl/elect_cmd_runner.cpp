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

#include "mongo/db/repl/elect_cmd_runner.h"

#include "mongo/base/status.h"
#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

    ElectCmdRunner::ElectCmdRunner() : _receivedVotes(0), 
                                       _actualResponses(0) {
    }

    Status ElectCmdRunner::start(
        ReplicationExecutor* executor,
        const ReplicationExecutor::EventHandle& evh,
        const ReplicaSetConfig& currentConfig,
        int selfIndex,
        const std::vector<HostAndPort>& hosts) {

        _sufficientResponsesReceived = evh;
        
        // We start with voting for ourselves, then request votes from other members.
        const MemberConfig& selfConfig = currentConfig.getMemberAt(selfIndex);
        _receivedVotes = selfConfig.getNumVotes();

        BSONObj replSetElectCmd = BSON("replSetElect" << 1 <<
                                       "set" << currentConfig.getReplSetName() <<
                                       "who" << selfConfig.getHostAndPort().toString() <<
                                       "whoid" << selfConfig.getId() <<
                                       "cfgver" << currentConfig.getConfigVersion() <<
                                       "round" << static_cast<long long>(executor->nextRandomInt64(
                                               std::numeric_limits<int64_t>::max())));

        // Schedule a RemoteCommandRequest for each non-DOWN node
        for (std::vector<HostAndPort>::const_iterator it = hosts.begin(); 
             it != hosts.end(); 
             ++it) {
            const StatusWith<ReplicationExecutor::CallbackHandle> cbh =
                executor->scheduleRemoteCommand(
                    ReplicationExecutor::RemoteCommandRequest(
                        *it,
                        "admin",
                        replSetElectCmd,
                        Milliseconds(30*1000)),   // trying to match current Socket timeout
                    stdx::bind(&ElectCmdRunner::_onReplSetElectResponse,
                               this,
                               stdx::placeholders::_1));
            if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
                return cbh.getStatus();
            }
            fassert(18683, cbh.getStatus());

            _responseCallbacks.push_back(cbh.getValue());
        }
        
        if (_responseCallbacks.size() == 0) {
            _signalSufficientResponsesReceived(executor);
        }

        return Status::OK();
    }

    void ElectCmdRunner::_onReplSetElectResponse(
        const ReplicationExecutor::RemoteCommandCallbackData& cbData) {
        ++_actualResponses;
        if (cbData.response.getStatus() == ErrorCodes::CallbackCanceled) {
            return;
        }

        if (cbData.response.isOK()) {
            BSONObj res = cbData.response.getValue().data;
            LOG(1) << "replSet elect res: " << res.toString();
            BSONElement vote(res["vote"]); 
            if (vote.type() != mongo::NumberInt) {
                error() << "wrong type for vote argument in replSetElect command: " << 
                    typeName(vote.type());
                _signalSufficientResponsesReceived(cbData.executor);
                return;
            }

            _receivedVotes += vote._numberInt();
        }
        else {
            warning() << "elect command to " << cbData.request.target.toString() << " failed: " <<
                cbData.response.getStatus();
        }
        if (_actualResponses == _responseCallbacks.size()) {
            _signalSufficientResponsesReceived(cbData.executor);
        }    
    }

    void ElectCmdRunner::_signalSufficientResponsesReceived(ReplicationExecutor* executor) {
        if (_sufficientResponsesReceived.isValid()) {

            // Cancel any remaining command callbacks.
            std::for_each(_responseCallbacks.begin(),
                          _responseCallbacks.end(),
                          stdx::bind(&ReplicationExecutor::cancel,
                                     executor,
                                     stdx::placeholders::_1));   

            executor->signalEvent(_sufficientResponsesReceived);
            _sufficientResponsesReceived = ReplicationExecutor::EventHandle();
        }
    }

    int ElectCmdRunner::getReceivedVotes() const {
        return _receivedVotes;
    }

} // namespace repl
} // namespace mongo

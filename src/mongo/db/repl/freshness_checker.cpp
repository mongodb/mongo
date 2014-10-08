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
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

    FreshnessChecker::Algorithm::Algorithm(
            OpTime lastOpTimeApplied,
            const ReplicaSetConfig& rsConfig,
            int selfIndex,
            const std::vector<HostAndPort>& targets) :
        _actualResponses(0),
        _freshest(true),
        _tied(false),
        _lastOpTimeApplied(lastOpTimeApplied),
        _rsConfig(rsConfig),
        _selfIndex(selfIndex),
        _targets(targets) {
    }

    FreshnessChecker::Algorithm::~Algorithm() {}

    std::vector<ReplicationExecutor::RemoteCommandRequest>
    FreshnessChecker::Algorithm::getRequests() const {

        const MemberConfig& selfConfig = _rsConfig.getMemberAt(_selfIndex);

        // gather all not-down nodes, get their fullnames(or hostandport's)
        // schedule fresh command for each node
        BSONObjBuilder freshCmdBuilder;
        freshCmdBuilder.append("replSetFresh", 1);
        freshCmdBuilder.append("set", _rsConfig.getReplSetName());
        freshCmdBuilder.append("opTime", Date_t(_lastOpTimeApplied.asDate()));
        freshCmdBuilder.append("who", selfConfig.getHostAndPort().toString());
        freshCmdBuilder.appendIntOrLL("cfgver", _rsConfig.getConfigVersion());
        freshCmdBuilder.append("id", selfConfig.getId());
        const BSONObj replSetFreshCmd = freshCmdBuilder.obj();

        std::vector<ReplicationExecutor::RemoteCommandRequest> requests;
        for (std::vector<HostAndPort>::const_iterator it = _targets.begin();
             it != _targets.end();
             ++it) {
            invariant(*it != selfConfig.getHostAndPort());
            requests.push_back(ReplicationExecutor::RemoteCommandRequest(
                        *it,
                        "admin",
                        replSetFreshCmd,
                        Milliseconds(30*1000)));   // trying to match current Socket timeout
        }

        return requests;
    }

    void FreshnessChecker::Algorithm::processResponse(
                    const ReplicationExecutor::RemoteCommandRequest& request,
                    const ResponseStatus& response) {
        ++_actualResponses;

        if (!response.isOK()) {
            // command failed, so nothing further to do.
            return;
        }

        const BSONObj res = response.getValue().data;

        if (res["fresher"].trueValue()) {
            log() << "not electing self, we are not freshest";
            _freshest = false;
            return;
        }

        if (res["opTime"].type() != mongo::Date) {
            error() << "wrong type for opTime argument in replSetFresh response: " <<
                typeName(res["opTime"].type());
            _freshest = false;
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
                log() << "not electing self, " << request.target.toString() <<
                    " would veto with '" << msg << "'";
            }
            else {
                log() << "not electing self, " << request.target.toString() <<
                    " would veto";
            }
            _freshest = false;
            return;
        }
    }

    bool FreshnessChecker::Algorithm::hasReceivedSufficientResponses() const {
        if (!_freshest) {
            return true;
        }
        if (_actualResponses == _targets.size()) {
            return true;
        }
        return false;
    }

    void FreshnessChecker::getResults(bool* freshest, bool* tied) const {
        *freshest = _algorithm->isFreshest();
        *tied = _algorithm->isTiedForFreshest();
    }

    long long FreshnessChecker::getOriginalConfigVersion() const {
        return _originalConfigVersion;
    }

    FreshnessChecker::FreshnessChecker() : _isCanceled(false) {}
    FreshnessChecker::~FreshnessChecker() {}

    StatusWith<ReplicationExecutor::EventHandle> FreshnessChecker::start(
            ReplicationExecutor* executor,
            const OpTime& lastOpTimeApplied,
            const ReplicaSetConfig& currentConfig,
            int selfIndex,
            const std::vector<HostAndPort>& targets,
            const stdx::function<void ()>& onCompletion) {

        _originalConfigVersion = currentConfig.getConfigVersion();
        _algorithm.reset(new Algorithm(lastOpTimeApplied, currentConfig, selfIndex, targets));
        _runner.reset(new ScatterGatherRunner(_algorithm.get()));
        return _runner->start(executor, onCompletion);
    }

    void FreshnessChecker::cancel(ReplicationExecutor* executor) {
        _isCanceled = true;
        _runner->cancel(executor);
    }

} // namespace repl
} // namespace mongo

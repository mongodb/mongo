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
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

using executor::RemoteCommandRequest;

FreshnessChecker::Algorithm::Algorithm(Timestamp lastOpTimeApplied,
                                       const ReplicaSetConfig& rsConfig,
                                       int selfIndex,
                                       const std::vector<HostAndPort>& targets)
    : _responsesProcessed(0),
      _failedVoterResponses(0),
      _lastOpTimeApplied(lastOpTimeApplied),
      _rsConfig(rsConfig),
      _selfIndex(selfIndex),
      _targets(targets),
      _votingTargets(0),
      _losableVoters(0),
      _myVote(0),
      _abortReason(None) {
    // Count voting targets (since the targets could be a subset of members).
    for (std::vector<HostAndPort>::const_iterator it = _targets.begin(); it != _targets.end();
         ++it) {
        const MemberConfig* member = _rsConfig.findMemberByHostAndPort(*it);
        if (member && member->isVoter())
            ++_votingTargets;
    }

    _myVote = _rsConfig.getMemberAt(_selfIndex).isVoter() ? 1 : 0;
    _losableVoters = std::max(0, ((_votingTargets + _myVote) - _rsConfig.getMajorityVoteCount()));
}

FreshnessChecker::Algorithm::~Algorithm() {}

std::vector<RemoteCommandRequest> FreshnessChecker::Algorithm::getRequests() const {
    const MemberConfig& selfConfig = _rsConfig.getMemberAt(_selfIndex);

    // gather all not-down nodes, get their fullnames(or hostandport's)
    // schedule fresh command for each node
    BSONObjBuilder freshCmdBuilder;
    freshCmdBuilder.append("replSetFresh", 1);
    freshCmdBuilder.append("set", _rsConfig.getReplSetName());
    freshCmdBuilder.append("opTime", Date_t::fromMillisSinceEpoch(_lastOpTimeApplied.asLL()));
    freshCmdBuilder.append("who", selfConfig.getHostAndPort().toString());
    freshCmdBuilder.appendIntOrLL("cfgver", _rsConfig.getConfigVersion());
    freshCmdBuilder.append("id", selfConfig.getId());
    const BSONObj replSetFreshCmd = freshCmdBuilder.obj();

    std::vector<RemoteCommandRequest> requests;
    for (std::vector<HostAndPort>::const_iterator it = _targets.begin(); it != _targets.end();
         ++it) {
        invariant(*it != selfConfig.getHostAndPort());
        requests.push_back(RemoteCommandRequest(
            *it,
            "admin",
            replSetFreshCmd,
            Milliseconds(30 * 1000)));  // trying to match current Socket timeout
    }

    return requests;
}

bool FreshnessChecker::Algorithm::hadTooManyFailedVoterResponses() const {
    const bool tooManyLostVoters = (_failedVoterResponses > _losableVoters);

    LOG(3) << "hadTooManyFailedVoterResponses(" << tooManyLostVoters
           << ") = " << _failedVoterResponses << " failed responses <"
           << " (" << _votingTargets << " total voters - " << _rsConfig.getMajorityVoteCount()
           << " majority voters - me (" << _myVote << ")) -- losableVotes: " << _losableVoters;
    return tooManyLostVoters;
}

bool FreshnessChecker::Algorithm::_isVotingMember(const HostAndPort hap) const {
    const MemberConfig* member = _rsConfig.findMemberByHostAndPort(hap);
    invariant(member);
    return member->isVoter();
}

void FreshnessChecker::Algorithm::processResponse(const RemoteCommandRequest& request,
                                                  const ResponseStatus& response) {
    ++_responsesProcessed;
    bool votingMember = _isVotingMember(request.target);

    Status status = Status::OK();

    if (!response.isOK() ||
        !((status = getStatusFromCommandResult(response.getValue().data)).isOK())) {
        if (votingMember) {
            ++_failedVoterResponses;
            if (hadTooManyFailedVoterResponses()) {
                _abortReason = QuorumUnreachable;
            }
        }
        if (!response.isOK()) {  // network/executor error
            LOG(2) << "FreshnessChecker: Got failed response from " << request.target;
        } else {  // command error, like unauth
            LOG(2) << "FreshnessChecker: Got error response from " << request.target << " :"
                   << status;
        }
        return;
    }

    const BSONObj res = response.getValue().data;

    LOG(2) << "FreshnessChecker: Got response from " << request.target << " of " << res;

    if (res["fresher"].trueValue()) {
        log() << "not electing self, we are not freshest";
        _abortReason = FresherNodeFound;
        return;
    }

    if (res["opTime"].type() != mongo::Date) {
        error() << "wrong type for opTime argument in replSetFresh response: "
                << typeName(res["opTime"].type());
        _abortReason = FresherNodeFound;
        return;
    }
    Timestamp remoteTime(res["opTime"].date());
    if (remoteTime == _lastOpTimeApplied) {
        _abortReason = FreshnessTie;
    }
    if (remoteTime > _lastOpTimeApplied) {
        // something really wrong (rogue command?)
        _abortReason = FresherNodeFound;
        return;
    }

    if (res["veto"].trueValue()) {
        BSONElement msg = res["errmsg"];
        if (msg.type() == String) {
            log() << "not electing self, " << request.target.toString() << " would veto with '"
                  << msg.String() << "'";
        } else {
            log() << "not electing self, " << request.target.toString() << " would veto";
        }
        _abortReason = FresherNodeFound;
        return;
    }
}

bool FreshnessChecker::Algorithm::hasReceivedSufficientResponses() const {
    return (_abortReason != None && _abortReason != FreshnessTie) ||
        (_responsesProcessed == static_cast<int>(_targets.size()));
}

FreshnessChecker::ElectionAbortReason FreshnessChecker::Algorithm::shouldAbortElection() const {
    return _abortReason;
}

FreshnessChecker::ElectionAbortReason FreshnessChecker::shouldAbortElection() const {
    return _algorithm->shouldAbortElection();
}

long long FreshnessChecker::getOriginalConfigVersion() const {
    return _originalConfigVersion;
}

FreshnessChecker::FreshnessChecker() : _isCanceled(false) {}
FreshnessChecker::~FreshnessChecker() {}

StatusWith<ReplicationExecutor::EventHandle> FreshnessChecker::start(
    ReplicationExecutor* executor,
    const Timestamp& lastOpTimeApplied,
    const ReplicaSetConfig& currentConfig,
    int selfIndex,
    const std::vector<HostAndPort>& targets) {
    _originalConfigVersion = currentConfig.getConfigVersion();
    _algorithm.reset(new Algorithm(lastOpTimeApplied, currentConfig, selfIndex, targets));
    _runner.reset(new ScatterGatherRunner(_algorithm.get(), executor));
    return _runner->start();
}

void FreshnessChecker::cancel() {
    _isCanceled = true;
    _runner->cancel();
}

}  // namespace repl
}  // namespace mongo

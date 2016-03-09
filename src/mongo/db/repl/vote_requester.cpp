/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/db/repl/vote_requester.h"

#include "mongo/base/status.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

using executor::RemoteCommandRequest;

VoteRequester::Algorithm::Algorithm(const ReplicaSetConfig& rsConfig,
                                    long long candidateIndex,
                                    long long term,
                                    bool dryRun,
                                    OpTime lastDurableOpTime)
    : _rsConfig(rsConfig),
      _candidateIndex(candidateIndex),
      _term(term),
      _dryRun(dryRun),
      _lastDurableOpTime(lastDurableOpTime) {
    // populate targets with all voting members that aren't this node
    long long index = 0;
    for (auto member = _rsConfig.membersBegin(); member != _rsConfig.membersEnd(); member++) {
        if (member->isVoter() && index != candidateIndex) {
            _targets.push_back(member->getHostAndPort());
        }
        index++;
    }
}

VoteRequester::Algorithm::~Algorithm() {}

std::vector<RemoteCommandRequest> VoteRequester::Algorithm::getRequests() const {
    BSONObjBuilder requestVotesCmdBuilder;
    requestVotesCmdBuilder.append("replSetRequestVotes", 1);
    requestVotesCmdBuilder.append("setName", _rsConfig.getReplSetName());
    requestVotesCmdBuilder.append("dryRun", _dryRun);
    requestVotesCmdBuilder.append("term", _term);
    requestVotesCmdBuilder.append("candidateIndex", _candidateIndex);
    requestVotesCmdBuilder.append("configVersion", _rsConfig.getConfigVersion());

    _lastDurableOpTime.append(&requestVotesCmdBuilder, "lastCommittedOp");

    const BSONObj requestVotesCmd = requestVotesCmdBuilder.obj();

    std::vector<RemoteCommandRequest> requests;
    for (const auto& target : _targets) {
        requests.push_back(RemoteCommandRequest(
            target, "admin", requestVotesCmd, _rsConfig.getElectionTimeoutPeriod()));
    }

    return requests;
}

void VoteRequester::Algorithm::processResponse(const RemoteCommandRequest& request,
                                               const ResponseStatus& response) {
    _responsesProcessed++;
    if (!response.isOK()) {  // failed response
        log() << "VoteRequester: Got failed response from " << request.target << ": "
              << response.getStatus();
    } else {
        _responders.insert(request.target);
        ReplSetRequestVotesResponse voteResponse;
        const auto status = voteResponse.initialize(response.getValue().data);
        if (!status.isOK()) {
            log() << "VoteRequester: Got error processing response with status: " << status
                  << ", resp:" << response.getValue().data;
        }

        if (voteResponse.getVoteGranted()) {
            LOG(3) << "VoteRequester: Got yes vote from " << request.target
                   << ", resp:" << response.getValue().data;
            _votes++;
        } else {
            log() << "VoteRequester: Got no vote from " << request.target
                  << " because: " << voteResponse.getReason()
                  << ", resp:" << response.getValue().data;
        }

        if (voteResponse.getTerm() > _term) {
            _staleTerm = true;
        }
    }
}

bool VoteRequester::Algorithm::hasReceivedSufficientResponses() const {
    return _staleTerm || _votes == _rsConfig.getMajorityVoteCount() ||
        _responsesProcessed == static_cast<int>(_targets.size());
}

VoteRequester::Result VoteRequester::Algorithm::getResult() const {
    if (_staleTerm) {
        return Result::kStaleTerm;
    } else if (_votes >= _rsConfig.getMajorityVoteCount()) {
        return Result::kSuccessfullyElected;
    } else {
        return Result::kInsufficientVotes;
    }
}

unordered_set<HostAndPort> VoteRequester::Algorithm::getResponders() const {
    return _responders;
}

VoteRequester::VoteRequester() : _isCanceled(false) {}
VoteRequester::~VoteRequester() {}

StatusWith<ReplicationExecutor::EventHandle> VoteRequester::start(ReplicationExecutor* executor,
                                                                  const ReplicaSetConfig& rsConfig,
                                                                  long long candidateIndex,
                                                                  long long term,
                                                                  bool dryRun,
                                                                  OpTime lastDurableOpTime) {
    _algorithm.reset(new Algorithm(rsConfig, candidateIndex, term, dryRun, lastDurableOpTime));
    _runner.reset(new ScatterGatherRunner(_algorithm.get(), executor));
    return _runner->start();
}

void VoteRequester::cancel() {
    _isCanceled = true;
    _runner->cancel();
}

VoteRequester::Result VoteRequester::getResult() const {
    return _algorithm->getResult();
}

unordered_set<HostAndPort> VoteRequester::getResponders() const {
    return _algorithm->getResponders();
}

}  // namespace repl
}  // namespace mongo

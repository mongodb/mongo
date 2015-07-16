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
                                    long long candidateId,
                                    long long term,
                                    bool dryRun,
                                    OpTime lastOplogEntry)
    : _rsConfig(rsConfig),
      _candidateId(candidateId),
      _term(term),
      _dryRun(dryRun),
      _lastOplogEntry(lastOplogEntry) {
    // populate targets with all voting members that aren't this node
    for (auto member = _rsConfig.membersBegin(); member != _rsConfig.membersEnd(); member++) {
        if (member->isVoter() && member->getId() != candidateId) {
            _targets.push_back(member->getHostAndPort());
        }
    }
}

VoteRequester::Algorithm::~Algorithm() {}

std::vector<RemoteCommandRequest> VoteRequester::Algorithm::getRequests() const {
    BSONObjBuilder requestVotesCmdBuilder;
    requestVotesCmdBuilder.append("replSetRequestVotes", 1);
    requestVotesCmdBuilder.append("setName", _rsConfig.getReplSetName());
    requestVotesCmdBuilder.append("dryRun", _dryRun);
    requestVotesCmdBuilder.append("term", _term);
    requestVotesCmdBuilder.append("candidateId", _candidateId);
    requestVotesCmdBuilder.append("configVersion", _rsConfig.getConfigVersion());

    BSONObjBuilder lastCommittedOp(requestVotesCmdBuilder.subobjStart("lastCommittedOp"));
    lastCommittedOp.append("ts", _lastOplogEntry.getTimestamp());
    lastCommittedOp.append("term", _lastOplogEntry.getTerm());
    lastCommittedOp.done();

    const BSONObj requestVotesCmd = requestVotesCmdBuilder.obj();

    std::vector<RemoteCommandRequest> requests;
    for (const auto& target : _targets) {
        requests.push_back(RemoteCommandRequest(
            target,
            "admin",
            requestVotesCmd,
            Milliseconds(30 * 1000)));  // trying to match current Socket timeout
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
        ReplSetRequestVotesResponse voteResponse;
        voteResponse.initialize(response.getValue().data);
        if (voteResponse.getVoteGranted()) {
            _votes++;
        } else {
            log() << "VoteRequester: Got no vote from " << request.target
                  << " because: " << voteResponse.getReason();
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

VoteRequester::VoteRequestResult VoteRequester::Algorithm::getResult() const {
    if (_staleTerm) {
        return StaleTerm;
    } else if (_votes >= _rsConfig.getMajorityVoteCount()) {
        return SuccessfullyElected;
    } else {
        return InsufficientVotes;
    }
}

VoteRequester::VoteRequester() : _isCanceled(false) {}
VoteRequester::~VoteRequester() {}

StatusWith<ReplicationExecutor::EventHandle> VoteRequester::start(
    ReplicationExecutor* executor,
    const ReplicaSetConfig& rsConfig,
    long long candidateId,
    long long term,
    bool dryRun,
    OpTime lastOplogEntry,
    const stdx::function<void()>& onCompletion) {
    _algorithm.reset(new Algorithm(rsConfig, candidateId, term, dryRun, lastOplogEntry));
    _runner.reset(new ScatterGatherRunner(_algorithm.get()));
    return _runner->start(executor, onCompletion);
}

void VoteRequester::cancel(ReplicationExecutor* executor) {
    _isCanceled = true;
    _runner->cancel(executor);
}

VoteRequester::VoteRequestResult VoteRequester::getResult() const {
    return _algorithm->getResult();
}

}  // namespace repl
}  // namespace mongo

/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplicationElection

#include "mongo/platform/basic.h"

#include "mongo/db/repl/vote_requester.h"

#include "mongo/base/status.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

const Milliseconds maximumVoteRequestTimeoutMS(30 * 1000);

}  // namespace

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

VoteRequester::Algorithm::Algorithm(const ReplSetConfig& rsConfig,
                                    long long candidateIndex,
                                    long long term,
                                    bool dryRun,
                                    OpTime lastDurableOpTime,
                                    int primaryIndex)
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
        if (index == primaryIndex) {
            _primaryHost = member->getHostAndPort();
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
            target,
            "admin",
            requestVotesCmd,
            nullptr,
            std::min(_rsConfig.getElectionTimeoutPeriod(), maximumVoteRequestTimeoutMS)));
    }

    return requests;
}

void VoteRequester::Algorithm::processResponse(const RemoteCommandRequest& request,
                                               const RemoteCommandResponse& response) {
    auto logLine = log();
    logLine << "VoteRequester(term " << _term << (_dryRun ? " dry run" : "") << ") ";
    _responsesProcessed++;
    if (!response.isOK()) {  // failed response
        logLine << "failed to receive response from " << request.target << ": " << response.status;
        return;
    }
    _responders.insert(request.target);

    // If the primary's vote is a yes, we will set _primaryVote to be Yes.
    if (_primaryHost == request.target) {
        _primaryVote = PrimaryVote::No;
    }
    ReplSetRequestVotesResponse voteResponse;
    auto status = getStatusFromCommandResult(response.data);
    if (status.isOK()) {
        status = voteResponse.initialize(response.data);
    }
    if (!status.isOK()) {
        logLine << "received an invalid response from " << request.target << ": " << status;
        logLine << "; response message: " << response.data;
        return;
    }

    if (voteResponse.getVoteGranted()) {
        logLine << "received a yes vote from " << request.target;
        if (_primaryHost == request.target) {
            _primaryVote = PrimaryVote::Yes;
        }
        _votes++;
    } else {
        logLine << "received a no vote from " << request.target << " with reason \""
                << voteResponse.getReason() << '"';
    }

    if (voteResponse.getTerm() > _term) {
        _staleTerm = true;
    }
    logLine << "; response message: " << response.data;
}

bool VoteRequester::Algorithm::hasReceivedSufficientResponses() const {
    if (_primaryHost && _primaryVote == PrimaryVote::No) {
        return true;
    }

    if (_primaryHost && _primaryVote == PrimaryVote::Pending) {
        return false;
    }

    return _staleTerm || _votes >= _rsConfig.getMajorityVoteCount() ||
        _responsesProcessed == static_cast<int>(_targets.size());
}

VoteRequester::Result VoteRequester::Algorithm::getResult() const {
    if (_staleTerm) {
        return Result::kStaleTerm;
    } else if (_primaryHost && _primaryVote != PrimaryVote::Yes) {
        return Result::kPrimaryRespondedNo;
    } else if (_votes >= _rsConfig.getMajorityVoteCount()) {
        return Result::kSuccessfullyElected;
    } else {
        return Result::kInsufficientVotes;
    }
}

stdx::unordered_set<HostAndPort> VoteRequester::Algorithm::getResponders() const {
    return _responders;
}

VoteRequester::VoteRequester() : _isCanceled(false) {}
VoteRequester::~VoteRequester() {}

StatusWith<executor::TaskExecutor::EventHandle> VoteRequester::start(
    executor::TaskExecutor* executor,
    const ReplSetConfig& rsConfig,
    long long candidateIndex,
    long long term,
    bool dryRun,
    OpTime lastDurableOpTime,
    int primaryIndex) {
    _algorithm = std::make_shared<Algorithm>(
        rsConfig, candidateIndex, term, dryRun, lastDurableOpTime, primaryIndex);
    _runner = stdx::make_unique<ScatterGatherRunner>(_algorithm, executor, "vote request");
    return _runner->start();
}

void VoteRequester::cancel() {
    _isCanceled = true;
    _runner->cancel();
}

VoteRequester::Result VoteRequester::getResult() const {
    return _algorithm->getResult();
}

stdx::unordered_set<HostAndPort> VoteRequester::getResponders() const {
    return _algorithm->getResponders();
}

}  // namespace repl
}  // namespace mongo

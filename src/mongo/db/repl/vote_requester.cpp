// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/vote_requester.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <memory>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationElection


namespace mongo {
namespace repl {
using namespace std::literals::string_view_literals;

namespace {

const Milliseconds maximumVoteRequestTimeoutMS(30 * 1000);

}  // namespace

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

VoteRequester::Algorithm::Algorithm(const ReplSetConfig& rsConfig,
                                    long long candidateIndex,
                                    long long term,
                                    bool dryRun,
                                    OpTime lastWrittenOpTime,
                                    OpTime lastAppliedOpTime,
                                    int primaryIndex)
    : _rsConfig(rsConfig),
      _candidateIndex(candidateIndex),
      _term(term),
      _dryRun(dryRun),
      _lastWrittenOpTime(lastWrittenOpTime),
      _lastAppliedOpTime(lastAppliedOpTime) {
    // populate targets with all voting members that aren't this node
    long long index = 0;
    for (auto member = _rsConfig.membersBegin(); member != _rsConfig.membersEnd(); member++) {
        if (member->isVoter() && index != candidateIndex) {
            _targets.push_back(member->getHostAndPortPriority());
        }
        if (index == primaryIndex) {
            _primaryHost = member->getHostAndPortPriority();
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

    if (_rsConfig.getConfigTerm() != -1) {
        requestVotesCmdBuilder.append("configTerm", _rsConfig.getConfigTerm());
    }

    _lastWrittenOpTime.append("lastWrittenOpTime", &requestVotesCmdBuilder);
    _lastAppliedOpTime.append("lastAppliedOpTime", &requestVotesCmdBuilder);

    const BSONObj requestVotesCmd = requestVotesCmdBuilder.obj();

    std::vector<RemoteCommandRequest> requests;
    for (const auto& target : _targets) {
        requests.push_back(RemoteCommandRequest(
            target,
            DatabaseName::kAdmin,
            requestVotesCmd,
            nullptr,
            std::min(_rsConfig.getElectionTimeoutPeriod(), maximumVoteRequestTimeoutMS)));
    }

    return requests;
}

void VoteRequester::Algorithm::processResponse(const RemoteCommandRequest& request,
                                               const RemoteCommandResponse& response) {
    ReplSetRequestVotesResponse voteResponse;
    Status status = Status::OK();

    // All local variables captured in logAttrs needs to be above the guard that logs.
    logv2::DynamicAttributes logAttrs;
    auto logAtExit =
        ScopeGuard([&logAttrs]() { LOGV2(51799, "VoteRequester processResponse", logAttrs); });
    logAttrs.add("term", _term);
    logAttrs.add("dryRun", _dryRun);

    _responsesProcessed++;
    if (!response.isOK()) {  // failed response
        logAttrs.add("failReason", "failed to receive response"sv);
        logAttrs.add("error", response.status);
        logAttrs.add("from", request.target);
        return;
    }
    _responders.insert(request.target);

    // If the primary's vote is a yes, we will set _primaryVote to be Yes.
    if (_primaryHost == request.target) {
        _primaryVote = PrimaryVote::No;
    }

    status = getStatusFromCommandResult(response.data);
    if (status.isOK()) {
        status = voteResponse.initialize(response.data);
    }
    if (!status.isOK()) {
        logAttrs.add("failReason", "received an invalid response"sv);
        logAttrs.add("error", status);
        logAttrs.add("from", request.target);
        logAttrs.add("message", response.data);
        return;
    }

    if (voteResponse.getVoteGranted()) {
        logAttrs.add("vote", "yes"sv);
        logAttrs.add("from", request.target);
        if (_primaryHost == request.target) {
            _primaryVote = PrimaryVote::Yes;
        }
        _votes++;
    } else {
        logAttrs.add("vote", "no"sv);
        logAttrs.add("from", request.target);
        logAttrs.add("reason", voteResponse.getReason());
    }

    if (voteResponse.getTerm() > _term) {
        _staleTerm = true;
    }

    logAttrs.add("message", response.data);
}

bool VoteRequester::Algorithm::hasReceivedSufficientResponses() const {
    if (_primaryHost && _primaryVote == PrimaryVote::No) {
        return true;
    }

    // We require the primary's response during catchup takeover. An error response from the primary
    // is not a yes, no or pending, but is still a response. Therefore, we handle the case in which
    // we have received some response (even if error) from all nodes first.
    if (_responsesProcessed == static_cast<int>(_targets.size())) {
        return true;
    }

    if (_primaryHost && _primaryVote == PrimaryVote::Pending) {
        return false;
    }

    return _staleTerm || _votes >= _rsConfig.getMajorityVoteCount();
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

VoteRequester::VoteRequester() {}
VoteRequester::~VoteRequester() {}

StatusWith<executor::TaskExecutor::EventHandle> VoteRequester::start(
    executor::TaskExecutor* executor,
    const ReplSetConfig& rsConfig,
    long long candidateIndex,
    long long term,
    bool dryRun,
    OpTime lastWrittenOpTime,
    OpTime lastAppliedOpTime,
    int primaryIndex) {
    _algorithm = std::make_shared<Algorithm>(
        rsConfig, candidateIndex, term, dryRun, lastWrittenOpTime, lastAppliedOpTime, primaryIndex);
    _runner = std::make_unique<ScatterGatherRunner>(_algorithm, executor, "vote request");
    return _runner->start();
}

void VoteRequester::cancel() {
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

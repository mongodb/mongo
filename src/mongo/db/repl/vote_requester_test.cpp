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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace repl {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using unittest::assertGet;

bool stringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST(ReplSetRequestVotes, ArgsAcceptsUnknownField) {
    ReplSetRequestVotesArgs requestVoteArgs;
    BSONObjBuilder bob;
    requestVoteArgs.addToBSON(&bob);
    bob.append("unknownField", 1);  // append an unknown field.
    BSONObj cmdObj = bob.obj();
    ASSERT_OK(requestVoteArgs.initialize(cmdObj));

    // The serialized object should be the same as the original except for the unknown field.
    BSONObjBuilder bob2;
    requestVoteArgs.addToBSON(&bob2);
    bob2.append("unknownField", 1);
    ASSERT_BSONOBJ_EQ(bob2.obj(), cmdObj);
}

TEST(ReplSetRequestVotes, ResponseAcceptsUnknownField) {
    ReplSetRequestVotesResponse requestVoteRes;
    BSONObjBuilder bob;
    requestVoteRes.addToBSON(&bob);
    bob.append("unknownField", 1);  // append an unknown field.
    BSONObj cmdObj = bob.obj();
    ASSERT_OK(requestVoteRes.initialize(cmdObj));

    // The serialized object should be the same as the original except for the unknown field.
    BSONObjBuilder bob2;
    requestVoteRes.addToBSON(&bob2);
    bob2.append("unknownField", 1);
    ASSERT_BSONOBJ_EQ(bob2.obj(), cmdObj);
}


class VoteRequesterTest : public mongo::unittest::Test {
public:
    virtual void setUp() {
        ReplSetConfig config(
            ReplSetConfig::parse(BSON("_id"
                                      << "rs0"
                                      << "version" << 2 << "protocolVersion" << 1 << "members"
                                      << BSON_ARRAY(
                                             BSON("_id" << 0 << "host"
                                                        << "host0")
                                             << BSON("_id" << 1 << "host"
                                                           << "host1")
                                             << BSON("_id" << 2 << "host"
                                                           << "host2")
                                             << BSON("_id" << 3 << "host"
                                                           << "host3"
                                                           << "votes" << 0 << "priority" << 0)
                                             << BSON("_id" << 4 << "host"
                                                           << "host4"
                                                           << "votes" << 0 << "priority" << 0)))));
        ASSERT_OK(config.validate());
        long long candidateId = 0;
        long long term = 2;
        OpTime lastOplogEntry = OpTime(Timestamp(999, 0), 1);

        _requester.reset(new VoteRequester::Algorithm(config,
                                                      candidateId,
                                                      term,
                                                      false,  // not a dryRun
                                                      lastOplogEntry,
                                                      lastOplogEntry,
                                                      -1));
    }

    virtual void tearDown() {
        _requester.reset(nullptr);
    }

protected:
    int64_t countLogLinesContaining(const std::string& needle) {
        const auto& msgs = getCapturedTextFormatLogMessages();
        return std::count_if(
            msgs.begin(), msgs.end(), [&](const auto& s) { return stringContains(s, needle); });
    }

    bool hasReceivedSufficientResponses() {
        return _requester->hasReceivedSufficientResponses();
    }

    void processResponse(const RemoteCommandRequest& request,
                         const RemoteCommandResponse& response) {
        if (!response.isOK()) {
            _requester->processResponse(request, response);
            return;
        }
        BSONObjBuilder builder(response.data);
        // Appends ok:1.0 (status ok) to response data if 'ok' field is missing.
        CommandHelpers::appendCommandStatusNoThrow(builder, Status::OK());
        RemoteCommandResponse responseWithCmdStatus = response;
        responseWithCmdStatus.data = builder.obj();
        _requester->processResponse(request, responseWithCmdStatus);
    }

    int getNumResponders() {
        return _requester->getResponders().size();
    }

    VoteRequester::Result getResult() {
        return _requester->getResult();
    }

    RemoteCommandRequest requestFrom(std::string hostname) {
        return RemoteCommandRequest(HostAndPort(hostname),
                                    DatabaseName::kEmpty,  // fields do not matter in VoteRequester
                                    BSONObj(),
                                    nullptr,
                                    Milliseconds(0));
    }

    RemoteCommandResponse badRemoteCommandResponse() {
        return RemoteCommandResponse(ErrorCodes::NodeNotFound, "not on my watch");
    }

    RemoteCommandResponse callbackCanceledCommandResponse() {
        return RemoteCommandResponse(ErrorCodes::CallbackCanceled, "Testing canceled callback");
    }

    RemoteCommandResponse votedYes() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(true);
        response.setTerm(1);
        return RemoteCommandResponse(response.toBSON(), Milliseconds(10));
    }

    RemoteCommandResponse votedYesStatusNotOkBecauseFailedToStoreLastVote() {
        ReplSetRequestVotesResponse response;
        BSONObjBuilder result;
        response.setVoteGranted(true);
        response.setTerm(1);
        response.addToBSON(&result);
        auto status =
            Status(ErrorCodes::InterruptedDueToReplStateChange, "operation was interrupted");
        CommandHelpers::appendCommandStatusNoThrow(result, status);
        return RemoteCommandResponse(result.obj(), Milliseconds(10));
    }

    RemoteCommandResponse votedNoBecauseConfigVersionDoesNotMatch() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(1);
        response.setReason("candidate's config version differs from mine");
        return RemoteCommandResponse(response.toBSON(), Milliseconds(10));
    }

    RemoteCommandResponse votedNoBecauseSetNameDiffers() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(1);
        response.setReason("candidate's set name differs from mine");
        return RemoteCommandResponse(response.toBSON(), Milliseconds(10));
    }

    RemoteCommandResponse votedNoBecauseLastOpTimeIsGreater() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(1);
        response.setReason("candidate's data is staler than mine");
        return RemoteCommandResponse(response.toBSON(), Milliseconds(10));
    }

    RemoteCommandResponse votedNoBecauseTermIsGreater() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(3);
        response.setReason("candidate's term is lower than mine");
        return RemoteCommandResponse(response.toBSON(), Milliseconds(10));
    }

    RemoteCommandResponse votedNoBecauseAlreadyVoted() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(2);
        response.setReason("already voted for another candidate this term");
        return RemoteCommandResponse(response.toBSON(), Milliseconds(10));
    }

    std::unique_ptr<VoteRequester::Algorithm> _requester;
};

class VoteRequesterDryRunTest : public VoteRequesterTest {
public:
    virtual void setUp() {
        ReplSetConfig config(
            ReplSetConfig::parse(BSON("_id"
                                      << "rs0"
                                      << "version" << 2 << "protocolVersion" << 1 << "members"
                                      << BSON_ARRAY(
                                             BSON("_id" << 0 << "host"
                                                        << "host0")
                                             << BSON("_id" << 1 << "host"
                                                           << "host1")
                                             << BSON("_id" << 2 << "host"
                                                           << "host2")
                                             << BSON("_id" << 3 << "host"
                                                           << "host3"
                                                           << "votes" << 0 << "priority" << 0)
                                             << BSON("_id" << 4 << "host"
                                                           << "host4"
                                                           << "votes" << 0 << "priority" << 0)))));
        ASSERT_OK(config.validate());
        long long candidateId = 0;
        long long term = 2;
        OpTime lastOplogEntry = OpTime(Timestamp(999, 0), 1);

        _requester.reset(new VoteRequester::Algorithm(config,
                                                      candidateId,
                                                      term,
                                                      true,  // dryRun
                                                      lastOplogEntry,
                                                      lastOplogEntry,
                                                      -1));
    }
};

class VoteRequesterCatchupTakeoverDryRunTest : public VoteRequesterTest {
public:
    virtual void setUp() {
        ReplSetConfig config(
            ReplSetConfig::parse(BSON("_id"
                                      << "rs0"
                                      << "version" << 2 << "protocolVersion" << 1 << "members"
                                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                               << "host0")
                                                    << BSON("_id" << 1 << "host"
                                                                  << "host1")
                                                    << BSON("_id" << 2 << "host"
                                                                  << "host2")
                                                    << BSON("_id" << 3 << "host"
                                                                  << "host3")
                                                    << BSON("_id" << 4 << "host"
                                                                  << "host4")))));
        ASSERT_OK(config.validate());
        long long candidateId = 0;
        long long term = 2;
        int primaryIndex = 1;
        OpTime lastOplogEntry = OpTime(Timestamp(999, 0), 1);

        _requester.reset(new VoteRequester::Algorithm(config,
                                                      candidateId,
                                                      term,
                                                      true,  // dryRun
                                                      lastOplogEntry,
                                                      lastOplogEntry,
                                                      primaryIndex));
    }
};

TEST_F(VoteRequesterTest, ImmediateGoodResponseWinElection) {
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(1, getNumResponders());
}

TEST_F(VoteRequesterTest, VoterFailedToStoreLastVote) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedYesStatusNotOkBecauseFailedToStoreLastVote());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("failReason"
                                                            << "received an invalid response"
                                                            << "from"
                                                            << "host1:27017"))));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterTest, BadConfigVersionWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseConfigVersionDoesNotMatch());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterTest, SetNameDiffersWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseSetNameDiffers());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterTest, LastOpTimeIsGreaterWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseLastOpTimeIsGreater());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));

    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterTest, FailedToContactWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), badRemoteCommandResponse());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("failReason"
                                                            << "failed to receive response"
                                                            << "from"
                                                            << "host1:27017"))));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(1, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterTest, AlreadyVotedWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseAlreadyVoted());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterTest, StaleTermLoseElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseTermIsGreater());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kStaleTerm == getResult());
    ASSERT_EQUALS(1, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterTest, NotEnoughVotesLoseElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseSetNameDiffers());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), badRemoteCommandResponse());

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("failReason"
                                                            << "failed to receive response"
                                                            << "from"
                                                            << "host2:27017"))));
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kInsufficientVotes == getResult());
    ASSERT_EQUALS(1, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterTest, CallbackCanceledNotEnoughVotesLoseElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseAlreadyVoted());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), callbackCanceledCommandResponse());
    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("failReason"
                                                            << "failed to receive response"
                                                            << "from"
                                                            << "host2:27017"))));
    // Make sure processing the callbackCanceled Response was necessary to get sufficient responses.
    ASSERT_TRUE(hasReceivedSufficientResponses());
    // Because of the CallbackCanceled Response, host2 doesn't count as a responder.
    ASSERT_EQUALS(1, getNumResponders());
    ASSERT(VoteRequester::Result::kInsufficientVotes == getResult());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterDryRunTest, ImmediateGoodResponseWinElection) {
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(1, getNumResponders());
}

TEST_F(VoteRequesterDryRunTest, BadConfigVersionWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseConfigVersionDoesNotMatch());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterDryRunTest, SetNameDiffersWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseSetNameDiffers());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterDryRunTest, LastOpTimeIsGreaterWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseLastOpTimeIsGreater());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterDryRunTest, FailedToContactWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), badRemoteCommandResponse());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(1, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterDryRunTest, AlreadyVotedWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseAlreadyVoted());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterDryRunTest, StaleTermLoseElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseTermIsGreater());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kStaleTerm == getResult());
    ASSERT_EQUALS(1, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterDryRunTest, NotEnoughVotesLoseElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseSetNameDiffers());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    processResponse(requestFrom("host2"), badRemoteCommandResponse());
    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("failReason"
                                                            << "failed to receive response"
                                                            << "from"
                                                            << "host2:27017"))));
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kInsufficientVotes == getResult());
    ASSERT_EQUALS(1, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterCatchupTakeoverDryRunTest, CatchupTakeoverPrimarySaysYesWinElection) {
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedYes());
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
}

TEST_F(VoteRequesterCatchupTakeoverDryRunTest, CatchupTakeoverPrimarySaysYesButNotEnoughVotes) {
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedYes());
    ASSERT(VoteRequester::Result::kInsufficientVotes == getResult());
    processResponse(requestFrom("host2"), votedNoBecauseLastOpTimeIsGreater());
    processResponse(requestFrom("host3"), votedNoBecauseLastOpTimeIsGreater());
    processResponse(requestFrom("host4"), votedNoBecauseLastOpTimeIsGreater());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kInsufficientVotes == getResult());
    ASSERT_EQUALS(4, getNumResponders());
}

TEST_F(VoteRequesterCatchupTakeoverDryRunTest, CatchupTakeoverPrimarySaysNoLoseElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host2"), votedYes());
    processResponse(requestFrom("host3"), votedYes());

    // This covers the case that the Vote Requester is cancelled partway through
    // the dry run before the primary responded.
    ASSERT(VoteRequester::Result::kPrimaryRespondedNo == getResult());

    // It also tests that even if a majority of yes votes have already been received,
    // it still needs to wait for a yes response from the primary.
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), votedNoBecauseLastOpTimeIsGreater());
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("vote"
                                                                      << "no"
                                                                      << "from"
                                                                      << "host1:27017"))));
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kPrimaryRespondedNo == getResult());
    ASSERT_EQUALS(3, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterCatchupTakeoverDryRunTest,
       CatchupTakeoverAllNodesRespondedMeansSufficientResponses) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());

    // Getting a good response from the other secondaries is insufficient.
    processResponse(requestFrom("host2"), votedYes());
    processResponse(requestFrom("host3"), votedYes());
    processResponse(requestFrom("host4"), votedYes());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    // Getting a bad response from the primary means all targets have responded, and so we have
    // received sufficient responses.
    processResponse(requestFrom("host1"), badRemoteCommandResponse());
    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("failReason"
                                                            << "failed to receive response"
                                                            << "from"
                                                            << "host1:27017"))));
    ASSERT_TRUE(hasReceivedSufficientResponses());

    // A bad response from the primary is equivalent to it saying NO.
    ASSERT(VoteRequester::Result::kPrimaryRespondedNo == getResult());

    // Only the secondaries are counted; the primary is excluded from the responders since it gave a
    // bad response.
    ASSERT_EQUALS(3, getNumResponders());
    stopCapturingLogMessages();
}

}  // namespace
}  // namespace repl
}  // namespace mongo

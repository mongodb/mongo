/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <memory>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using unittest::assertGet;

bool stringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}


class VoteRequesterTest : public mongo::unittest::Test {
public:
    virtual void setUp() {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(BSON("_id"
                                         << "rs0"
                                         << "version"
                                         << 2
                                         << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "host0")
                                                       << BSON("_id" << 1 << "host"
                                                                     << "host1")
                                                       << BSON("_id" << 2 << "host"
                                                                     << "host2")
                                                       << BSON("_id" << 3 << "host"
                                                                     << "host3"
                                                                     << "votes"
                                                                     << 0
                                                                     << "priority"
                                                                     << 0)
                                                       << BSON("_id" << 4 << "host"
                                                                     << "host4"
                                                                     << "votes"
                                                                     << 0
                                                                     << "priority"
                                                                     << 0)))));
        ASSERT_OK(config.validate());
        long long candidateId = 0;
        long long term = 2;
        OpTime lastOplogEntry = OpTime(Timestamp(999, 0), 1);

        _requester.reset(new VoteRequester::Algorithm(config,
                                                      candidateId,
                                                      term,
                                                      false,  // not a dryRun
                                                      lastOplogEntry));
    }

    virtual void tearDown() {
        _requester.reset(NULL);
    }

protected:
    int64_t countLogLinesContaining(const std::string& needle) {
        return std::count_if(getCapturedLogMessages().begin(),
                             getCapturedLogMessages().end(),
                             stdx::bind(stringContains, stdx::placeholders::_1, needle));
    }

    bool hasReceivedSufficientResponses() {
        return _requester->hasReceivedSufficientResponses();
    }

    void processResponse(const RemoteCommandRequest& request, const ResponseStatus& response) {
        _requester->processResponse(request, response);
    }

    int getNumResponders() {
        return _requester->getResponders().size();
    }

    VoteRequester::Result getResult() {
        return _requester->getResult();
    }

    RemoteCommandRequest requestFrom(std::string hostname) {
        return RemoteCommandRequest(HostAndPort(hostname),
                                    "",  // fields do not matter in VoteRequester
                                    BSONObj(),
                                    Milliseconds(0));
    }

    ResponseStatus badResponseStatus() {
        return ResponseStatus(ErrorCodes::NodeNotFound, "not on my watch");
    }

    ResponseStatus votedYes() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(true);
        response.setTerm(1);
        return ResponseStatus(
            NetworkInterfaceMock::Response(response.toBSON(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus votedNoBecauseConfigVersionDoesNotMatch() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(1);
        response.setReason("candidate's config version differs from mine");
        return ResponseStatus(
            NetworkInterfaceMock::Response(response.toBSON(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus votedNoBecauseSetNameDiffers() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(1);
        response.setReason("candidate's set name differs from mine");
        return ResponseStatus(
            NetworkInterfaceMock::Response(response.toBSON(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus votedNoBecauseLastOpTimeIsGreater() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(1);
        response.setReason("candidate's data is staler than mine");
        return ResponseStatus(
            NetworkInterfaceMock::Response(response.toBSON(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus votedNoBecauseTermIsGreater() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(3);
        response.setReason("candidate's term is lower than mine");
        return ResponseStatus(
            NetworkInterfaceMock::Response(response.toBSON(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus votedNoBecauseAlreadyVoted() {
        ReplSetRequestVotesResponse response;
        response.setVoteGranted(false);
        response.setTerm(2);
        response.setReason("already voted for another candidate this term");
        return ResponseStatus(
            NetworkInterfaceMock::Response(response.toBSON(), BSONObj(), Milliseconds(10)));
    }

    std::unique_ptr<VoteRequester::Algorithm> _requester;
};

class VoteRequesterDryRunTest : public VoteRequesterTest {
public:
    virtual void setUp() {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(BSON("_id"
                                         << "rs0"
                                         << "version"
                                         << 2
                                         << "members"
                                         << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "host0")
                                                       << BSON("_id" << 1 << "host"
                                                                     << "host1")
                                                       << BSON("_id" << 2 << "host"
                                                                     << "host2")
                                                       << BSON("_id" << 3 << "host"
                                                                     << "host3"
                                                                     << "votes"
                                                                     << 0
                                                                     << "priority"
                                                                     << 0)
                                                       << BSON("_id" << 4 << "host"
                                                                     << "host4"
                                                                     << "votes"
                                                                     << 0
                                                                     << "priority"
                                                                     << 0)))));
        ASSERT_OK(config.validate());
        long long candidateId = 0;
        long long term = 2;
        OpTime lastOplogEntry = OpTime(Timestamp(999, 0), 1);

        _requester.reset(new VoteRequester::Algorithm(config,
                                                      candidateId,
                                                      term,
                                                      true,  // dryRun
                                                      lastOplogEntry));
    }
};

TEST_F(VoteRequesterTest, ImmediateGoodResponseWinElection) {
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(1, getNumResponders());
}

TEST_F(VoteRequesterTest, BadConfigVersionWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), votedNoBecauseConfigVersionDoesNotMatch());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterTest, FailedToContactWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), badResponseStatus());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1, countLogLinesContaining("Got failed response from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
    processResponse(requestFrom("host2"), badResponseStatus());
    ASSERT_EQUALS(1, countLogLinesContaining("Got failed response from host2"));
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kInsufficientVotes == getResult());
    ASSERT_EQUALS(1, getNumResponders());
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
    processResponse(requestFrom("host2"), votedYes());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kSuccessfullyElected == getResult());
    ASSERT_EQUALS(2, getNumResponders());
    stopCapturingLogMessages();
}

TEST_F(VoteRequesterDryRunTest, FailedToContactWinElection) {
    startCapturingLogMessages();
    ASSERT_FALSE(hasReceivedSufficientResponses());
    processResponse(requestFrom("host1"), badResponseStatus());
    ASSERT_FALSE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(1, countLogLinesContaining("Got failed response from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Got no vote from host1"));
    processResponse(requestFrom("host2"), badResponseStatus());
    ASSERT_EQUALS(1, countLogLinesContaining("Got failed response from host2"));
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT(VoteRequester::Result::kInsufficientVotes == getResult());
    ASSERT_EQUALS(1, getNumResponders());
    stopCapturingLogMessages();
}

}  // namespace
}  // namespace repl
}  // namespace mongo

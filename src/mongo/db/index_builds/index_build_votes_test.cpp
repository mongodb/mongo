// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_votes.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/repl_index_build_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::index_builds::votes {
namespace {

class IndexBuildVotesTest : public CatalogTestFixture {
protected:
    // Answers each attempt at 'command' with the next response in 'responses'.
    void scriptResponses(std::string_view command,
                         std::vector<BSONObj> responses,
                         std::function<void()> beforeResponse = {}) {
        auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
            repl::ReplicationCoordinator::get(operationContext()));
        replCoord->setRunCmdOnPrimaryAndAwaitResponseFunction(
            [this,
             command = std::string{command},
             responses = std::move(responses),
             beforeResponse =
                 std::move(beforeResponse)](OperationContext*,
                                            const DatabaseName&,
                                            const BSONObj& cmdObj,
                                            repl::ReplicationCoordinator::OnRemoteCmdScheduledFn,
                                            repl::ReplicationCoordinator::OnRemoteCmdCompleteFn) {
                ASSERT_EQ(command, cmdObj.firstElementFieldNameStringData());
                ASSERT_LT(attempts, responses.size());
                if (beforeResponse) {
                    beforeResponse();
                }
                return responses[attempts++];
            });
    }

    AbortVoteResult runAbortVote(std::vector<BSONObj> responses,
                                 std::function<void()> beforeResponse = {}) {
        scriptResponses("voteAbortIndexBuild", std::move(responses), std::move(beforeResponse));
        return runVoteAbortCommand(operationContext(), replState, "test");
    }

    void runCommitVote(std::vector<BSONObj> responses, std::function<void()> beforeResponse = {}) {
        scriptResponses("voteCommitIndexBuild", std::move(responses), std::move(beforeResponse));
        runVoteCommitCommand(operationContext(), replState);
    }

    ReplIndexBuildState replState{UUID::gen(),
                                  UUID::gen(),
                                  DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                  {},
                                  IndexBuildProtocol::kTwoPhase,
                                  Date_t::now()};
    size_t attempts = 0;
};


const auto kNotPrimary =
    BSON("ok" << 0 << "code" << ErrorCodes::NotWritablePrimary << "errmsg" << "not primary");
const auto kWriteConcernError =
    BSON("ok" << 1 << "writeConcernError"
              << BSON("code" << ErrorCodes::WriteConcernTimeout << "errmsg"
                             << "waiting for replication timed out"));
const auto kCannotAbort =
    BSON("ok" << 0 << "code" << 7329202 << "errmsg" << "Index build cannot be aborted");

TEST_F(IndexBuildVotesTest, AbortVoteAccepted) {
    EXPECT_EQ(AbortVoteResult::kAccepted, runAbortVote({BSON("ok" << 1)}));
    EXPECT_EQ(1U, attempts);
}

TEST_F(IndexBuildVotesTest, AbortVoteAcceptedDespiteWriteConcernError) {
    EXPECT_EQ(AbortVoteResult::kAccepted, runAbortVote({kWriteConcernError}));
    EXPECT_EQ(1U, attempts);
}

TEST_F(IndexBuildVotesTest, AbortVoteRetriesAfterPrimaryChange) {
    EXPECT_EQ(AbortVoteResult::kAccepted, runAbortVote({kNotPrimary, BSON("ok" << 1)}));
    EXPECT_EQ(2U, attempts);
}

TEST_F(IndexBuildVotesTest, AbortVoteStopsAtRefusal) {
    EXPECT_EQ(AbortVoteResult::kRefused, runAbortVote({kCannotAbort}));
    EXPECT_EQ(1U, attempts);
}

TEST_F(IndexBuildVotesTest, AbortVoteEndsWhenBuildIsSignaled) {
    // A commit can reach this node before the primary answers. The vote is not retried.
    EXPECT_EQ(AbortVoteResult::kSignaled, runAbortVote({kNotPrimary}, [&] {
                  replState.setCommitQuorumSatisfied(operationContext());
              }));
    EXPECT_EQ(1U, attempts);
}

TEST_F(IndexBuildVotesTest, CommitVoteRetriesUntilAccepted) {
    // A commit vote retries any error, including a write concern error.
    runCommitVote({kCannotAbort, kWriteConcernError, BSON("ok" << 1)});
    EXPECT_EQ(3U, attempts);
}

TEST_F(IndexBuildVotesTest, CommitVoteEndsWhenBuildIsSignaled) {
    runCommitVote({kNotPrimary}, [&] { replState.setCommitQuorumSatisfied(operationContext()); });
    EXPECT_EQ(1U, attempts);
}

}  // namespace
}  // namespace mongo::index_builds::votes

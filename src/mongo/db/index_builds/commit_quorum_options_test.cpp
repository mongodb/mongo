// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/commit_quorum_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CommitQuorumOptionsTest, ParseReturnsFailedToParseOnEmptyElement) {
    BSONObj obj = BSON("commitQuorum" << BSONObj());
    auto status = CommitQuorumOptions().parse(obj.getField("commitQuorum"));
    EXPECT_EQ(ErrorCodes::FailedToParse, status);
    EXPECT_EQ("commitQuorum has to be a number or a string", status.reason());
}

TEST(CommitQuorumOptionsTest, ParseReturnsFailedToParseIfCommitQuorumIsNotNumberOrString) {
    BSONObj obj = BSON("commitQuorum" << BSONObj());
    auto status = CommitQuorumOptions().parse(obj.getField("commitQuorum"));
    EXPECT_EQ(ErrorCodes::FailedToParse, status);
    EXPECT_EQ("commitQuorum has to be a number or a string", status.reason());
}

TEST(CommitQuorumOptionsTest, CommitQuorumZeroRoundsUpToOne) {
    BSONObj obj = BSON("commitQuorum" << 0);
    CommitQuorumOptions options;
    ASSERT_OK(options.parse(obj.getField("commitQuorum")));
    EXPECT_EQ(1, options.numNodes);
}

TEST(CommitQuorumOptionsTest, ParseReturnsFailedToParseIfCommitQuorumIsNegative) {
    BSONObj obj = BSON("commitQuorum" << -1);
    auto status = CommitQuorumOptions().parse(obj.getField("commitQuorum"));
    EXPECT_EQ(ErrorCodes::FailedToParse, status);
    EXPECT_EQ("commitQuorum has to be a non-negative number and not greater than 50",
              status.reason());
}

TEST(CommitQuorumOptionsTest,
     ParseReturnsFailedToParseIfCommitQuorumIsGreaterThanMaxReplSetMembersSize) {
    BSONObj obj = BSON("commitQuorum" << 70);
    auto status = CommitQuorumOptions().parse(obj.getField("commitQuorum"));
    EXPECT_EQ(ErrorCodes::FailedToParse, status);
    EXPECT_EQ("commitQuorum has to be a non-negative number and not greater than 50",
              status.reason());
}

TEST(CommitQuorumOptionsTest, ParseSetsNumNodesIfCommitQuorumIsANumber) {
    CommitQuorumOptions options;
    BSONObj obj = BSON("commitQuorum" << 3);
    ASSERT_OK(options.parse(obj.getField("commitQuorum")));
    EXPECT_EQ(3, options.numNodes);
    EXPECT_EQ("", options.mode);
}

TEST(CommitQuorumOptionsTest, ParseSetsModeIfCommitQuorumIskMajorityString) {
    CommitQuorumOptions options;
    BSONObj obj = BSON("commitQuorum" << CommitQuorumOptions::kMajority);
    ASSERT_OK(options.parse(obj.getField("commitQuorum")));
    EXPECT_EQ(-1, options.numNodes);
    EXPECT_EQ(CommitQuorumOptions::kMajority, options.mode);
}

TEST(CommitQuorumOptionsTest, ParseSetsModeIfCommitQuorumIskVotingMembersString) {
    CommitQuorumOptions options;
    BSONObj obj = BSON("commitQuorum" << CommitQuorumOptions::kVotingMembers);
    ASSERT_OK(options.parse(obj.getField("commitQuorum")));
    EXPECT_EQ(-1, options.numNodes);
    EXPECT_EQ(CommitQuorumOptions::kVotingMembers, options.mode);
}

TEST(CommitQuorumOptionsTest, ToBSON) {
    {
        CommitQuorumOptions options;
        BSONObj obj = BSON("commitQuorum" << 1);
        ASSERT_OK(options.parse(obj.getField("commitQuorum")));
        EXPECT_TRUE(options.toBSON().woCompare(BSON("commitQuorum" << 1)) == 0);
    }

    {
        CommitQuorumOptions options;
        BSONObj obj = BSON("commitQuorum" << "someTag");
        ASSERT_OK(options.parse(obj.getField("commitQuorum")));
        EXPECT_TRUE(options.toBSON().woCompare(BSON("commitQuorum" << "someTag")) == 0);
    }

    {
        // Strings take precedence over numbers.
        CommitQuorumOptions options;
        options.mode = "majority";
        options.numNodes = 5;
        EXPECT_TRUE(options.toBSON().woCompare(BSON("commitQuorum" << "majority")) == 0);
    }
}

}  // namespace
}  // namespace mongo

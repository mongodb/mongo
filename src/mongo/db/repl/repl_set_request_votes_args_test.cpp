// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/repl_set_request_votes_args.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>
#include <ostream>

namespace mongo {
namespace repl {
namespace {

TEST(ReplSetRequestVotesArgs, CorrectReplSetRequestVotesArgs) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj =
        BSON("term" << 1 << "candidateIndex" << 1 << "configVersion" << 1 << "configTerm" << 1
                    << "setName"
                    << "test"
                    << "dryRun" << true << "lastAppliedOpTime" << OpTime(Timestamp(50), 1)
                    << "lastWrittenOpTime" << OpTime(Timestamp(50), 1));
    Status result = args.initialize(initializerObj);
    ASSERT_TRUE(result.isOK());

    ASSERT_EQUALS("test", args.getSetName());
    ASSERT_EQUALS(1, args.getTerm());
    ASSERT_EQUALS(1, args.getCandidateIndex());
    ASSERT_EQUALS(1, args.getConfigVersion());
    ASSERT_EQUALS(1, args.getConfigTerm());
    ASSERT_EQUALS(OpTime(Timestamp(50), 1), args.getLastAppliedOpTime());
    ASSERT_EQUALS(OpTime(Timestamp(50), 1), args.getLastWrittenOpTime());
    ASSERT_EQUALS(true, args.isADryRun());
}

TEST(ReplSetRequestVotesArgs, InitializeWrongTypesForTerm) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj = BSON(
        "term" << "1"
               << "candidateIndex" << 1 << "configVersion" << 1 << "configTerm" << 1 << "setName"
               << "test"
               << "dryRun" << true << "lastAppliedOpTime" << OpTime(Timestamp(50), 1)
               << "lastWrittenOpTime" << OpTime(Timestamp(50), 1));
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesArgs, InitializeWrongTypesForCandidateIndex) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj =
        BSON("term" << 1 << "candidateIndex"
                    << "1"
                    << "configVersion" << 1 << "configTerm" << 1 << "setName"
                    << "test"
                    << "dryRun" << true << "lastAppliedOpTime" << OpTime(Timestamp(50), 1)
                    << "lastWrittenOpTime" << OpTime(Timestamp(50), 1));
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesArgs, InitializeWrongTypesForConfigVersion) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj =
        BSON("term" << 1 << "candidateIndex" << 1 << "configVersion"
                    << "1"
                    << "configTerm" << 1 << "setName"
                    << "test"
                    << "dryRun" << true << "lastAppliedOpTime" << OpTime(Timestamp(50), 1)
                    << "lastWrittenOpTime" << OpTime(Timestamp(50), 1));
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesArgs, InitializeWrongTypesForConfigTerm) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj =
        BSON("term" << 1 << "candidateIndex" << 1 << "configVersion" << 1 << "configTerm"
                    << "1"
                    << "setName"
                    << "test"
                    << "dryRun" << true << "lastAppliedOpTime" << OpTime(Timestamp(50), 1)
                    << "lastWrittenOpTime" << OpTime(Timestamp(50), 1));
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesArgs, InitializeWrongTypesForSetName) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj =
        BSON("term" << 1 << "candidateIndex" << 1 << "configVersion" << 1 << "configTerm" << 1
                    << "setName" << 1 << "dryRun" << true << "lastAppliedOpTime"
                    << OpTime(Timestamp(50), 1) << "lastWrittenOpTime" << OpTime(Timestamp(50), 1));
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesArgs, InitializeWrongTypesForDryRun) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj =
        BSON("term" << 1 << "candidateIndex" << 1 << "configVersion" << 1 << "configTerm" << 1
                    << "setName"
                    << "test"
                    << "dryRun" << 2 << "lastAppliedOpTime" << OpTime(Timestamp(50), 1)
                    << "lastWrittenOpTime" << OpTime(Timestamp(50), 1));
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesArgs, InitializeWrongTypesForLastAppliedOpTime) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj = BSON("term" << 1 << "candidateIndex" << 1 << "configVersion" << 1
                                         << "configTerm" << 1 << "setName"
                                         << "test"
                                         << "dryRun" << true << "lastAppliedOpTime" << 1
                                         << "lastWrittenOpTime" << OpTime(Timestamp(50), 1));
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesArgs, InitializeWrongTypesForLastWrittenOpTime) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj = BSON("term" << 1 << "candidateIndex" << 1 << "configVersion" << 1
                                         << "configTerm" << 1 << "setName"
                                         << "test"
                                         << "dryRun" << true << "lastAppliedOpTime"
                                         << OpTime(Timestamp(50), 1) << "lastWrittenOpTime" << 1);
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesArgs, InitializeWithNoLastWrittenOpTime) {
    ReplSetRequestVotesArgs args;
    BSONObj initializerObj =
        BSON("term" << 1 << "candidateIndex" << 1 << "configVersion" << 1 << "configTerm" << 1
                    << "setName"
                    << "test"
                    << "dryRun" << true << "lastAppliedOpTime" << OpTime(Timestamp(50), 1));
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(OpTime(Timestamp(50), 1), args.getLastWrittenOpTime());
}

TEST(ReplSetRequestVotesResponse, CorrectReplSetRequestVotesResponse) {
    ReplSetRequestVotesResponse args;
    BSONObj initializerObj = BSON("term" << 1 << "reason"
                                         << "hi"
                                         << "voteGranted" << true);
    Status result = args.initialize(initializerObj);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(1, args.getTerm());
    ASSERT_EQUALS(true, args.getVoteGranted());
    ASSERT_EQUALS("hi", args.getReason());
}

TEST(ReplSetRequestVotesResponse, InitializeWrongTypesForTerm) {
    ReplSetRequestVotesResponse args;
    BSONObj initializerObj = BSON("term" << "1"
                                         << "reason"
                                         << "hi"
                                         << "voteGranted" << true);
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesResponse, InitializeWrongTypesForReason) {
    ReplSetRequestVotesResponse args;
    BSONObj initializerObj = BSON("term" << 1 << "reason" << 1 << "voteGranted" << true);
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

TEST(ReplSetRequestVotesResponse, InitializeWrongTypesForVoteGranted) {
    ReplSetRequestVotesResponse args;
    BSONObj initializerObj = BSON("term" << 1 << "reason"
                                         << "hi"
                                         << "voteGranted" << 2);
    Status result = args.initialize(initializerObj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, result);
}

}  // namespace
}  // namespace repl
}  // namespace mongo

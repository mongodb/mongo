/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

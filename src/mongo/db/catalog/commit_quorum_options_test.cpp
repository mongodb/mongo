/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CommitQuorumOptionsTest, ParseReturnsFailedToParseOnEmptyDocument) {
    auto status = CommitQuorumOptions().parse({});
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("commit quorum object cannot be empty", status.reason());
}

TEST(CommitQuorumOptionsTest, ParseReturnsFailedToParseIfCommitQuorumIsNotNumberOrString) {
    auto status = CommitQuorumOptions().parse(BSON("commitQuorum" << BSONObj()));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("commitQuorum has to be a number or a string", status.reason());
}

TEST(CommitQuorumOptionsTest, ParseSetsNumNodesIfCommitQuorumIsANumber) {
    CommitQuorumOptions options;
    ASSERT_OK(options.parse(BSON("commitQuorum" << 3)));
    ASSERT_EQUALS(3, options.numNodes);
    ASSERT_EQUALS("", options.mode);
}

TEST(CommitQuorumOptionsTest, ParseSetsModeIfCommitQuorumIsAString) {
    CommitQuorumOptions options;
    ASSERT_OK(options.parse(BSON("commitQuorum" << CommitQuorumOptions::kMajority)));
    ASSERT_EQUALS(0, options.numNodes);
    ASSERT_EQUALS(CommitQuorumOptions::kMajority, options.mode);
}

TEST(CommitQuorumOptionsTest, ParseReturnsFailedToParseOnUnknownField) {
    auto status = CommitQuorumOptions().parse(BSON("x" << 123));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("unrecognized commit quorum field: x", status.reason());
}

TEST(CommitQuorumOptionsTest, ToBSON) {
    {
        CommitQuorumOptions options;
        ASSERT_OK(options.parse(BSON("commitQuorum" << 1)));
        ASSERT_TRUE(options.toBSON().woCompare(BSON("commitQuorum" << 1)) == 0);
    }

    {
        CommitQuorumOptions options;
        ASSERT_OK(options.parse(BSON("commitQuorum"
                                     << "someTag")));
        ASSERT_TRUE(options.toBSON().woCompare(BSON("commitQuorum"
                                                    << "someTag")) == 0);
    }

    {
        // Strings take precedence over numbers.
        CommitQuorumOptions options;
        options.mode = "majority";
        options.numNodes = 5;
        ASSERT_TRUE(options.toBSON().woCompare(BSON("commitQuorum"
                                                    << "majority")) == 0);
    }
}

}  // namespace
}  // namespace mongo

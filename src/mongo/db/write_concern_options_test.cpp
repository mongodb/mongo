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

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnEmptyDocument) {
    auto status = WriteConcernOptions::parse({}).getStatus();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("write concern object cannot be empty", status.reason());
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnInvalidJValue) {
    auto status = WriteConcernOptions::parse(BSON("j"
                                                  << "abc"))
                      .getStatus();
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnInvalidFSyncValue) {
    auto status = WriteConcernOptions::parse(BSON("fsync"
                                                  << "abc"))
                      .getStatus();
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseIfBothJAndFSyncAreTrue) {
    auto status = WriteConcernOptions::parse(BSON("j" << true << "fsync" << true)).getStatus();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("fsync and j options cannot be used together", status.reason());
}

TEST(WriteConcernOptionsTest, ParseSetsSyncModeToJournelIfJIsTrue) {
    auto sw = WriteConcernOptions::parse(BSON("j" << true));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::JOURNAL == options.syncMode);
    ASSERT_EQUALS(1, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseSetsSyncModeToFSyncIfFSyncIsTrue) {
    auto sw = WriteConcernOptions::parse(BSON("fsync" << true));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::FSYNC == options.syncMode);
    ASSERT_EQUALS(1, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseSetsSyncModeToNoneIfJIsFalse) {
    auto sw = WriteConcernOptions::parse(BSON("j" << false));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::NONE == options.syncMode);
    ASSERT_EQUALS(1, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseLeavesSyncModeAsUnsetIfFSyncIsFalse) {
    auto sw = WriteConcernOptions::parse(BSON("fsync" << false));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseIfWIsNotNumberOrStringOrObject) {
    auto status = WriteConcernOptions::parse(BSON("w" << true)).getStatus();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_STRING_CONTAINS(status.reason(), "w has to be a number, string, or object");
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseIfWIsNegativeOrExceedsMaxMembers) {
    auto st1 = WriteConcernOptions::parse(BSON("w" << 123)).getStatus();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, st1);

    auto st2 = WriteConcernOptions::parse(BSON("w" << -1)).getStatus();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, st2);
}

TEST(WriteConcernOptionsTest, ParseSetsWNumNodesIfWIsANumber) {
    auto sw = WriteConcernOptions::parse(BSON("w" << 3));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(3, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseSetsWTimeoutToZeroIfWTimeoutIsNotANumber) {
    auto sw = WriteConcernOptions::parse(BSON("wtimeout"
                                              << "abc"));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseWTimeoutAsNumber) {
    auto sw = WriteConcernOptions::parse(BSON("wtimeout" << 123));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(Milliseconds{123}, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseWTimeoutAsNaNDouble) {
    const double nan = std::nan("1");
    auto sw = WriteConcernOptions::parse(BSON("wtimeout" << nan));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseWTimeoutAsDoubleLargerThanInt) {
    // Set wtimeout to a double with value larger than INT_MAX.
    auto sw = WriteConcernOptions::parse(BSON("wtimeout" << 2999999999.0));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(options.wTimeout, Milliseconds{2999999999});
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnUnknownField) {
    auto status = WriteConcernOptions::parse(BSON("x" << 123)).getStatus();
    ASSERT_NOT_OK(status);
}

void _testIgnoreWriteConcernField(const char* fieldName) {
    auto sw = WriteConcernOptions::parse(BSON(fieldName << 1));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, stdx::get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}
TEST(WriteConcernOptionsTest, ParseIgnoresSpecialFields) {
    _testIgnoreWriteConcernField("wElectionId");
    _testIgnoreWriteConcernField("wOpTime");
    _testIgnoreWriteConcernField("getLastError");
}

TEST(WriteConcernOptionsTest, ParseWithTags) {
    auto status = WriteConcernOptions::parse(BSON("w" << BSON("abc"
                                                              << "def")))
                      .getStatus();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_STRING_CONTAINS(status.reason(),
                           "tags must be a single level document with only number values");

    status = WriteConcernOptions::parse(BSON("w" << BSONObj())).getStatus();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_STRING_CONTAINS(status.reason(), "tagged write concern requires tags");

    auto sw = WriteConcernOptions::parse(BSON("w" << BSON("abc" << 1)));
    ASSERT_OK(sw.getStatus());

    WriteConcernOptions wc = sw.getValue();
    ASSERT_EQ(wc.wTimeout, WriteConcernOptions::kNoTimeout);
    ASSERT_TRUE(wc.needToWaitForOtherNodes());

    auto tags = stdx::get<WTags>(wc.w);
    ASSERT(tags == (WTags{{"abc", 1}}));
    ASSERT_BSONOBJ_EQ(wc.toBSON(), BSON("w" << BSON("abc" << 1) << "wtimeout" << 0));

    auto wc2 = uassertStatusOK(WriteConcernOptions::parse(BSON("w" << BSON("abc" << 1))));
    ASSERT(wc == wc2);
    auto wc3 = uassertStatusOK(WriteConcernOptions::parse(BSON("w" << BSON("def" << 1))));
    ASSERT(wc != wc3);
    auto wc4 = uassertStatusOK(WriteConcernOptions::parse(BSON("w"
                                                               << "majority")));
    ASSERT(wc != wc4);
    auto wc5 = uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 2)));
    ASSERT(wc != wc5);
}

}  // namespace
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/write_concern_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cmath>

#include <absl/container/flat_hash_map.h>

namespace mongo {
namespace {

TEST(WriteConcernOptionsTest, WriteConcernOptionsTimeout) {
    WriteConcernOptions::Timeout five{Milliseconds{5}}, ten{Milliseconds{10}};

    ASSERT_LT(WriteConcernOptions::kNoWaiting, five);
    ASSERT_LT(WriteConcernOptions::kNoWaiting, ten);
    ASSERT_LT(WriteConcernOptions::kNoWaiting, WriteConcernOptions::kNoTimeout);
    ASSERT_GT(WriteConcernOptions::kNoTimeout, five);
    ASSERT_GT(WriteConcernOptions::kNoTimeout, ten);
    ASSERT_LT(five, ten);
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnEmptyDocument) {
    auto status = WriteConcernOptions::parse({}).getStatus();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("write concern object cannot be empty", status.reason());
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnInvalidJValue) {
    auto status = WriteConcernOptions::parse(BSON("j" << "abc")).getStatus();
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status);
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnInvalidFSyncValue) {
    auto status = WriteConcernOptions::parse(BSON("fsync" << "abc")).getStatus();
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
    ASSERT_EQUALS(1, get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseSetsSyncModeToFSyncIfFSyncIsTrue) {
    auto sw = WriteConcernOptions::parse(BSON("fsync" << true));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::FSYNC == options.syncMode);
    ASSERT_EQUALS(1, get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseSetsSyncModeToNoneIfJIsFalse) {
    auto sw = WriteConcernOptions::parse(BSON("j" << false));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::NONE == options.syncMode);
    ASSERT_EQUALS(1, get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseLeavesSyncModeAsUnsetIfFSyncIsFalse) {
    auto sw = WriteConcernOptions::parse(BSON("fsync" << false));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, get<int64_t>(options.w));
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
    ASSERT_EQUALS(3, get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseSetsWTimeoutToZeroIfWTimeoutIsNotANumber) {
    auto sw = WriteConcernOptions::parse(BSON("wtimeout" << "abc"));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseWTimeoutAsNumber) {
    auto sw = WriteConcernOptions::parse(BSON("wtimeout" << 123));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, get<int64_t>(options.w));
    ASSERT_EQUALS(Milliseconds{123}, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseWTimeoutAsNaNDouble) {
    const double nan = std::nan("1");
    auto sw = WriteConcernOptions::parse(BSON("wtimeout" << nan));
    ASSERT_OK(sw.getStatus());
    WriteConcernOptions options = sw.getValue();
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseWTimeoutAsDoubleLargerThanIntFails) {
    // Set wtimeout to a double with value larger than INT_MAX.
    auto sw = WriteConcernOptions::parse(BSON("wtimeout" << 2999999999.0));
    auto status = sw.getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
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
    ASSERT_EQUALS(1, get<int64_t>(options.w));
    ASSERT_EQUALS(WriteConcernOptions::kNoTimeout, options.wTimeout);
}
TEST(WriteConcernOptionsTest, ParseIgnoresSpecialFields) {
    _testIgnoreWriteConcernField("wElectionId");
    _testIgnoreWriteConcernField("wOpTime");
    _testIgnoreWriteConcernField("getLastError");
}

TEST(WriteConcernOptionsTest, ParseWithTags) {
    auto status = WriteConcernOptions::parse(BSON("w" << BSON("abc" << "def"))).getStatus();
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

    auto tags = get<WTags>(wc.w);
    ASSERT(tags == (WTags{{"abc", 1}}));
    ASSERT_BSONOBJ_EQ(wc.toBSON(), BSON("w" << BSON("abc" << 1) << "wtimeout" << 0));

    auto wc2 = uassertStatusOK(WriteConcernOptions::parse(BSON("w" << BSON("abc" << 1))));
    ASSERT(wc == wc2);
    auto wc3 = uassertStatusOK(WriteConcernOptions::parse(BSON("w" << BSON("def" << 1))));
    ASSERT(wc != wc3);
    auto wc4 = uassertStatusOK(WriteConcernOptions::parse(BSON("w" << "majority")));
    ASSERT(wc != wc4);
    auto wc5 = uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 2)));
    ASSERT(wc != wc5);
}

TEST(WriteConcernOptionsTest, ParseWithWNan) {
    auto sw = WriteConcernOptions::parse(BSON("w" << std::nan("1")));
    auto status = sw.getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "w cannot be NaN");
}

}  // namespace
}  // namespace mongo

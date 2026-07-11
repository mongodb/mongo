// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/get_status_from_command_result.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/rpc/get_status_from_command_result_write_util.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo {

namespace {
Status statusFor(const std::string& json) {
    return getStatusFromCommandResult(fromjson(json));
}
}  // namespace

TEST(GetStatusFromCommandResult, Ok) {
    ASSERT_OK(statusFor("{ok: 1.0}"));
}

TEST(GetStatusFromCommandResult, OkIgnoresCode) {
    ASSERT_OK(statusFor("{ok: 1.0, code: 12345, errmsg: 'oh no!'}"));
}

TEST(GetStatusFromCommandResult, SimpleBad) {
    const auto status = statusFor("{ok: 0.0, code: 12345, errmsg: 'oh no!'}");
    ASSERT_EQ(status, ErrorCodes::duplicateCodeForTest(12345));
    ASSERT_EQ(status.reason(), "oh no!");
}

TEST(GetStatusFromCommandResult, SimpleNoCode) {
    const auto status = statusFor("{ok: 0.0, errmsg: 'oh no!'}");
    ASSERT_EQ(status, ErrorCodes::UnknownError);
    ASSERT_EQ(status.reason(), "oh no!");
    ASSERT(!status.extraInfo());
}

TEST(GetStatusFromCommandResult, ExtraInfoParserFails) {
    const auto status = statusFor("{ok: 0.0, code: 236, errmsg: 'oh no!', data: 123}");
    ASSERT_EQ(status, ErrorCodes::duplicateCodeForTest(40681));
    ASSERT(!status.extraInfo());
}

TEST(GetStatusFromCommandResult, ExtraInfoParserSucceeds) {
    ErrorExtraInfoExample::EnableParserForTest whenInScope;
    const auto status = statusFor("{ok: 0.0, code: 236, errmsg: 'oh no!', data: 123}");
    ASSERT_EQ(status, ErrorCodes::ForTestingErrorExtraInfo);
    ASSERT_EQ(status.reason(), "oh no!");
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<ErrorExtraInfoExample>());
    ASSERT_EQ(status.extraInfo<ErrorExtraInfoExample>()->data, 123);
}

TEST(GetStatusFromCommandResult, BulkWriteResponseSucceeds) {
    ASSERT_OK(getFirstWriteErrorStatusFromBulkWriteResult(
        fromjson("{ok: 1.0, cursor: {id: 0, firstBatch: [{ok: 1.0}, {ok: 1.0}]}}")));
}

TEST(GetStatusFromCommandResult, BulkWriteResponseFails) {
    ErrorExtraInfoExample::EnableParserForTest whenInScope;
    auto status = getFirstWriteErrorStatusFromBulkWriteResult(
        fromjson(("{ok: 1.0, cursor: {id: 0, firstBatch: [{ok: 1.0}, {ok: 0.0, code: 236, errmsg: "
                  "'oh no!', data: 123}]}}")));
    ASSERT_EQ(status, ErrorCodes::ForTestingErrorExtraInfo);
    ASSERT_EQ(status.reason(), "oh no!");
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<ErrorExtraInfoExample>());
    ASSERT_EQ(status.extraInfo<ErrorExtraInfoExample>()->data, 123);
}

}  // namespace mongo

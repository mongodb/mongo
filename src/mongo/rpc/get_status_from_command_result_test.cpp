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
#include "mongo/db/json.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"
#include "mongo/unittest/unittest_helpers.h"

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

}  // namespace mongo

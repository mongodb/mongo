/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/traces/traceparent.h"

#include "mongo/base/error_codes.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::otel::traces {
namespace {

struct AcceptCase {
    std::string_view name;
    std::string_view value;
};

class TraceparentAccepts : public unittest::Test, public testing::WithParamInterface<AcceptCase> {};

TEST_P(TraceparentAccepts, IsOk) {
    EXPECT_EQ(validateW3CTraceparent(GetParam().value), Status::OK());
}

INSTANTIATE_TEST_SUITE_P(
    TraceparentValidation,
    TraceparentAccepts,
    testing::Values(
        AcceptCase{"ValidTraceparent1", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"},
        AcceptCase{"ValidTraceparent2", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-ff"},
        AcceptCase{"ValidTraceparent3", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00"}),
    [](const testing::TestParamInfo<AcceptCase>& info) { return std::string{info.param.name}; });

struct RejectCase {
    std::string_view name;
    std::string_view value;
    std::string_view expectedMessage;
};

class TraceparentRejects : public unittest::Test, public testing::WithParamInterface<RejectCase> {};

TEST_P(TraceparentRejects, FailsWithMessage) {
    const auto& param = GetParam();
    auto status = validateW3CTraceparent(param.value);
    EXPECT_EQ(status.code(), ErrorCodes::BadValue) << "value: " << param.value;
    EXPECT_THAT(
        validateW3CTraceparent(GetParam().value),
        unittest::match::StatusIs(ErrorCodes::BadValue, testing::HasSubstr(param.expectedMessage)));
}

constexpr auto kBadLength = "traceparent must be exactly 55 characters";
constexpr auto kBadDelimiter = "traceparent fields must be delimited by '-'";

INSTANTIATE_TEST_SUITE_P(
    TraceparentValidation,
    TraceparentRejects,
    testing::Values(
        RejectCase{"Empty", "", kBadLength},
        RejectCase{
            "TooShort", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0", kBadLength},
        RejectCase{
            "TooLong", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0100", kBadLength},
        RejectCase{
            "TrailingData", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-", kBadLength},
        RejectCase{"VersionTooLong",
                   "000-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
                   kBadLength},
        RejectCase{"VersionTooShort",
                   "0-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
                   kBadLength},
        RejectCase{"TraceIdTooLong",
                   "00-4bf92f3577b34da6a3ce929d0e0e47366-00f067aa0ba902b7-01",
                   kBadLength},
        RejectCase{"TraceIdTooShort",
                   "00-4bf92f3577b34da6a3ce929d0e0e473-00f067aa0ba902b7-01",
                   kBadLength},
        RejectCase{"ParentIdTooLong",
                   "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b77-01",
                   kBadLength},
        RejectCase{"ParentIdTooShort",
                   "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b-01",
                   kBadLength},
        RejectCase{
            "FlagsTooLong", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-011", kBadLength},
        RejectCase{
            "FlagsTooShort", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-1", kBadLength},
        RejectCase{"VersionTooLongKeepingTotalLength",
                   "004-bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
                   kBadDelimiter},
        RejectCase{"VersionTooShortKeepingTotalLength",
                   "0-04bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
                   "traceparent version must be two lowercase hexadecimal digits"},
        RejectCase{"TraceIdTooLongKeepingTotalLength",
                   "00-4bf92f3577b34da6a3ce929d0e0e47360-0f067aa0ba902b7-01",
                   kBadDelimiter},
        RejectCase{"TraceIdTooShortKeepingTotalLength",
                   "00-4bf92f3577b34da6a3ce929d0e0e473-600f067aa0ba902b7-01",
                   "traceparent trace-id must be 32 lowercase hexadecimal digits"},
        RejectCase{"ParentIdTooLongKeepingTotalLength",
                   "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b70-1",
                   kBadDelimiter},
        RejectCase{"ParentIdTooShortKeepingTotalLength",
                   "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b-701",
                   "traceparent parent-id must be 16 lowercase hexadecimal digits"},
        RejectCase{"UppercaseHex",
                   "00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01",
                   "traceparent trace-id must be 32 lowercase hexadecimal digits"},
        RejectCase{"NonHexCharacter",
                   "00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01",
                   "traceparent trace-id must be 32 lowercase hexadecimal digits"},
        RejectCase{"ForbiddenVersionFf",
                   "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
                   "traceparent version 'ff' is forbidden by the W3C spec"},
        RejectCase{"AllZeroTraceId",
                   "00-00000000000000000000000000000000-00f067aa0ba902b7-01",
                   "traceparent trace-id must not be all zeroes"},
        RejectCase{"AllZeroParentId",
                   "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01",
                   "traceparent parent-id must not be all zeroes"},
        RejectCase{"MissingDelimiter",
                   "000-bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
                   kBadDelimiter}),
    [](const testing::TestParamInfo<RejectCase>& info) { return std::string{info.param.name}; });

}  // namespace
}  // namespace mongo::otel::traces

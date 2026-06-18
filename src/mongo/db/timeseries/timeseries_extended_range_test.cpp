/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/timeseries/timeseries_extended_range.h"

#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(TimeseriesExtendedRangeSupport, DateOutsideStandardRange) {
    Date_t minStandard = Date_t::fromDurationSinceEpoch(Milliseconds(0));
    Date_t maxStandard = Date_t::fromDurationSinceEpoch(Seconds((1LL << 31) - 1));

    Date_t extendedLow = Date_t::fromDurationSinceEpoch(Milliseconds(-1));
    Date_t extendedHigh = Date_t::fromDurationSinceEpoch(Seconds(1LL << 31));

    EXPECT_FALSE(timeseries::dateOutsideStandardRange(minStandard));
    EXPECT_FALSE(timeseries::dateOutsideStandardRange(maxStandard));

    EXPECT_TRUE(timeseries::dateOutsideStandardRange(extendedLow));
    EXPECT_TRUE(timeseries::dateOutsideStandardRange(extendedHigh));
}

TEST(TimeseriesExtendedRangeSupport, BucketsHaveDateOutsideStandardRange) {
    TimeseriesOptions options;
    options.setTimeField("time"sv);

    const std::vector<InsertStatement> standardRange = {
        {0,
         mongo::fromjson(
             R"({"control": {"min": {"time": {"$date": "1970-01-01T00:00:00.000Z"}}}})")},
        {1,
         mongo::fromjson(
             R"({"control": {"min": {"time": {"$date": "2000-01-01T00:00:00.000Z"}}}})")},
        {2,
         mongo::fromjson(
             R"({"control": {"min": {"time": {"$date": "2038-01-01T00:00:00.000Z"}}}})")},
    };

    const std::vector<InsertStatement> extendedRangeLow = {
        // Dates before Unix epoch have to be hard-coded as seconds-offset rather than date strings
        {3,
         // -((1 << 31) + 1) seconds
         mongo::fromjson(R"({"control": {"min": {"time": {"$date": -2147483649000}}}})")},
        {4, mongo::fromjson(R"({"control": {"min": {"time": {"$date": -1000}}}})")},
    };

    const std::vector<InsertStatement> extendedRangeHigh = {
        {5,
         mongo::fromjson(
             R"({"control": {"min": {"time": {"$date": "2039-01-01T00:00:00.000Z"}}}})")},
        {6,
         mongo::fromjson(
             R"({"control": {"min": {"time": {"$date": "2110-01-01T00:00:00.000Z"}}}})")},
    };

    const std::vector<InsertStatement> extendedRangeMillisecondsLow = {
        {7, mongo::fromjson(R"({"control": {"min": {"time": {"$date": -999}}}})")},
    };

    // This date is one millisecond after the maximum (the largest 32 bit integer)
    // number of seconds since the epoch.
    const std::vector<InsertStatement> extendedRangeMillisecondsHigh = {
        {8,
         mongo::fromjson(
             R"({"control": {"min": {"time": {"$date": "2038-01-19T03:14:07.001Z"}}}})")},
    };

    EXPECT_FALSE(timeseries::bucketsHaveDateOutsideStandardRange(options, standardRange));
    EXPECT_TRUE(timeseries::bucketsHaveDateOutsideStandardRange(options, extendedRangeLow));
    EXPECT_TRUE(timeseries::bucketsHaveDateOutsideStandardRange(options, extendedRangeHigh));
    EXPECT_TRUE(
        timeseries::bucketsHaveDateOutsideStandardRange(options, extendedRangeMillisecondsHigh));

    std::vector<InsertStatement> mixed = {standardRange[0], standardRange[1], extendedRangeLow[0]};
    EXPECT_TRUE(timeseries::bucketsHaveDateOutsideStandardRange(options, mixed));

    std::vector<InsertStatement> mixedWithMilliseconds = {standardRange[0],
                                                          extendedRangeMillisecondsLow[0]};
    EXPECT_TRUE(timeseries::bucketsHaveDateOutsideStandardRange(options, mixedWithMilliseconds));
}

TEST(TimeseriesExtendedRangeSupport, BucketSplitAcrossEpochalypse) {
    const std::vector<InsertStatement> inserts = {InsertStatement(1, mongo::fromjson(R"BSON({
                "control": {
                    "min": {
                        "time": {"$date": "2038-01-19T03:14:06Z"}
                    },
                    "max": {
                        "time": {"$date": "2038-01-19T03:14:10Z"}
                    }
                },
                "data": "x"
            })BSON"))};
    const TimeseriesOptions options("time");
    EXPECT_FALSE(timeseries::bucketsHaveDateOutsideStandardRange(options, inserts));
}

using OIDExtendedRangeTestParams = std::pair<std::string_view, bool>;
class OIDExtendedRangeTests : public testing::TestWithParam<std::pair<std::string_view, bool>> {};

TEST_P(OIDExtendedRangeTests, OIDHasExtendedRangeTimeComponent) {
    const auto [oidSd, isExtendedRange] = GetParam();
    const auto oid = OID::createFromString(oidSd);
    EXPECT_EQ(isExtendedRange, timeseries::oidHasExtendedRangeTime(oid))
        << fmt::format("Timestamp component was {0:d}, ({0:#010x})", oid.getTimestamp());
}

INSTANTIATE_TEST_SUITE_P(
    OIDExtendedRange,
    OIDExtendedRangeTests,
    testing::Values(OIDExtendedRangeTestParams{"e980f6cc8bee049fcc1c4d88"sv, true},
                    OIDExtendedRangeTestParams{"61be04541ad72e8d5d257550"sv, false},
                    OIDExtendedRangeTestParams{"091cd800d486dbad9374ac77"sv, false}));

}  // namespace
}  // namespace mongo

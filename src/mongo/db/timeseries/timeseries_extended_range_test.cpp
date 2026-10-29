// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

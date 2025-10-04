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

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

TEST(TimeseriesExtendedRangeSupport, DateOutsideStandardRange) {
    Date_t minStandard = Date_t::fromDurationSinceEpoch(Milliseconds(0));
    Date_t maxStandard = Date_t::fromDurationSinceEpoch(Seconds((1LL << 31) - 1));

    Date_t extendedLow = Date_t::fromDurationSinceEpoch(Milliseconds(-1));
    Date_t extendedHigh = Date_t::fromDurationSinceEpoch(Seconds(1LL << 31));

    ASSERT_FALSE(timeseries::dateOutsideStandardRange(minStandard));
    ASSERT_FALSE(timeseries::dateOutsideStandardRange(maxStandard));

    ASSERT_TRUE(timeseries::dateOutsideStandardRange(extendedLow));
    ASSERT_TRUE(timeseries::dateOutsideStandardRange(extendedHigh));
}

TEST(TimeseriesExtendedRangeSupport, BucketsHaveDateOutsideStandardRange) {
    TimeseriesOptions options;
    options.setTimeField("time"_sd);

    std::vector<InsertStatement> standardRange = {
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

    std::vector<InsertStatement> extendedRangeLow = {
        // Dates before Unix epoch have to be hard-coded as seconds-offset rather than date strings
        {3,
         // -((1 << 31) + 1) seconds
         mongo::fromjson(R"({"control": {"min": {"time": {"$date": -2147483649000}}}})")},
        {4, mongo::fromjson(R"({"control": {"min": {"time": {"$date": -1000}}}})")},
    };

    std::vector<InsertStatement> extendedRangeHigh = {
        {5,
         mongo::fromjson(
             R"({"control": {"min": {"time": {"$date": "2039-01-01T00:00:00.000Z"}}}})")},
        {6,
         mongo::fromjson(
             R"({"control": {"min": {"time": {"$date": "2110-01-01T00:00:00.000Z"}}}})")},
    };

    std::vector<InsertStatement> extendedRangeMillisecondsLow = {
        {7, mongo::fromjson(R"({"control": {"min": {"time": {"$date": -999}}}})")},
    };

    // This date is one millisecond after the maximum (the largest 32 bit integer)
    // number of seconds since the epoch.
    std::vector<InsertStatement> extendedRangeMillisecondsHigh = {
        {8,
         mongo::fromjson(
             R"({"control": {"min": {"time": {"$date": "2038-01-19T03:14:07.001Z"}}}})")},
    };

    ASSERT_FALSE(timeseries::bucketsHaveDateOutsideStandardRange(
        options, standardRange.begin(), standardRange.end()));
    ASSERT_TRUE(timeseries::bucketsHaveDateOutsideStandardRange(
        options, extendedRangeLow.begin(), extendedRangeLow.end()));
    ASSERT_TRUE(timeseries::bucketsHaveDateOutsideStandardRange(
        options, extendedRangeHigh.begin(), extendedRangeHigh.end()));
    ASSERT_TRUE(timeseries::bucketsHaveDateOutsideStandardRange(
        options, extendedRangeMillisecondsHigh.begin(), extendedRangeMillisecondsHigh.end()));

    std::vector<InsertStatement> mixed = {standardRange[0], standardRange[1], extendedRangeLow[0]};
    ASSERT_TRUE(
        timeseries::bucketsHaveDateOutsideStandardRange(options, mixed.begin(), mixed.end()));

    std::vector<InsertStatement> mixedWithMilliseconds = {standardRange[0],
                                                          extendedRangeMillisecondsLow[0]};
    ASSERT_TRUE(timeseries::bucketsHaveDateOutsideStandardRange(
        options, mixedWithMilliseconds.begin(), mixedWithMilliseconds.end()));
}

}  // namespace
}  // namespace mongo

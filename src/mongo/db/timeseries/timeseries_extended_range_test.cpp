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

#include "mongo/bson/json.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(TimeseriesExtendedRangeSupport, DateOutsideStandardRange) {
    Date_t minStandard = Date_t::fromDurationSinceEpoch(Seconds(0));
    Date_t maxStandard = Date_t::fromDurationSinceEpoch(Seconds((1LL << 31) - 1));

    Date_t extendedLow = Date_t::fromDurationSinceEpoch(Seconds(-1));
    Date_t extendedHigh = Date_t::fromDurationSinceEpoch(Seconds(1LL << 31));

    ASSERT_FALSE(timeseries::dateOutsideStandardRange(minStandard));
    ASSERT_FALSE(timeseries::dateOutsideStandardRange(maxStandard));

    ASSERT_TRUE(timeseries::dateOutsideStandardRange(extendedLow));
    ASSERT_TRUE(timeseries::dateOutsideStandardRange(extendedHigh));
}

TEST(TimeseriesExtendedRangeSupport, MeasurementsHaveDateOutsideStandardRange) {
    TimeseriesOptions options;
    options.setTimeField("time"_sd);

    std::vector<BSONObj> standardRange = {
        mongo::fromjson(R"({"time": {"$date": "1970-01-01T00:00:00.000Z"}})"),
        mongo::fromjson(R"({"time": {"$date": "2000-01-01T00:00:00.000Z"}})"),
        mongo::fromjson(R"({"time": {"$date": "2038-01-01T00:00:00.000Z"}})"),
    };

    std::vector<BSONObj> extendedRangeLow = {
        // Dates before Unix epoch have to be hard-coded as seconds-offset rather than date strings
        mongo::fromjson(R"({"time": {"$date": -2147483649000}})"),  // -((1 << 31) + 1) seconds
        mongo::fromjson(R"({"time": {"$date": -1000}})"),
    };

    std::vector<BSONObj> extendedRangeHigh = {
        mongo::fromjson(R"({"time": {"$date": "2039-01-01T00:00:00.000Z"}})"),
        mongo::fromjson(R"({"time": {"$date": "2110-01-01T00:00:00.000Z"}})"),
    };

    ASSERT_FALSE(timeseries::measurementsHaveDateOutsideStandardRange(options, standardRange));
    ASSERT_TRUE(timeseries::measurementsHaveDateOutsideStandardRange(options, extendedRangeLow));
    ASSERT_TRUE(timeseries::measurementsHaveDateOutsideStandardRange(options, extendedRangeHigh));

    std::vector<BSONObj> mixed = {standardRange[0], standardRange[1], extendedRangeLow[0]};
    ASSERT_TRUE(timeseries::measurementsHaveDateOutsideStandardRange(options, mixed));
}

}  // namespace
}  // namespace mongo

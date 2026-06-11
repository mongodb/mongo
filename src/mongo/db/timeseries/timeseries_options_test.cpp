/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/timeseries/timeseries_options.h"

#include "mongo/base/string_data.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <string>
#include <tuple>
#include <vector>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/posix_time/time_parsers.hpp>
#include <boost/move/utility_core.hpp>


namespace mongo {

auto createTimeseriesOptionsWithGranularity(BucketGranularityEnum granularity) {
    auto options = TimeseriesOptions{};
    options.setGranularity(granularity);
    return options;
}

auto createTimeseriesOptionsWithBucketMaxSpanAndRoundingSeconds(
    const boost::optional<std::int32_t>& bucketMaxSpanSeconds,
    const boost::optional<std::int32_t>& bucketRoundingSeconds) {
    auto options = TimeseriesOptions{};
    options.setBucketMaxSpanSeconds(bucketMaxSpanSeconds);
    options.setBucketRoundingSeconds(bucketRoundingSeconds);
    return options;
}

TEST(TimeseriesOptionsTest, RoundTimestampToGranularity) {
    TimeseriesOptions optionsSeconds =
        createTimeseriesOptionsWithGranularity(BucketGranularityEnum::Seconds);
    TimeseriesOptions optionsMinutes =
        createTimeseriesOptionsWithGranularity(BucketGranularityEnum::Minutes);
    TimeseriesOptions optionsHours =
        createTimeseriesOptionsWithGranularity(BucketGranularityEnum::Hours);
    std::vector<std::tuple<TimeseriesOptions, std::string, std::string>> testCases{
        {optionsSeconds, "2021-01-01T00:00:00.000Z", "2021-01-01T00:00:00.000Z"},
        {optionsSeconds, "2021-01-01T00:00:00.001Z", "2021-01-01T00:00:00.000Z"},
        {optionsSeconds, "2021-01-01T00:00:15.555Z", "2021-01-01T00:00:00.000Z"},
        {optionsSeconds, "2021-01-01T00:00:30.555Z", "2021-01-01T00:00:00.000Z"},
        {optionsSeconds, "2021-01-01T00:00:45.555Z", "2021-01-01T00:00:00.000Z"},
        {optionsSeconds, "2021-01-01T00:00:59.999Z", "2021-01-01T00:00:00.000Z"},

        {optionsMinutes, "2021-01-01T00:00:00.000Z", "2021-01-01T00:00:00.000Z"},
        {optionsMinutes, "2021-01-01T00:00:00.001Z", "2021-01-01T00:00:00.000Z"},
        {optionsMinutes, "2021-01-01T00:15:00.000Z", "2021-01-01T00:00:00.000Z"},
        {optionsMinutes, "2021-01-01T00:30:00.000Z", "2021-01-01T00:00:00.000Z"},
        {optionsMinutes, "2021-01-01T00:45:00.000Z", "2021-01-01T00:00:00.000Z"},
        {optionsMinutes, "2021-01-01T00:59:59.999Z", "2021-01-01T00:00:00.000Z"},

        {optionsHours, "2021-01-01T00:00:00.000Z", "2021-01-01T00:00:00.000Z"},
        {optionsHours, "2021-01-01T00:00:00.001Z", "2021-01-01T00:00:00.000Z"},
        {optionsHours, "2021-01-01T06:00:00.000Z", "2021-01-01T00:00:00.000Z"},
        {optionsHours, "2021-01-01T12:00:00.000Z", "2021-01-01T00:00:00.000Z"},
        {optionsHours, "2021-01-01T18:00:00.000Z", "2021-01-01T00:00:00.000Z"},
        {optionsHours, "2021-01-01T23:59:59.999Z", "2021-01-01T00:00:00.000Z"},
    };

    for (const auto& [granularity, input, expectedOutput] : testCases) {
        auto inputDate = dateFromISOString(input);
        ASSERT_OK(inputDate);
        auto roundedDate =
            timeseries::roundTimestampToGranularity(inputDate.getValue(), granularity);
        EXPECT_EQ(dateToISOStringUTC(roundedDate), expectedOutput);
    }
}

TEST(TimeseriesOptionsTest, RoundTimestampBySeconds) {
    std::vector<std::tuple<long long, std::string, std::string>> testCases{
        {60l, "2024-08-08T00:00:00.000Z", "2024-08-08T00:00:00.000Z"},
        {60l, "2024-08-08T00:00:00.001Z", "2024-08-08T00:00:00.000Z"},
        {60l, "2024-08-08T00:00:15.555Z", "2024-08-08T00:00:00.000Z"},
        {60l, "2024-08-08T00:00:30.555Z", "2024-08-08T00:00:00.000Z"},
        {60l, "2024-08-08T00:00:45.555Z", "2024-08-08T00:00:00.000Z"},
        {60l, "2024-08-08T00:00:59.999Z", "2024-08-08T00:00:00.000Z"},

        {60l, "2024-08-08T05:04:00.000Z", "2024-08-08T05:04:00.000Z"},
        {60l, "2024-08-08T05:04:00.001Z", "2024-08-08T05:04:00.000Z"},
        {60l, "2024-08-08T05:04:15.555Z", "2024-08-08T05:04:00.000Z"},
        {60l, "2024-08-08T05:04:30.555Z", "2024-08-08T05:04:00.000Z"},
        {60l, "2024-08-08T05:04:45.555Z", "2024-08-08T05:04:00.000Z"},
        {60l, "2024-08-08T05:04:59.999Z", "2024-08-08T05:04:00.000Z"},

        {3600l, "2024-08-08T00:00:00.000Z", "2024-08-08T00:00:00.000Z"},
        {3600l, "2024-08-08T00:00:00.001Z", "2024-08-08T00:00:00.000Z"},
        {3600l, "2024-08-08T00:15:00.000Z", "2024-08-08T00:00:00.000Z"},
        {3600l, "2024-08-08T00:30:00.000Z", "2024-08-08T00:00:00.000Z"},
        {3600l, "2024-08-08T00:45:00.000Z", "2024-08-08T00:00:00.000Z"},
        {3600l, "2024-08-08T00:59:59.999Z", "2024-08-08T00:00:00.000Z"},

        {86400l, "2024-08-08T00:00:00.000Z", "2024-08-08T00:00:00.000Z"},
        {86400l, "2024-08-08T00:00:00.001Z", "2024-08-08T00:00:00.000Z"},
        {86400l, "2024-08-08T06:00:00.000Z", "2024-08-08T00:00:00.000Z"},
        {86400l, "2024-08-08T12:00:00.000Z", "2024-08-08T00:00:00.000Z"},
        {86400l, "2024-08-08T18:00:00.000Z", "2024-08-08T00:00:00.000Z"},
        {86400l, "2024-08-08T23:59:59.999Z", "2024-08-08T00:00:00.000Z"},
    };

    for (const auto& [roundingSeconds, input, expectedOutput] : testCases) {
        auto inputDate = dateFromISOString(input);
        ASSERT_OK(inputDate);
        auto roundedDate =
            timeseries::roundTimestampBySeconds(inputDate.getValue(), roundingSeconds);
        EXPECT_EQ(dateToISOStringUTC(roundedDate), expectedOutput);
    }
}

TEST(TimeseriesOptionsTest, ExtendedRangeRoundTimestamp) {
    std::vector<std::tuple<long long, std::string, std::string>> testCases{
        {60l, "1901-01-01T00:00:12.345", "1901-01-01T00:00:00"},
        {60l, "1901-01-01T00:04:12.345", "1901-01-01T00:04:00"},
        {60l, "1901-01-01T02:04:12.345", "1901-01-01T02:04:00"},

        {60l, "1969-01-01T00:00:12.345", "1969-01-01T00:00:00"},
        {60l, "1969-01-01T00:04:12.345", "1969-01-01T00:04:00"},
        {60l, "1969-01-01T02:04:12.345", "1969-01-01T02:04:00"},

        {60l, "2040-01-01T00:00:12.345", "2040-01-01T00:00:00"},
        {60l, "2040-01-01T00:04:12.345", "2040-01-01T00:04:00"},
        {60l, "2040-01-01T02:04:12.345", "2040-01-01T02:04:00"},

        {60l, "2108-01-01T00:00:12.345", "2108-01-01T00:00:00"},
        {60l, "2108-01-01T00:04:12.345", "2108-01-01T00:04:00"},
        {60l, "2108-01-01T02:04:12.345", "2108-01-01T02:04:00"},

        {3600l, "1901-01-01T00:00:12.345", "1901-01-01T00:00:00"},
        {3600l, "1901-01-01T00:04:12.345", "1901-01-01T00:00:00"},
        {3600l, "1901-01-01T02:04:12.345", "1901-01-01T02:00:00"},

        {3600l, "1969-01-01T00:00:12.345", "1969-01-01T00:00:00"},
        {3600l, "1969-01-01T00:04:12.345", "1969-01-01T00:00:00"},
        {3600l, "1969-01-01T02:04:12.345", "1969-01-01T02:00:00"},

        {3600l, "2040-01-01T00:00:12.345", "2040-01-01T00:00:00"},
        {3600l, "2040-01-01T00:04:12.345", "2040-01-01T00:00:00"},
        {3600l, "2040-01-01T02:04:12.345", "2040-01-01T02:00:00"},

        {3600l, "2108-01-01T00:00:12.345", "2108-01-01T00:00:00"},
        {3600l, "2108-01-01T00:04:12.345", "2108-01-01T00:00:00"},
        {3600l, "2108-01-01T02:04:12.345", "2108-01-01T02:00:00"},

        {86400l, "1901-01-01T00:00:12.345", "1901-01-01T00:00:00"},
        {86400l, "1901-01-01T00:04:12.345", "1901-01-01T00:00:00"},
        {86400l, "1901-01-01T02:04:12.345", "1901-01-01T00:00:00"},

        {86400l, "1969-01-01T00:00:12.345", "1969-01-01T00:00:00"},
        {86400l, "1969-01-01T00:04:12.345", "1969-01-01T00:00:00"},
        {86400l, "1969-01-01T02:04:12.345", "1969-01-01T00:00:00"},

        {86400l, "2040-01-01T00:00:12.345", "2040-01-01T00:00:00"},
        {86400l, "2040-01-01T00:04:12.345", "2040-01-01T00:00:00"},
        {86400l, "2040-01-01T02:04:12.345", "2040-01-01T00:00:00"},

        {86400l, "2108-01-01T00:00:12.345", "2108-01-01T00:00:00"},
        {86400l, "2108-01-01T00:04:12.345", "2108-01-01T00:00:00"},
        {86400l, "2108-01-01T02:04:12.345", "2108-01-01T00:00:00"},
    };

    // TODO SERVER-94228: Support ISO 8601 date parsing and formatting of dates prior to 1970.
    static constexpr auto epoch = boost::posix_time::ptime(boost::gregorian::date(1970, 1, 1));
    auto parse = [](const std::string& input) {
        auto ptime = boost::posix_time::from_iso_extended_string(input);
        return Date_t::fromMillisSinceEpoch((ptime - epoch).total_milliseconds());
    };
    auto format = [](Date_t date) {
        boost::posix_time::milliseconds ms(date.toMillisSinceEpoch());
        return boost::posix_time::to_iso_extended_string(epoch + ms);
    };

    for (const auto& [roundingSeconds, input, expectedOutput] : testCases) {
        Date_t inputDate = parse(input);
        auto roundedDate = timeseries::roundTimestampBySeconds(inputDate, roundingSeconds);
        // We should always round down
        EXPECT_LE(roundedDate, inputDate);
        // The rounding amount should be less than the rounding seconds
        EXPECT_LT((inputDate - roundedDate).count(), roundingSeconds * 1000);
        // Ensure that we've rounded to an even number according to our rounding seconds
        EXPECT_EQ(durationCount<Seconds>(roundedDate.toDurationSinceEpoch()) % roundingSeconds, 0);
        // Validate the expected output
        EXPECT_EQ(format(roundedDate), expectedOutput);
    }
}

TEST(TimeseriesOptionsTest, ExtendedRoundMilliTimestampBySeconds) {
    std::vector<std::tuple<long long, Date_t, Date_t>> testCases{
        {60l, Date_t::fromMillisSinceEpoch(-1), Date_t::fromMillisSinceEpoch(-60000)},
        {60l, Date_t::fromMillisSinceEpoch(-1000), Date_t::fromMillisSinceEpoch(-60000)},
        {60l, Date_t::fromMillisSinceEpoch(-1001), Date_t::fromMillisSinceEpoch(-60000)},
        {60l, Date_t::fromMillisSinceEpoch(-60000), Date_t::fromMillisSinceEpoch(-60000)},
        {60l, Date_t::fromMillisSinceEpoch(-60001), Date_t::fromMillisSinceEpoch(-120000)},
        {60l, Date_t::min(), Date_t::min()},

        {3600l, Date_t::fromMillisSinceEpoch(-1), Date_t::fromMillisSinceEpoch(-3600000)},
        {3600l, Date_t::fromMillisSinceEpoch(-1000), Date_t::fromMillisSinceEpoch(-3600000)},
        {3600l, Date_t::fromMillisSinceEpoch(-1001), Date_t::fromMillisSinceEpoch(-3600000)},
        {3600l, Date_t::fromMillisSinceEpoch(-3600000), Date_t::fromMillisSinceEpoch(-3600000)},
        {3600l, Date_t::fromMillisSinceEpoch(-3600001), Date_t::fromMillisSinceEpoch(-7200000)},
        {3600l, Date_t::min(), Date_t::min()},

        {86400l, Date_t::fromMillisSinceEpoch(-1), Date_t::fromMillisSinceEpoch(-86400000)},
        {86400l, Date_t::fromMillisSinceEpoch(-1000), Date_t::fromMillisSinceEpoch(-86400000)},
        {86400l, Date_t::fromMillisSinceEpoch(-1001), Date_t::fromMillisSinceEpoch(-86400000)},
        {86400l, Date_t::fromMillisSinceEpoch(-86400000), Date_t::fromMillisSinceEpoch(-86400000)},
        {86400l, Date_t::fromMillisSinceEpoch(-86400001), Date_t::fromMillisSinceEpoch(-172800000)},
        {86400l, Date_t::min(), Date_t::min()},
    };

    for (const auto& [roundingSeconds, input, expectedOutput] : testCases) {
        auto roundedDate = timeseries::roundTimestampBySeconds(input, roundingSeconds);
        EXPECT_EQ(roundedDate, expectedOutput);
    }
}

TEST(TimeseriesOptionsTest, CanUseFixedBucketOptimizations) {
    auto withFixedBucketing = [](TimeseriesOptions options) {
        options.setFixedBucketing(true);
        return options;
    };

    const auto optionsEqualAndNone =
        createTimeseriesOptionsWithBucketMaxSpanAndRoundingSeconds(boost::none, boost::none);
    const auto optionsEqualNotNone =
        createTimeseriesOptionsWithBucketMaxSpanAndRoundingSeconds(8675309, 8675309);
    const auto optionsMaxSpanAndNone =
        createTimeseriesOptionsWithBucketMaxSpanAndRoundingSeconds(42, boost::none);
    const auto optionsNoneAndRounding =
        createTimeseriesOptionsWithBucketMaxSpanAndRoundingSeconds(boost::none, 10019);
    const auto optionsValuesNotEqual =
        createTimeseriesOptionsWithBucketMaxSpanAndRoundingSeconds(1633, 77);

    // Flag off (default): always returns false regardless of options.
    for (const auto& opts : {withFixedBucketing(optionsEqualAndNone),
                             withFixedBucketing(optionsEqualNotNone),
                             withFixedBucketing(optionsMaxSpanAndNone),
                             withFixedBucketing(optionsNoneAndRounding),
                             withFixedBucketing(optionsValuesNotEqual),
                             optionsEqualAndNone,
                             optionsEqualNotNone,
                             optionsMaxSpanAndNone,
                             optionsNoneAndRounding,
                             optionsValuesNotEqual}) {
        EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(opts));
    }

    RAIIServerParameterControllerForTest flagController("featureFlagFixedBucketingOptimizations",
                                                        true);

    // Flag on: result depends on fixedBucketing field and maxSpan == rounding.
    EXPECT_TRUE(timeseries::canUseFixedBucketOptimizations(withFixedBucketing(optionsEqualAndNone)))
        << "BucketMaxSpanSeconds=none, BucketRoundingSeconds=none, "
        << "fixedBucketing=true implies buckets should be fixed.";

    EXPECT_TRUE(timeseries::canUseFixedBucketOptimizations(withFixedBucketing(optionsEqualNotNone)))
        << "BucketMaxSpanSeconds=value, BucketRoundingSeconds=value, "
        << "fixedBucketing=true implies buckets should be fixed.";

    EXPECT_FALSE(
        timeseries::canUseFixedBucketOptimizations(withFixedBucketing(optionsMaxSpanAndNone)))
        << "BucketMaxSpanSeconds=value, BucketRoundingSeconds=none, "
        << "fixedBucketing=true implies buckets should not be fixed.";

    EXPECT_FALSE(
        timeseries::canUseFixedBucketOptimizations(withFixedBucketing(optionsNoneAndRounding)))
        << "BucketMaxSpanSeconds=none, BucketRoundingSeconds=value, "
        << "fixedBucketing=true implies buckets should not be fixed.";

    EXPECT_FALSE(
        timeseries::canUseFixedBucketOptimizations(withFixedBucketing(optionsValuesNotEqual)))
        << "BucketMaxSpanSeconds=value1, BucketRoundingSeconds=value2, "
        << "fixedBucketing=true implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsEqualAndNone))
        << "BucketMaxSpanSeconds=none, BucketRoundingSeconds=none, "
        << "fixedBucketing unset implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsEqualNotNone))
        << "BucketMaxSpanSeconds=value, BucketRoundingSeconds=value, "
        << "fixedBucketing unset implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsMaxSpanAndNone))
        << "BucketMaxSpanSeconds=value, BucketRoundingSeconds=none, "
        << "fixedBucketing unset implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsNoneAndRounding))
        << "BucketMaxSpanSeconds=none, BucketRoundingSeconds=value, "
        << "fixedBucketing unset implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsValuesNotEqual))
        << "BucketMaxSpanSeconds=value1, BucketRoundingSeconds=value2, "
        << "fixedBucketing unset implies buckets should not be fixed.";
}

TEST(TimeseriesOptionsTest, OptionsAreEqualFixedBucketing) {
    auto makeOptions = [](boost::optional<bool> fixedBucketing) {
        TimeseriesOptions options{"time"};
        if (fixedBucketing.has_value()) {
            options.setFixedBucketing(*fixedBucketing);
        }
        return options;
    };

    const auto unset = makeOptions(boost::none);
    const auto enabled = makeOptions(true);
    const auto disabled = makeOptions(false);

    // Same values compare equal.
    EXPECT_TRUE(timeseries::optionsAreEqual(unset, unset));
    EXPECT_TRUE(timeseries::optionsAreEqual(enabled, enabled));
    EXPECT_TRUE(timeseries::optionsAreEqual(disabled, disabled));

    // Different values compare unequal. In particular `unset` is distinct from `false` — even
    // though `OptionalBool::operator bool()` would conflate them, optionsAreEqual treats all
    // three states (unset, false, true) as distinct.
    EXPECT_FALSE(timeseries::optionsAreEqual(unset, enabled));
    EXPECT_FALSE(timeseries::optionsAreEqual(unset, disabled));
    EXPECT_FALSE(timeseries::optionsAreEqual(enabled, disabled));
}

// Verify `applyTimeseriesOptionsModifications`'s `fixedBucketing` handling across the matrix of
// {unset, false, true} × {bucketing change, no change}: only `true + change` flips to false,
// all other combinations leave the field unchanged. Spot-check the `true → false` case for
// granularity changes at the end since they share the same code path.
TEST(TimeseriesOptionsTest, FixedBucketingHandling) {
    auto makeOptions = [](boost::optional<bool> fixedBucketing) {
        TimeseriesOptions options{"time"};
        options.setBucketMaxSpanSeconds(100);
        options.setBucketRoundingSeconds(100);
        if (fixedBucketing.has_value()) {
            options.setFixedBucketing(*fixedBucketing);
        }
        return options;
    };

    CollModTimeseries changeMod;
    changeMod.setBucketMaxSpanSeconds(200);
    changeMod.setBucketRoundingSeconds(200);

    // Same span/rounding values as the collection — no-op via numerical comparison.
    CollModTimeseries sameValuesMod;
    sameValuesMod.setBucketMaxSpanSeconds(100);
    sameValuesMod.setBucketRoundingSeconds(100);

    // unset + bucketing change → fixedBucketing stays unset.
    {
        auto res =
            timeseries::applyTimeseriesOptionsModifications(makeOptions(boost::none), changeMod);
        ASSERT_OK(res.getStatus());
        auto [newOpts, updated] = res.getValue();
        ASSERT_TRUE(updated);
        ASSERT_FALSE(newOpts.getFixedBucketing().has_value());
    }

    // false + bucketing change → fixedBucketing stays false.
    {
        auto res = timeseries::applyTimeseriesOptionsModifications(makeOptions(false), changeMod);
        ASSERT_OK(res.getStatus());
        auto [newOpts, updated] = res.getValue();
        ASSERT_TRUE(updated);
        ASSERT_TRUE(newOpts.getFixedBucketing().has_value());
        ASSERT_FALSE(newOpts.getFixedBucketing());
    }

    // true + bucketing change → fixedBucketing becomes false.
    {
        auto res = timeseries::applyTimeseriesOptionsModifications(makeOptions(true), changeMod);
        ASSERT_OK(res.getStatus());
        auto [newOpts, updated] = res.getValue();
        ASSERT_TRUE(updated);
        ASSERT_TRUE(newOpts.getFixedBucketing().has_value());
        ASSERT_FALSE(newOpts.getFixedBucketing());
    }

    // true + same bucketing values → fixedBucketing untouched, no update.
    {
        auto res =
            timeseries::applyTimeseriesOptionsModifications(makeOptions(true), sameValuesMod);
        ASSERT_OK(res.getStatus());
        auto [newOpts, updated] = res.getValue();
        ASSERT_FALSE(updated);
        ASSERT_TRUE(newOpts.getFixedBucketing().has_value());
        ASSERT_TRUE(newOpts.getFixedBucketing());
    }

    // Granularity changes go through the same code path as explicit span/rounding changes. Only
    // test the significant true → false case; the other cases (where fixedBucketing stays
    // unchanged) are already covered by the sub-blocks above.
    {
        TimeseriesOptions opts{"time"};
        opts.setGranularity(BucketGranularityEnum::Seconds);
        opts.setFixedBucketing(true);

        CollModTimeseries mod;
        mod.setGranularity(BucketGranularityEnum::Minutes);

        auto res = timeseries::applyTimeseriesOptionsModifications(opts, mod);
        ASSERT_OK(res.getStatus());
        auto [newOpts, updated] = res.getValue();
        ASSERT_TRUE(updated);
        ASSERT_TRUE(newOpts.getFixedBucketing().has_value());
        ASSERT_FALSE(newOpts.getFixedBucketing());
    }
}

TEST(TimeseriesOptionsTest, BSONColumnMemEstimationCalculations) {
    // The calculations for BSONColumn memory estimation in bson_validate.cpp rely on the defaults
    // for some server parameters. If these change, we also need to recalculate and potentially
    // adjust the memory threshold of the 'bsonMaxExpandedMemUsage' parameter.
    EXPECT_EQ(gTimeseriesBucketMinCount, 10);
}

}  // namespace mongo

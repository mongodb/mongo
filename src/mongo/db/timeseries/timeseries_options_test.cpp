// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/timeseries_options.h"

#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
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

    // Flag off: always returns false regardless of options.
    {
        unittest::ServerParameterGuard flagController("featureFlagFixedBucketingOptimizations",
                                                      false);
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
            EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(opts, false));
        }
    }

    // Flag on, no extended range data: result depends on fixedBucketing field and
    // maxSpan == rounding.
    unittest::ServerParameterGuard flagController("featureFlagFixedBucketingOptimizations", true);

    EXPECT_TRUE(
        timeseries::canUseFixedBucketOptimizations(withFixedBucketing(optionsEqualAndNone), false))
        << "BucketMaxSpanSeconds=none, BucketRoundingSeconds=none, "
        << "fixedBucketing=true implies buckets should be fixed.";

    EXPECT_TRUE(
        timeseries::canUseFixedBucketOptimizations(withFixedBucketing(optionsEqualNotNone), false))
        << "BucketMaxSpanSeconds=value, BucketRoundingSeconds=value, "
        << "fixedBucketing=true implies buckets should be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(
        withFixedBucketing(optionsMaxSpanAndNone), false))
        << "BucketMaxSpanSeconds=value, BucketRoundingSeconds=none, "
        << "fixedBucketing=true implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(
        withFixedBucketing(optionsNoneAndRounding), false))
        << "BucketMaxSpanSeconds=none, BucketRoundingSeconds=value, "
        << "fixedBucketing=true implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(
        withFixedBucketing(optionsValuesNotEqual), false))
        << "BucketMaxSpanSeconds=value1, BucketRoundingSeconds=value2, "
        << "fixedBucketing=true implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsEqualAndNone, false))
        << "BucketMaxSpanSeconds=none, BucketRoundingSeconds=none, "
        << "fixedBucketing unset implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsEqualNotNone, false))
        << "BucketMaxSpanSeconds=value, BucketRoundingSeconds=value, "
        << "fixedBucketing unset implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsMaxSpanAndNone, false))
        << "BucketMaxSpanSeconds=value, BucketRoundingSeconds=none, "
        << "fixedBucketing unset implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsNoneAndRounding, false))
        << "BucketMaxSpanSeconds=none, BucketRoundingSeconds=value, "
        << "fixedBucketing unset implies buckets should not be fixed.";

    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(optionsValuesNotEqual, false))
        << "BucketMaxSpanSeconds=value1, BucketRoundingSeconds=value2, "
        << "fixedBucketing unset implies buckets should not be fixed.";
}

TEST(TimeseriesOptionsTest, CanUseFixedBucketOptimizationsRequiresKnownExtendedRangeState) {
    unittest::ServerParameterGuard flagController("featureFlagFixedBucketingOptimizations", true);

    auto options = createTimeseriesOptionsWithBucketMaxSpanAndRoundingSeconds(3600, 3600);
    options.setFixedBucketing(true);

    // Otherwise-eligible options only unlock the optimization when the caller affirmatively
    // knows there's no extended-range data. Omitting the argument, or explicitly passing
    // boost::none or true, must conservatively disable it.
    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(options))
        << "Omitting hasExtendedRangeData should conservatively disable the optimization.";
    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(options, boost::none))
        << "Unknown extended-range status should conservatively disable the optimization.";
    EXPECT_FALSE(timeseries::canUseFixedBucketOptimizations(options, true))
        << "Known extended-range data must disable the optimization.";
    EXPECT_TRUE(timeseries::canUseFixedBucketOptimizations(options, false))
        << "Confirmed absence of extended-range data should allow the optimization.";
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

// Verify `setFixedBucketingDefaultForNewCollection`: only flagEnabled=true with an unset field
// sets fixedBucketing to true; all other combinations leave the field unchanged.
// Parameters: (flagEnabled, initialValue, expectedValue).
using SetFixedBucketingTestParams = std::tuple<bool, boost::optional<bool>, boost::optional<bool>>;
class SetFixedBucketingDefaultTest : public testing::TestWithParam<SetFixedBucketingTestParams> {};

TEST_P(SetFixedBucketingDefaultTest, SetFixedBucketingDefaultForNewCollection) {
    auto [flagEnabled, initial, expected] = GetParam();
    TimeseriesOptions opts{"time"};
    if (initial.has_value())
        opts.setFixedBucketing(*initial);
    timeseries::setFixedBucketingDefaultForNewCollection(opts, flagEnabled);
    ASSERT_EQ(opts.getFixedBucketing().has_value(), expected.has_value());
    if (expected.has_value()) {
        ASSERT_EQ(static_cast<bool>(opts.getFixedBucketing()), *expected);
    }
}

INSTANTIATE_TEST_SUITE_P(TimeseriesOptions,
                         SetFixedBucketingDefaultTest,
                         testing::Values(
                             // flag on: unset => true, explicit values preserved
                             SetFixedBucketingTestParams{true, boost::none, true},
                             SetFixedBucketingTestParams{true, false, false},
                             SetFixedBucketingTestParams{true, true, true},
                             // flag off: no-op regardless of initial value
                             SetFixedBucketingTestParams{false, boost::none, boost::none},
                             SetFixedBucketingTestParams{false, false, false},
                             SetFixedBucketingTestParams{false, true, true}));

// Verify `inheritFixedBucketingIfOmitted`: if requested has no fixedBucketing, inherit from
// existing; if requested has it set (true or false), leave it unchanged.
// Parameters: (requestedValue, existingValue, expectedValue).
using InheritFixedBucketingTestParams =
    std::tuple<boost::optional<bool>, boost::optional<bool>, boost::optional<bool>>;
class InheritFixedBucketingTest : public testing::TestWithParam<InheritFixedBucketingTestParams> {};

TEST_P(InheritFixedBucketingTest, InheritFixedBucketingIfOmitted) {
    auto [requestedVal, existingVal, expectedVal] = GetParam();
    TimeseriesOptions requested{"time"};
    if (requestedVal.has_value())
        requested.setFixedBucketing(*requestedVal);
    TimeseriesOptions existing{"time"};
    if (existingVal.has_value())
        existing.setFixedBucketing(*existingVal);
    timeseries::inheritFixedBucketingIfOmitted(requested, existing);
    ASSERT_EQ(requested.getFixedBucketing().has_value(), expectedVal.has_value());
    if (expectedVal.has_value()) {
        ASSERT_EQ(static_cast<bool>(requested.getFixedBucketing()), *expectedVal);
    }
}

INSTANTIATE_TEST_SUITE_P(TimeseriesOptions,
                         InheritFixedBucketingTest,
                         testing::Values(
                             // requested unset: inherits from existing
                             InheritFixedBucketingTestParams{boost::none, true, true},
                             InheritFixedBucketingTestParams{boost::none, false, false},
                             InheritFixedBucketingTestParams{boost::none, boost::none, boost::none},
                             // requested set: unchanged regardless of existing
                             InheritFixedBucketingTestParams{true, true, true},
                             InheritFixedBucketingTestParams{true, false, true},
                             InheritFixedBucketingTestParams{true, boost::none, true},
                             InheritFixedBucketingTestParams{false, true, false},
                             InheritFixedBucketingTestParams{false, false, false},
                             InheritFixedBucketingTestParams{false, boost::none, false}));

TEST(TimeseriesOptionsTest, BSONColumnMemEstimationCalculations) {
    // The calculations for BSONColumn memory estimation in bson_validate.cpp rely on the defaults
    // for some server parameters. If these change, we also need to recalculate and potentially
    // adjust the memory threshold of the 'bsonMaxExpandedMemUsage' parameter.
    EXPECT_EQ(gTimeseriesBucketMinCount, 10);
}

}  // namespace mongo

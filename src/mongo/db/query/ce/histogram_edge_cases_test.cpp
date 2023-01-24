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

#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/ce/histogram_predicate_estimation.h"
#include "mongo/db/query/ce/test_utils.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stats/array_histogram.h"
#include "mongo/db/query/stats/maxdiff_test_utils.h"
#include "mongo/db/query/stats/value_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer::ce {
namespace {
namespace value = sbe::value;

using stats::ArrayHistogram;
using stats::makeInt64Value;
using stats::SBEValue;
using stats::ScalarHistogram;
using stats::TypeCounts;

constexpr double kErrorBound = 0.01;

TEST(EstimatorTest, OneBucketIntHistogram) {
    // Data set of 10 values, each with frequency 3, in the range (-inf, 100].
    // Example: { -100, -20, 0, 20, 50, 60, 70, 80, 90, 100}.
    std::vector<BucketData> data{{100, 3.0, 27.0, 9.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(30.0, getTotals(hist).card);

    // Estimates with the bucket bound.
    ASSERT_EQ(3.0, estimateIntValCard(hist, 100, EstimationType::kEqual));
    ASSERT_EQ(27.0, estimateIntValCard(hist, 100, EstimationType::kLess));
    ASSERT_EQ(30.0, estimateIntValCard(hist, 100, EstimationType::kLessOrEqual));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 100, EstimationType::kGreater));
    ASSERT_EQ(3.0, estimateIntValCard(hist, 100, EstimationType::kGreaterOrEqual));

    // Estimates with a value inside the bucket.
    ASSERT_EQ(3.0, estimateIntValCard(hist, 10, EstimationType::kEqual));
    // No interpolation possible for estimates of inequalities in a single bucket. The estimates
    // are based on the default cardinality of half bucket +/- the estimate of equality inside of
    // the bucket.
    ASSERT_EQ(10.5, estimateIntValCard(hist, 10, EstimationType::kLess));
    ASSERT_EQ(13.5, estimateIntValCard(hist, 10, EstimationType::kLessOrEqual));
    ASSERT_EQ(16.5, estimateIntValCard(hist, 10, EstimationType::kGreater));
    ASSERT_EQ(19.5, estimateIntValCard(hist, 10, EstimationType::kGreaterOrEqual));

    // Estimates for a value larger than the last bucket bound.
    ASSERT_EQ(0.0, estimateIntValCard(hist, 1000, EstimationType::kEqual));
    ASSERT_EQ(30.0, estimateIntValCard(hist, 1000, EstimationType::kLess));
    ASSERT_EQ(30.0, estimateIntValCard(hist, 1000, EstimationType::kLessOrEqual));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 1000, EstimationType::kGreater));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 1000, EstimationType::kGreaterOrEqual));
}

TEST(EstimatorTest, OneExclusiveBucketIntHistogram) {
    // Data set of a single value.
    // By exclusive bucket we mean a bucket with only boundary, that is the range frequency and NDV
    // are zero.
    std::vector<BucketData> data{{100, 2.0, 0.0, 0.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(2.0, getTotals(hist).card);

    // Estimates with the bucket boundary.
    ASSERT_EQ(2.0, estimateIntValCard(hist, 100, EstimationType::kEqual));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 100, EstimationType::kLess));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 100, EstimationType::kGreater));

    ASSERT_EQ(0.0, estimateIntValCard(hist, 0, EstimationType::kEqual));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 0, EstimationType::kLess));
    ASSERT_EQ(2.0, estimateIntValCard(hist, 0, EstimationType::kGreater));

    ASSERT_EQ(0.0, estimateIntValCard(hist, 1000, EstimationType::kEqual));
    ASSERT_EQ(2.0, estimateIntValCard(hist, 1000, EstimationType::kLess));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 1000, EstimationType::kGreater));
}

TEST(EstimatorTest, OneBucketTwoIntValuesHistogram) {
    // Data set of two values, example {5, 100, 100}.
    std::vector<BucketData> data{{100, 2.0, 1.0, 1.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(3.0, getTotals(hist).card);

    // Estimates with the bucket boundary.
    ASSERT_EQ(2.0, estimateIntValCard(hist, 100, EstimationType::kEqual));
    ASSERT_EQ(1.0, estimateIntValCard(hist, 100, EstimationType::kLess));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 100, EstimationType::kGreater));

    ASSERT_EQ(1.0, estimateIntValCard(hist, 10, EstimationType::kEqual));
    // Default estimate of half of the bucket's range frequency = 0.5.
    ASSERT_EQ(0.5, estimateIntValCard(hist, 10, EstimationType::kLess));
    ASSERT_EQ(2.5, estimateIntValCard(hist, 10, EstimationType::kGreater));

    ASSERT_EQ(0.0, estimateIntValCard(hist, 1000, EstimationType::kEqual));
    ASSERT_EQ(3.0, estimateIntValCard(hist, 1000, EstimationType::kLess));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 1000, EstimationType::kGreater));
}

TEST(EstimatorTest, OneBucketTwoIntValuesHistogram2) {
    // Similar to the above test with higher frequency for the second value.
    // Example {5, 5, 5, 100, 100}.
    std::vector<BucketData> data{{100, 2.0, 3.0, 1.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(5.0, getTotals(hist).card);

    // Estimates with the bucket boundary.
    ASSERT_EQ(2.0, estimateIntValCard(hist, 100, EstimationType::kEqual));
    ASSERT_EQ(3.0, estimateIntValCard(hist, 100, EstimationType::kLess));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 100, EstimationType::kGreater));

    ASSERT_EQ(3.0, estimateIntValCard(hist, 10, EstimationType::kEqual));
    // Default estimate of half of the bucket's range frequency = 1.5.
    ASSERT_EQ(1.5, estimateIntValCard(hist, 10, EstimationType::kLess));
    ASSERT_EQ(3.5, estimateIntValCard(hist, 10, EstimationType::kGreater));

    ASSERT_EQ(0.0, estimateIntValCard(hist, 1000, EstimationType::kEqual));
    ASSERT_EQ(5.0, estimateIntValCard(hist, 1000, EstimationType::kLess));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 1000, EstimationType::kGreater));
}

TEST(EstimatorTest, TwoBucketsIntHistogram) {
    // Data set of 10 values in the range [1, 100].
    std::vector<BucketData> data{{1, 1.0, 0.0, 0.0}, {100, 3.0, 26.0, 8.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(30.0, getTotals(hist).card);

    // Estimates for a value smaller than the first bucket.
    ASSERT_EQ(0.0, estimateIntValCard(hist, -42, EstimationType::kEqual));
    ASSERT_EQ(0.0, estimateIntValCard(hist, -42, EstimationType::kLess));
    ASSERT_EQ(0.0, estimateIntValCard(hist, -42, EstimationType::kLessOrEqual));
    ASSERT_EQ(30.0, estimateIntValCard(hist, -42, EstimationType::kGreater));
    ASSERT_EQ(30.0, estimateIntValCard(hist, -42, EstimationType::kGreaterOrEqual));

    // Estimates with bucket bounds.
    ASSERT_EQ(1.0, estimateIntValCard(hist, 1, EstimationType::kEqual));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 1, EstimationType::kLess));
    ASSERT_EQ(1.0, estimateIntValCard(hist, 1, EstimationType::kLessOrEqual));
    ASSERT_EQ(29.0, estimateIntValCard(hist, 1, EstimationType::kGreater));
    ASSERT_EQ(30.0, estimateIntValCard(hist, 1, EstimationType::kGreaterOrEqual));

    ASSERT_EQ(3.0, estimateIntValCard(hist, 100, EstimationType::kEqual));
    ASSERT_EQ(27.0, estimateIntValCard(hist, 100, EstimationType::kLess));
    ASSERT_EQ(30.0, estimateIntValCard(hist, 100, EstimationType::kLessOrEqual));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 100, EstimationType::kGreater));
    ASSERT_EQ(3.0, estimateIntValCard(hist, 100, EstimationType::kGreaterOrEqual));

    // Estimates with a value inside the bucket. The estimates use interpolation.
    // The bucket ratio for the value of 10 is smaller than the estimate for equality
    // and the estimates for Less and LessOrEqual are the same.
    ASSERT_APPROX_EQUAL(3.25, estimateIntValCard(hist, 10, EstimationType::kEqual), kErrorBound);
    ASSERT_APPROX_EQUAL(3.36, estimateIntValCard(hist, 10, EstimationType::kLess), kErrorBound);
    ASSERT_APPROX_EQUAL(
        3.36, estimateIntValCard(hist, 10, EstimationType::kLessOrEqual), kErrorBound);

    ASSERT_APPROX_EQUAL(26.64, estimateIntValCard(hist, 10, EstimationType::kGreater), kErrorBound);
    ASSERT_APPROX_EQUAL(
        26.64, estimateIntValCard(hist, 10, EstimationType::kGreaterOrEqual), kErrorBound);

    // Different estimates for Less and LessOrEqual for the value of 50.
    ASSERT_APPROX_EQUAL(3.25, estimateIntValCard(hist, 50, EstimationType::kEqual), kErrorBound);
    ASSERT_APPROX_EQUAL(10.61, estimateIntValCard(hist, 50, EstimationType::kLess), kErrorBound);
    ASSERT_APPROX_EQUAL(
        13.87, estimateIntValCard(hist, 50, EstimationType::kLessOrEqual), kErrorBound);
    ASSERT_APPROX_EQUAL(16.13, estimateIntValCard(hist, 50, EstimationType::kGreater), kErrorBound);
    ASSERT_APPROX_EQUAL(
        19.38, estimateIntValCard(hist, 50, EstimationType::kGreaterOrEqual), kErrorBound);
}

TEST(EstimatorTest, ThreeExclusiveBucketsIntHistogram) {
    std::vector<BucketData> data{{1, 1.0, 0.0, 0.0}, {10, 8.0, 0.0, 0.0}, {100, 1.0, 0.0, 0.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(10.0, getTotals(hist).card);

    ASSERT_EQ(0.0, estimateIntValCard(hist, 5, EstimationType::kEqual));
    ASSERT_EQ(1.0, estimateIntValCard(hist, 5, EstimationType::kLess));
    ASSERT_EQ(1.0, estimateIntValCard(hist, 5, EstimationType::kLessOrEqual));
    ASSERT_EQ(9.0, estimateIntValCard(hist, 5, EstimationType::kGreater));
    ASSERT_EQ(9.0, estimateIntValCard(hist, 5, EstimationType::kGreaterOrEqual));
}
TEST(EstimatorTest, OneBucketStrHistogram) {
    std::vector<BucketData> data{{"xyz", 3.0, 27.0, 9.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(30.0, getTotals(hist).card);

    // Estimates with bucket bound.
    auto [tag, value] = value::makeNewString("xyz"_sd);
    value::ValueGuard vg(tag, value);
    double expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(3.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(27.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_EQ(30.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_EQ(3.0, expectedCard);

    // Estimates for a value inside the bucket. Since there is no low value bound in the histogram
    // all values smaller than the upper bound will be estimated the same way using half of the
    // bucket cardinality.
    std::tie(tag, value) = value::makeNewString("a"_sd);
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(3.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(10.5, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_EQ(13.5, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(16.5, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_EQ(19.5, expectedCard);

    std::tie(tag, value) = value::makeNewString(""_sd);
    // In the special case of a single string bucket, we estimate empty string equality as for any
    // other string value. In practice if there are at least 2 buckets for the string data and an
    // empty string in the data set, it will be chosen as a bound for the first bucket and produce
    // precise estimates.
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(3.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_EQ(30.0, expectedCard);

    // Estimates for a value larger than the upper bound.
    std::tie(tag, value) = value::makeNewString("z"_sd);
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(30.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);
}

TEST(EstimatorTest, TwoBucketsStrHistogram) {
    // Data set of 100 strings in the range ["abc", "xyz"], with average frequency of 2.
    std::vector<BucketData> data{{"abc", 2.0, 0.0, 0.0}, {"xyz", 3.0, 95.0, 48.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(100.0, getTotals(hist).card);

    // Estimates for a value smaller than the first bucket bound.
    auto [tag, value] = value::makeNewString("a"_sd);
    value::ValueGuard vg(tag, value);

    double expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(100.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_EQ(100.0, expectedCard);

    // Estimates with bucket bounds.
    std::tie(tag, value) = value::makeNewString("abc"_sd);
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(2.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_EQ(2.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(98.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_EQ(100.0, expectedCard);

    std::tie(tag, value) = value::makeNewString("xyz"_sd);
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(3.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(97.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_EQ(100.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_EQ(3.0, expectedCard);

    // Estimates for a value inside the bucket.
    std::tie(tag, value) = value::makeNewString("sun"_sd);
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(1.98, expectedCard, kErrorBound);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(74.39, expectedCard, kErrorBound);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(76.37, expectedCard, kErrorBound);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(23.64, expectedCard, kErrorBound);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_APPROX_EQUAL(25.62, expectedCard, kErrorBound);

    // Estimate for a value very close to the bucket bound.
    std::tie(tag, value) = value::makeNewString("xyw"_sd);
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(1.98, expectedCard, kErrorBound);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(95.02, expectedCard, kErrorBound);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(96.99, expectedCard, kErrorBound);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(3.0, expectedCard, kErrorBound);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_APPROX_EQUAL(4.98, expectedCard, kErrorBound);
}

TEST(EstimatorTest, TwoBucketsDateHistogram) {
    // June 6, 2017 -- June 7, 2017.
    const int64_t startInstant = 1496777923000LL;
    const int64_t endInstant = 1496864323000LL;
    const auto startDate = Date_t::fromMillisSinceEpoch(startInstant);
    const auto endDate = Date_t::fromMillisSinceEpoch(endInstant);

    std::vector<BucketData> data{{Value(startDate), 3.0, 0.0, 0.0},
                                 {Value(endDate), 1.0, 96.0, 48.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(100.0, getTotals(hist).card);

    const auto valueBefore = value::bitcastFrom<int64_t>(startInstant - 1);
    double expectedCard =
        estimate(hist, value::TypeTags::Date, valueBefore, EstimationType::kEqual).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Date, valueBefore, EstimationType::kLess).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard =
        estimate(hist, value::TypeTags::Date, valueBefore, EstimationType::kGreater).card;
    ASSERT_EQ(100.0, expectedCard);

    const auto valueStart = value::bitcastFrom<int64_t>(startInstant);
    expectedCard = estimate(hist, value::TypeTags::Date, valueStart, EstimationType::kEqual).card;
    ASSERT_EQ(3.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Date, valueStart, EstimationType::kLess).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Date, valueStart, EstimationType::kGreater).card;
    ASSERT_EQ(97.0, expectedCard);

    const auto valueEnd = value::bitcastFrom<int64_t>(endInstant);
    expectedCard = estimate(hist, value::TypeTags::Date, valueEnd, EstimationType::kEqual).card;
    ASSERT_EQ(1.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Date, valueEnd, EstimationType::kLess).card;
    ASSERT_EQ(99.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Date, valueEnd, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);

    const auto valueIn = value::bitcastFrom<int64_t>(startInstant + 43000000);
    expectedCard = estimate(hist, value::TypeTags::Date, valueIn, EstimationType::kEqual).card;
    ASSERT_EQ(2.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Date, valueIn, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(48.77, expectedCard, kErrorBound);
    expectedCard = estimate(hist, value::TypeTags::Date, valueIn, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(49.22, expectedCard, kErrorBound);

    const auto valueAfter = value::bitcastFrom<int64_t>(endInstant + 100);
    expectedCard = estimate(hist, value::TypeTags::Date, valueAfter, EstimationType::kEqual).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Date, valueAfter, EstimationType::kLess).card;
    ASSERT_EQ(100.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Date, valueAfter, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);
}

TEST(EstimatorTest, TwoBucketsTimestampHistogram) {
    // June 6, 2017 -- June 7, 2017 in seconds.
    const int64_t startInstant = 1496777923LL;
    const int64_t endInstant = 1496864323LL;
    const Timestamp startTs{Seconds(startInstant), 0};
    const Timestamp endTs{Seconds(endInstant), 0};

    std::vector<BucketData> data{{Value(startTs), 3.0, 0.0, 0.0}, {Value(endTs), 1.0, 96.0, 48.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(100.0, getTotals(hist).card);

    const auto valueBefore = value::bitcastFrom<int64_t>(startTs.asULL() - 1);
    CEType expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueBefore, EstimationType::kEqual).card};
    ASSERT_EQ(0.0, expectedCard._value);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueBefore, EstimationType::kLess).card};
    ASSERT_EQ(0.0, expectedCard._value);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueBefore, EstimationType::kGreater).card};
    ASSERT_EQ(100.0, expectedCard._value);

    const auto valueStart = value::bitcastFrom<int64_t>(
        startTs.asULL());  // NB: startTs.asInt64() produces different value.
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueStart, EstimationType::kEqual).card};
    ASSERT_EQ(3.0, expectedCard._value);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueStart, EstimationType::kLess).card};
    ASSERT_EQ(0.0, expectedCard._value);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueStart, EstimationType::kGreater).card};
    ASSERT_EQ(97.0, expectedCard._value);

    const auto valueEnd = value::bitcastFrom<int64_t>(endTs.asULL());
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueEnd, EstimationType::kEqual).card};
    ASSERT_EQ(1.0, expectedCard._value);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueEnd, EstimationType::kLess).card};
    ASSERT_EQ(99.0, expectedCard._value);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueEnd, EstimationType::kGreater).card};
    ASSERT_EQ(0.0, expectedCard._value);

    const auto valueIn = value::bitcastFrom<int64_t>((startTs.asULL() + endTs.asULL()) / 2);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueIn, EstimationType::kEqual).card};
    ASSERT_EQ(2.0, expectedCard._value);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueIn, EstimationType::kLess).card};
    ASSERT_CE_APPROX_EQUAL(49.0, expectedCard, kErrorBound);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueIn, EstimationType::kGreater).card};
    ASSERT_CE_APPROX_EQUAL(49.0, expectedCard, kErrorBound);

    const auto valueAfter = value::bitcastFrom<int64_t>(endTs.asULL() + 100);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueAfter, EstimationType::kEqual).card};
    ASSERT_EQ(0.0, expectedCard._value);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueAfter, EstimationType::kLess).card};
    ASSERT_EQ(100.0, expectedCard._value);
    expectedCard = {
        estimate(hist, value::TypeTags::Timestamp, valueAfter, EstimationType::kGreater).card};
    ASSERT_EQ(0.0, expectedCard._value);
}

TEST(EstimatorTest, TwoBucketsObjectIdHistogram) {
    const auto startOid = OID("63340d8d27afef2de7357e8d");
    const auto endOid = OID("63340dbed6cd8af737d4139a");
    ASSERT_TRUE(startOid < endOid);

    std::vector<BucketData> data{{Value(startOid), 2.0, 0.0, 0.0},
                                 {Value(endOid), 1.0, 97.0, 77.0}};
    const ScalarHistogram hist = createHistogram(data);
    if constexpr (kCETestLogOnly) {
        std::cout << hist.serialize() << std::endl;
    }

    ASSERT_EQ(100.0, getTotals(hist).card);

    auto [tag, value] = value::makeNewObjectId();
    value::ValueGuard vg(tag, value);
    const auto oidBefore = OID("63340d8d27afef2de7357e8c");
    oidBefore.view().readInto(value::getObjectIdView(value));

    double expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(100.0, expectedCard);

    // Bucket bounds.
    startOid.view().readInto(value::getObjectIdView(value));
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(2.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(98.0, expectedCard);

    endOid.view().readInto(value::getObjectIdView(value));
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(1.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(99.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);

    // ObjectId value inside the bucket.
    const auto oidInside = OID("63340db2cd4d46ff39178e9d");
    oidInside.view().readInto(value::getObjectIdView(value));
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(1.25, expectedCard, kErrorBound);

    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(74.00, expectedCard, kErrorBound);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(24.74, expectedCard, kErrorBound);

    const auto oidAfter = OID("63340dbed6cd8af737d4139b");
    oidAfter.view().readInto(value::getObjectIdView(value));
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(100.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);
}

TEST(EstimatorTest, TwoExclusiveBucketsMixedHistogram) {
    // Data set of mixed data types: 3 integers and 5 strings.
    constexpr double numInts = 3.0;
    constexpr double numStrs = 5.0;
    constexpr double collCard = numInts + numStrs;
    std::vector<BucketData> data{{1, numInts, 0.0, 0.0}, {"abc", numStrs, 0.0, 0.0}};
    const ScalarHistogram hist = createHistogram(data);
    const auto arrHist = ArrayHistogram::make(hist,
                                              TypeCounts{{value::TypeTags::NumberInt64, numInts},
                                                         {value::TypeTags::StringSmall, numStrs}},
                                              collCard);

    const auto [tagLowDbl, valLowDbl] =
        std::make_pair(value::TypeTags::NumberDouble,
                       value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));

    // (NaN, 1).
    CEType expectedCard = estimateCardRange(*arrHist,
                                            false /* lowInclusive */,
                                            tagLowDbl,
                                            valLowDbl,
                                            false /* highInclusive */,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(1),
                                            true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(0.0, expectedCard, kErrorBound);

    // (NaN, 5).
    expectedCard = estimateCardRange(*arrHist,
                                     false /* lowInclusive */,
                                     tagLowDbl,
                                     valLowDbl,
                                     false /* highInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(5),
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(3.0, expectedCard, kErrorBound);

    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);
    auto [tag, value] = value::makeNewString("a"_sd);
    value::ValueGuard vg(tag, value);

    // [0, "").
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(0),
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(numInts, expectedCard, kErrorBound);

    // ["", "a"].
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* highInclusive */,
                                     tag,
                                     value,
                                     true /* includeScalar */);

    ASSERT_CE_APPROX_EQUAL(0.0, expectedCard, kErrorBound);

    std::tie(tag, value) = value::makeNewString("xyz"_sd);
    // ["", "xyz"].
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* highInclusive */,
                                     tag,
                                     value,
                                     true /* includeScalar */);

    ASSERT_CE_APPROX_EQUAL(numStrs, expectedCard, kErrorBound);
}

TEST(EstimatorTest, TwoBucketsMixedHistogram) {
    // Data set of mixed data types: 20 integers and 80 strings.
    // Histogram with one bucket per data type.
    std::vector<BucketData> data{{100, 3.0, 17.0, 9.0}, {"pqr", 5.0, 75.0, 25.0}};
    const ScalarHistogram hist = createHistogram(data);
    const auto arrHist = ArrayHistogram::make(
        hist,
        TypeCounts{{value::TypeTags::NumberInt64, 20}, {value::TypeTags::StringSmall, 80}},
        100.0 /* sampleSize */);

    ASSERT_EQ(100.0, getTotals(hist).card);

    // Estimates with the bucket bounds.
    ASSERT_EQ(3.0, estimateIntValCard(hist, 100, EstimationType::kEqual));
    ASSERT_EQ(17.0, estimateIntValCard(hist, 100, EstimationType::kLess));
    ASSERT_EQ(80.0, estimateIntValCard(hist, 100, EstimationType::kGreater));

    auto [tag, value] = value::makeNewString("pqr"_sd);
    value::ValueGuard vg(tag, value);
    CEType expectedCard{estimate(hist, tag, value, EstimationType::kEqual).card};
    ASSERT_EQ(5.0, expectedCard._value);
    expectedCard = {estimate(hist, tag, value, EstimationType::kLess).card};
    ASSERT_EQ(95.0, expectedCard._value);
    expectedCard = {estimate(hist, tag, value, EstimationType::kGreater).card};
    ASSERT_EQ(0.0, expectedCard._value);

    // Estimates for a value smaller than the first bucket bound.
    ASSERT_APPROX_EQUAL(1.88, estimateIntValCard(hist, 50, EstimationType::kEqual), kErrorBound);
    ASSERT_APPROX_EQUAL(6.61, estimateIntValCard(hist, 50, EstimationType::kLess), kErrorBound);
    ASSERT_APPROX_EQUAL(
        8.49, estimateIntValCard(hist, 50, EstimationType::kLessOrEqual), kErrorBound);
    ASSERT_APPROX_EQUAL(91.5, estimateIntValCard(hist, 50, EstimationType::kGreater), kErrorBound);
    ASSERT_APPROX_EQUAL(
        93.39, estimateIntValCard(hist, 50, EstimationType::kGreaterOrEqual), kErrorBound);

    // Estimates for a value between bucket bounds.
    ASSERT_EQ(0.0, estimateIntValCard(hist, 105, EstimationType::kEqual));

    std::tie(tag, value) = value::makeNewString("a"_sd);
    expectedCard = {estimate(hist, tag, value, EstimationType::kEqual).card};
    ASSERT_CE_APPROX_EQUAL(3.0, expectedCard, kErrorBound);
    expectedCard = {estimate(hist, tag, value, EstimationType::kLess).card};
    ASSERT_CE_APPROX_EQUAL(54.5, expectedCard, kErrorBound);
    expectedCard = {estimate(hist, tag, value, EstimationType::kLessOrEqual).card};
    ASSERT_CE_APPROX_EQUAL(57.5, expectedCard, kErrorBound);
    expectedCard = {estimate(hist, tag, value, EstimationType::kGreater).card};
    ASSERT_CE_APPROX_EQUAL(42.5, expectedCard, kErrorBound);
    expectedCard = {estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card};
    ASSERT_CE_APPROX_EQUAL(45.5, expectedCard, kErrorBound);

    // Range estimates, including min/max values per data type.
    const auto [tagLowDbl, valLowDbl] =
        std::make_pair(value::TypeTags::NumberDouble,
                       value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
    const auto [tagHighInt, valHighInt] =
        std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1000000));

    // [NaN, 25].
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     tagLowDbl,
                                     valLowDbl,
                                     true /* highInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(25),
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(8.49, expectedCard, kErrorBound);

    // [25, 1000000].
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(25),
                                     true /* highInclusive */,
                                     tagHighInt,
                                     valHighInt,
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(13.38, expectedCard, kErrorBound);

    // [NaN, 1000000].
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     tagLowDbl,
                                     valLowDbl,
                                     true /* highInclusive */,
                                     tagHighInt,
                                     valHighInt,
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(20.0, expectedCard, kErrorBound);

    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);

    // [NaN, "").
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     tagLowDbl,
                                     valLowDbl,
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(20.0, expectedCard, kErrorBound);

    // [25, "").
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(25),
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(13.39, expectedCard, kErrorBound);

    // ["", "a"].
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* highInclusive */,
                                     tag,
                                     value,
                                     true /* includeScalar */);

    ASSERT_CE_APPROX_EQUAL(37.49, expectedCard, kErrorBound);

    // ["", {}).
    auto [tagObj, valObj] = value::makeNewObject();
    value::ValueGuard vgObj(tagObj, valObj);
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     false /* highInclusive */,
                                     tagObj,
                                     valObj,
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(80.0, expectedCard, kErrorBound);

    // ["a", {}).
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     tag,
                                     value,
                                     false /* highInclusive */,
                                     tagObj,
                                     valObj,
                                     true /* includeScalar */);

    ASSERT_CE_APPROX_EQUAL(45.5, expectedCard, kErrorBound);
}

/**
 * Tests for cardinality estimates for queries over minimum values of date, timestamp, and objectId
 * types. When the histogram has at least 2 buckets per data type, the minimum value, if present in
 * the data, is picked as a bound for the first bucket for the corresponding data type. In this case
 * the cardinality estimates are precise. To test the approximate estimation, we force the histogram
 * generation to use one bucket per type (except the first numeric type).
 */
TEST(EstimatorTest, MinValueMixedHistogramFromData) {
    const int64_t startInstant = 1506777923000LL;
    const int64_t endInstant = 1516864323000LL;
    const Timestamp startTs{Seconds(1516864323LL), 0};
    const Timestamp endTs{Seconds(1526864323LL), 0};
    const auto startOid = OID("63340d8d27afef2de7357e8d");
    const auto endOid = OID("63340dbed6cd8af737d4139a");

    std::vector<SBEValue> data;
    data.emplace_back(value::TypeTags::Date, value::bitcastFrom<int64_t>(startInstant));
    data.emplace_back(value::TypeTags::Date, value::bitcastFrom<int64_t>(endInstant));

    data.emplace_back(value::TypeTags::Timestamp, value::bitcastFrom<int64_t>(startTs.asULL()));
    data.emplace_back(value::TypeTags::Timestamp, value::bitcastFrom<int64_t>(endTs.asULL()));

    auto [tag, val] = makeInt64Value(100);
    data.emplace_back(tag, val);
    std::tie(tag, val) = makeInt64Value(1000);
    data.emplace_back(tag, val);

    auto [strTag, strVal] = value::makeNewString("abc"_sd);
    value::ValueGuard strVG(strTag, strVal);
    auto [copyTag, copyVal] = value::copyValue(strTag, strVal);
    data.emplace_back(copyTag, copyVal);
    std::tie(strTag, strVal) = value::makeNewString("xyz"_sd);
    std::tie(copyTag, copyVal) = value::copyValue(strTag, strVal);
    data.emplace_back(copyTag, copyVal);

    auto [objTag, objVal] = value::makeNewObjectId();
    value::ValueGuard objVG(objTag, objVal);
    startOid.view().readInto(value::getObjectIdView(objVal));
    std::tie(tag, val) = copyValue(objTag, objVal);
    data.emplace_back(tag, val);
    endOid.view().readInto(value::getObjectIdView(objVal));
    std::tie(tag, val) = copyValue(objTag, objVal);
    data.emplace_back(tag, val);

    sortValueVector(data);

    // Force each type except numbers to use a single bucket. This way there is no bucket for the
    // min value if present in the data and it needs to be estimated.
    const ScalarHistogram& hist = makeHistogram(data, 6);

    // Mixed data are sorted in the histogram according to the BSON order as defined in bsontypes.h
    // the canonicalizeBSONTypeUnsafeLookup function.
    if constexpr (kCETestLogOnly) {
        std::cout << printValueArray(data) << "\n";
        std::cout << "Mixed types " << hist.dump();
    }

    // Assert that our scalar histogram is what we expect.
    auto expected = fromjson(
        "{ \
        buckets: [ \
            { \
                boundaryCount: 1.0, \
                rangeCount: 0.0, \
                rangeDistincts: 0.0, \
                cumulativeCount: 1.0, \
                cumulativeDistincts: 1.0 \
            }, \
            { \
                boundaryCount: 1.0, \
                rangeCount: 0.0, \
                rangeDistincts: 0.0, \
                cumulativeCount: 2.0, \
                cumulativeDistincts: 2.0 \
            }, \
            { \
                boundaryCount: 1.0, \
                rangeCount: 1.0, \
                rangeDistincts: 1.0, \
                cumulativeCount: 4.0, \
                cumulativeDistincts: 4.0 \
            }, \
            { \
                boundaryCount: 1.0, \
                rangeCount: 1.0, \
                rangeDistincts: 1.0, \
                cumulativeCount: 6.0, \
                cumulativeDistincts: 6.0 \
            }, \
            { \
                boundaryCount: 1.0, \
                rangeCount: 1.0, \
                rangeDistincts: 1.0, \
                cumulativeCount: 8.0, \
                cumulativeDistincts: 8.0 \
            }, \
            { \
                boundaryCount: 1.0, \
                rangeCount: 1.0, \
                rangeDistincts: 1.0, \
                cumulativeCount: 10.0, \
                cumulativeDistincts: 10.0 \
            } \
        ], \
        bounds: [ 100, 1000, 'xyz', ObjectId('63340dbed6cd8af737d4139a'), \
                  new Date(1516864323000), Timestamp(1526864323, 0) ] \
	}");
    ASSERT_BSONOBJ_EQ(expected, hist.serialize());

    // Minimum ObjectId.
    auto&& [minOid, inclOid] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::ObjectId);
    auto [minOidTag, minOidVal] = minOid->cast<mongo::optimizer::Constant>()->get();
    CEType expectedCard = {estimate(hist, minOidTag, minOidVal, EstimationType::kEqual).card};
    ASSERT_EQ(1.0, expectedCard._value);

    // Minimum date.
    const auto&& [minDate, inclDate] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::Date);
    const auto [minDateTag, minDateVal] = minDate->cast<mongo::optimizer::Constant>()->get();
    expectedCard = {estimate(hist, minDateTag, minDateVal, EstimationType::kEqual).card};
    ASSERT_EQ(1.0, expectedCard._value);

    // Minimum timestamp.
    auto&& [minTs, inclTs] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::Timestamp);
    auto [minTsTag, minTsVal] = minTs->cast<mongo::optimizer::Constant>()->get();
    expectedCard = {estimate(hist, minTsTag, minTsVal, EstimationType::kEqual).card};
    ASSERT_EQ(1.0, expectedCard._value);

    // Add minimum values to the data set and create another histogram.
    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);
    std::tie(copyTag, copyVal) = value::copyValue(tagLowStr, valLowStr);
    data.emplace_back(copyTag, copyVal);
    data.emplace_back(minDateTag, minDateVal);
    data.emplace_back(minTsTag, minTsVal);

    sortValueVector(data);
    const ScalarHistogram& hist2 = makeHistogram(data, 6);
    if constexpr (kCETestLogOnly) {
        std::cout << printValueArray(data) << "\n";
        std::cout << "Mixed types " << hist2.dump();
    }

    // Precise estimate for equality to empty string, it is a bucket boundary.
    expectedCard = {estimate(hist2, tagLowStr, valLowStr, EstimationType::kEqual).card};
    ASSERT_EQ(1.0, expectedCard._value);
    // Equality to the minimum date/ts value is estimated by range_frequency/NDV.
    expectedCard = {estimate(hist2, minDateTag, minDateVal, EstimationType::kEqual).card};
    ASSERT_EQ(1.0, expectedCard._value);
    expectedCard = {estimate(hist2, minTsTag, minTsVal, EstimationType::kEqual).card};
    ASSERT_EQ(1.0, expectedCard._value);

    // Inequality predicates using min values.
    const auto arrHist = ArrayHistogram::make(hist2,
                                              TypeCounts{
                                                  {value::TypeTags::NumberInt64, 2},
                                                  {value::TypeTags::StringSmall, 3},
                                                  {value::TypeTags::ObjectId, 2},
                                                  {value::TypeTags::Date, 3},
                                                  {value::TypeTags::Timestamp, 3},
                                              },
                                              13.0 /* sampleSize */);
    // [minDate, startInstant], estimated by the half of the date bucket.
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     minDateTag,
                                     minDateVal,
                                     true /* highInclusive */,
                                     value::TypeTags::Date,
                                     value::bitcastFrom<int64_t>(startInstant),
                                     true /* includeScalar */);
    ASSERT_EQ(1.0, expectedCard._value);

    // [minDate, endInstant], estimated by the entire date bucket.
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     minDateTag,
                                     minDateVal,
                                     true /* highInclusive */,
                                     value::TypeTags::Date,
                                     value::bitcastFrom<int64_t>(endInstant),
                                     true /* includeScalar */);
    ASSERT_EQ(3.0, expectedCard._value);

    // [minDate, minTs), estimated by the entire date bucket.
    // (is this interval possible or is it better to have maxDate upper bound?).
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     minDateTag,
                                     minDateVal,
                                     false /* highInclusive */,
                                     minTsTag,
                                     minTsVal,
                                     true /* includeScalar */);
    ASSERT_EQ(3.0, expectedCard._value);

    // [minTs, startTs], estimated by the half of the timestamp bucket.
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     minTsTag,
                                     minTsVal,
                                     true /* highInclusive */,
                                     value::TypeTags::Timestamp,
                                     value::bitcastFrom<int64_t>(startTs.asULL()),
                                     true /* includeScalar */);
    ASSERT_EQ(1.0, expectedCard._value);

    // [minTs, endTs], estimated by the entire timestamp bucket.
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     minTsTag,
                                     minTsVal,
                                     true /* highInclusive */,
                                     value::TypeTags::Timestamp,
                                     value::bitcastFrom<int64_t>(endTs.asULL()),
                                     true /* includeScalar */);
    ASSERT_EQ(3.0, expectedCard._value);

    // [minTs, maxTs], estimated by the entire timestamp bucket.
    auto&& [maxTs, inclMaxTs] = getMinMaxBoundForType(false /*isMin*/, value::TypeTags::Timestamp);
    const auto [maxTsTag, maxTsVal] = maxTs->cast<mongo::optimizer::Constant>()->get();
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     minTsTag,
                                     minTsVal,
                                     true /* highInclusive */,
                                     maxTsTag,
                                     maxTsVal,
                                     true /* includeScalar */);
    ASSERT_EQ(3.0, expectedCard._value);
}

TEST(EstimatorTest, MinValueMixedHistogramFromBuckets) {
    const auto endOid = OID("63340dbed6cd8af737d4139a");
    const auto endDate = Date_t::fromMillisSinceEpoch(1526864323000LL);
    const Timestamp endTs{Seconds(1526864323LL), 0};

    std::vector<BucketData> data{
        {0, 1.0, 0.0, 0.0},
        {100, 4.0, 95.0, 30.0},
        {"xyz", 5.0, 95.0, 25.0},
        {Value(endOid), 5.0, 95.0, 50.0},
        {Value(endDate), 4.0, 96.0, 24.0},
        {Value(endTs), 5.0, 95.0, 50.0},
    };
    const ScalarHistogram hist = createHistogram(data);
    if constexpr (kCETestLogOnly) {
        std::cout << "Mixed types " << hist.dump();
    }
    ASSERT_EQ(500.0, getTotals(hist).card);

    // Minimum ObjectId.
    auto&& [minOid, inclOid] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::ObjectId);
    auto [minOidTag, minOidVal] = minOid->cast<mongo::optimizer::Constant>()->get();
    CEType expectedCard{estimate(hist, minOidTag, minOidVal, EstimationType::kEqual).card};
    ASSERT_CE_APPROX_EQUAL(1.9, expectedCard, kErrorBound);

    // Minimum date.
    const auto&& [minDate, inclDate] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::Date);
    const auto [minDateTag, minDateVal] = minDate->cast<mongo::optimizer::Constant>()->get();
    expectedCard = {estimate(hist, minDateTag, minDateVal, EstimationType::kEqual).card};
    ASSERT_EQ(4.0, expectedCard._value);

    // Minimum timestamp.
    auto&& [minTs, inclTs] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::Timestamp);
    auto [minTsTag, minTsVal] = minTs->cast<mongo::optimizer::Constant>()->get();
    expectedCard = {estimate(hist, minTsTag, minTsVal, EstimationType::kEqual).card};
    ASSERT_CE_APPROX_EQUAL(1.9, expectedCard, kErrorBound);

    // Inequality predicates using min values.
    const auto arrHist = ArrayHistogram::make(hist,
                                              TypeCounts{
                                                  {value::TypeTags::NumberInt64, 100},
                                                  {value::TypeTags::StringSmall, 100},
                                                  {value::TypeTags::ObjectId, 100},
                                                  {value::TypeTags::Date, 100},
                                                  {value::TypeTags::Timestamp, 100},
                                              },
                                              500.0 /* sampleSize */);
    // [minDate, innerDate], estimated by the half of the date bucket.
    const int64_t innerDate = 1516864323000LL;
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     minDateTag,
                                     minDateVal,
                                     true /* highInclusive */,
                                     value::TypeTags::Date,
                                     value::bitcastFrom<int64_t>(innerDate),
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(48.0, expectedCard, kErrorBound);

    // [minTs, innerTs], estimated by the half of the timestamp bucket.
    const Timestamp innerTs{Seconds(1516864323LL), 0};
    expectedCard = estimateCardRange(*arrHist,
                                     true /* lowInclusive */,
                                     minTsTag,
                                     minTsVal,
                                     true /* highInclusive */,
                                     value::TypeTags::Timestamp,
                                     value::bitcastFrom<int64_t>(innerTs.asULL()),
                                     true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(47.5, expectedCard, kErrorBound);
}
}  // namespace
}  // namespace mongo::optimizer::ce

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

#include "mongo/db/query/ce/array_histogram.h"
#include "mongo/db/query/ce/ce_test_utils.h"
#include "mongo/db/query/ce/histogram_estimation.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
namespace {

using namespace sbe;

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
    ASSERT_APPROX_EQUAL(3.3, estimateIntValCard(hist, 10, EstimationType::kEqual), 0.1);
    ASSERT_APPROX_EQUAL(3.4, estimateIntValCard(hist, 10, EstimationType::kLess), 0.1);
    ASSERT_APPROX_EQUAL(3.4, estimateIntValCard(hist, 10, EstimationType::kLessOrEqual), 0.1);
    ASSERT_APPROX_EQUAL(26.6, estimateIntValCard(hist, 10, EstimationType::kGreater), 0.1);
    ASSERT_APPROX_EQUAL(26.6, estimateIntValCard(hist, 10, EstimationType::kGreaterOrEqual), 0.1);

    // Different estimates for Less and LessOrEqual for the value of 50.
    ASSERT_APPROX_EQUAL(3.3, estimateIntValCard(hist, 50, EstimationType::kEqual), 0.1);
    ASSERT_APPROX_EQUAL(10.6, estimateIntValCard(hist, 50, EstimationType::kLess), 0.1);
    ASSERT_APPROX_EQUAL(13.9, estimateIntValCard(hist, 50, EstimationType::kLessOrEqual), 0.1);
    ASSERT_APPROX_EQUAL(16.1, estimateIntValCard(hist, 50, EstimationType::kGreater), 0.1);
    ASSERT_APPROX_EQUAL(19.4, estimateIntValCard(hist, 50, EstimationType::kGreaterOrEqual), 0.1);
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
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(kMinCard, expectedCard);
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
    ASSERT_APPROX_EQUAL(2.0, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(74.4, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(76.4, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(23.6, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_APPROX_EQUAL(25.6, expectedCard, 0.1);

    // Estimate for a value very close to the bucket bound.
    std::tie(tag, value) = value::makeNewString("xyw"_sd);
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(2.0, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(95.0, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(97.0, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(3.0, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_APPROX_EQUAL(5.0, expectedCard, 0.1);
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
    ASSERT_APPROX_EQUAL(48.8, expectedCard, 0.1);
    expectedCard = estimate(hist, value::TypeTags::Date, valueIn, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(49.2, expectedCard, 0.1);

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
    double expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueBefore, EstimationType::kEqual).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueBefore, EstimationType::kLess).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueBefore, EstimationType::kGreater).card;
    ASSERT_EQ(100.0, expectedCard);

    const auto valueStart = value::bitcastFrom<int64_t>(
        startTs.asULL());  // NB: startTs.asInt64() produces different value.
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueStart, EstimationType::kEqual).card;
    ASSERT_EQ(3.0, expectedCard);
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueStart, EstimationType::kLess).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueStart, EstimationType::kGreater).card;
    ASSERT_EQ(97.0, expectedCard);

    const auto valueEnd = value::bitcastFrom<int64_t>(endTs.asULL());
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueEnd, EstimationType::kEqual).card;
    ASSERT_EQ(1.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Timestamp, valueEnd, EstimationType::kLess).card;
    ASSERT_EQ(99.0, expectedCard);
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueEnd, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);

    const auto valueIn = value::bitcastFrom<int64_t>((startTs.asULL() + endTs.asULL()) / 2);
    expectedCard = estimate(hist, value::TypeTags::Timestamp, valueIn, EstimationType::kEqual).card;
    ASSERT_EQ(2.0, expectedCard);
    expectedCard = estimate(hist, value::TypeTags::Timestamp, valueIn, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(49.0, expectedCard, 0.1);
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueIn, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(49.0, expectedCard, 0.1);

    const auto valueAfter = value::bitcastFrom<int64_t>(endTs.asULL() + 100);
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueAfter, EstimationType::kEqual).card;
    ASSERT_EQ(0.0, expectedCard);
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueAfter, EstimationType::kLess).card;
    ASSERT_EQ(100.0, expectedCard);
    expectedCard =
        estimate(hist, value::TypeTags::Timestamp, valueAfter, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);
}

TEST(EstimatorTest, TwoBucketsObjectIdHistogram) {
    const auto startOid = OID("63340d8d27afef2de7357e8d");
    const auto endOid = OID("63340dbed6cd8af737d4139a");
    ASSERT_TRUE(startOid < endOid);

    std::vector<BucketData> data{{Value(startOid), 2.0, 0.0, 0.0},
                                 {Value(endOid), 1.0, 97.0, 77.0}};
    const ScalarHistogram hist = createHistogram(data);

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
    ASSERT_APPROX_EQUAL(1.2, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(83.9, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(14.8, expectedCard, 0.1);

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
    std::vector<BucketData> data{{1, 3.0, 0.0, 0.0}, {"abc", 5.0, 0.0, 0.0}};
    const ScalarHistogram hist = createHistogram(data);
    const ArrayHistogram arrHist(
        hist, TypeCounts{{value::TypeTags::NumberInt64, 3}, {value::TypeTags::StringSmall, 5}});

    const auto [tagLowDbl, valLowDbl] =
        std::make_pair(value::TypeTags::NumberDouble,
                       value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));

    // (NaN, 1).
    double expectedCard = estimateCardRange(arrHist,
                                            false /* lowInclusive */,
                                            tagLowDbl,
                                            valLowDbl,
                                            false /* highInclusive */,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(1),
                                            true /* includeScalar */);
    ASSERT_APPROX_EQUAL(0.0, expectedCard, 0.1);

    // (NaN, 5).
    expectedCard = estimateCardRange(arrHist,
                                     false /* lowInclusive */,
                                     tagLowDbl,
                                     valLowDbl,
                                     false /* highInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(5),
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(3.0, expectedCard, 0.1);

    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);
    auto [tag, value] = value::makeNewString("a"_sd);
    value::ValueGuard vg(tag, value);

    // [0, "").
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(0),
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(3.0, expectedCard, 0.1);

    // ["", "a"].
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* highInclusive */,
                                     tag,
                                     value,
                                     true /* includeScalar */);

    ASSERT_APPROX_EQUAL(0.0, expectedCard, 0.1);

    std::tie(tag, value) = value::makeNewString("xyz"_sd);
    // ["", "xyz"].
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* highInclusive */,
                                     tag,
                                     value,
                                     true /* includeScalar */);

    ASSERT_APPROX_EQUAL(5.0, expectedCard, 0.1);
}

TEST(EstimatorTest, TwoBucketsMixedHistogram) {
    // Data set of mixed data types: 20 integers and 80 strings.
    // Histogram with one bucket per data type.
    std::vector<BucketData> data{{100, 3.0, 17.0, 9.0}, {"pqr", 5.0, 75.0, 25.0}};
    const ScalarHistogram hist = createHistogram(data);
    const ArrayHistogram arrHist(
        hist, TypeCounts{{value::TypeTags::NumberInt64, 20}, {value::TypeTags::StringSmall, 80}});

    ASSERT_EQ(100.0, getTotals(hist).card);

    // Estimates with the bucket bounds.
    ASSERT_EQ(3.0, estimateIntValCard(hist, 100, EstimationType::kEqual));
    ASSERT_EQ(17.0, estimateIntValCard(hist, 100, EstimationType::kLess));
    ASSERT_EQ(80.0, estimateIntValCard(hist, 100, EstimationType::kGreater));

    auto [tag, value] = value::makeNewString("pqr"_sd);
    value::ValueGuard vg(tag, value);
    double expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_EQ(5.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_EQ(95.0, expectedCard);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_EQ(0.0, expectedCard);

    // Estimates for a value smaller than the first bucket bound.
    ASSERT_APPROX_EQUAL(1.9, estimateIntValCard(hist, 50, EstimationType::kEqual), 0.1);
    ASSERT_APPROX_EQUAL(6.6, estimateIntValCard(hist, 50, EstimationType::kLess), 0.1);
    ASSERT_APPROX_EQUAL(8.5, estimateIntValCard(hist, 50, EstimationType::kLessOrEqual), 0.1);
    ASSERT_APPROX_EQUAL(91.5, estimateIntValCard(hist, 50, EstimationType::kGreater), 0.1);
    ASSERT_APPROX_EQUAL(93.4, estimateIntValCard(hist, 50, EstimationType::kGreaterOrEqual), 0.1);

    // Estimates for a value between bucket bounds.
    ASSERT_EQ(0.0, estimateIntValCard(hist, 105, EstimationType::kEqual));

    std::tie(tag, value) = value::makeNewString("a"_sd);
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(3.0, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(54.5, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(57.5, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreater).card;
    ASSERT_APPROX_EQUAL(42.5, expectedCard, 0.1);
    expectedCard = estimate(hist, tag, value, EstimationType::kGreaterOrEqual).card;
    ASSERT_APPROX_EQUAL(45.5, expectedCard, 0.1);

    // Range estimates, including min/max values per data type.
    const auto [tagLowDbl, valLowDbl] =
        std::make_pair(value::TypeTags::NumberDouble,
                       value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
    const auto [tagHighInt, valHighInt] =
        std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1000000));

    // [NaN, 25].
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowDbl,
                                     valLowDbl,
                                     true /* highInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(25),
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(8.49, expectedCard, 0.1);

    // [25, 1000000].
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(25),
                                     true /* highInclusive */,
                                     tagHighInt,
                                     valHighInt,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(13.38, expectedCard, 0.1);

    // [NaN, 1000000].
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowDbl,
                                     valLowDbl,
                                     true /* highInclusive */,
                                     tagHighInt,
                                     valHighInt,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(19.99, expectedCard, 0.1);

    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);

    // [NaN, "").
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowDbl,
                                     valLowDbl,
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(19.99, expectedCard, 0.1);

    // [25, "").
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     value::TypeTags::NumberInt32,
                                     value::bitcastFrom<int64_t>(25),
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(13.39, expectedCard, 0.1);

    // ["", "a"].
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* highInclusive */,
                                     tag,
                                     value,
                                     true /* includeScalar */);

    ASSERT_APPROX_EQUAL(37.49, expectedCard, 0.1);

    // ["", {}).
    auto [tagObj, valObj] = value::makeNewObject();
    value::ValueGuard vgObj(tagObj, valObj);
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     false /* highInclusive */,
                                     tagObj,
                                     valObj,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(79.99, expectedCard, 0.1);

    // ["a", {}).
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tag,
                                     value,
                                     false /* highInclusive */,
                                     tagObj,
                                     valObj,
                                     true /* includeScalar */);

    ASSERT_APPROX_EQUAL(45.5, expectedCard, 0.1);
}

}  // namespace
}  // namespace mongo::ce

/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/compiler/ce/histogram/histogram_test_utils.h"
#include "mongo/db/query/compiler/stats/max_diff.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::ce {

namespace {
namespace value = sbe::value;

using stats::CEHistogram;
using stats::getMaxBound;
using stats::getMinBound;
using stats::ScalarHistogram;
using stats::TypeCounts;

auto NumberInt64 = sbe::value::TypeTags::NumberInt64;
auto kEqual = EstimationType::kEqual;
auto kLess = EstimationType::kLess;
auto kLessOrEqual = EstimationType::kLessOrEqual;
auto kGreater = EstimationType::kGreater;
auto kGreaterOrEqual = EstimationType::kGreaterOrEqual;
auto Date = value::TypeTags::Date;
auto TimeStamp = value::TypeTags::Timestamp;

// --------------------- SCALAR HISTOGRAM ESTIMATION TESTS ---------------------
// The tests included in this section of the file evaluate the functionality and correctness of the
// ScalarHistogram.
// The tests generates histograms either by defining the specific set of buckets or by generating
// data and estimates the frequency of various keys values. The tests either perform exact
// assertions comparing the estimated cardinality with the correct value, or approximate assertions
// accepting 'kErrorBound' error.

TEST(ScalarHistogramEstimatorInterpolationTest, ManualHistogram) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 1.0, 10.0, 5.0},
                                 {20, 3.0, 15.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(55.0, getTotals(hist).card);

    ASSERT_EQ(1.0, estimateCardinalityScalarHistogramInteger(hist, 0, kEqual));
    ASSERT_EQ(2.0, estimateCardinalityScalarHistogramInteger(hist, 5, kEqual));
    ASSERT_EQ(0.0, estimateCardinalityScalarHistogramInteger(hist, 35, kEqual));

    ASSERT_EQ(15.5, estimateCardinalityScalarHistogramInteger(hist, 15, kLess));
    ASSERT_EQ(20.5, estimateCardinalityScalarHistogramInteger(hist, 15, kLessOrEqual));
    ASSERT_EQ(28, estimateCardinalityScalarHistogramInteger(hist, 20, kLess));
    ASSERT_EQ(31.0, estimateCardinalityScalarHistogramInteger(hist, 20, kLessOrEqual));

    ASSERT_EQ(42, estimateCardinalityScalarHistogramInteger(hist, 10, kGreater));
    ASSERT_EQ(43, estimateCardinalityScalarHistogramInteger(hist, 10, kGreaterOrEqual));
    ASSERT_EQ(19, estimateCardinalityScalarHistogramInteger(hist, 25, kGreater));
    ASSERT_EQ(21.5, estimateCardinalityScalarHistogramInteger(hist, 25, kGreaterOrEqual));
}

TEST(ScalarHistogramEstimatorInterpolationTest, UniformIntEstimate) {
    // This hard-codes a maxdiff histogram with 10 buckets built off a uniform int distribution with
    // a minimum of 0, a maximum of 1000, and 70 distinct values.
    std::vector<BucketData> data{{2, 1, 0, 0},
                                 {57, 3, 2, 1},
                                 {179, 5, 10, 6},
                                 {317, 5, 9, 6},
                                 {344, 3, 0, 0},
                                 {558, 4, 19, 12},
                                 {656, 2, 4, 3},
                                 {798, 3, 7, 4},
                                 {951, 5, 17, 7},
                                 {986, 1, 0, 0}};
    const ScalarHistogram hist = createHistogram(data);

    // Predicates over bucket bound.
    double expectedCard =
        estimateCardinalityScalarHistogramInteger(hist, 558, EstimationType::kEqual);
    ASSERT_EQ(4.0, expectedCard);
    expectedCard = estimateCardinalityScalarHistogramInteger(hist, 558, EstimationType::kLess);
    ASSERT_EQ(57.0, expectedCard);
    expectedCard =
        estimateCardinalityScalarHistogramInteger(hist, 558, EstimationType::kLessOrEqual);
    ASSERT_EQ(61.0, expectedCard);

    // Predicates over value inside of a bucket.

    // Query: [{$match: {a: {$eq: 530}}}].
    expectedCard = estimateCardinalityScalarHistogramInteger(hist, 530, EstimationType::kEqual);
    ASSERT_APPROX_EQUAL(1.6, expectedCard, 0.1);  // Actual: 1.

    // Query: [{$match: {a: {$lt: 530}}}].
    expectedCard = estimateCardinalityScalarHistogramInteger(hist, 530, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(52.9, expectedCard, 0.1);  // Actual: 50.

    // Query: [{$match: {a: {$lte: 530}}}].
    expectedCard =
        estimateCardinalityScalarHistogramInteger(hist, 530, EstimationType::kLessOrEqual);
    ASSERT_APPROX_EQUAL(54.5, expectedCard, 0.1);  // Actual: 51.

    // Query: [{$match: {a: {$eq: 400}}}].
    expectedCard = estimateCardinalityScalarHistogramInteger(hist, 400, EstimationType::kEqual);
    ASSERT_APPROX_EQUAL(1.6, expectedCard, 0.1);  // Actual: 1.

    // Query: [{$match: {a: {$lt: 400}}}].
    expectedCard = estimateCardinalityScalarHistogramInteger(hist, 400, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(41.3, expectedCard, 0.1);  // Actual: 39.

    // Query: [{$match: {a: {$lte: 400}}}].
    expectedCard =
        estimateCardinalityScalarHistogramInteger(hist, 400, EstimationType::kLessOrEqual);
    ASSERT_APPROX_EQUAL(43.0, expectedCard, 0.1);  // Actual: 40.
}

TEST(ScalarHistogramEstimatorInterpolationTest, NormalIntEstimate) {
    // This hard-codes a maxdiff histogram with 10 buckets built off a normal int distribution with
    // a minimum of 0, a maximum of 1000, and 70 distinct values.
    std::vector<BucketData> data{{2, 1, 0, 0},
                                 {317, 8, 20, 15},
                                 {344, 2, 0, 0},
                                 {388, 3, 0, 0},
                                 {423, 4, 2, 2},
                                 {579, 4, 12, 8},
                                 {632, 3, 2, 1},
                                 {696, 3, 5, 3},
                                 {790, 5, 4, 2},
                                 {993, 1, 21, 9}};
    const ScalarHistogram hist = createHistogram(data);

    // Predicates over bucket bound.
    double expectedCard =
        estimateCardinalityScalarHistogramInteger(hist, 696, EstimationType::kEqual);
    ASSERT_EQ(3.0, expectedCard);
    expectedCard = estimateCardinalityScalarHistogramInteger(hist, 696, EstimationType::kLess);
    ASSERT_EQ(66.0, expectedCard);
    expectedCard =
        estimateCardinalityScalarHistogramInteger(hist, 696, EstimationType::kLessOrEqual);
    ASSERT_EQ(69.0, expectedCard);

    // Predicates over value inside of a bucket.

    // Query: [{$match: {a: {$eq: 150}}}].
    expectedCard = estimateCardinalityScalarHistogramInteger(hist, 150, EstimationType::kEqual);
    ASSERT_APPROX_EQUAL(1.3, expectedCard, 0.1);  // Actual: 1.

    // Query: [{$match: {a: {$lt: 150}}}].
    expectedCard = estimateCardinalityScalarHistogramInteger(hist, 150, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(9.1, expectedCard, 0.1);  // Actual: 9.

    // Query: [{$match: {a: {$lte: 150}}}].
    expectedCard =
        estimateCardinalityScalarHistogramInteger(hist, 150, EstimationType::kLessOrEqual);
    ASSERT_APPROX_EQUAL(10.4, expectedCard, 0.1);  // Actual: 10.
}

TEST(ScalarHistogramEstimatorInterpolationTest, UniformStrEstimate) {
    // This hard-codes a maxdiff histogram with 10 buckets built off a uniform string distribution
    // with a minimum length of 3, a maximum length of 5, and 80 distinct values.
    std::vector<BucketData> data{{{"0ejz", 2, 0, 0},
                                  {"8DCaq", 3, 4, 4},
                                  {"Cy5Kw", 3, 3, 3},
                                  {"WXX7w", 3, 31, 20},
                                  {"YtzS", 2, 0, 0},
                                  {"fuK", 5, 13, 7},
                                  {"gLkp", 3, 0, 0},
                                  {"ixmVx", 2, 6, 2},
                                  {"qou", 1, 9, 6},
                                  {"z2b", 1, 9, 6}}};
    const ScalarHistogram hist = createHistogram(data);

    // Predicates over value inside of a bucket.
    const auto [tag, value] = value::makeNewString("TTV"_sd);
    value::ValueGuard vg(tag, value);

    // Query: [{$match: {a: {$eq: 'TTV'}}}].
    double expectedCard = estimateCardinality(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(1.55, expectedCard, 0.1);  // Actual: 2.

    // Query: [{$match: {a: {$lt: 'TTV'}}}].
    expectedCard = estimateCardinality(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(39.8, expectedCard, 0.1);  // Actual: 39.

    // Query: [{$match: {a: {$lte: 'TTV'}}}].
    expectedCard = estimateCardinality(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(41.3, expectedCard, 0.1);  // Actual: 41.
}

TEST(ScalarHistogramEstimatorInterpolationTest, NormalStrEstimate) {
    // This hard-codes a maxdiff histogram with 10 buckets built off a normal string distribution
    // with a minimum length of 3, a maximum length of 5, and 80 distinct values.
    std::vector<BucketData> data{{
        {"0ejz", 1, 0, 0},
        {"4FGjc", 3, 5, 3},
        {"9bU3", 2, 3, 2},
        {"Cy5Kw", 3, 3, 3},
        {"Lm4U", 2, 11, 5},
        {"TTV", 5, 14, 8},
        {"YtzS", 2, 3, 2},
        {"o9cD4", 6, 26, 16},
        {"qfmnP", 1, 4, 2},
        {"xqbi", 2, 4, 4},
    }};
    const ScalarHistogram hist = createHistogram(data);

    // Predicates over bucket bound.
    auto [tag, value] = value::makeNewString("TTV"_sd);
    value::ValueGuard vg(tag, value);

    // Query: [{$match: {a: {$eq: 'TTV'}}}].
    double expectedCard = estimateCardinality(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(5.0, expectedCard, 0.1);  // Actual: 5.

    // Query: [{$match: {a: {$lt: 'TTV'}}}].
    expectedCard = estimateCardinality(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(47.0, expectedCard, 0.1);  // Actual: 47.

    // Query: [{$match: {a: {$lte: 'TTV'}}}].
    expectedCard = estimateCardinality(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(52.0, expectedCard, 0.1);  // Actual: 52.

    // Predicates over value inside of a bucket.
    std::tie(tag, value) = value::makeNewString("Pfa"_sd);

    // Query: [{$match: {a: {$eq: 'Pfa'}}}].
    expectedCard = estimateCardinality(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(1.75, expectedCard, 0.1);  // Actual: 2.

    // Query: [{$match: {a: {$lt: 'Pfa'}}}].
    expectedCard = estimateCardinality(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(38.3, expectedCard, 0.1);  // Actual: 35.

    // Query: [{$match: {a: {$lte: 'Pfa'}}}].
    expectedCard = estimateCardinality(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(40.0, expectedCard, 0.1);  // Actual: 37.
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, OneBucketIntHistogram) {

    std::vector<BucketData> data{{100, 3.0, 27.0, 9.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(30.0, getTotals(hist).card);

    // Estimate with the bucket bound.
    ASSERT_EQ(3.0, estimateCardinality(hist, NumberInt64, 100, kEqual).card);
    ASSERT_EQ(27.0, estimateCardinality(hist, NumberInt64, 100, kLess).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, NumberInt64, 100, kLessOrEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 100, kGreater).card);
    ASSERT_EQ(3.0, estimateCardinality(hist, NumberInt64, 100, kGreaterOrEqual).card);

    // Estimate with a value inside the bucket.
    ASSERT_EQ(3.0, estimateCardinality(hist, NumberInt64, 10, kEqual).card);
    ASSERT_EQ(10.5, estimateCardinality(hist, NumberInt64, 10, kLess).card);
    ASSERT_EQ(13.5, estimateCardinality(hist, NumberInt64, 10, kLessOrEqual).card);
    ASSERT_EQ(16.5, estimateCardinality(hist, NumberInt64, 10, kGreater).card);
    ASSERT_EQ(19.5, estimateCardinality(hist, NumberInt64, 10, kGreaterOrEqual).card);

    // Estimate for a value larger than the last bucket bound.
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1000, kEqual).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, NumberInt64, 1000, kLess).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, NumberInt64, 1000, kLessOrEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1000, kGreater).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1000, kGreaterOrEqual).card);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, OneExclusiveBucketIntHistogram) {
    // Data set of a single value.
    // By exclusive bucket we mean a bucket with only boundary, that is the range frequency and
    // NDV are zero.
    std::vector<BucketData> data{{100, 2.0, 0.0, 0.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(2.0, getTotals(hist).card);

    // Estimates with the bucket boundary.

    ASSERT_EQ(2.0, estimateCardinality(hist, NumberInt64, 100, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 100, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 100, kGreater).card);

    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 0, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 0, kLess).card);
    ASSERT_EQ(2.0, estimateCardinality(hist, NumberInt64, 0, kGreater).card);

    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1000, kEqual).card);
    ASSERT_EQ(2.0, estimateCardinality(hist, NumberInt64, 1000, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1000, kGreater).card);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, OneBucketTwoIntValuesHistogram) {
    // Data set of two values, example {5, 100, 100}.
    std::vector<BucketData> data{{100, 2.0, 1.0, 1.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(3.0, getTotals(hist).card);

    // Estimates with the bucket boundary.
    ASSERT_EQ(2.0, estimateCardinality(hist, NumberInt64, 100, kEqual).card);
    ASSERT_EQ(1.0, estimateCardinality(hist, NumberInt64, 100, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 100, kGreater).card);

    ASSERT_EQ(1.0, estimateCardinality(hist, NumberInt64, 10, kEqual).card);
    ASSERT_EQ(0.5, estimateCardinality(hist, NumberInt64, 10, kLess).card);
    ASSERT_EQ(2.5, estimateCardinality(hist, NumberInt64, 10, kGreater).card);

    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1000, kEqual).card);
    ASSERT_EQ(3.0, estimateCardinality(hist, NumberInt64, 1000, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1000, kGreater).card);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, OneBucketTwoIntValuesHistogram2) {
    std::vector<BucketData> data{{100, 2.0, 3.0, 1.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(5.0, getTotals(hist).card);

    // Estimates with the bucket boundary.
    ASSERT_EQ(2.0, estimateCardinality(hist, NumberInt64, 100, kEqual).card);
    ASSERT_EQ(3.0, estimateCardinality(hist, NumberInt64, 100, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 100, kGreater).card);

    ASSERT_EQ(3.0, estimateCardinality(hist, NumberInt64, 10, kEqual).card);
    ASSERT_EQ(1.5, estimateCardinality(hist, NumberInt64, 10, kLess).card);
    ASSERT_EQ(3.5, estimateCardinality(hist, NumberInt64, 10, kGreater).card);

    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1000, kEqual).card);
    ASSERT_EQ(5.0, estimateCardinality(hist, NumberInt64, 1000, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1000, kGreater).card);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, TwoBucketsIntHistogram) {
    std::vector<BucketData> data{{1, 1.0, 0.0, 0.0}, {100, 3.0, 26.0, 8.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(30.0, getTotals(hist).card);

    // Estimates for a value smaller than the first bucket.
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, -42, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, -42, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, -42, kLessOrEqual).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, NumberInt64, -42, kGreater).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, NumberInt64, -42, kGreaterOrEqual).card);

    // Estimates with bucket bounds.
    ASSERT_EQ(1.0, estimateCardinality(hist, NumberInt64, 1, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 1, kLess).card);
    ASSERT_EQ(1.0, estimateCardinality(hist, NumberInt64, 1, kLessOrEqual).card);
    ASSERT_EQ(29.0, estimateCardinality(hist, NumberInt64, 1, kGreater).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, NumberInt64, 1, kGreaterOrEqual).card);

    ASSERT_EQ(3.0, estimateCardinality(hist, NumberInt64, 100, kEqual).card);
    ASSERT_EQ(27.0, estimateCardinality(hist, NumberInt64, 100, kLess).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, NumberInt64, 100, kLessOrEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 100, kGreater).card);
    ASSERT_EQ(3.0, estimateCardinality(hist, NumberInt64, 100, kGreaterOrEqual).card);

    // Estimates with a value inside the bucket. The estimates use interpolation.
    ASSERT_APPROX_EQUAL(3.25, estimateCardinality(hist, NumberInt64, 10, kEqual).card, kErrorBound);
    ASSERT_APPROX_EQUAL(3.36, estimateCardinality(hist, NumberInt64, 10, kLess).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        3.36, estimateCardinality(hist, NumberInt64, 10, kLessOrEqual).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        26.64, estimateCardinality(hist, NumberInt64, 10, kGreater).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        26.64, estimateCardinality(hist, NumberInt64, 10, kGreaterOrEqual).card, kErrorBound);

    ASSERT_APPROX_EQUAL(3.25, estimateCardinality(hist, NumberInt64, 50, kEqual).card, kErrorBound);
    ASSERT_APPROX_EQUAL(10.61, estimateCardinality(hist, NumberInt64, 50, kLess).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        13.87, estimateCardinality(hist, NumberInt64, 50, kLessOrEqual).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        16.13, estimateCardinality(hist, NumberInt64, 50, kGreater).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        19.38, estimateCardinality(hist, NumberInt64, 50, kGreaterOrEqual).card, kErrorBound);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, ThreeExclusiveBucketsIntHistogram) {
    std::vector<BucketData> data{{1, 1.0, 0.0, 0.0}, {10, 8.0, 0.0, 0.0}, {100, 1.0, 0.0, 0.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(10.0, getTotals(hist).card);

    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 5, kEqual).card);
    ASSERT_EQ(1.0, estimateCardinality(hist, NumberInt64, 5, kLess).card);
    ASSERT_EQ(1.0, estimateCardinality(hist, NumberInt64, 5, kLessOrEqual).card);
    ASSERT_EQ(9.0, estimateCardinality(hist, NumberInt64, 5, kGreater).card);
    ASSERT_EQ(9.0, estimateCardinality(hist, NumberInt64, 5, kGreaterOrEqual).card);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, OneBucketStrHistogram) {
    std::vector<BucketData> data{{"xyz", 3.0, 27.0, 9.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(30.0, getTotals(hist).card);

    // Estimates with bucket bound.
    auto [tag, value] = value::makeNewString("xyz"_sd);
    value::ValueGuard vg(tag, value);
    ASSERT_EQ(3.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(27.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, tag, value, kLessOrEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kGreater).card);
    ASSERT_EQ(3.0, estimateCardinality(hist, tag, value, kGreaterOrEqual).card);

    // Estimates for a value inside the bucket. Since there is no low value bound in the histogram
    // all values smaller than the upper bound will be estimated the same way using half of the
    // bucket cardinality.
    std::tie(tag, value) = value::makeNewString("a"_sd);
    ASSERT_EQ(3.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(10.5, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(13.5, estimateCardinality(hist, tag, value, kLessOrEqual).card);
    ASSERT_EQ(16.5, estimateCardinality(hist, tag, value, kGreater).card);
    ASSERT_EQ(19.5, estimateCardinality(hist, tag, value, kGreaterOrEqual).card);

    std::tie(tag, value) = value::makeNewString(""_sd);
    // In the special case of a single string bucket, we estimate empty string equality as for any
    // other string value. In practice if there are at least 2 buckets for the string data and an
    // empty string in the data set, it will be chosen as a bound for the first bucket and produce
    // precise estimates.
    ASSERT_EQ(3.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, tag, value, kGreaterOrEqual).card);

    // Estimates for a value larger than the upper bound.
    std::tie(tag, value) = value::makeNewString("z"_sd);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(30.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kGreater).card);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, TwoBucketsStrHistogram) {
    // Data set of 100 strings in the range ["abc", "xyz"], with average frequency of 2.
    std::vector<BucketData> data{{"abc", 2.0, 0.0, 0.0}, {"xyz", 3.0, 95.0, 48.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(100.0, getTotals(hist).card);

    // Estimates for a value smaller than the first bucket bound.
    auto [tag, value] = value::makeNewString("a"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kLessOrEqual).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, tag, value, kGreater).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, tag, value, kGreaterOrEqual).card);

    // Estimates with bucket bounds.
    std::tie(tag, value) = value::makeNewString("abc"_sd);
    ASSERT_EQ(2.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(2.0, estimateCardinality(hist, tag, value, kLessOrEqual).card);
    ASSERT_EQ(98.0, estimateCardinality(hist, tag, value, kGreater).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, tag, value, kGreaterOrEqual).card);

    std::tie(tag, value) = value::makeNewString("xyz"_sd);
    ASSERT_EQ(3.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(97.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, tag, value, kLessOrEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kGreater).card);
    ASSERT_EQ(3.0, estimateCardinality(hist, tag, value, kGreaterOrEqual).card);

    // Estimates for a value inside the bucket.
    std::tie(tag, value) = value::makeNewString("sun"_sd);
    ASSERT_APPROX_EQUAL(1.98, estimateCardinality(hist, tag, value, kEqual).card, kErrorBound);
    ASSERT_APPROX_EQUAL(74.39, estimateCardinality(hist, tag, value, kLess).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        76.37, estimateCardinality(hist, tag, value, kLessOrEqual).card, kErrorBound);
    ASSERT_APPROX_EQUAL(23.64, estimateCardinality(hist, tag, value, kGreater).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        25.62, estimateCardinality(hist, tag, value, kGreaterOrEqual).card, kErrorBound);

    // Estimate for a value very close to the bucket bound.
    std::tie(tag, value) = value::makeNewString("xyw"_sd);
    ASSERT_APPROX_EQUAL(1.98, estimateCardinality(hist, tag, value, kEqual).card, kErrorBound);
    ASSERT_APPROX_EQUAL(95.02, estimateCardinality(hist, tag, value, kLess).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        96.99, estimateCardinality(hist, tag, value, kLessOrEqual).card, kErrorBound);
    ASSERT_APPROX_EQUAL(3.0, estimateCardinality(hist, tag, value, kGreater).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        4.98, estimateCardinality(hist, tag, value, kGreaterOrEqual).card, kErrorBound);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, TwoBucketsDateHistogram) {
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
    ASSERT_EQ(0.0, estimateCardinality(hist, Date, valueBefore, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, Date, valueBefore, kLess).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, Date, valueBefore, kGreater).card);

    const auto valueStart = value::bitcastFrom<int64_t>(startInstant);
    ASSERT_EQ(3.0, estimateCardinality(hist, Date, valueStart, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, Date, valueStart, kLess).card);
    ASSERT_EQ(97.0, estimateCardinality(hist, Date, valueStart, kGreater).card);

    const auto valueEnd = value::bitcastFrom<int64_t>(endInstant);
    ASSERT_EQ(1.0, estimateCardinality(hist, Date, valueEnd, kEqual).card);
    ASSERT_EQ(99.0, estimateCardinality(hist, Date, valueEnd, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, Date, valueEnd, kGreater).card);

    const auto valueIn = value::bitcastFrom<int64_t>(startInstant + 43000000);
    ASSERT_EQ(2.0, estimateCardinality(hist, Date, valueIn, kEqual).card);
    ASSERT_APPROX_EQUAL(48.77, estimateCardinality(hist, Date, valueIn, kLess).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        49.22, estimateCardinality(hist, Date, valueIn, kGreater).card, kErrorBound);

    const auto valueAfter = value::bitcastFrom<int64_t>(endInstant + 100);
    ASSERT_EQ(0.0, estimateCardinality(hist, Date, valueAfter, kEqual).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, Date, valueAfter, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, Date, valueAfter, kGreater).card);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, TwoBucketsTimestampHistogram) {
    // June 6, 2017 -- June 7, 2017 in seconds.
    const int64_t startInstant = 1496777923LL;
    const int64_t endInstant = 1496864323LL;
    const Timestamp startTs{Seconds(startInstant), 0};
    const Timestamp endTs{Seconds(endInstant), 0};

    std::vector<BucketData> data{{Value(startTs), 3.0, 0.0, 0.0}, {Value(endTs), 1.0, 96.0, 48.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(100.0, getTotals(hist).card);

    const auto valueBefore = value::bitcastFrom<int64_t>(startTs.asULL() - 1);
    ASSERT_EQ(0.0, estimateCardinality(hist, TimeStamp, valueBefore, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, TimeStamp, valueBefore, kLess).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, TimeStamp, valueBefore, kGreater).card);

    const auto valueStart = value::bitcastFrom<int64_t>(
        startTs.asULL());  // NB: startTs.asInt64() produces different value.
    ASSERT_EQ(3.0, estimateCardinality(hist, TimeStamp, valueStart, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, TimeStamp, valueStart, kLess).card);
    ASSERT_EQ(97.0, estimateCardinality(hist, TimeStamp, valueStart, kGreater).card);

    const auto valueEnd = value::bitcastFrom<int64_t>(endTs.asULL());
    ASSERT_EQ(1.0, estimateCardinality(hist, TimeStamp, valueEnd, kEqual).card);
    ASSERT_EQ(99.0, estimateCardinality(hist, TimeStamp, valueEnd, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, TimeStamp, valueEnd, kGreater).card);

    const auto valueIn = value::bitcastFrom<int64_t>((startTs.asULL() + endTs.asULL()) / 2);
    ASSERT_EQ(2.0, estimateCardinality(hist, TimeStamp, valueIn, kEqual).card);
    ASSERT_APPROX_EQUAL(
        49.0, estimateCardinality(hist, TimeStamp, valueIn, kLess).card, kErrorBound);
    ASSERT_APPROX_EQUAL(
        49.0, estimateCardinality(hist, TimeStamp, valueIn, kGreater).card, kErrorBound);

    const auto valueAfter = value::bitcastFrom<int64_t>(endTs.asULL() + 100);
    ASSERT_EQ(0.0, estimateCardinality(hist, TimeStamp, valueAfter, kEqual).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, TimeStamp, valueAfter, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, TimeStamp, valueAfter, kGreater).card);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, TwoBucketsObjectIdHistogram) {
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
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, tag, value, kGreater).card);

    // Bucket bounds.
    startOid.view().readInto(value::getObjectIdView(value));
    ASSERT_EQ(2.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(98.0, estimateCardinality(hist, tag, value, kGreater).card);

    endOid.view().readInto(value::getObjectIdView(value));
    ASSERT_EQ(1.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(99.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kGreater).card);

    // ObjectId value inside the bucket.
    const auto oidInside = OID("63340db2cd4d46ff39178e9d");
    oidInside.view().readInto(value::getObjectIdView(value));
    ASSERT_APPROX_EQUAL(1.25, estimateCardinality(hist, tag, value, kEqual).card, kErrorBound);
    ASSERT_APPROX_EQUAL(74.00, estimateCardinality(hist, tag, value, kLess).card, kErrorBound);
    ASSERT_APPROX_EQUAL(24.74, estimateCardinality(hist, tag, value, kGreater).card, kErrorBound);

    const auto oidAfter = OID("63340dbed6cd8af737d4139b");
    oidAfter.view().readInto(value::getObjectIdView(value));
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kEqual).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, tag, value, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, tag, value, kGreater).card);
}

/**
 * Tests for cardinality estimates for queries over minimum values of date, timestamp, and objectId
 * types. When the histogram has at least 2 buckets per data type, the minimum value, if present in
 * the data, is picked as a bound for the first bucket for the corresponding data type. In this case
 * the cardinality estimates are precise. To test the approximate estimation, we force the histogram
 * generation to use one bucket per type (except the first numeric type).
 */
TEST(ScalarHistogramEstimatorEdgeCasesTest, MinValueMixedHistogramFromData) {
    const int64_t startInstant = 1506777923000LL;
    const int64_t endInstant = 1516864323000LL;
    const Timestamp startTs{Seconds(1516864323LL), 0};
    const Timestamp endTs{Seconds(1526864323LL), 0};
    const auto startOid = OID("63340d8d27afef2de7357e8d");
    const auto endOid = OID("63340dbed6cd8af737d4139a");

    std::vector<stats::SBEValue> data;
    data.emplace_back(Date, value::bitcastFrom<int64_t>(startInstant));
    data.emplace_back(Date, value::bitcastFrom<int64_t>(endInstant));

    data.emplace_back(value::TypeTags::Timestamp, value::bitcastFrom<int64_t>(startTs.asULL()));
    data.emplace_back(value::TypeTags::Timestamp, value::bitcastFrom<int64_t>(endTs.asULL()));

    auto [tag, val] = stats::makeInt64Value(100);
    data.emplace_back(tag, val);
    std::tie(tag, val) = stats::makeInt64Value(1000);
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
    auto&& [minOid, inclOid] = getMinBound(value::TypeTags::ObjectId);
    auto minOidTag = minOid.getTag();
    auto minOidVal = minOid.getValue();
    ASSERT_EQ(1.0, estimateCardinality(hist, minOidTag, minOidVal, kEqual).card);

    // Minimum date.
    const auto&& [minDate, inclDate] = getMinBound(Date);
    const auto minDateTag = minDate.getTag();
    const auto minDateVal = minDate.getValue();
    ASSERT_EQ(1.0, estimateCardinality(hist, minDateTag, minDateVal, kEqual).card);

    // Minimum timestamp.
    auto&& [minTs, inclTs] = getMinBound(value::TypeTags::Timestamp);
    auto minTsTag = minTs.getTag();
    auto minTsVal = minTs.getValue();
    ASSERT_EQ(1.0, estimateCardinality(hist, minTsTag, minTsVal, kEqual).card);

    // Add minimum values to the data set and create another histogram.
    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);
    std::tie(copyTag, copyVal) = value::copyValue(tagLowStr, valLowStr);
    data.emplace_back(copyTag, copyVal);
    data.emplace_back(minDateTag, minDateVal);
    data.emplace_back(minTsTag, minTsVal);

    sortValueVector(data);
    const ScalarHistogram& hist2 = makeHistogram(data, 6);

    // Precise estimate for equality to empty string, it is a bucket boundary.
    ASSERT_EQ(1.0, estimateCardinality(hist2, tagLowStr, valLowStr, kEqual).card);

    // Equality to the minimum date/ts value is estimated by range_frequency/NDV.
    ASSERT_EQ(1.0, estimateCardinality(hist2, minDateTag, minDateVal, kEqual).card);
    ASSERT_EQ(1.0, estimateCardinality(hist2, minTsTag, minTsVal, kEqual).card);

    // [minDate, startInstant], estimated by the half of the date bucket.
    auto expectedCard = estimateCardinalityRange(hist2,
                                                 true /* lowInclusive */,
                                                 minDateTag,
                                                 minDateVal,
                                                 true /* highInclusive */,
                                                 value::TypeTags::Date,
                                                 value::bitcastFrom<int64_t>(startInstant));
    ASSERT_EQ(1.0, expectedCard.card);

    // [minDate, endInstant], estimated by the entire date bucket.
    expectedCard = estimateCardinalityRange(hist2,
                                            true,
                                            minDateTag,
                                            minDateVal,
                                            true,
                                            value::TypeTags::Date,
                                            value::bitcastFrom<int64_t>(endInstant));
    ASSERT_EQ(3.0, expectedCard.card);

    // [minDate, minTs), estimated by the entire date bucket.
    // (is this interval possible or is it better to have maxDate upper bound?).
    expectedCard =
        estimateCardinalityRange(hist2, true, minDateTag, minDateVal, false, minTsTag, minTsVal);
    ASSERT_EQ(3.0, expectedCard.card);

    // [minTs, startTs], estimated by the half of the timestamp bucket.
    expectedCard = estimateCardinalityRange(hist2,
                                            true,
                                            minTsTag,
                                            minTsVal,
                                            true,
                                            value::TypeTags::Timestamp,
                                            value::bitcastFrom<int64_t>(startTs.asULL()));
    ASSERT_EQ(1.0, expectedCard.card);

    // [minTs, endTs], estimated by the entire timestamp bucket.
    expectedCard = estimateCardinalityRange(hist2,
                                            true,
                                            minTsTag,
                                            minTsVal,
                                            true,
                                            value::TypeTags::Timestamp,
                                            value::bitcastFrom<int64_t>(endTs.asULL()));
    ASSERT_EQ(3.0, expectedCard.card);

    // [minTs, maxTs], estimated by the entire timestamp bucket.
    auto&& [maxTs, inclMaxTs] = getMaxBound(value::TypeTags::Timestamp);
    const auto maxTsTag = maxTs.getTag();
    const auto maxTsVal = maxTs.getValue();
    expectedCard =
        estimateCardinalityRange(hist2, true, minTsTag, minTsVal, true, maxTsTag, maxTsVal);
    ASSERT_EQ(3.0, expectedCard.card);
}

TEST(ScalarHistogramEstimatorEdgeCasesTest, MinValueMixedHistogramFromBuckets) {
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
    ASSERT_EQ(500.0, getTotals(hist).card);

    // Minimum ObjectId.
    auto&& [minOid, inclOid] = getMinBound(value::TypeTags::ObjectId);
    auto minOidTag = minOid.getTag();
    auto minOidVal = minOid.getValue();
    ASSERT_APPROX_EQUAL(
        1.9, estimateCardinality(hist, minOidTag, minOidVal, kEqual).card, kErrorBound);

    // Minimum date.
    const auto&& [minDate, inclDate] = getMinBound(Date);
    const auto minDateTag = minDate.getTag();
    const auto minDateVal = minDate.getValue();
    ASSERT_EQ(4.0, estimateCardinality(hist, minDateTag, minDateVal, kEqual).card);

    // Minimum timestamp.
    auto&& [minTs, inclTs] = getMinBound(value::TypeTags::Timestamp);
    auto minTsTag = minTs.getTag();
    auto minTsVal = minTs.getValue();
    ASSERT_APPROX_EQUAL(
        1.9, estimateCardinality(hist, minTsTag, minTsVal, kEqual).card, kErrorBound);

    // [minDate, innerDate], estimated by the half of the date bucket.
    const int64_t innerDate = 1516864323000LL;
    auto expectedCard = estimateCardinalityRange(hist,
                                                 true,
                                                 minDateTag,
                                                 minDateVal,
                                                 true,
                                                 value::TypeTags::Date,
                                                 value::bitcastFrom<int64_t>(innerDate));
    ASSERT_APPROX_EQUAL(48.0, expectedCard.card, kErrorBound);

    // [minTs, innerTs], estimated by the half of the timestamp bucket.
    const Timestamp innerTs{Seconds(1516864323LL), 0};
    expectedCard = estimateCardinalityRange(hist,
                                            true,
                                            minTsTag,
                                            minTsVal,
                                            true,
                                            value::TypeTags::Timestamp,
                                            value::bitcastFrom<int64_t>(innerTs.asULL()));
    ASSERT_APPROX_EQUAL(47.5, expectedCard.card, kErrorBound);
}


// --------------------- CE HISTOGRAM ESTIMATION TESTS ---------------------
// The tests included in this section of the file evaluate the functionality and correctness of the
// CEHistogram.
// The tests generates ce_histograms and estimates the frequency of various keys values. The
// tests either perform exact assertions comparing the estimated cardinality with the correct value,
// or approximate assertions accepting 'kErrorBound' error.

/**
 * Structure representing a range query and its estimated and actual cardinalities.
 * Used to record hand-crafted queries over a pre-generated dataset.
 */
struct QuerySpec {
    // Low bound of the query range.
    int32_t low;
    // Upper bound of the query range.
    int32_t high;
    // Estimated cardinality of $match query.
    double estMatch;
    // Actual cardinality of $match query.
    double actMatch;
    // Estimated cardinality of $elemMatch query.
    double estElemMatch;
    // Actual cardinality of $elemMatch query.
    double actElemMatch;
};

static std::pair<double, double> computeErrors(size_t actualCard, double estimatedCard) {
    double error = estimatedCard - actualCard;
    double relError = (actualCard == 0) ? (estimatedCard == 0 ? 0.0 : -1.0) : error / actualCard;
    return std::make_pair(error, relError);
}

static std::string serializeQuery(QuerySpec& q, bool isElemMatch) {
    std::ostringstream os;
    os << "{$match: {a: {";
    if (isElemMatch) {
        os << "$elemMatch: {";
    }
    os << "$gt: " << q.low;
    os << ", $lt: " << q.high;
    if (isElemMatch) {
        os << "}";
    }
    os << "}}}\n";
    return os.str();
}

static std::string computeRMSE(std::vector<QuerySpec>& querySet, bool isElemMatch) {
    double rms = 0.0, relRms = 0.0, meanAbsSelErr = 0.0;
    size_t trialSize = querySet.size();
    const size_t dataSize = 1000;

    std::ostringstream os;
    os << "\nQueries:\n";
    for (auto& q : querySet) {
        double estimatedCard = isElemMatch ? q.estElemMatch : q.estMatch;
        double actualCard = isElemMatch ? q.actElemMatch : q.actMatch;

        auto [error, relError] = computeErrors(actualCard, estimatedCard);
        rms += error * error;
        relRms += relError * relError;
        meanAbsSelErr += std::abs(error);
        os << serializeQuery(q, isElemMatch);
        os << "Estimated: " << estimatedCard << " Actual " << actualCard << " (Error: " << error
           << " RelError: " << relError << ")\n\n";
    }
    rms = std::sqrt(rms / trialSize);
    relRms = std::sqrt(relRms / trialSize);
    meanAbsSelErr /= (trialSize * dataSize);

    os << "=====" << (isElemMatch ? " ElemMatch errors: " : "Match errors:") << "=====\n";
    os << "RMSE : " << rms << " RelRMSE : " << relRms
       << " MeanAbsSelectivityError: " << meanAbsSelErr << std::endl;
    return os.str();
}

TEST(CEHistogramEstimatorTest, ManualHistogram) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 1.0, 10.0, 5.0},
                                 {20, 3.0, 15.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const double intCnt = 55;
    const ScalarHistogram& hist = createHistogram(data);
    const auto ceHist = CEHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(3.0, estimateCardinalityEq(*ceHist, NumberInt64, 20, true).card);
    ASSERT_EQ(1.0, estimateCardinalityEq(*ceHist, NumberInt64, 50, true).card);
    ASSERT_EQ(0,
              estimateCardinalityEq(*ceHist, NumberInt64, 40, false)
                  .card);  // should be 2.0 for includeScalar: true
    // value not in data
    ASSERT_EQ(0, estimateCardinalityEq(*ceHist, NumberInt64, 60, true).card);
}

TEST(CEHistogramEstimatorTest, UniformIntHistogram) {
    std::vector<BucketData> data{{2, 1, 0, 0},
                                 {57, 3, 2, 1},
                                 {179, 5, 10, 6},
                                 {317, 5, 9, 6},
                                 {344, 3, 0, 0},
                                 {558, 4, 19, 12},
                                 {656, 2, 4, 3},
                                 {798, 3, 7, 4},
                                 {951, 5, 17, 7},
                                 {986, 1, 0, 0}};
    const ScalarHistogram& hist = createHistogram(data);
    const double intCnt = 100;
    const auto ceHist = CEHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(4.0, estimateCardinalityEq(*ceHist, NumberInt64, 558, true).card);
    ASSERT_APPROX_EQUAL(1.6,
                        estimateCardinalityEq(*ceHist, NumberInt64, 530, true).card,
                        0.1);  // Actual: 1.
    ASSERT_APPROX_EQUAL(1.6,
                        estimateCardinalityEq(*ceHist, NumberInt64, 400, true).card,
                        0.1);  // Actual: 1.
}

TEST(CEHistogramEstimatorTest, NormalIntArrayHistogram) {
    std::vector<BucketData> data{{2, 1, 0, 0},
                                 {317, 8, 20, 15},
                                 {344, 2, 0, 0},
                                 {388, 3, 0, 0},
                                 {423, 4, 2, 2},
                                 {579, 4, 12, 8},
                                 {632, 3, 2, 1},
                                 {696, 3, 5, 3},
                                 {790, 5, 4, 2},
                                 {993, 1, 21, 9}};
    const ScalarHistogram hist = createHistogram(data);
    const double intCnt = 100;
    const auto ceHist = CEHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(3.0, estimateCardinalityEq(*ceHist, NumberInt64, 696, true).card);
    ASSERT_APPROX_EQUAL(1.3,
                        estimateCardinalityEq(*ceHist, NumberInt64, 150, true).card,
                        0.1);  // Actual: 1.
}

TEST(CEHistogramEstimatorTest, SkewedIntHistogram) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 150.0, 10.0, 5.0},
                                 {20, 100.0, 14.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const double intCnt = 300;
    const ScalarHistogram& hist = createHistogram(data);
    const auto ceHist = CEHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(150.0, estimateCardinalityEq(*ceHist, NumberInt64, 10, true).card);
    ASSERT_EQ(100.0, estimateCardinalityEq(*ceHist, NumberInt64, 20, true).card);
    ASSERT_EQ(1.0, estimateCardinalityEq(*ceHist, NumberInt64, 30, true).card);
    ASSERT_EQ(0, estimateCardinalityEq(*ceHist, NumberInt64, 40, false).card);
}

TEST(CEHistogramEstimatorTest, StringHistogram) {
    std::vector<BucketData> data{
        {"testA", 5.0, 2.0, 1.0}, {"testB", 3.0, 2.0, 2.0}, {"testC", 2.0, 1.0, 1.0}};
    const double strCnt = 15;
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(strCnt, getTotals(hist).card);

    const auto ceHist =
        CEHistogram::make(hist, stats::TypeCounts{{value::TypeTags::StringSmall, strCnt}}, strCnt);

    auto [tag, value] = value::makeNewString("testA"_sd);
    value::ValueGuard vg(tag, value);
    ASSERT_EQ(5.0, estimateCardinalityEq(*ceHist, tag, value, true).card);

    std::tie(tag, value) = value::makeNewString("testB"_sd);
    ASSERT_EQ(3.0, estimateCardinalityEq(*ceHist, tag, value, true).card);

    std::tie(tag, value) = value::makeNewString("testC"_sd);
    ASSERT_EQ(0, estimateCardinalityEq(*ceHist, tag, value, false).card);
}

TEST(CEHistogramEstimatorTest, UniformStrHistogram) {
    std::vector<BucketData> data{{{"0ejz", 2, 0, 0},
                                  {"8DCaq", 3, 4, 4},
                                  {"Cy5Kw", 3, 3, 3},
                                  {"WXX7w", 3, 31, 20},
                                  {"YtzS", 2, 0, 0},
                                  {"fuK", 5, 13, 7},
                                  {"gLkp", 3, 0, 0},
                                  {"ixmVx", 2, 6, 2},
                                  {"qou", 1, 9, 6},
                                  {"z2b", 1, 9, 6}}};
    const double strCnt = 100;
    const ScalarHistogram& hist = createHistogram(data);

    const auto ceHist =
        CEHistogram::make(hist, stats::TypeCounts{{value::TypeTags::StringSmall, strCnt}}, strCnt);

    const auto [tag, value] = value::makeNewString("TTV"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_APPROX_EQUAL(
        1.55, estimateCardinalityEq(*ceHist, tag, value, true).card, 0.1);  // Actual: 2.
}

TEST(CEHistogramEstimatorTest, NormalStrHistogram) {
    std::vector<BucketData> data{{
        {"0ejz", 1, 0, 0},
        {"4FGjc", 3, 5, 3},
        {"9bU3", 2, 3, 2},
        {"Cy5Kw", 3, 3, 3},
        {"Lm4U", 2, 11, 5},
        {"TTV", 5, 14, 8},
        {"YtzS", 2, 3, 2},
        {"o9cD4", 6, 26, 16},
        {"qfmnP", 1, 4, 2},
        {"xqbi", 2, 4, 4},
    }};
    const double strCnt = 100;
    const ScalarHistogram& hist = createHistogram(data);

    const auto ceHist =
        CEHistogram::make(hist, stats::TypeCounts{{value::TypeTags::StringSmall, strCnt}}, strCnt);

    auto [tag, value] = value::makeNewString("TTV"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_APPROX_EQUAL(
        5.0, estimateCardinalityEq(*ceHist, tag, value, true).card, 0.1);  // Actual: 5.

    std::tie(tag, value) = value::makeNewString("Pfa"_sd);
    ASSERT_APPROX_EQUAL(
        1.75, estimateCardinalityEq(*ceHist, tag, value, true).card, 0.1);  // Actual: 2.
}

TEST(CEHistogramEstimatorTest, IntStrHistogram) {
    std::vector<BucketData> data{{1, 1.0, 0.0, 0.0}, {"test", 20.0, 0.0, 0.0}};
    const double intCnt = 1;
    const double strCnt = 20;
    const double totalCnt = intCnt + strCnt;
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(totalCnt, getTotals(hist).card);

    const auto ceHist = CEHistogram::make(
        hist,
        stats::TypeCounts{{NumberInt64, intCnt}, {value::TypeTags::StringSmall, strCnt}},
        totalCnt);
    auto [tag, value] = value::makeNewString("test"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_EQ(20.0, estimateCardinalityEq(*ceHist, tag, value, true).card);
    ASSERT_EQ(1.0, estimateCardinalityEq(*ceHist, NumberInt64, 1, true).card);
    ASSERT_EQ(0, estimateCardinalityEq(*ceHist, tag, value, false).card);
    ASSERT_EQ(0, estimateCardinalityEq(*ceHist, NumberInt64, 1, false).card);
}

TEST(CEHistogramEstimatorTest, UniformIntStrHistogram) {
    std::vector<BucketData> data{{
        {2, 3, 0, 0},       {19, 4, 1, 1},      {226, 2, 49, 20},  {301, 5, 12, 4},
        {317, 3, 0, 0},     {344, 2, 3, 1},     {423, 5, 18, 6},   {445, 3, 0, 0},
        {495, 3, 4, 2},     {542, 5, 9, 3},     {696, 3, 44, 19},  {773, 4, 11, 5},
        {805, 2, 8, 4},     {931, 5, 21, 8},    {998, 4, 21, 3},   {"8N4", 5, 31, 14},
        {"MIb", 5, 45, 17}, {"Zgi", 3, 55, 22}, {"pZ", 6, 62, 25}, {"yUwxz", 5, 29, 12},
    }};
    const double intCnt = 254;
    const double strCnt = 246;
    const double totalCnt = intCnt + strCnt;
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(totalCnt, getTotals(hist).card);

    const auto ceHist = CEHistogram::make(
        hist,
        stats::TypeCounts{{NumberInt64, intCnt}, {value::TypeTags::StringSmall, strCnt}},
        totalCnt);

    ASSERT_APPROX_EQUAL(7.0,
                        estimateCardinalityEq(*ceHist, NumberInt64, 993, true).card,
                        0.1);  // Actual: 9

    auto [tag, value] = value::makeNewString("04e"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_APPROX_EQUAL(
        2.2, estimateCardinalityEq(*ceHist, tag, value, true).card, 0.1);  // Actual: 3.

    value::TypeTags lowTag = value::TypeTags::NumberInt64;
    value::Value lowVal = 100000000;

    ASSERT_APPROX_EQUAL(0.0,
                        estimateCardinalityEq(*ceHist, lowTag, lowVal, true).card,
                        0.1);  // Actual: 0

    // Query: [{$match: {a: {$lt: '04e'}}}].
    auto expectedCard = estimateCardinalityRange(*ceHist,
                                                 false /* lowInclusive */,
                                                 lowTag,
                                                 lowVal,
                                                 false /* highInclusive */,
                                                 tag,
                                                 value,
                                                 true /* includeScalar */,
                                                 ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(13.3, expectedCard.card, 0.1);  // Actual: 0.

    // Query: [{$match: {a: {$lte: '04e'}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            true /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(15.5, expectedCard.card, 0.1);  // Actual: 3.

    // Value towards the end of the bucket gets the same half bucket estimate.
    std::tie(tag, value) = value::makeNewString("8B5"_sd);

    // Query: [{$match: {a: {$lt: '8B5'}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            false /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(13.3, expectedCard.card, 0.1);  // Actual: 24.

    // Query: [{$match: {a: {$lte: '8B5'}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            true /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(15.5, expectedCard.card, 0.1);  // Actual: 29.
}

TEST(CEHistogramEstimatorInterpolationTest, UniformIntStrEstimate) {
    // This hard-codes a maxdiff histogram with 20 buckets built off of a uniform distribution with
    // two types occurring with equal probability:
    // - 100 distinct ints between 0 and 1000, and
    // - 100 distinct strings of length between 2 and 5.
    constexpr double numInt = 254;
    constexpr double numStr = 246;
    constexpr double collCard = numInt + numStr;
    std::vector<BucketData> data{{
        {2, 3, 0, 0},       {19, 4, 1, 1},      {226, 2, 49, 20},  {301, 5, 12, 4},
        {317, 3, 0, 0},     {344, 2, 3, 1},     {423, 5, 18, 6},   {445, 3, 0, 0},
        {495, 3, 4, 2},     {542, 5, 9, 3},     {696, 3, 44, 19},  {773, 4, 11, 5},
        {805, 2, 8, 4},     {931, 5, 21, 8},    {998, 4, 21, 3},   {"8N4", 5, 31, 14},
        {"MIb", 5, 45, 17}, {"Zgi", 3, 55, 22}, {"pZ", 6, 62, 25}, {"yUwxz", 5, 29, 12},
    }};
    const ScalarHistogram hist = createHistogram(data);
    const auto ceHist = CEHistogram::make(
        hist,
        TypeCounts{{value::TypeTags::NumberInt64, numInt}, {value::TypeTags::StringSmall, numStr}},
        collCard);

    // Predicates over value inside of the last numeric bucket.

    // Query: [{$match: {a: {$eq: 993}}}].
    EstimationResult expectedCard{
        estimateCardinalityScalarHistogramInteger(hist, 993, EstimationType::kEqual)};
    ASSERT_APPROX_EQUAL(7.0, expectedCard.card, 0.1);  // Actual: 9.

    // Query: [{$match: {a: {$lt: 993}}}].
    expectedCard = {estimateCardinalityScalarHistogramInteger(hist, 993, EstimationType::kLess)};
    ASSERT_APPROX_EQUAL(241.4, expectedCard.card, 0.1);  // Actual: 241.

    // Query: [{$match: {a: {$lte: 993}}}].
    expectedCard = {
        estimateCardinalityScalarHistogramInteger(hist, 993, EstimationType::kLessOrEqual)};
    ASSERT_APPROX_EQUAL(248.4, expectedCard.card, 0.1);  // Actual: 250.

    // Predicates over value inside of the first string bucket.
    auto [tag, value] = value::makeNewString("04e"_sd);
    value::ValueGuard vg(tag, value);

    // Query: [{$match: {a: {$eq: '04e'}}}].
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kEqual).card};
    ASSERT_APPROX_EQUAL(2.2, expectedCard.card, 0.1);  // Actual: 3.

    value::TypeTags lowTag = value::TypeTags::NumberInt64;
    value::Value lowVal = 100000000;

    // Type bracketing: low value of different type than the bucket bound.
    // Query: [{$match: {a: {$eq: 100000000}}}].
    expectedCard = estimateCardinalityEq(*ceHist, lowTag, lowVal, true /* includeScalar */);
    ASSERT_APPROX_EQUAL(0.0, expectedCard.card, 0.1);  // Actual: 0.

    // No interpolation for inequality to values inside the first string bucket, fallback to half of
    // the bucket frequency.

    // Query: [{$match: {a: {$lt: '04e'}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            false /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(13.3, expectedCard.card, 0.1);  // Actual: 0.

    // Query: [{$match: {a: {$lte: '04e'}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            true /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(15.5, expectedCard.card, 0.1);  // Actual: 3.

    // Value towards the end of the bucket gets the same half bucket estimate.
    std::tie(tag, value) = value::makeNewString("8B5"_sd);

    // Query: [{$match: {a: {$lt: '8B5'}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            false /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(13.3, expectedCard.card, 0.1);  // Actual: 24.

    // Query: [{$match: {a: {$lte: '8B5'}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            true /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(15.5, expectedCard.card, 0.1);  // Actual: 29.
}

TEST(CEHistogramEstimatorInterpolationTest, UniformIntArrayOnlyEstimate) {
    // This hard-codes a maxdiff histogram with 10 buckets built off of an array distribution with
    // arrays between 3 and 5 elements long, each containing 100 distinct ints uniformly distributed
    // between 0 and 1000. There are no scalar elements.
    std::vector<BucketData> scalarData{{}};
    const ScalarHistogram scalarHist = createHistogram(scalarData);

    std::vector<BucketData> minData{{
        {5, 3, 0, 0},   {19, 5, 2, 1},  {57, 4, 4, 3},  {116, 7, 13, 7}, {198, 3, 15, 6},
        {228, 2, 3, 2}, {254, 4, 0, 0}, {280, 2, 2, 1}, {335, 3, 5, 3},  {344, 2, 0, 0},
        {388, 3, 0, 0}, {420, 2, 0, 0}, {454, 1, 6, 3}, {488, 2, 1, 1},  {530, 1, 0, 0},
        {561, 1, 0, 0}, {609, 1, 0, 0}, {685, 1, 0, 0}, {713, 1, 0, 0},  {758, 1, 0, 0},
    }};
    const ScalarHistogram minHist = createHistogram(minData);

    std::vector<BucketData> maxData{{
        {301, 1, 0, 0},  {408, 2, 0, 0}, {445, 1, 0, 0}, {605, 2, 0, 0},  {620, 1, 0, 0},
        {665, 1, 1, 1},  {687, 3, 0, 0}, {704, 2, 6, 2}, {718, 2, 2, 1},  {741, 2, 1, 1},
        {752, 2, 0, 0},  {823, 7, 3, 3}, {827, 1, 0, 0}, {852, 3, 0, 0},  {864, 5, 0, 0},
        {909, 7, 12, 5}, {931, 2, 3, 1}, {939, 3, 0, 0}, {970, 2, 12, 4}, {998, 1, 10, 4},
    }};
    const ScalarHistogram maxHist = createHistogram(maxData);

    std::vector<BucketData> uniqueData{{
        {5, 3, 0, 0},    {19, 6, 2, 1},    {57, 4, 4, 3},    {116, 7, 15, 8},  {228, 2, 38, 13},
        {254, 7, 0, 0},  {269, 10, 0, 0},  {280, 7, 3, 1},   {306, 4, 1, 1},   {317, 4, 0, 0},
        {344, 2, 19, 5}, {423, 2, 27, 8},  {507, 2, 22, 13}, {704, 8, 72, 34}, {718, 6, 3, 1},
        {758, 3, 13, 4}, {864, 7, 35, 14}, {883, 4, 0, 0},   {939, 5, 32, 10}, {998, 1, 24, 9},
    }};
    const ScalarHistogram uniqueHist = createHistogram(uniqueData);

    const auto ceHist = CEHistogram::make(scalarHist,
                                          TypeCounts{{value::TypeTags::Array, 100}},
                                          uniqueHist,
                                          minHist,
                                          maxHist,
                                          // There are 100 non-empty int-only arrays.
                                          TypeCounts{{value::TypeTags::NumberInt64, 100}},
                                          100.0 /* sampleSize */);

    // Query in the middle of the domain: estimate from ArrayUnique histogram.
    value::TypeTags lowTag = value::TypeTags::NumberInt64;
    value::Value lowVal = 500;
    value::TypeTags highTag = value::TypeTags::NumberInt64;
    value::Value highVal = 600;

    // Test interpolation for query: [{$match: {a: {$elemMatch: {$gt: 500, $lt: 600}}}}].
    auto expectedCard = estimateCardinalityRange(*ceHist,
                                                 false /* lowInclusive */,
                                                 lowTag,
                                                 lowVal,
                                                 false /* highInclusive */,
                                                 highTag,
                                                 highVal,
                                                 false /* includeScalar */,
                                                 ArrayRangeEstimationAlgo::kExactArrayCE);
    ASSERT_APPROX_EQUAL(27.0, expectedCard.card, 0.1);  // actual 21.

    // Test interpolation for query: [{$match: {a: {$gt: 500, $lt: 600}}}].
    // Note: although there are no scalars, the estimate is different than the
    // above since we use different formulas.
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            false /* highInclusive */,
                                            highTag,
                                            highVal,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(92.0, expectedCard.card, 0.1);  // actual 92.

    // Query at the end of the domain: more precise estimates from ArrayMin, ArrayMax histograms.
    lowVal = 10;
    highVal = 110;

    // Test interpolation for query: [{$match: {a: {$elemMatch: {$gt: 10, $lt: 110}}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            false /* highInclusive */,
                                            highTag,
                                            highVal,
                                            false /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kExactArrayCE);
    ASSERT_APPROX_EQUAL(24.1, expectedCard.card, 0.1);  // actual 29.

    // Test interpolation for query: [{$match: {a: {$gt: 10, $lt: 110}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            false /* highInclusive */,
                                            highTag,
                                            highVal,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(27.8, expectedCard.card, 0.1);  // actual 31.
}

TEST(CEHistogramEstimatorInterpolationTest, UniformIntMixedArrayEstimate) {
    // This hard-codes a maxdiff histogram with 20 buckets built off of a mixed distribution split
    // with equal probability between:
    // - an array distribution between 3 and 5 elements long, each containing 80 distinct ints
    // uniformly distributed between 0 and 1000, and
    // - a uniform int distribution with 80 distinct ints between 0 and 1000.
    std::vector<BucketData> scalarData{{
        {25, 1, 0, 0},  {41, 2, 0, 0},  {142, 2, 3, 3},  {209, 3, 3, 1}, {243, 1, 2, 1},
        {296, 3, 4, 3}, {321, 5, 4, 2}, {480, 3, 9, 8},  {513, 3, 3, 2}, {554, 1, 0, 0},
        {637, 3, 3, 2}, {666, 2, 1, 1}, {697, 2, 2, 1},  {750, 3, 3, 2}, {768, 4, 0, 0},
        {791, 4, 3, 3}, {851, 2, 2, 2}, {927, 2, 10, 6}, {958, 3, 2, 1}, {980, 3, 0, 0},
    }};
    const ScalarHistogram scalarHist = createHistogram(scalarData);

    std::vector<BucketData> minData{{
        {3, 3, 0, 0},   {5, 8, 0, 0},   {9, 3, 0, 0},   {19, 2, 0, 0},  {49, 7, 4, 2},
        {69, 6, 0, 0},  {115, 3, 5, 3}, {125, 2, 0, 0}, {146, 1, 2, 1}, {198, 2, 4, 3},
        {214, 2, 0, 0}, {228, 3, 0, 0}, {260, 3, 4, 1}, {280, 1, 2, 2}, {330, 2, 2, 1},
        {344, 6, 0, 0}, {388, 2, 0, 0}, {420, 2, 0, 0}, {461, 2, 8, 4}, {696, 1, 2, 1},
    }};
    const ScalarHistogram minHist = createHistogram(minData);

    std::vector<BucketData> maxData{{
        {301, 1, 0, 0}, {445, 1, 0, 0},  {491, 1, 0, 0}, {533, 3, 0, 0},  {605, 3, 0, 0},
        {620, 2, 0, 0}, {647, 3, 0, 0},  {665, 4, 0, 0}, {713, 3, 10, 4}, {741, 3, 0, 0},
        {814, 3, 2, 2}, {839, 2, 1, 1},  {864, 1, 2, 2}, {883, 3, 0, 0},  {893, 7, 0, 0},
        {898, 5, 0, 0}, {909, 1, 12, 3}, {931, 2, 2, 1}, {953, 6, 3, 2},  {993, 1, 7, 5},
    }};
    const ScalarHistogram maxHist = createHistogram(maxData);

    std::vector<BucketData> uniqueData{{
        {3, 3, 0, 0},     {19, 5, 11, 2},   {49, 7, 5, 3},    {69, 8, 0, 0},    {75, 3, 0, 0},
        {125, 2, 10, 5},  {228, 3, 27, 14}, {260, 4, 5, 1},   {344, 6, 36, 13}, {423, 4, 20, 8},
        {605, 4, 61, 28}, {665, 8, 12, 6},  {758, 4, 41, 16}, {768, 5, 0, 0},   {776, 3, 0, 0},
        {864, 3, 15, 10}, {883, 8, 0, 0},   {911, 2, 28, 6},  {953, 6, 8, 4},   {993, 1, 7, 5},
    }};
    const ScalarHistogram uniqueHist = createHistogram(uniqueData);

    TypeCounts typeCounts{{value::TypeTags::NumberInt64, 106}, {value::TypeTags::Array, 94}};
    const auto ceHist = CEHistogram::make(scalarHist,
                                          typeCounts,
                                          uniqueHist,
                                          minHist,
                                          maxHist,
                                          // There are 94 non-empty int-only arrays.
                                          TypeCounts{{value::TypeTags::NumberInt64, 94}},
                                          200.0 /* sampleSize */);

    value::TypeTags lowTag = value::TypeTags::NumberInt64;
    value::Value lowVal = 500;
    value::TypeTags highTag = value::TypeTags::NumberInt64;
    value::Value highVal = 550;

    // Test interpolation for query: [{$match: {a: {$gt: 500, $lt: 550}}}].
    auto expectedCard = estimateCardinalityRange(*ceHist,
                                                 false /* lowInclusive */,
                                                 lowTag,
                                                 lowVal,
                                                 false /* highInclusive */,
                                                 highTag,
                                                 highVal,
                                                 true /* includeScalar */,
                                                 ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(92.9, expectedCard.card, 0.1);  // Actual: 94.

    // Test interpolation for query: [{$match: {a: {$elemMatch: {$gt: 500, $lt: 550}}}}].
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            false /* highInclusive */,
                                            highTag,
                                            highVal,
                                            false /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kExactArrayCE);
    ASSERT_APPROX_EQUAL(11.0, expectedCard.card, 0.1);  // Actual: 8.
}

TEST(CEHistogramEstimatorEdgeCasesTest, TwoExclusiveBucketsMixedHistogram) {
    // Data set of mixed data types: 3 integers and 5 strings.
    constexpr double numInts = 3.0;
    constexpr double numStrs = 5.0;
    constexpr double collCard = numInts + numStrs;
    std::vector<BucketData> data{{1, numInts, 0.0, 0.0}, {"abc", numStrs, 0.0, 0.0}};
    const ScalarHistogram hist = createHistogram(data);
    const auto ceHist = CEHistogram::make(hist,
                                          TypeCounts{{value::TypeTags::NumberInt64, numInts},
                                                     {value::TypeTags::StringSmall, numStrs}},
                                          collCard);

    const auto [tagLowDbl, valLowDbl] =
        std::make_pair(value::TypeTags::NumberDouble,
                       value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));

    // [{$match: {a: {$elemMatch: {$gt: NaN, $lt: 1}}}}]
    auto expectedCard = estimateCardinalityRange(*ceHist,
                                                 false /* lowInclusive */,
                                                 tagLowDbl,
                                                 valLowDbl,
                                                 false /* highInclusive */,
                                                 value::TypeTags::NumberInt32,
                                                 value::bitcastFrom<int64_t>(1),
                                                 true /* includeScalar */,
                                                 ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(0.0, expectedCard.card, kErrorBound);

    // [{$match: {a: {$gt: NaN, $lt: 5}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            false /* lowInclusive */,
                                            tagLowDbl,
                                            valLowDbl,
                                            false /* highInclusive */,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(5),
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(3.0, expectedCard.card, kErrorBound);

    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);
    auto [tag, value] = value::makeNewString("a"_sd);
    value::ValueGuard vg(tag, value);

    // [{$match: {a: {$gte: 0, $lt: ""}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(0),
                                            false /* highInclusive */,
                                            tagLowStr,
                                            valLowStr,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(numInts, expectedCard.card, kErrorBound);

    // [{$match: {a: {$gte: "", $lte: "a"}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            tagLowStr,
                                            valLowStr,
                                            true /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);

    ASSERT_APPROX_EQUAL(0.0, expectedCard.card, kErrorBound);

    std::tie(tag, value) = value::makeNewString("xyz"_sd);
    // [{$match: {a: {$gte: "", $lte: "xyz"}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            tagLowStr,
                                            valLowStr,
                                            true /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);

    ASSERT_APPROX_EQUAL(numStrs, expectedCard.card, kErrorBound);
}

TEST(CEHistogramEstimatorEdgeCasesTest, TwoBucketsMixedHistogram) {
    // Data set of mixed data types: 20 integers and 80 strings.
    // Histogram with one bucket per data type.
    std::vector<BucketData> data{{100, 3.0, 17.0, 9.0}, {"pqr", 5.0, 75.0, 25.0}};
    const ScalarHistogram hist = createHistogram(data);
    const auto ceHist = CEHistogram::make(
        hist,
        TypeCounts{{value::TypeTags::NumberInt64, 20}, {value::TypeTags::StringSmall, 80}},
        100.0 /* sampleSize */);

    ASSERT_EQ(100.0, getTotals(hist).card);

    // Estimates with the bucket bounds.
    ASSERT_EQ(3.0, estimateCardinalityScalarHistogramInteger(hist, 100, EstimationType::kEqual));
    ASSERT_EQ(17.0, estimateCardinalityScalarHistogramInteger(hist, 100, EstimationType::kLess));
    ASSERT_EQ(80.0, estimateCardinalityScalarHistogramInteger(hist, 100, EstimationType::kGreater));

    auto [tag, value] = value::makeNewString("pqr"_sd);
    value::ValueGuard vg(tag, value);
    auto expectedCard{estimateCardinality(hist, tag, value, EstimationType::kEqual)};
    ASSERT_EQ(5.0, expectedCard.card);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kLess)};
    ASSERT_EQ(95.0, expectedCard.card);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kGreater)};
    ASSERT_EQ(0.0, expectedCard.card);

    // Estimates for a value smaller than the first bucket bound.
    ASSERT_APPROX_EQUAL(1.88,
                        estimateCardinalityScalarHistogramInteger(hist, 50, EstimationType::kEqual),
                        kErrorBound);
    ASSERT_APPROX_EQUAL(6.61,
                        estimateCardinalityScalarHistogramInteger(hist, 50, EstimationType::kLess),
                        kErrorBound);
    ASSERT_APPROX_EQUAL(
        8.49,
        estimateCardinalityScalarHistogramInteger(hist, 50, EstimationType::kLessOrEqual),
        kErrorBound);
    ASSERT_APPROX_EQUAL(
        91.5,
        estimateCardinalityScalarHistogramInteger(hist, 50, EstimationType::kGreater),
        kErrorBound);
    ASSERT_APPROX_EQUAL(
        93.39,
        estimateCardinalityScalarHistogramInteger(hist, 50, EstimationType::kGreaterOrEqual),
        kErrorBound);

    // Estimates for a value between bucket bounds.
    ASSERT_EQ(0.0, estimateCardinalityScalarHistogramInteger(hist, 105, EstimationType::kEqual));

    std::tie(tag, value) = value::makeNewString("a"_sd);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kEqual)};
    ASSERT_APPROX_EQUAL(3.0, expectedCard.card, kErrorBound);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kLess)};
    ASSERT_APPROX_EQUAL(54.5, expectedCard.card, kErrorBound);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kLessOrEqual)};
    ASSERT_APPROX_EQUAL(57.5, expectedCard.card, kErrorBound);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kGreater)};
    ASSERT_APPROX_EQUAL(42.5, expectedCard.card, kErrorBound);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kGreaterOrEqual)};
    ASSERT_APPROX_EQUAL(45.5, expectedCard.card, kErrorBound);

    // Range estimates, including min/max values per data type.
    const auto [tagLowDbl, valLowDbl] =
        std::make_pair(value::TypeTags::NumberDouble,
                       value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
    const auto [tagHighInt, valHighInt] =
        std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1000000));

    // [{$match: {a: {$gte: NaN, $lte: 25}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            tagLowDbl,
                                            valLowDbl,
                                            true /* highInclusive */,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(25),
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(8.49, expectedCard.card, kErrorBound);

    // [{$match: {a: {$gte: 25, $lte: 1000000}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(25),
                                            true /* highInclusive */,
                                            tagHighInt,
                                            valHighInt,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(13.38, expectedCard.card, kErrorBound);

    // [{$match: {a: {$gte: NaN, $lte: 1000000}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            tagLowDbl,
                                            valLowDbl,
                                            true /* highInclusive */,
                                            tagHighInt,
                                            valHighInt,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(20.0, expectedCard.card, kErrorBound);

    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);

    // [{$match: {a: {$gte: NaN, $lt: ""}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            tagLowDbl,
                                            valLowDbl,
                                            false /* highInclusive */,
                                            tagLowStr,
                                            valLowStr,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(20.0, expectedCard.card, kErrorBound);

    // [{$match: {a: {$gte: 25, $lt: ""}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(25),
                                            false /* highInclusive */,
                                            tagLowStr,
                                            valLowStr,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(13.39, expectedCard.card, kErrorBound);

    // [{$match: {a: {$gte: "", $lte: "a"}}}]
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            tagLowStr,
                                            valLowStr,
                                            true /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);

    ASSERT_APPROX_EQUAL(37.49, expectedCard.card, kErrorBound);

    // ["", {}).
    auto [tagObj, valObj] = value::makeNewObject();
    value::ValueGuard vgObj(tagObj, valObj);
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            tagLowStr,
                                            valLowStr,
                                            false /* highInclusive */,
                                            tagObj,
                                            valObj,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);
    ASSERT_APPROX_EQUAL(80.0, expectedCard.card, kErrorBound);

    // ["a", {}).
    expectedCard = estimateCardinalityRange(*ceHist,
                                            true /* lowInclusive */,
                                            tag,
                                            value,
                                            false /* highInclusive */,
                                            tagObj,
                                            valObj,
                                            true /* includeScalar */,
                                            ArrayRangeEstimationAlgo::kConjunctArrayCE);

    ASSERT_APPROX_EQUAL(45.5, expectedCard.card, kErrorBound);
}

TEST(CEHistogramEstimatorDataTest, Histogram1000ArraysSmall10Buckets) {
    std::vector<BucketData> scalarData{{}};
    const ScalarHistogram scalarHist = createHistogram(scalarData);

    std::vector<BucketData> minData{{0, 5.0, 0.0, 0.0},
                                    {553, 2.0, 935.0, 303.0},
                                    {591, 4.0, 2.0, 1.0},
                                    {656, 2.0, 21.0, 12.0},
                                    {678, 3.0, 6.0, 3.0},
                                    {693, 2.0, 1.0, 1.0},
                                    {730, 1.0, 6.0, 3.0},
                                    {788, 1.0, 2.0, 2.0},
                                    {847, 2.0, 4.0, 1.0},
                                    {867, 1.0, 0.0, 0.0}};

    const ScalarHistogram aMinHist = createHistogram(minData);

    std::vector<BucketData> maxData{{117, 1.0, 0.0, 0.0},
                                    {210, 1.0, 1.0, 1.0},
                                    {591, 1.0, 8.0, 4.0},
                                    {656, 1.0, 0.0, 0.0},
                                    {353, 2.0, 18.0, 9.0},
                                    {610, 5.0, 125.0, 65.0},
                                    {733, 8.0, 134.0, 53.0},
                                    {768, 6.0, 50.0, 16.0},
                                    {957, 8.0, 448.0, 137.0},
                                    {1000, 7.0, 176.0, 40.0}};

    const ScalarHistogram aMaxHist = createHistogram(maxData);

    std::vector<BucketData> uniqueData{{0, 5.0, 0.0, 0.0},
                                       {16, 11.0, 74.0, 13.0},
                                       {192, 13.0, 698.0, 148.0},
                                       {271, 9.0, 312.0, 70.0},
                                       {670, 7.0, 1545.0, 355.0},
                                       {712, 9.0, 159.0, 32.0},
                                       {776, 11.0, 247.0, 54.0},
                                       {869, 9.0, 361.0, 85.0},
                                       {957, 8.0, 323.0, 76.0},
                                       {1000, 7.0, 188.0, 40.0}};

    const ScalarHistogram aUniqueHist = createHistogram(uniqueData);

    TypeCounts typeCounts;
    TypeCounts arrayTypeCounts;
    // Dataset generated as 1000 arrays of size between 3 to 5.
    typeCounts.insert({value::TypeTags::Array, 1000});
    arrayTypeCounts.insert({value::TypeTags::NumberInt32, 1000});

    constexpr double collCard = 1000.0;
    const auto ceHist = CEHistogram::make(
        scalarHist, typeCounts, aUniqueHist, aMinHist, aMaxHist, arrayTypeCounts, collCard);

    std::vector<QuerySpec> querySet{{10, 20, 35.7, 93.0, 37.8, 39.0},
                                    {10, 60, 103.3, 240.0, 158.0, 196.0},
                                    {320, 330, 550.7, 746.0, 26.0, 30.0},
                                    {320, 400, 669.1, 832.0, 231.5, 298.0},
                                    {980, 990, 88.8, 101.0, 36.5, 41.0},
                                    {970, 1050, 129.7, 141.0, 129.7, 141.0}};

    for (const auto q : querySet) {
        // $match query, includeScalar = true.
        auto estCard = estimateCardinalityRange(*ceHist,
                                                false /* lowInclusive */,
                                                value::TypeTags::NumberInt32,
                                                value::bitcastFrom<int32_t>(q.low),
                                                false /* highInclusive */,
                                                value::TypeTags::NumberInt32,
                                                value::bitcastFrom<int32_t>(q.high),
                                                true /* includeScalar */,
                                                ArrayRangeEstimationAlgo::kConjunctArrayCE);
        std::cout << estCard.card << " -> " << q.estMatch << std::endl;
        ASSERT_APPROX_EQUAL(estCard.card, q.estMatch, 0.1);

        // $elemMatch query, includeScalar = false.
        estCard = estimateCardinalityRange(*ceHist,
                                           false /* lowInclusive */,
                                           value::TypeTags::NumberInt32,
                                           value::bitcastFrom<int32_t>(q.low),
                                           false /* highInclusive */,
                                           value::TypeTags::NumberInt32,
                                           value::bitcastFrom<int32_t>(q.high),
                                           false /* includeScalar */,
                                           ArrayRangeEstimationAlgo::kExactArrayCE);
        ASSERT_APPROX_EQUAL(estCard.card, q.estElemMatch, 0.1);

        LOGV2(9163800,
              "RMSE for $match query",
              "RMSE"_attr = computeRMSE(querySet, false /* isElemMatch */));

        LOGV2(9163801,
              "RMSE for $elemMatch query",
              "RMSE"_attr = computeRMSE(querySet, true /* isElemMatch */));
    }
}

TEST(CEHistogramEstimatorDataTest, Histogram1000ArraysLarge10Buckets) {
    std::vector<BucketData> scalarData{{}};
    const ScalarHistogram scalarHist = createHistogram(scalarData);

    std::vector<BucketData> minData{{0, 2.0, 0.0, 0.0},
                                    {1324, 4.0, 925.0, 408.0},
                                    {1389, 5.0, 7.0, 5.0},
                                    {1521, 2.0, 16.0, 10.0},
                                    {1621, 2.0, 13.0, 7.0},
                                    {1852, 5.0, 10.0, 9.0},
                                    {1864, 2.0, 0.0, 0.0},
                                    {1971, 1.0, 3.0, 3.0},
                                    {2062, 2.0, 0.0, 0.0},
                                    {2873, 1.0, 0.0, 0.0}};

    const ScalarHistogram aMinHist = createHistogram(minData);

    std::vector<BucketData> maxData{{2261, 1.0, 0.0, 0.0},
                                    {2673, 1.0, 0.0, 0.0},
                                    {2930, 1.0, 1.0, 1.0},
                                    {3048, 2.0, 2.0, 2.0},
                                    {3128, 3.0, 1.0, 1.0},
                                    {3281, 2.0, 0.0, 0.0},
                                    {3378, 2.0, 7.0, 5.0},
                                    {3453, 4.0, 2.0, 2.0},
                                    {3763, 6.0, 44.0, 23.0},
                                    {5000, 1.0, 920.0, 416.0}};

    const ScalarHistogram aMaxHist = createHistogram(maxData);

    std::vector<BucketData> uniqueData{{0, 2.0, 0.0, 0.0},
                                       {1106, 9.0, 1970.0, 704.0},
                                       {1542, 11.0, 736.0, 280.0},
                                       {3267, 6.0, 3141.0, 1097.0},
                                       {3531, 6.0, 461.0, 175.0},
                                       {3570, 7.0, 48.0, 20.0},
                                       {4573, 8.0, 1851.0, 656.0},
                                       {4619, 6.0, 65.0, 30.0},
                                       {4782, 5.0, 265.0, 99.0},
                                       {5000, 1.0, 342.0, 135.0}};

    const ScalarHistogram aUniqueHist = createHistogram(uniqueData);

    TypeCounts typeCounts;
    TypeCounts arrayTypeCounts;
    // Dataset generated as 1000 arrays of size between 8 to 10.
    typeCounts.insert({value::TypeTags::Array, 1000});
    arrayTypeCounts.insert({value::TypeTags::NumberInt32, 1000});

    constexpr double collCard = 1000.0;
    const auto ceHist = CEHistogram::make(
        scalarHist, typeCounts, aUniqueHist, aMinHist, aMaxHist, arrayTypeCounts, collCard);

    std::vector<QuerySpec> querySet{{10, 20, 13.7, 39.0, 9.7, 26.0},
                                    {10, 60, 41.6, 108.0, 55.7, 101.0},
                                    {1000, 1010, 705.4, 861.0, 9.7, 7.0},
                                    {1000, 1050, 733.3, 884.0, 55.7, 87.0},
                                    {3250, 3300, 988.0, 988.0, 59.3, 86.0},
                                    {4970, 4980, 23.3, 53.0, 8.5, 16.0}};

    for (const auto q : querySet) {
        // $match query, includeScalar = true.
        auto estCard = estimateCardinalityRange(*ceHist,
                                                false /* lowInclusive */,
                                                value::TypeTags::NumberInt32,
                                                value::bitcastFrom<int32_t>(q.low),
                                                false /* highInclusive */,
                                                value::TypeTags::NumberInt32,
                                                value::bitcastFrom<int32_t>(q.high),
                                                true /* includeScalar */,
                                                ArrayRangeEstimationAlgo::kConjunctArrayCE);
        ASSERT_APPROX_EQUAL(estCard.card, q.estMatch, 0.1);

        // $elemMatch query, includeScalar = false.
        estCard = estimateCardinalityRange(*ceHist,
                                           false /* lowInclusive */,
                                           value::TypeTags::NumberInt32,
                                           value::bitcastFrom<int32_t>(q.low),
                                           false /* highInclusive */,
                                           value::TypeTags::NumberInt32,
                                           value::bitcastFrom<int32_t>(q.high),
                                           false /* includeScalar */,
                                           ArrayRangeEstimationAlgo::kExactArrayCE);
        ASSERT_APPROX_EQUAL(estCard.card, q.estElemMatch, 0.1);

        LOGV2(9163802,
              "RMSE for $match query",
              "RMSE"_attr = computeRMSE(querySet, false /* isElemMatch */));

        LOGV2(9163803,
              "RMSE for $elemMatch query",
              "RMSE"_attr = computeRMSE(querySet, true /* isElemMatch */));
    }
}

TEST(CEHistogramEstimatorCanEstimateTest, EstimateCardinalityEqViaTypeCountsIntegerFail) {

    int numberOfBuckets = 3;
    std::vector<stats::SBEValue> data = {stats::makeInt64Value(1),
                                         stats::makeInt64Value(2),
                                         stats::makeInt64Value(3),
                                         stats::makeInt64Value(4),
                                         stats::makeInt64Value(4),
                                         stats::makeInt64Value(2),
                                         stats::makeInt64Value(1)};

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {
        stats::SBEValue val = stats::makeInt64Value(1);

        ASSERT_FALSE(estimateCardinalityEqViaTypeCounts(
            *ceHist, sbe::value::TypeTags::NumberDouble, val.getValue()));
    }
}

TEST(CEHistogramEstimatorCanEstimateTest, EstimateCardinalityEqViaTypeCountsDoubleFail) {

    int numberOfBuckets = 3;
    std::vector<stats::SBEValue> data = {stats::makeDoubleValue(100.047),
                                         stats::makeDoubleValue(178.127),
                                         stats::makeDoubleValue(861.267),
                                         stats::makeDoubleValue(446.197),
                                         stats::makeDoubleValue(763.798),
                                         stats::makeDoubleValue(428.679),
                                         stats::makeDoubleValue(432.447)};

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {
        stats::SBEValue val = stats::makeDoubleValue(420.21);

        ASSERT_FALSE(estimateCardinalityEqViaTypeCounts(
            *ceHist, sbe::value::TypeTags::NumberDouble, val.getValue()));
    }
}

TEST(CEHistogramEstimatorCanEstimateTest, EstimateViaTypeCountsBooleanMixInclusiveBounds) {

    size_t trueValues = 24, falseValues = 6, size = trueValues + falseValues;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: false}}
        Interval interval(fromjson("{'': false, '': false}"), true, true);
        auto [valTag, val] = sbe::bson::convertFrom<false>(interval.start);
        auto estimation = estimateCardinalityEqViaTypeCounts(*ceHist, valTag, val);
        ASSERT_TRUE(estimation);
        ASSERT_EQ(falseValues, estimation.get().card);
    }

    {  // {a: {$eq: true}}
        Interval interval(fromjson("{'': true, '': true}"), true, true);
        auto [valTag, val] = sbe::bson::convertFrom<false>(interval.start);
        auto estimation = estimateCardinalityEqViaTypeCounts(*ceHist, valTag, val);
        ASSERT_TRUE(estimation);
        ASSERT_EQ(trueValues, estimation.get().card);
    }

    {  // {a: {$or: {{$eq: false}, {$eq: true}} }
        Interval interval(fromjson("{'': false, '': true}"), true, true);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
        ASSERT_TRUE(estimation);
        ASSERT_EQ(size, estimation.get().card);
    }
}

TEST(CEHistogramEstimatorCanEstimateTest, EstimateViaTypeCountsBooleanMixNotInclusiveBounds) {

    size_t trueValues = 24, falseValues = 6;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$lt: true}}
        Interval interval(fromjson("{'': false, '': true}"), true, false);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
        ASSERT_TRUE(estimation);
        ASSERT_EQ(falseValues, estimation.get().card);
    }

    {  // {a: {$gt: false}}
        Interval interval(fromjson("{'': false, '': true}"), false, true);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
        ASSERT_TRUE(estimation);
        ASSERT_EQ(trueValues, estimation.get().card);
    }
}

DEATH_TEST(CEHistogramEstimatorCanEstimateTest,
           EstimateViaTypeCountsBooleanMixUnorderedValues1,
           "Tripwire assertion") {

    size_t trueValues = 24, falseValues = 6;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // [true, false]
        Interval interval(fromjson("{'': true, '': false}"), true, true);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);

        // This should fail using tassert 9163900.
        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
    }
}

DEATH_TEST(CEHistogramEstimatorCanEstimateTest,
           EstimateViaTypeCountsBooleanMixUnorderedValues2,
           "Tripwire assertion") {

    size_t trueValues = 24, falseValues = 6;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // (true, false]
        Interval interval(fromjson("{'': true, '': false}"), false, true);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);

        // This should fail using tassert 9163900.
        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
    }
}

DEATH_TEST(CEHistogramEstimatorCanEstimateTest,
           EstimateViaTypeCountsBooleanRangeWithSameValues1,
           "Tripwire assertion") {

    size_t trueValues = 8, falseValues = 2;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // [false, false)
        Interval interval(fromjson("{'': false, '': false}"), true, false);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);

        // This should fail using tassert 9163900.
        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
    }
}

DEATH_TEST(CEHistogramEstimatorCanEstimateTest,
           EstimateViaTypeCountsBooleanRangeWithSameValues3,
           "Tripwire assertion") {

    size_t trueValues = 8, falseValues = 2;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // (false, false]
        Interval interval(fromjson("{'': false, '': false}"), false, true);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);

        // This should fail using tassert 9163900.
        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
    }
}

DEATH_TEST(CEHistogramEstimatorCanEstimateTest,
           EstimateViaTypeCountsBooleanRangeWithSameValues2,
           "Tripwire assertion") {

    size_t trueValues = 8, falseValues = 2;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // [true, true)
        Interval interval(fromjson("{'': true, '': true}"), true, false);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);

        // This should fail using tassert 9163900.
        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
    }
}

DEATH_TEST(CEHistogramEstimatorCanEstimateTest,
           EstimateViaTypeCountsBooleanRangeWithSameValues4,
           "Tripwire assertion") {

    size_t trueValues = 8, falseValues = 2;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // (true, true]
        Interval interval(fromjson("{'': true, '': true}"), false, true);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);

        // This should fail using tassert 9163900.
        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
    }
}

TEST(CEHistogramEstimatorCanEstimateTest, EstimateViaTypeCountsNull) {

    size_t sizeNull = 10, sizeNothing = 5, size = sizeNull + sizeNothing;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data;
    for (size_t i = 0; i < sizeNull; i++) {
        data.push_back(stats::makeNullValue());
    }
    for (size_t i = 0; i < sizeNothing; i++) {
        data.push_back({sbe::value::TypeTags::Nothing, 0});
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: null}}
        Interval interval(fromjson("{'': null, '': null}"), true, true);
        auto [valTag, val] = sbe::bson::convertFrom<false>(interval.start);
        auto estimation = estimateCardinalityEqViaTypeCounts(*ceHist, valTag, val);

        ASSERT_TRUE(estimation);
        ASSERT_EQ(size, estimation.get().card);
    }

    {
        // Alternative representation of a null interval.
        Interval interval(fromjson("{'': null, '': NaN}"), true, false);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
        sbe::value::ValueGuard startGuard{startTag, startVal};
        sbe::value::ValueGuard endGuard{endTag, endVal};

        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);

        ASSERT_TRUE(estimation);
        ASSERT_EQ(size, estimation.get().card);
    }
}

TEST(CEHistogramEstimatorCanEstimateTest, EstimateViaTypeCountsNaN) {

    size_t sizeNaN = 3;  //, totalSize = size + sizeNaN;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data = {stats::makeDoubleValue(100.047),
                                         stats::makeDoubleValue(178.127),
                                         stats::makeDoubleValue(861.267),
                                         stats::makeDoubleValue(446.197),
                                         stats::makeDoubleValue(763.798),
                                         stats::makeDoubleValue(428.679),
                                         stats::makeDoubleValue(432.447)};

    // add NaN values.
    for (size_t i = 0; i < sizeNaN; i++) {
        data.push_back(stats::makeNaNValue());
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: NaN}}
        Interval interval(fromjson("{'': NaN, '': NaN}"), true, true);
        auto [valTag, val] = sbe::bson::convertFrom<false>(interval.start);
        auto estimation = estimateCardinalityEqViaTypeCounts(*ceHist, valTag, val);

        ASSERT_TRUE(estimation);
        ASSERT_EQ(sizeNaN, estimation.get().card);
    }
}

TEST(CEHistogramEstimatorCanEstimateTest, EstimateViaTypeCountsAllString) {

    size_t size = 10;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data = {
        value::makeNewString("wc2VFWKqCZT3V8GVLWqAJ442vWYgKJIviv9pZqrrGD4Yyjk9epx9J9RflpASGi97BCS"),
        value::makeNewString("LjzZ9RmI4KsGgU8DEiEIe9VWFUicFHSyD5irCgWXUwh0kBV3ADkaOzxejDLK3FHt0Vl"),
        value::makeNewString("MZUjm9UCx5Kv97nuc3dXDul7NW8iCOTlY0MbCjeyxi18dCw"),
        value::makeNewString("fpOYzNMqdqeBvPKIDQ5LwrgeiYWdPfIWrWJTtPVn1khtHcQ5IyWeQBu8IS4gLzqGgUj"),
        value::makeNewString("eoktVgPzGp6NvUYZPAAy0uYv342tXltHYqX4oAxwIB1DnLPO4C3DqmhyuvKdPHxjVpM"),
        value::makeNewString("IO8ycvxdMyRveS4hMdej2O8FN2WipSbvi116Sdf97hAM4VtrGOMMqxpBwqIY5szeZC1"),
        value::makeNewString("GPqhYMa7tcl0pp5cmQqpbEt11dZjXKkxwNaZE0TOSxQeLk6xSmDY2PDfZ0XFeLlCZmH"),
        value::makeNewString("kEqBJ7aCd0ROzP6ScOiWm4xWVWPwwTvXtv7119VdSOAtiZKlmTqXvOoJvKJAnEAqrdd"),
        value::makeNewString("OsNrN0e2BxnRA8mwTQKGtgXx8GbJZmvDH38RJJywp614ff36UFfPttEuAUj1oaIM5vg"),
        value::makeNewString("rPfTNYop7sT4hUnkkg4VBKoWLlD1vJxpVWKLOx4uoJPphSU7MeOFWNU7MMksJiua4Q")};

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {$and: [{a: {$gte: ""}},{a: {$lt: {}}}]}
        Interval interval(fromjson("{'': \"\", '': {}}"), true, false);

        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
        sbe::value::ValueGuard startGuard{startTag, startVal};
        sbe::value::ValueGuard endGuard{endTag, endVal};

        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);

        ASSERT_TRUE(estimation);
        ASSERT_EQ(size, estimation.get().card);
    }
}

TEST(CEHistogramEstimatorCanEstimateTest, EstimateViaTypeCountsMixTypes) {

    size_t strCount = 10, sizeNaN = 3, trueValues = 5, falseValues = 5;
    size_t numberOfBuckets = 10;

    std::vector<stats::SBEValue> data = {
        value::makeNewString("wc2VFWKqCZT3V8GVLWqAJ442vWYgKJIviv9pZqrrGD4Yyjk9epx9J9RflpASGi97BCS"),
        value::makeNewString("LjzZ9RmI4KsGgU8DEiEIe9VWFUicFHSyD5irCgWXUwh0kBV3ADkaOzxejDLK3FHt0Vl"),
        value::makeNewString("MZUjm9UCx5Kv97nuc3dXDul7NW8iCOTlY0MbCjeyxi18dCw"),
        value::makeNewString("fpOYzNMqdqeBvPKIDQ5LwrgeiYWdPfIWrWJTtPVn1khtHcQ5IyWeQBu8IS4gLzqGgUj"),
        value::makeNewString("eoktVgPzGp6NvUYZPAAy0uYv342tXltHYqX4oAxwIB1DnLPO4C3DqmhyuvKdPHxjVpM"),
        value::makeNewString("IO8ycvxdMyRveS4hMdej2O8FN2WipSbvi116Sdf97hAM4VtrGOMMqxpBwqIY5szeZC1"),
        value::makeNewString("GPqhYMa7tcl0pp5cmQqpbEt11dZjXKkxwNaZE0TOSxQeLk6xSmDY2PDfZ0XFeLlCZmH"),
        value::makeNewString("kEqBJ7aCd0ROzP6ScOiWm4xWVWPwwTvXtv7119VdSOAtiZKlmTqXvOoJvKJAnEAqrdd"),
        value::makeNewString("OsNrN0e2BxnRA8mwTQKGtgXx8GbJZmvDH38RJJywp614ff36UFfPttEuAUj1oaIM5vg"),
        value::makeNewString("rPfTNYop7sT4hUnkkg4VBKoWLlD1vJxpVWKLOx4uoJPphSU7MeOFWNU7MMksJiua4Q"),
        stats::makeDoubleValue(100.047),
        stats::makeDoubleValue(178.127),
        stats::makeDoubleValue(861.267),
        stats::makeDoubleValue(446.197),
        stats::makeDoubleValue(763.798),
        stats::makeDoubleValue(428.679),
        stats::makeDoubleValue(432.447)};

    // add True boolean values.
    for (size_t i = 0; i < trueValues; i++) {
        data.push_back(stats::makeBooleanValue(1 /*true*/));
    }

    // add True boolean values.
    for (size_t i = 0; i < falseValues; i++) {
        data.push_back(stats::makeBooleanValue(0 /*false*/));
    }

    // add NaN values.
    for (size_t i = 0; i < sizeNaN; i++) {
        data.push_back(stats::makeNaNValue());
    }

    auto ceHist = stats::createCEHistogram(data, numberOfBuckets);

    {  // {a: {$eq: true}}
        Interval interval(fromjson("{'': true, '': true}"), true, true);
        auto [valTag, val] = sbe::bson::convertFrom<false>(interval.start);
        auto estimation = estimateCardinalityEqViaTypeCounts(*ceHist, valTag, val);
        ASSERT_TRUE(estimation);
        ASSERT_EQ(trueValues, estimation.get().card);
    }


    {  // {a: {$eq: false}}
        Interval interval(fromjson("{'': false, '': false}"), true, true);
        auto [valTag, val] = sbe::bson::convertFrom<false>(interval.start);
        auto estimation = estimateCardinalityEqViaTypeCounts(*ceHist, valTag, val);
        ASSERT_TRUE(estimation);
        ASSERT_EQ(falseValues, estimation.get().card);
    }

    {  // {a: {$eq: NaN}}
        Interval interval(fromjson("{'': NaN, '': NaN}"), true, true);
        auto [valTag, val] = sbe::bson::convertFrom<false>(interval.start);
        auto estimation = estimateCardinalityEqViaTypeCounts(*ceHist, valTag, val);
        ASSERT_TRUE(estimation);
        ASSERT_EQ(sizeNaN, estimation.get().card);
    }

    {  // {$and: [{a: {$gte: ""}},{a: {$lt: {}}}]}
        Interval interval(fromjson("{'': \"\", '': {}}"), true, false);
        bool startInclusive = interval.startInclusive;
        bool endInclusive = interval.endInclusive;
        auto [startTag, startVal] = sbe::bson::convertFrom<false>(interval.start);
        auto [endTag, endVal] = sbe::bson::convertFrom<false>(interval.end);
        sbe::value::ValueGuard startGuard{startTag, startVal};
        sbe::value::ValueGuard endGuard{endTag, endVal};

        auto estimation = estimateCardinalityRangeViaTypeCounts(
            *ceHist, startInclusive, startTag, startVal, endInclusive, endTag, endVal);
        ASSERT_TRUE(estimation);
        ASSERT_EQ(strCount, estimation.get().card);
    }
}

}  // namespace
}  // namespace mongo::ce

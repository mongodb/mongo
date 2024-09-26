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

#include <tuple>
#include <vector>

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_common.h"
#include "mongo/db/query/ce/cbp_histogram_ce/scalar_histogram_helpers.h"
#include "mongo/db/query/ce/cbp_histogram_ce/test_utils.h"
#include "mongo/db/query/stats/maxdiff_test_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::optimizer::cbp::ce {

namespace {
namespace value = sbe::value;

using stats::ScalarHistogram;

auto NumberInt64 = sbe::value::TypeTags::NumberInt64;
auto kEqual = EstimationType::kEqual;
auto kLess = EstimationType::kLess;
auto kLessOrEqual = EstimationType::kLessOrEqual;
auto kGreater = EstimationType::kGreater;
auto kGreaterOrEqual = EstimationType::kGreaterOrEqual;
auto Date = value::TypeTags::Date;
auto TimeStamp = value::TypeTags::Timestamp;

constexpr double kErrorBound = 0.01;

TEST(ScalarHistogramHelpersTest, ManualHistogram) {
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

TEST(ScalarHistogramHelpersTest, UniformIntEstimate) {
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

TEST(ScalarHistogramHelpersTest, NormalIntEstimate) {
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

TEST(ScalarHistogramHelpersTest, UniformStrEstimate) {
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

TEST(ScalarHistogramHelpersTest, NormalStrEstimate) {
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

TEST(ScalarHistogramHelpersTest, OneBucketIntHistogram) {

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

TEST(ScalarHistogramHelpersTest, OneExclusiveBucketIntHistogram) {
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

TEST(ScalarHistogramHelpersTest, OneBucketTwoIntValuesHistogram) {
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

TEST(ScalarHistogramHelpersTest, OneBucketTwoIntValuesHistogram2) {
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

TEST(ScalarHistogramHelpersTest, TwoBucketsIntHistogram) {
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

TEST(ScalarHistogramHelpersTest, ThreeExclusiveBucketsIntHistogram) {
    std::vector<BucketData> data{{1, 1.0, 0.0, 0.0}, {10, 8.0, 0.0, 0.0}, {100, 1.0, 0.0, 0.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(10.0, getTotals(hist).card);

    ASSERT_EQ(0.0, estimateCardinality(hist, NumberInt64, 5, kEqual).card);
    ASSERT_EQ(1.0, estimateCardinality(hist, NumberInt64, 5, kLess).card);
    ASSERT_EQ(1.0, estimateCardinality(hist, NumberInt64, 5, kLessOrEqual).card);
    ASSERT_EQ(9.0, estimateCardinality(hist, NumberInt64, 5, kGreater).card);
    ASSERT_EQ(9.0, estimateCardinality(hist, NumberInt64, 5, kGreaterOrEqual).card);
}

TEST(ScalarHistogramHelpersTest, OneBucketStrHistogram) {
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

TEST(ScalarHistogramHelpersTest, TwoBucketsStrHistogram) {
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

TEST(ScalarHistogramHelpersTest, TwoBucketsDateHistogram) {
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

TEST(ScalarHistogramHelpersTest, TwoBucketsTimestampHistogram) {
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
    ASSERT_CE_APPROX_EQUAL(
        49.0, estimateCardinality(hist, TimeStamp, valueIn, kLess).card, kErrorBound);
    ASSERT_CE_APPROX_EQUAL(
        49.0, estimateCardinality(hist, TimeStamp, valueIn, kGreater).card, kErrorBound);

    const auto valueAfter = value::bitcastFrom<int64_t>(endTs.asULL() + 100);
    ASSERT_EQ(0.0, estimateCardinality(hist, TimeStamp, valueAfter, kEqual).card);
    ASSERT_EQ(100.0, estimateCardinality(hist, TimeStamp, valueAfter, kLess).card);
    ASSERT_EQ(0.0, estimateCardinality(hist, TimeStamp, valueAfter, kGreater).card);
}

TEST(ScalarHistogramHelpersTest, TwoBucketsObjectIdHistogram) {
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

TEST(ScalarHistogramHelpersTest, MinValueMixedHistogramFromData) {
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
    auto&& [minOid, inclOid] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::ObjectId);
    auto [minOidTag, minOidVal] = minOid->cast<mongo::optimizer::Constant>()->get();
    ASSERT_EQ(1.0, estimateCardinality(hist, minOidTag, minOidVal, kEqual).card);

    // Minimum date.
    const auto&& [minDate, inclDate] = getMinMaxBoundForType(true /*isMin*/, Date);
    const auto [minDateTag, minDateVal] = minDate->cast<mongo::optimizer::Constant>()->get();
    ASSERT_EQ(1.0, estimateCardinality(hist, minDateTag, minDateVal, kEqual).card);

    // Minimum timestamp.
    auto&& [minTs, inclTs] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::Timestamp);
    auto [minTsTag, minTsVal] = minTs->cast<mongo::optimizer::Constant>()->get();
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
    auto&& [maxTs, inclMaxTs] = getMinMaxBoundForType(false /*isMin*/, value::TypeTags::Timestamp);
    const auto [maxTsTag, maxTsVal] = maxTs->cast<mongo::optimizer::Constant>()->get();
    expectedCard =
        estimateCardinalityRange(hist2, true, minTsTag, minTsVal, true, maxTsTag, maxTsVal);
    ASSERT_EQ(3.0, expectedCard.card);
}

TEST(ScalarHistogramHelpersTest, MinValueMixedHistogramFromBuckets) {
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
    auto&& [minOid, inclOid] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::ObjectId);
    auto [minOidTag, minOidVal] = minOid->cast<mongo::optimizer::Constant>()->get();
    ASSERT_CE_APPROX_EQUAL(
        1.9, estimateCardinality(hist, minOidTag, minOidVal, kEqual).card, kErrorBound);

    // Minimum date.
    const auto&& [minDate, inclDate] = getMinMaxBoundForType(true /*isMin*/, Date);
    const auto [minDateTag, minDateVal] = minDate->cast<mongo::optimizer::Constant>()->get();
    ASSERT_EQ(4.0, estimateCardinality(hist, minDateTag, minDateVal, kEqual).card);

    // Minimum timestamp.
    auto&& [minTs, inclTs] = getMinMaxBoundForType(true /*isMin*/, value::TypeTags::Timestamp);
    auto [minTsTag, minTsVal] = minTs->cast<mongo::optimizer::Constant>()->get();
    ASSERT_CE_APPROX_EQUAL(
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
    ASSERT_CE_APPROX_EQUAL(48.0, expectedCard.card, kErrorBound);

    // [minTs, innerTs], estimated by the half of the timestamp bucket.
    const Timestamp innerTs{Seconds(1516864323LL), 0};
    expectedCard = estimateCardinalityRange(hist,
                                            true,
                                            minTsTag,
                                            minTsVal,
                                            true,
                                            value::TypeTags::Timestamp,
                                            value::bitcastFrom<int64_t>(innerTs.asULL()));
    ASSERT_CE_APPROX_EQUAL(47.5, expectedCard.card, kErrorBound);
}

}  // namespace
}  // namespace mongo::optimizer::cbp::ce

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
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
namespace {

using namespace sbe;

const std::pair<value::TypeTags, value::Value> makeInt64Value(const int v) {
    return std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(v));
};

double estimateIntValCard(const ScalarHistogram& hist, const int v, const EstimationType type) {
    const auto [tag, val] = makeInt64Value(v);
    return estimate(hist, tag, val, type).card;
};

struct BucketData {
    Value _v;
    double _equalFreq;
    double _rangeFreq;
    double _ndv;

    BucketData(Value v, double equalFreq, double rangeFreq, double ndv)
        : _v(v), _equalFreq(equalFreq), _rangeFreq(rangeFreq), _ndv(ndv) {}
    BucketData(const std::string& v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
    BucketData(int v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
};

ScalarHistogram createHistogram(const std::vector<BucketData>& data) {
    sbe::value::Array array;
    for (const auto& item : data) {
        const auto [tag, val] = stage_builder::makeValue(item._v);
        array.push_back(tag, val);
    }

    value::Array bounds;
    std::vector<Bucket> buckets;

    double cumulativeFreq = 0.0;
    double cumulativeNDV = 0.0;

    for (size_t i = 0; i < data.size(); i++) {
        const auto [tag, val] = array.getAt(i);
        bounds.push_back(tag, val);

        const auto& item = data.at(i);
        cumulativeFreq += item._equalFreq + item._rangeFreq;
        cumulativeNDV += item._ndv + 1.0;
        buckets.emplace_back(
            item._equalFreq, item._rangeFreq, cumulativeFreq, item._ndv, cumulativeNDV);
    }

    return {std::move(bounds), std::move(buckets)};
}

TEST(EstimatorTest, ManualHistogram) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 1.0, 10.0, 5.0},
                                 {20, 3.0, 15.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(55.0, getTotals(hist).card);

    ASSERT_EQ(1.0, estimateIntValCard(hist, 0, EstimationType::kEqual));
    ASSERT_EQ(2.0, estimateIntValCard(hist, 5, EstimationType::kEqual));
    ASSERT_EQ(0.0, estimateIntValCard(hist, 35, EstimationType::kEqual));

    ASSERT_EQ(15.5, estimateIntValCard(hist, 15, EstimationType::kLess));
    ASSERT_EQ(20.5, estimateIntValCard(hist, 15, EstimationType::kLessOrEqual));
    ASSERT_EQ(28, estimateIntValCard(hist, 20, EstimationType::kLess));
    ASSERT_EQ(31.0, estimateIntValCard(hist, 20, EstimationType::kLessOrEqual));

    ASSERT_EQ(42, estimateIntValCard(hist, 10, EstimationType::kGreater));
    ASSERT_EQ(43, estimateIntValCard(hist, 10, EstimationType::kGreaterOrEqual));
    ASSERT_EQ(19, estimateIntValCard(hist, 25, EstimationType::kGreater));
    ASSERT_EQ(21.5, estimateIntValCard(hist, 25, EstimationType::kGreaterOrEqual));
}

TEST(EstimatorTest, UniformIntEstimate) {
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
    double expectedCard = estimateIntValCard(hist, 558, EstimationType::kEqual);
    ASSERT_EQ(4.0, expectedCard);
    expectedCard = estimateIntValCard(hist, 558, EstimationType::kLess);
    ASSERT_EQ(57.0, expectedCard);
    expectedCard = estimateIntValCard(hist, 558, EstimationType::kLessOrEqual);
    ASSERT_EQ(61.0, expectedCard);

    // Predicates over value inside of a bucket.

    // Query: [{$match: {a: {$eq: 530}}}].
    expectedCard = estimateIntValCard(hist, 530, EstimationType::kEqual);
    ASSERT_APPROX_EQUAL(1.6, expectedCard, 0.1);  // Actual: 1.

    // Query: [{$match: {a: {$lt: 530}}}].
    expectedCard = estimateIntValCard(hist, 530, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(52.9, expectedCard, 0.1);  // Actual: 50.

    // Query: [{$match: {a: {$lte: 530}}}].
    expectedCard = estimateIntValCard(hist, 530, EstimationType::kLessOrEqual);
    ASSERT_APPROX_EQUAL(54.5, expectedCard, 0.1);  // Actual: 51.

    // Query: [{$match: {a: {$eq: 400}}}].
    expectedCard = estimateIntValCard(hist, 400, EstimationType::kEqual);
    ASSERT_APPROX_EQUAL(1.6, expectedCard, 0.1);  // Actual: 1.

    // Query: [{$match: {a: {$lt: 400}}}].
    expectedCard = estimateIntValCard(hist, 400, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(41.3, expectedCard, 0.1);  // Actual: 39.

    // Query: [{$match: {a: {$lte: 400}}}].
    expectedCard = estimateIntValCard(hist, 400, EstimationType::kLessOrEqual);
    ASSERT_APPROX_EQUAL(43.0, expectedCard, 0.1);  // Actual: 40.
}

TEST(EstimatorTest, NormalIntEstimate) {
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
    double expectedCard = estimateIntValCard(hist, 696, EstimationType::kEqual);
    ASSERT_EQ(3.0, expectedCard);
    expectedCard = estimateIntValCard(hist, 696, EstimationType::kLess);
    ASSERT_EQ(66.0, expectedCard);
    expectedCard = estimateIntValCard(hist, 696, EstimationType::kLessOrEqual);
    ASSERT_EQ(69.0, expectedCard);

    // Predicates over value inside of a bucket.

    // Query: [{$match: {a: {$eq: 150}}}].
    expectedCard = estimateIntValCard(hist, 150, EstimationType::kEqual);
    ASSERT_APPROX_EQUAL(1.3, expectedCard, 0.1);  // Actual: 1.

    // Query: [{$match: {a: {$lt: 150}}}].
    expectedCard = estimateIntValCard(hist, 150, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(9.1, expectedCard, 0.1);  // Actual: 9.

    // Query: [{$match: {a: {$lte: 150}}}].
    expectedCard = estimateIntValCard(hist, 150, EstimationType::kLessOrEqual);
    ASSERT_APPROX_EQUAL(10.4, expectedCard, 0.1);  // Actual: 10.
}

TEST(EstimatorTest, UniformStrEstimate) {
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
    double expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(1.55, expectedCard, 0.1);  // Actual: 2.

    // Query: [{$match: {a: {$lt: 'TTV'}}}].
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(39.8, expectedCard, 0.1);  // Actual: 39.

    // Query: [{$match: {a: {$lte: 'TTV'}}}].
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(41.3, expectedCard, 0.1);  // Actual: 41.
}

TEST(EstimatorTest, NormalStrEstimate) {
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
    double expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(5.0, expectedCard, 0.1);  // Actual: 5.

    // Query: [{$match: {a: {$lt: 'TTV'}}}].
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(47.0, expectedCard, 0.1);  // Actual: 47.

    // Query: [{$match: {a: {$lte: 'TTV'}}}].
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(52.0, expectedCard, 0.1);  // Actual: 52.

    // Predicates over value inside of a bucket.
    std::tie(tag, value) = value::makeNewString("Pfa"_sd);

    // Query: [{$match: {a: {$eq: 'Pfa'}}}].
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(1.75, expectedCard, 0.1);  // Actual: 2.

    // Query: [{$match: {a: {$lt: 'Pfa'}}}].
    expectedCard = estimate(hist, tag, value, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(38.3, expectedCard, 0.1);  // Actual: 35.

    // Query: [{$match: {a: {$lte: 'Pfa'}}}].
    expectedCard = estimate(hist, tag, value, EstimationType::kLessOrEqual).card;
    ASSERT_APPROX_EQUAL(40.0, expectedCard, 0.1);  // Actual: 37.
}

TEST(EstimatorTest, UniformIntStrEstimate) {
    // This hard-codes a maxdiff histogram with 20 buckets built off of a uniform distribution with
    // two types occurring with equal probability:
    // - 100 distinct ints between 0 and 1000, and
    // - 100 distinct strings of length between 2 and 5.
    std::vector<BucketData> data{{
        {2, 3, 0, 0},       {19, 4, 1, 1},      {226, 2, 49, 20},  {301, 5, 12, 4},
        {317, 3, 0, 0},     {344, 2, 3, 1},     {423, 5, 18, 6},   {445, 3, 0, 0},
        {495, 3, 4, 2},     {542, 5, 9, 3},     {696, 3, 44, 19},  {773, 4, 11, 5},
        {805, 2, 8, 4},     {931, 5, 21, 8},    {998, 4, 21, 3},   {"8N4", 5, 31, 14},
        {"MIb", 5, 45, 17}, {"Zgi", 3, 55, 22}, {"pZ", 6, 62, 25}, {"yUwxz", 5, 29, 12},
    }};
    const ScalarHistogram hist = createHistogram(data);
    const ArrayHistogram arrHist(
        hist, TypeCounts{{value::TypeTags::NumberInt64, 254}, {value::TypeTags::StringSmall, 246}});

    // Predicates over value inside of the last numeric bucket.

    // Query: [{$match: {a: {$eq: 993}}}].
    double expectedCard = estimateIntValCard(hist, 993, EstimationType::kEqual);
    ASSERT_APPROX_EQUAL(7.0, expectedCard, 0.1);  // Actual: 9.

    // Query: [{$match: {a: {$lt: 993}}}].
    expectedCard = estimateIntValCard(hist, 993, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(241.4, expectedCard, 0.1);  // Actual: 241.

    // Query: [{$match: {a: {$lte: 993}}}].
    expectedCard = estimateIntValCard(hist, 993, EstimationType::kLessOrEqual);
    ASSERT_APPROX_EQUAL(248.4, expectedCard, 0.1);  // Actual: 250.

    // Predicates over value inside of the first string bucket.
    auto [tag, value] = value::makeNewString("04e"_sd);
    value::ValueGuard vg(tag, value);

    // Query: [{$match: {a: {$eq: '04e'}}}].
    expectedCard = estimate(hist, tag, value, EstimationType::kEqual).card;
    ASSERT_APPROX_EQUAL(2.2, expectedCard, 0.1);  // Actual: 3.

    value::TypeTags lowTag = value::TypeTags::NumberInt64;
    value::Value lowVal = 100000000;

    // Type bracketing: low value of different type than the bucket bound.
    // Query: [{$match: {a: {$eq: 100000000}}}].
    expectedCard = estimateCardEq(arrHist, lowTag, lowVal);
    ASSERT_APPROX_EQUAL(0.0, expectedCard, 0.1);  // Actual: 0.

    // No interpolation for inequality to values inside the first string bucket, fallback to half of
    // the bucket frequency.

    // Query: [{$match: {a: {$lt: '04e'}}}].
    expectedCard = estimateCardRange(arrHist, true, false, lowTag, lowVal, false, tag, value);
    ASSERT_APPROX_EQUAL(13.3, expectedCard, 0.1);  // Actual: 0.

    // Query: [{$match: {a: {$lte: '04e'}}}].
    expectedCard = estimateCardRange(
        arrHist, true, false, lowTag, lowVal, true /* highInclusive */, tag, value);
    ASSERT_APPROX_EQUAL(15.5, expectedCard, 0.1);  // Actual: 3.

    // Value towards the end of the bucket gets the same half bucket estimate.
    std::tie(tag, value) = value::makeNewString("8B5"_sd);

    // Query: [{$match: {a: {$lt: '8B5'}}}].
    expectedCard = estimateCardRange(arrHist, true, false, lowTag, lowVal, false, tag, value);
    ASSERT_APPROX_EQUAL(13.3, expectedCard, 0.1);  // Actual: 24.

    // Query: [{$match: {a: {$lte: '8B5'}}}].
    expectedCard = estimateCardRange(
        arrHist, true, false, lowTag, lowVal, true /* highInclusive */, tag, value);
    ASSERT_APPROX_EQUAL(15.5, expectedCard, 0.1);  // Actual: 29.
}

TEST(EstimatorTest, UniformIntArrayOnlyEstimate) {
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

    const ArrayHistogram arrHist(
        scalarHist, TypeCounts{}, uniqueHist, minHist, maxHist, TypeCounts{});

    // Query in the middle of the domain: estimate from ArrayUnique histogram.
    value::TypeTags lowTag = value::TypeTags::NumberInt64;
    value::Value lowVal = 500;
    value::TypeTags highTag = value::TypeTags::NumberInt64;
    value::Value highVal = 600;

    // Test interpolation for query: [{$match: {a: {$gt: 500, $lt: 600}}}].
    double expectedCard =
        estimateCardRange(arrHist, false, false, lowTag, lowVal, false, highTag, highVal);
    ASSERT_APPROX_EQUAL(8.63, expectedCard, 0.1);

    // Test interpolation for query: [{$match: {a: {$elemMatch: {$gt: 500, $lt: 600}}}}].
    // Note: this should be the same as above, since we have no scalars.
    expectedCard = estimateCardRange(arrHist, true, false, lowTag, lowVal, false, highTag, highVal);
    ASSERT_APPROX_EQUAL(8.63, expectedCard, 0.1);

    // Query at the end of the domain: more precise estimates from ArrayMin, ArrayMax histograms.
    lowVal = 10;
    highVal = 110;

    // Test interpolation for query: [{$match: {a: {$gt: 10, $lt: 110}}}].
    expectedCard =
        estimateCardRange(arrHist, false, false, lowTag, lowVal, false, highTag, highVal);
    ASSERT_APPROX_EQUAL(24.1, expectedCard, 0.1);

    // Test interpolation for query: [{$match: {a: {$elemMatch: {$gt: 500, $lt: 600}}}}].
    // Note: this should be the same as above, since we have no scalars.
    expectedCard = estimateCardRange(arrHist, true, false, lowTag, lowVal, false, highTag, highVal);
    ASSERT_APPROX_EQUAL(24.1, expectedCard, 0.1);
}

TEST(EstimatorTest, UniformIntMixedArrayEstimate) {
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

    const ArrayHistogram arrHist(
        scalarHist, TypeCounts{}, uniqueHist, minHist, maxHist, TypeCounts{});

    value::TypeTags lowTag = value::TypeTags::NumberInt64;
    value::Value lowVal = 500;
    value::TypeTags highTag = value::TypeTags::NumberInt64;
    value::Value highVal = 550;

    // Test interpolation for query: [{$match: {a: {$gt: 500, $lt: 550}}}].
    double expectedCard =
        estimateCardRange(arrHist, true, false, lowTag, lowVal, false, highTag, highVal);
    ASSERT_APPROX_EQUAL(9.8, expectedCard, 0.1);  // Actual: 94.

    // Test interpolation for query: [{$match: {a: {$elemMatch: {$gt: 500, $lt: 550}}}}].
    expectedCard =
        estimateCardRange(arrHist, false, false, lowTag, lowVal, false, highTag, highVal);
    ASSERT_APPROX_EQUAL(5.6, expectedCard, 0.1);  // Actual: 8.
}

}  // namespace
}  // namespace mongo::ce

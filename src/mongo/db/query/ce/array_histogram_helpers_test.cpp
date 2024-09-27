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

#include "mongo/db/query/ce/array_histogram_helpers.h"
#include "mongo/db/query/ce/histogram_common.h"
#include "mongo/db/query/ce/scalar_histogram_helpers.h"
#include "mongo/db/query/ce/test_utils.h"
#include "mongo/unittest/assert.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::ce {

namespace {
namespace value = sbe::value;

using stats::ArrayHistogram;
using stats::ScalarHistogram;
using stats::TypeCounts;

auto NumberInt64 = value::TypeTags::NumberInt64;

constexpr double kErrorBound = 0.01;

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

TEST(ArrayHistogramHelpersTest, ManualHistogram) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 1.0, 10.0, 5.0},
                                 {20, 3.0, 15.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const double intCnt = 55;
    const ScalarHistogram& hist = createHistogram(data);
    const auto arrHist =
        ArrayHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(3.0, estimateCardinalityEq(*arrHist, NumberInt64, 20, true).card);
    ASSERT_EQ(1.0, estimateCardinalityEq(*arrHist, NumberInt64, 50, true).card);
    ASSERT_EQ(0,
              estimateCardinalityEq(*arrHist, NumberInt64, 40, false)
                  .card);  // should be 2.0 for includeScalar: true
    // value not in data
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, NumberInt64, 60, true).card);
}

TEST(ArrayHistogramHelpersTest, UniformIntHistogram) {
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
    const auto arrHist =
        ArrayHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(4.0, estimateCardinalityEq(*arrHist, NumberInt64, 558, true).card);
    ASSERT_APPROX_EQUAL(1.6,
                        estimateCardinalityEq(*arrHist, NumberInt64, 530, true).card,
                        0.1);  // Actual: 1.
    ASSERT_APPROX_EQUAL(1.6,
                        estimateCardinalityEq(*arrHist, NumberInt64, 400, true).card,
                        0.1);  // Actual: 1.
}

TEST(ArrayHistogramHelpersTest, NormalIntArrayHistogram) {
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
    const auto arrHist =
        ArrayHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(3.0, estimateCardinalityEq(*arrHist, NumberInt64, 696, true).card);
    ASSERT_APPROX_EQUAL(1.3,
                        estimateCardinalityEq(*arrHist, NumberInt64, 150, true).card,
                        0.1);  // Actual: 1.
}

TEST(ArrayHistogramHelpersTest, SkewedIntHistogram) {
    std::vector<BucketData> data{{0, 1.0, 1.0, 1.0},
                                 {10, 150.0, 10.0, 5.0},
                                 {20, 100.0, 14.0, 3.0},
                                 {30, 1.0, 10.0, 4.0},
                                 {40, 2.0, 0.0, 0.0},
                                 {50, 1.0, 10.0, 5.0}};
    const double intCnt = 300;
    const ScalarHistogram& hist = createHistogram(data);
    const auto arrHist =
        ArrayHistogram::make(hist, stats::TypeCounts{{NumberInt64, intCnt}}, intCnt);

    ASSERT_EQ(150.0, estimateCardinalityEq(*arrHist, NumberInt64, 10, true).card);
    ASSERT_EQ(100.0, estimateCardinalityEq(*arrHist, NumberInt64, 20, true).card);
    ASSERT_EQ(1.0, estimateCardinalityEq(*arrHist, NumberInt64, 30, true).card);
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, NumberInt64, 40, false).card);
}

TEST(ArrayHistogramHelpersTest, StringHistogram) {
    std::vector<BucketData> data{
        {"testA", 5.0, 2.0, 1.0}, {"testB", 3.0, 2.0, 2.0}, {"testC", 2.0, 1.0, 1.0}};
    const double strCnt = 15;
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(strCnt, getTotals(hist).card);

    const auto arrHist = ArrayHistogram::make(
        hist, stats::TypeCounts{{value::TypeTags::StringSmall, strCnt}}, strCnt);

    auto [tag, value] = value::makeNewString("testA"_sd);
    value::ValueGuard vg(tag, value);
    ASSERT_EQ(5.0, estimateCardinalityEq(*arrHist, tag, value, true).card);

    std::tie(tag, value) = value::makeNewString("testB"_sd);
    ASSERT_EQ(3.0, estimateCardinalityEq(*arrHist, tag, value, true).card);

    std::tie(tag, value) = value::makeNewString("testC"_sd);
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, tag, value, false).card);
}

TEST(ArrayHistogramHelpersTest, UniformStrHistogram) {
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

    const auto arrHist = ArrayHistogram::make(
        hist, stats::TypeCounts{{value::TypeTags::StringSmall, strCnt}}, strCnt);

    const auto [tag, value] = value::makeNewString("TTV"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_APPROX_EQUAL(
        1.55, estimateCardinalityEq(*arrHist, tag, value, true).card, 0.1);  // Actual: 2.
}

TEST(ArrayHistogramHelpersTest, NormalStrHistogram) {
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

    const auto arrHist = ArrayHistogram::make(
        hist, stats::TypeCounts{{value::TypeTags::StringSmall, strCnt}}, strCnt);

    auto [tag, value] = value::makeNewString("TTV"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_APPROX_EQUAL(
        5.0, estimateCardinalityEq(*arrHist, tag, value, true).card, 0.1);  // Actual: 5.

    std::tie(tag, value) = value::makeNewString("Pfa"_sd);
    ASSERT_APPROX_EQUAL(
        1.75, estimateCardinalityEq(*arrHist, tag, value, true).card, 0.1);  // Actual: 2.
}

TEST(ArrayHistogramHelpersTest, IntStrHistogram) {
    std::vector<BucketData> data{{1, 1.0, 0.0, 0.0}, {"test", 20.0, 0.0, 0.0}};
    const double intCnt = 1;
    const double strCnt = 20;
    const double totalCnt = intCnt + strCnt;
    const ScalarHistogram& hist = createHistogram(data);

    ASSERT_EQ(totalCnt, getTotals(hist).card);

    const auto arrHist = ArrayHistogram::make(
        hist,
        stats::TypeCounts{{NumberInt64, intCnt}, {value::TypeTags::StringSmall, strCnt}},
        totalCnt);
    auto [tag, value] = value::makeNewString("test"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_EQ(20.0, estimateCardinalityEq(*arrHist, tag, value, true).card);
    ASSERT_EQ(1.0, estimateCardinalityEq(*arrHist, NumberInt64, 1, true).card);
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, tag, value, false).card);
    ASSERT_EQ(0, estimateCardinalityEq(*arrHist, NumberInt64, 1, false).card);
}

TEST(ArrayHistogramHelpersTest, UniformIntStrHistogram) {
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

    const auto arrHist = ArrayHistogram::make(
        hist,
        stats::TypeCounts{{NumberInt64, intCnt}, {value::TypeTags::StringSmall, strCnt}},
        totalCnt);

    ASSERT_APPROX_EQUAL(7.0,
                        estimateCardinalityEq(*arrHist, NumberInt64, 993, true).card,
                        0.1);  // Actual: 9

    auto [tag, value] = value::makeNewString("04e"_sd);
    value::ValueGuard vg(tag, value);

    ASSERT_APPROX_EQUAL(
        2.2, estimateCardinalityEq(*arrHist, tag, value, true).card, 0.1);  // Actual: 3.

    value::TypeTags lowTag = value::TypeTags::NumberInt64;
    value::Value lowVal = 100000000;

    ASSERT_APPROX_EQUAL(0.0,
                        estimateCardinalityEq(*arrHist, lowTag, lowVal, true).card,
                        0.1);  // Actual: 0

    // Query: [{$match: {a: {$lt: '04e'}}}].
    auto expectedCard = estimateCardinalityRange(*arrHist,
                                                 false /* lowInclusive */,
                                                 lowTag,
                                                 lowVal,
                                                 false /* highInclusive */,
                                                 tag,
                                                 value,
                                                 true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(13.3, expectedCard.card, 0.1);  // Actual: 0.

    // Query: [{$match: {a: {$lte: '04e'}}}].
    expectedCard =
        estimateCardinalityRange(*arrHist, false, lowTag, lowVal, true, tag, value, true);
    ASSERT_CE_APPROX_EQUAL(15.5, expectedCard.card, 0.1);  // Actual: 3.

    // Value towards the end of the bucket gets the same half bucket estimate.
    std::tie(tag, value) = value::makeNewString("8B5"_sd);

    // Query: [{$match: {a: {$lt: '8B5'}}}].
    expectedCard =
        estimateCardinalityRange(*arrHist, false, lowTag, lowVal, false, tag, value, true);
    ASSERT_CE_APPROX_EQUAL(13.3, expectedCard.card, 0.1);  // Actual: 24.

    // Query: [{$match: {a: {$lte: '8B5'}}}].
    expectedCard =
        estimateCardinalityRange(*arrHist, false, lowTag, lowVal, true, tag, value, true);
    ASSERT_CE_APPROX_EQUAL(15.5, expectedCard.card, 0.1);  // Actual: 29.
}

TEST(ArrayHistogramHelpersTest, UniformIntStrEstimate) {
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
    const auto arrHist = ArrayHistogram::make(
        hist,
        TypeCounts{{value::TypeTags::NumberInt64, numInt}, {value::TypeTags::StringSmall, numStr}},
        collCard);

    // Predicates over value inside of the last numeric bucket.

    // Query: [{$match: {a: {$eq: 993}}}].
    EstimationResult expectedCard{
        estimateCardinalityScalarHistogramInteger(hist, 993, EstimationType::kEqual)};
    ASSERT_CE_APPROX_EQUAL(7.0, expectedCard.card, 0.1);  // Actual: 9.

    // Query: [{$match: {a: {$lt: 993}}}].
    expectedCard = {estimateCardinalityScalarHistogramInteger(hist, 993, EstimationType::kLess)};
    ASSERT_CE_APPROX_EQUAL(241.4, expectedCard.card, 0.1);  // Actual: 241.

    // Query: [{$match: {a: {$lte: 993}}}].
    expectedCard = {
        estimateCardinalityScalarHistogramInteger(hist, 993, EstimationType::kLessOrEqual)};
    ASSERT_CE_APPROX_EQUAL(248.4, expectedCard.card, 0.1);  // Actual: 250.

    // Predicates over value inside of the first string bucket.
    auto [tag, value] = value::makeNewString("04e"_sd);
    value::ValueGuard vg(tag, value);

    // Query: [{$match: {a: {$eq: '04e'}}}].
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kEqual).card};
    ASSERT_CE_APPROX_EQUAL(2.2, expectedCard.card, 0.1);  // Actual: 3.

    value::TypeTags lowTag = value::TypeTags::NumberInt64;
    value::Value lowVal = 100000000;

    // Type bracketing: low value of different type than the bucket bound.
    // Query: [{$match: {a: {$eq: 100000000}}}].
    expectedCard = estimateCardinalityEq(*arrHist, lowTag, lowVal, true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(0.0, expectedCard.card, 0.1);  // Actual: 0.

    // No interpolation for inequality to values inside the first string bucket, fallback to half of
    // the bucket frequency.

    // Query: [{$match: {a: {$lt: '04e'}}}].
    expectedCard = estimateCardinalityRange(*arrHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            false /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(13.3, expectedCard.card, 0.1);  // Actual: 0.

    // Query: [{$match: {a: {$lte: '04e'}}}].
    expectedCard = estimateCardinalityRange(*arrHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            true /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(15.5, expectedCard.card, 0.1);  // Actual: 3.

    // Value towards the end of the bucket gets the same half bucket estimate.
    std::tie(tag, value) = value::makeNewString("8B5"_sd);

    // Query: [{$match: {a: {$lt: '8B5'}}}].
    expectedCard = estimateCardinalityRange(*arrHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            false /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(13.3, expectedCard.card, 0.1);  // Actual: 24.

    // Query: [{$match: {a: {$lte: '8B5'}}}].
    expectedCard = estimateCardinalityRange(*arrHist,
                                            false /* lowInclusive */,
                                            lowTag,
                                            lowVal,
                                            true /* highInclusive */,
                                            tag,
                                            value,
                                            true /* includeScalar */);
    ASSERT_CE_APPROX_EQUAL(15.5, expectedCard.card, 0.1);  // Actual: 29.
}

TEST(ArrayHistogramHelpersTest, UniformIntArrayOnlyEstimate) {
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

    const auto arrHist = ArrayHistogram::make(scalarHist,
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
    auto expectedCard =
        estimateCardinalityRange(*arrHist, false, lowTag, lowVal, false, highTag, highVal, false);
    ASSERT_CE_APPROX_EQUAL(27.0, expectedCard.card, 0.1);  // actual 21.

    // Test interpolation for query: [{$match: {a: {$gt: 500, $lt: 600}}}].
    // Note: although there are no scalars, the estimate is different than the
    // above since we use different formulas.
    expectedCard =
        estimateCardinalityRange(*arrHist, false, lowTag, lowVal, false, highTag, highVal, true);
    ASSERT_CE_APPROX_EQUAL(92.0, expectedCard.card, 0.1);  // actual 92.

    // Query at the end of the domain: more precise estimates from ArrayMin, ArrayMax histograms.
    lowVal = 10;
    highVal = 110;

    // Test interpolation for query: [{$match: {a: {$elemMatch: {$gt: 10, $lt: 110}}}}].
    expectedCard =
        estimateCardinalityRange(*arrHist, false, lowTag, lowVal, false, highTag, highVal, false);
    ASSERT_CE_APPROX_EQUAL(24.1, expectedCard.card, 0.1);  // actual 29.

    // Test interpolation for query: [{$match: {a: {$gt: 10, $lt: 110}}}].
    expectedCard =
        estimateCardinalityRange(*arrHist, false, lowTag, lowVal, false, highTag, highVal, true);
    ASSERT_CE_APPROX_EQUAL(27.8, expectedCard.card, 0.1);  // actual 31.
}

TEST(ArrayHistogramHelpersTest, UniformIntMixedArrayEstimate) {
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
    const auto arrHist = ArrayHistogram::make(scalarHist,
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
    auto expectedCard =
        estimateCardinalityRange(*arrHist, false, lowTag, lowVal, false, highTag, highVal, true);
    ASSERT_CE_APPROX_EQUAL(92.9, expectedCard.card, 0.1);  // Actual: 94.

    // Test interpolation for query: [{$match: {a: {$elemMatch: {$gt: 500, $lt: 550}}}}].
    expectedCard =
        estimateCardinalityRange(*arrHist, false, lowTag, lowVal, false, highTag, highVal, false);
    ASSERT_CE_APPROX_EQUAL(11.0, expectedCard.card, 0.1);  // Actual: 8.
}

TEST(ArrayHistogramHelpersTest, TwoExclusiveBucketsMixedHistogram) {
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
    auto expectedCard = estimateCardinalityRange(*arrHist,
                                                 false,
                                                 tagLowDbl,
                                                 valLowDbl,
                                                 false,
                                                 value::TypeTags::NumberInt32,
                                                 value::bitcastFrom<int64_t>(1),
                                                 true);
    ASSERT_CE_APPROX_EQUAL(0.0, expectedCard.card, kErrorBound);

    // (NaN, 5).
    expectedCard = estimateCardinalityRange(*arrHist,
                                            false,
                                            tagLowDbl,
                                            valLowDbl,
                                            false,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(5),
                                            true);
    ASSERT_CE_APPROX_EQUAL(3.0, expectedCard.card, kErrorBound);

    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);
    auto [tag, value] = value::makeNewString("a"_sd);
    value::ValueGuard vg(tag, value);

    // [0, "").
    expectedCard = estimateCardinalityRange(*arrHist,
                                            true,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(0),
                                            false,
                                            tagLowStr,
                                            valLowStr,
                                            true);
    ASSERT_CE_APPROX_EQUAL(numInts, expectedCard.card, kErrorBound);

    // ["", "a"].
    expectedCard =
        estimateCardinalityRange(*arrHist, true, tagLowStr, valLowStr, true, tag, value, true);

    ASSERT_CE_APPROX_EQUAL(0.0, expectedCard.card, kErrorBound);

    std::tie(tag, value) = value::makeNewString("xyz"_sd);
    // ["", "xyz"].
    expectedCard =
        estimateCardinalityRange(*arrHist, true, tagLowStr, valLowStr, true, tag, value, true);

    ASSERT_CE_APPROX_EQUAL(numStrs, expectedCard.card, kErrorBound);
}

TEST(ArrayHistogramHelpersTest, TwoBucketsMixedHistogram) {
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
    ASSERT_CE_APPROX_EQUAL(3.0, expectedCard.card, kErrorBound);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kLess)};
    ASSERT_CE_APPROX_EQUAL(54.5, expectedCard.card, kErrorBound);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kLessOrEqual)};
    ASSERT_CE_APPROX_EQUAL(57.5, expectedCard.card, kErrorBound);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kGreater)};
    ASSERT_CE_APPROX_EQUAL(42.5, expectedCard.card, kErrorBound);
    expectedCard = {estimateCardinality(hist, tag, value, EstimationType::kGreaterOrEqual)};
    ASSERT_CE_APPROX_EQUAL(45.5, expectedCard.card, kErrorBound);

    // Range estimates, including min/max values per data type.
    const auto [tagLowDbl, valLowDbl] =
        std::make_pair(value::TypeTags::NumberDouble,
                       value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
    const auto [tagHighInt, valHighInt] =
        std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1000000));

    // [NaN, 25].
    expectedCard = estimateCardinalityRange(*arrHist,
                                            true,
                                            tagLowDbl,
                                            valLowDbl,
                                            true,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(25),
                                            true);
    ASSERT_CE_APPROX_EQUAL(8.49, expectedCard.card, kErrorBound);

    // [25, 1000000].
    expectedCard = estimateCardinalityRange(*arrHist,
                                            true,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(25),
                                            true,
                                            tagHighInt,
                                            valHighInt,
                                            true);
    ASSERT_CE_APPROX_EQUAL(13.38, expectedCard.card, kErrorBound);

    // [NaN, 1000000].
    expectedCard = estimateCardinalityRange(
        *arrHist, true, tagLowDbl, valLowDbl, true, tagHighInt, valHighInt, true);
    ASSERT_CE_APPROX_EQUAL(20.0, expectedCard.card, kErrorBound);

    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);

    // [NaN, "").
    expectedCard = estimateCardinalityRange(
        *arrHist, true, tagLowDbl, valLowDbl, false, tagLowStr, valLowStr, true);
    ASSERT_CE_APPROX_EQUAL(20.0, expectedCard.card, kErrorBound);

    // [25, "").
    expectedCard = estimateCardinalityRange(*arrHist,
                                            true,
                                            value::TypeTags::NumberInt32,
                                            value::bitcastFrom<int64_t>(25),
                                            false,
                                            tagLowStr,
                                            valLowStr,
                                            true);
    ASSERT_CE_APPROX_EQUAL(13.39, expectedCard.card, kErrorBound);

    // ["", "a"].
    expectedCard =
        estimateCardinalityRange(*arrHist, true, tagLowStr, valLowStr, true, tag, value, true);

    ASSERT_CE_APPROX_EQUAL(37.49, expectedCard.card, kErrorBound);

    // ["", {}).
    auto [tagObj, valObj] = value::makeNewObject();
    value::ValueGuard vgObj(tagObj, valObj);
    expectedCard =
        estimateCardinalityRange(*arrHist, true, tagLowStr, valLowStr, false, tagObj, valObj, true);
    ASSERT_CE_APPROX_EQUAL(80.0, expectedCard.card, kErrorBound);

    // ["a", {}).
    expectedCard =
        estimateCardinalityRange(*arrHist, true, tag, value, false, tagObj, valObj, true);

    ASSERT_CE_APPROX_EQUAL(45.5, expectedCard.card, kErrorBound);
}

TEST(ArrayHistogramHelpersTest, Histogram1000ArraysSmall10Buckets) {
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
    const auto arrHist = ArrayHistogram::make(
        scalarHist, typeCounts, aUniqueHist, aMinHist, aMaxHist, arrayTypeCounts, collCard);

    std::vector<QuerySpec> querySet{{10, 20, 35.7, 93.0, 37.8, 39.0},
                                    {10, 60, 103.3, 240.0, 158.0, 196.0},
                                    {320, 330, 550.7, 746.0, 26.0, 30.0},
                                    {320, 400, 669.1, 832.0, 231.5, 298.0},
                                    {980, 990, 88.8, 101.0, 36.5, 41.0},
                                    {970, 1050, 129.7, 141.0, 129.7, 141.0}};

    for (const auto q : querySet) {
        // $match query, includeScalar = true.
        auto estCard = estimateCardinalityRange(*arrHist,
                                                false,
                                                value::TypeTags::NumberInt32,
                                                value::bitcastFrom<int32_t>(q.low),
                                                false,
                                                value::TypeTags::NumberInt32,
                                                value::bitcastFrom<int32_t>(q.high),
                                                true);
        ASSERT_CE_APPROX_EQUAL(estCard.card, q.estMatch, 0.1);

        // $elemMatch query, includeScalar = false.
        estCard = estimateCardinalityRange(*arrHist,
                                           false,
                                           value::TypeTags::NumberInt32,
                                           value::bitcastFrom<int32_t>(q.low),
                                           false,
                                           value::TypeTags::NumberInt32,
                                           value::bitcastFrom<int32_t>(q.high),
                                           false);
        ASSERT_CE_APPROX_EQUAL(estCard.card, q.estElemMatch, 0.1);

        LOGV2(9163800,
              "RMSE for $match query",
              "RMSE"_attr = computeRMSE(querySet, false /* isElemMatch */));

        LOGV2(9163801,
              "RMSE for $elemMatch query",
              "RMSE"_attr = computeRMSE(querySet, true /* isElemMatch */));
    }
}

TEST(ArrayHistogramHelpersTest, Histogram1000ArraysLarge10Buckets) {
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
    const auto arrHist = ArrayHistogram::make(
        scalarHist, typeCounts, aUniqueHist, aMinHist, aMaxHist, arrayTypeCounts, collCard);

    std::vector<QuerySpec> querySet{{10, 20, 13.7, 39.0, 9.7, 26.0},
                                    {10, 60, 41.6, 108.0, 55.7, 101.0},
                                    {1000, 1010, 705.4, 861.0, 9.7, 7.0},
                                    {1000, 1050, 733.3, 884.0, 55.7, 87.0},
                                    {3250, 3300, 988.0, 988.0, 59.3, 86.0},
                                    {4970, 4980, 23.3, 53.0, 8.5, 16.0}};

    for (const auto q : querySet) {
        // $match query, includeScalar = true.
        auto estCard = estimateCardinalityRange(*arrHist,
                                                false,
                                                value::TypeTags::NumberInt32,
                                                value::bitcastFrom<int32_t>(q.low),
                                                false,
                                                value::TypeTags::NumberInt32,
                                                value::bitcastFrom<int32_t>(q.high),
                                                true);
        ASSERT_CE_APPROX_EQUAL(estCard.card, q.estMatch, 0.1);

        // $elemMatch query, includeScalar = false.
        estCard = estimateCardinalityRange(*arrHist,
                                           false,
                                           value::TypeTags::NumberInt32,
                                           value::bitcastFrom<int32_t>(q.low),
                                           false,
                                           value::TypeTags::NumberInt32,
                                           value::bitcastFrom<int32_t>(q.high),
                                           false);
        ASSERT_CE_APPROX_EQUAL(estCard.card, q.estElemMatch, 0.1);

        LOGV2(9163802,
              "RMSE for $match query",
              "RMSE"_attr = computeRMSE(querySet, false /* isElemMatch */));

        LOGV2(9163803,
              "RMSE for $elemMatch query",
              "RMSE"_attr = computeRMSE(querySet, true /* isElemMatch */));
    }
}
}  // namespace
}  // namespace mongo::ce

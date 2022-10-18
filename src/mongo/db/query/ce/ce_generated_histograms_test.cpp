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

#include <string>
#include <vector>

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/array_histogram.h"
#include "mongo/db/query/ce/ce_test_utils.h"
#include "mongo/db/query/ce/histogram_estimation.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {
namespace {

using namespace sbe;

constexpr double kErrorBound = 0.1;

TEST(EstimatorTest, UniformIntStrEstimate) {
    /* The code in this comment generates a dataset and creates the histogram used in this test. To
    recreate the data set and the histogram, place this code in a unit test which uses the utilities
    from rand_utils_new.cpp.

    constexpr int minLen = 3, maxLen = 5;
    constexpr int minVal = 0, maxVal = 1000;
    constexpr size_t dataSize = 1000;
    constexpr size_t nBuckets = std::min(20UL, dataSize);

    MixedDistributionDescriptor dd{{DistrType::kUniform, 1.0}};
    TypeDistrVector td;
    td.emplace_back(std::make_unique<IntDistribution>(dd, 0.5, 250, minVal, maxVal));
    td.emplace_back(std::make_unique<StrDistribution>(dd, 0.5, 250, minLen, maxLen));

    std::mt19937_64 gen(0);
    DatasetDescriptorNew desc{std::move(td), gen};

    std::vector<SBEValue> dataset;
    dataset = desc.genRandomDataset(dataSize);

    const ScalarHistogram& hist = makeHistogram(dataset, nBuckets);
    */

    std::vector<BucketData> data{
        {2, 5, 0, 0},       {57, 4, 21, 12},     {159, 4, 59, 24},    {172, 5, 0, 0},
        {184, 4, 2, 2},     {344, 4, 73, 32},    {363, 4, 1, 1},      {420, 3, 16, 10},
        {516, 2, 49, 23},   {758, 4, 113, 54},   {931, 5, 104, 41},   {998, 4, 29, 12},
        {"3vL", 6, 30, 11}, {"9WUk", 1, 59, 24}, {"HraK", 4, 56, 26}, {"Zujbu", 1, 130, 64},
        {"kEr", 5, 80, 40}, {"rupc", 6, 44, 21}, {"up1O", 5, 16, 7},  {"ztf", 5, 37, 17}};

    const ScalarHistogram hist = createHistogram(data);
    const ArrayHistogram arrHist(
        hist, TypeCounts{{value::TypeTags::NumberInt64, 515}, {value::TypeTags::StringSmall, 485}});

    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);
    const auto [tagAbc, valAbc] = value::makeNewString("abc"_sd);
    value::ValueGuard vg(tagAbc, valAbc);
    auto [tagObj, valObj] = value::makeNewObject();
    value::ValueGuard vgObj(tagObj, valObj);

    // Predicates over bucket bound.
    // Actual cardinality {$eq: 804} = 2.
    double expectedCard = estimateIntValCard(hist, 804, EstimationType::kEqual);
    ASSERT_APPROX_EQUAL(2.5, expectedCard, kErrorBound);

    // Actual cardinality {$lt: 100} = 40.
    expectedCard = estimateIntValCard(hist, 100, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(52.4, expectedCard, kErrorBound);

    // Range query crossing the type brackets.
    // Actual cardinality {$gt: 100} = 475.
    expectedCard = estimateCardRange(arrHist,
                                     false /* lowInclusive */,
                                     value::TypeTags::NumberInt64,
                                     value::bitcastFrom<int64_t>(100),
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(460.1, expectedCard, kErrorBound);

    // Actual cardinality {$lt: 'abc'} = 291.
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* highInclusive */,
                                     tagAbc,
                                     valAbc,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(319.9, expectedCard, kErrorBound);

    // Actual cardinality {$gte: 'abc'} = 194.
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagAbc,
                                     valAbc,
                                     false /* highInclusive */,
                                     tagObj,
                                     valObj,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(167.0, expectedCard, kErrorBound);

    // Queries over the low string bound.
    // Actual cardinality {$eq: ''} = 0.
    expectedCard = estimateCardEq(arrHist, tagLowStr, valLowStr, true);
    ASSERT_APPROX_EQUAL(0.01, expectedCard, 0.001);

    // Actual cardinality {$gt: ''} = 485.
    expectedCard = estimateCardRange(arrHist,
                                     false /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     false /* highInclusive */,
                                     tagObj,
                                     valObj,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(485, expectedCard, 0.001);
}

TEST(EstimatorTest, IntStrArrayEstimate) {
    /* The code in this comment generates a dataset of 1000 integers, strings and arrays of integers
       and strings and creates the histogram used in this test. To recreate the data set and the
       histogram, place this code in a unit test which uses the utilities from rand_utils_new.cpp.

       constexpr int minLen = 2, maxLen = 5;
       constexpr int minVal = 0, maxVal = 1000;
       constexpr size_t dataSize = 1000;
       constexpr size_t nBuckets = std::min(20UL, dataSize);

       MixedDistributionDescriptor dd{{DistrType::kUniform, 1.0}};
       TypeDistrVector td1;
       td1.emplace_back(std::make_unique<IntDistribution>(dd, 0.7, 200, minVal, maxVal));
       td1.emplace_back(std::make_unique<StrDistribution>(dd, 0.3, 100, minLen, maxLen));

       std::mt19937_64 gen(5);
       auto desc1 = std::make_unique<DatasetDescriptorNew>(std::move(td1), gen);

       TypeDistrVector td2;
       td2.emplace_back(std::make_unique<IntDistribution>(dd, 0.4, 200, minVal, maxVal));
       td2.emplace_back(std::make_unique<StrDistribution>(dd, 0.3, 200, minLen, maxLen));
       td2.emplace_back(std::make_unique<ArrDistribution>(dd, 0.3, 200, 2, 6, std::move(desc1),
       0.0));

       DatasetDescriptorNew desc{std::move(td2), gen};
       std::vector<SBEValue> dataset;
       dataset = desc.genRandomDataset(dataSize);

       const ScalarHistogram& hist = makeHistogram(dataset, nBuckets);
        */

    std::vector<BucketData> scalarData{
        {10, 1, 0, 0},    {11, 4, 0, 0},       {44, 2, 5, 2},         {213, 3, 40, 20},
        {256, 5, 13, 6},  {270, 3, 9, 2},      {407, 3, 56, 28},      {510, 3, 32, 16},
        {524, 3, 0, 0},   {561, 5, 16, 8},     {583, 3, 4, 3},        {599, 3, 1, 1},
        {663, 5, 19, 9},  {681, 5, 6, 2},      {873, 5, 75, 37},      {909, 4, 16, 7},
        {994, 3, 36, 14}, {"9TcY", 4, 44, 23}, {"Zow00", 5, 134, 67}, {"zsS", 2, 130, 66},
    };

    const ScalarHistogram scalarHist = createHistogram(scalarData);

    std::vector<BucketData> minData{
        {12, 5, 0, 0},      {17, 8, 0, 0},        {28, 7, 7, 1},        {55, 5, 22, 5},
        {110, 5, 45, 11},   {225, 4, 43, 15},     {563, 3, 98, 36},     {643, 4, 3, 2},
        {701, 4, 9, 5},     {845, 1, 6, 4},       {921, 2, 0, 0},       {980, 1, 0, 0},
        {"1l", 9, 16, 4},   {"8YN", 4, 19, 5},    {"PE2OO", 2, 41, 15}, {"WdJ", 8, 25, 7},
        {"dKb7", 9, 17, 6}, {"msdP", 12, 25, 10}, {"t7wmp", 5, 15, 6},  {"yx", 2, 13, 4},
    };

    const ScalarHistogram minHist = createHistogram(minData);

    std::vector<BucketData> maxData{
        {26, 2, 0, 0},    {79, 3, 0, 0},      {147, 1, 0, 0},      {207, 2, 0, 0},
        {362, 6, 7, 5},   {563, 3, 47, 19},   {603, 9, 2, 1},      {676, 6, 21, 10},
        {702, 6, 9, 4},   {712, 6, 0, 0},     {759, 8, 4, 1},      {774, 6, 3, 1},
        {831, 9, 28, 9},  {948, 7, 51, 15},   {981, 3, 33, 8},     {"9Iey", 4, 20, 8},
        {"Ji", 3, 21, 8}, {"WdJ", 9, 26, 10}, {"msdP", 9, 59, 20}, {"zbI", 3, 68, 16},
    };

    const ScalarHistogram maxHist = createHistogram(maxData);

    std::vector<BucketData> uniqueData{
        {12, 5, 0, 0},      {28, 8, 15, 2},      {55, 8, 23, 5},       {110, 5, 59, 12},
        {225, 8, 79, 18},   {362, 8, 88, 20},    {507, 10, 165, 36},   {572, 5, 25, 6},
        {603, 12, 25, 3},   {712, 6, 106, 19},   {759, 11, 17, 4},     {774, 6, 3, 1},
        {831, 14, 50, 13},  {981, 3, 105, 25},   {"547DP", 4, 43, 9},  {"9Iey", 4, 8, 1},
        {"WdJ", 9, 85, 26}, {"ZGYcw", 2, 14, 4}, {"msdP", 14, 80, 21}, {"zbI", 3, 74, 17},
    };

    const ScalarHistogram uniqueHist = createHistogram(uniqueData);

    TypeCounts typeCounts{{value::TypeTags::NumberInt64, 388},
                          {value::TypeTags::StringSmall, 319},
                          {value::TypeTags::Array, 293}};
    TypeCounts arrayTypeCounts{{value::TypeTags::NumberInt64, 874},
                               {value::TypeTags::StringSmall, 340}};
    const ArrayHistogram arrHist(
        scalarHist, typeCounts, uniqueHist, minHist, maxHist, arrayTypeCounts);

    const auto [tagLowDbl, valLowDbl] =
        std::make_pair(value::TypeTags::NumberDouble,
                       value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
    const auto [tagLowStr, valLowStr] = value::makeNewString(""_sd);
    value::ValueGuard vgLowStr(tagLowStr, valLowStr);

    // Actual cardinality {$lt: 100} = 115.
    double expectedCard = estimateCardRange(arrHist,
                                            false /* lowInclusive */,
                                            tagLowDbl,
                                            valLowDbl,
                                            false /* highInclusive */,
                                            value::TypeTags::NumberInt64,
                                            value::bitcastFrom<int64_t>(100),
                                            true /* includeScalar */);
    ASSERT_APPROX_EQUAL(109.9, expectedCard, kErrorBound);

    // Actual cardinality {$gt: 502} = 434.
    expectedCard = estimateCardRange(arrHist,
                                     false /* lowInclusive */,
                                     value::TypeTags::NumberInt64,
                                     value::bitcastFrom<int64_t>(500),
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(443.8, expectedCard, kErrorBound);

    // Actual cardinality {$gte: 502} = 437.
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     value::TypeTags::NumberInt64,
                                     value::bitcastFrom<int64_t>(500),
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(448.3, expectedCard, kErrorBound);

    // Actual cardinality {$eq: ''} = 0.
    expectedCard = estimateCardEq(arrHist, tagLowStr, valLowStr, true /* includeScalar */);
    ASSERT_APPROX_EQUAL(0.02, expectedCard, 0.001);

    // Actual cardinality {$eq: 'DD2'} = 2.
    auto [tagStr, valStr] = value::makeNewString("DD2"_sd);
    value::ValueGuard vg(tagStr, valStr);
    expectedCard = estimateCardEq(arrHist, tagStr, valStr, true /* includeScalar */);
    ASSERT_APPROX_EQUAL(5.27, expectedCard, kErrorBound);

    // Actual cardinality {$lte: 'DD2'} = 120.
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* highInclusive */,
                                     tagStr,
                                     valStr,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(160.6, expectedCard, kErrorBound);

    // Actual cardinality {$gt: 'DD2'} = 450.
    auto [tagObj, valObj] = value::makeNewObject();
    value::ValueGuard vgObj(tagObj, valObj);
    expectedCard = estimateCardRange(arrHist,
                                     false /* lowInclusive */,
                                     tagStr,
                                     valStr,
                                     false /* highInclusive */,
                                     tagObj,
                                     valObj,
                                     true /* includeScalar */);
    ASSERT_APPROX_EQUAL(411.2, expectedCard, kErrorBound);

    // Queries with $elemMatch.
    const auto [tagInt, valInt] =
        std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(603));

    // Actual cardinality {$match: {a: {$elemMatch: {$eq: 603}}}} = 12.
    expectedCard = estimateCardEq(arrHist, tagInt, valInt, false /* includeScalar */);
    ASSERT_APPROX_EQUAL(12.0, expectedCard, kErrorBound);

    // Actual cardinality {$match: {a: {$elemMatch: {$lte: 603}}}} = 252.
    expectedCard = estimateCardRange(arrHist,
                                     false /* lowInclusive */,
                                     tagLowDbl,
                                     valLowDbl,
                                     true /* highInclusive */,
                                     tagInt,
                                     valInt,
                                     false /* includeScalar */);
    ASSERT_APPROX_EQUAL(293.0, expectedCard, kErrorBound);

    // Actual cardinality {$match: {a: {$elemMatch: {$gte: 603}}}} = 200.
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagInt,
                                     valInt,
                                     false /* highInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     false /* includeScalar */);
    ASSERT_APPROX_EQUAL(250.8, expectedCard, kErrorBound);

    // Actual cardinality {$match: {a: {$elemMatch: {$eq: 'cu'}}}} = 7.
    std::tie(tagStr, valStr) = value::makeNewString("cu"_sd);
    expectedCard = estimateCardEq(arrHist, tagStr, valStr, false /* includeScalar */);
    ASSERT_APPROX_EQUAL(3.8, expectedCard, kErrorBound);

    // Actual cardinality {$match: {a: {$elemMatch: {$gte: 'cu'}}}} = 125.
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagStr,
                                     valStr,
                                     false /* highInclusive */,
                                     tagObj,
                                     valObj,
                                     false /* includeScalar */);
    ASSERT_APPROX_EQUAL(109.7, expectedCard, kErrorBound);

    // Actual cardinality {$match: {a: {$elemMatch: {$lte: 'cu'}}}} = 141.
    expectedCard = estimateCardRange(arrHist,
                                     true /* lowInclusive */,
                                     tagLowStr,
                                     valLowStr,
                                     true /* highInclusive */,
                                     tagStr,
                                     valStr,
                                     false /* includeScalar */);
    ASSERT_APPROX_EQUAL(156.1, expectedCard, kErrorBound);
}
}  // namespace
}  // namespace mongo::ce

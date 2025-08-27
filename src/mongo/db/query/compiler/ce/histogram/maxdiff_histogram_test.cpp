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

#include "mongo/db/query/compiler/ce/histogram/histogram_test_utils.h"
#include "mongo/db/query/compiler/stats/max_diff.h"
#include "mongo/db/query/compiler/stats/maxdiff_test_utils.h"
#include "mongo/db/query/compiler/stats/rand_utils.h"
#include "mongo/db/query/compiler/stats/rand_utils_new.h"
#include "mongo/db/service_context_test_fixture.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::ce {
namespace {
namespace value = sbe::value;

using TypeTags = value::TypeTags;

using stats::DataDistribution;
using stats::genFixedValueArray;
using stats::getDataDistribution;
using stats::makeInt64Value;
using stats::ScalarHistogram;
using stats::TypeCounts;

const double kTolerance = 0.001;

class HistogramTest : public ServiceContextTest {};

TEST_F(HistogramTest, BasicCreate) {
    std::vector<BucketData> data{{0, 1.0, 11.0, 1.0},
                                 {10, 2.0, 12.0, 2.0},
                                 {20, 3.0, 13.0, 3.0},
                                 {30, 4.0, 14.0, 4.0},
                                 {40, 5.0, 15.0, 5.0},
                                 {50, 6.0, 16.0, 6.0}};
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_EQ(102.0, getTotals(hist).card);

    ASSERT_APPROX_EQUAL(1.0, estimateCard(hist, 0, EstimationType::kEqual), kTolerance);
    ASSERT_APPROX_EQUAL(6.0, estimateCard(hist, 5, EstimationType::kEqual), kTolerance);
    ASSERT_APPROX_EQUAL(0.0, estimateCard(hist, 55, EstimationType::kEqual), kTolerance);

    ASSERT_APPROX_EQUAL(28.1667, estimateCard(hist, 15, EstimationType::kLess), kTolerance);
    ASSERT_APPROX_EQUAL(32.5, estimateCard(hist, 15, EstimationType::kLessOrEqual), kTolerance);
    ASSERT_APPROX_EQUAL(39, estimateCard(hist, 20, EstimationType::kLess), kTolerance);
    ASSERT_APPROX_EQUAL(42.0, estimateCard(hist, 20, EstimationType::kLessOrEqual), kTolerance);

    ASSERT_APPROX_EQUAL(69.5, estimateCard(hist, 15, EstimationType::kGreater), kTolerance);
}

TEST_F(HistogramTest, CreateFixed) {
    std::vector<BucketData> data;
    for (int i = 0; i < 100; i++) {
        data.push_back(BucketData{i * 10, 1.0, 0.0, 0.0});
    }
    const ScalarHistogram hist = createHistogram(data);

    ASSERT_APPROX_EQUAL(50.0, estimateCard(hist, 50 * 10, EstimationType::kLess), kTolerance);
}

TEST_F(HistogramTest, MaxDiffBoundariesInt) {
    constexpr size_t nElems = 100;
    constexpr size_t nBuckets = 10;

    auto data = genFixedValueArray(nElems, 1.0, 0.0);
    auto opCtx = makeOperationContext();
    const size_t actualCard = getActualCard(opCtx.get(), data, "[{$match: {a: {$lt: 3}}}]");
    ASSERT_EQ(8, actualCard);

    sortValueVector(data);
    const DataDistribution& dataDistrib = getDataDistribution(data);

    auto result = generateTopKBuckets(dataDistrib, nBuckets);
    std::vector<int> expectedBounds = {0, 1, 3, 4, 5, 6, 7, 8, 9, 24};
    for (size_t i = 0; i < expectedBounds.size(); i++) {
        ASSERT_EQ(result[i]._idx, expectedBounds[i]);
    }
}

TEST_F(HistogramTest, MaxDiffBoundariesString) {
    constexpr size_t nElems = 100;
    constexpr size_t nBuckets = 10;

    auto data = genFixedValueArray(nElems, 0.0, 1.0);
    auto opCtx = makeOperationContext();
    const size_t actualCard =
        getActualCard(opCtx.get(), data, "[{$match: {a: {$lt: '91YgOvBB'}}}]");
    ASSERT_EQ(15, actualCard);

    sortValueVector(data);
    const DataDistribution& dataDistrib = getDataDistribution(data);

    auto result = generateTopKBuckets(dataDistrib, nBuckets);
    std::vector<int> expectedBounds = {0, 3, 7, 8, 12, 13, 14, 15, 16, 19};
    for (size_t i = 0; i < expectedBounds.size(); i++) {
        ASSERT_EQ(result[i]._idx, expectedBounds[i]);
    }
}

TEST_F(HistogramTest, MaxDiffTestSkewedInt) {
    constexpr size_t nElems = 24;
    constexpr size_t nBuckets = 5;

    std::vector<stats::SBEValue> data;
    auto rawData =
        std::vector<int>{0, 1, 1, 1, 2, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7};
    for (size_t i = 0; i < nElems; i++) {
        const auto [tag, val] = makeInt64Value(rawData[i]);
        data.emplace_back(tag, val);
    }
    auto opCtx = makeOperationContext();
    const size_t actualCard = getActualCard(opCtx.get(), data, "[{$match: {a: {$lt: 4}}}]");

    ASSERT_EQ(8, actualCard);

    sortValueVector(data);

    const DataDistribution& dataDistrib = getDataDistribution(data);

    auto result = generateTopKBuckets(dataDistrib, nBuckets, stats::SortArg::kArea);
    std::vector<int> expectedBounds = {0, 4, 5, 6, 7};
    for (size_t i = 0; i < expectedBounds.size(); i++) {
        ASSERT_EQ(result[i]._idx, expectedBounds[i]);
    }

    auto resultAreaDiff = generateTopKBuckets(dataDistrib, nBuckets);
    std::vector<int> expectedBoundsAreaDiff = {0, 1, 2, 3, 7};
    for (size_t i = 0; i < expectedBoundsAreaDiff.size(); i++) {
        ASSERT_EQ(resultAreaDiff[i]._idx, expectedBoundsAreaDiff[i]);
    }
}

TEST_F(HistogramTest, MaxDiffTestInt) {
    constexpr size_t nElems = 100;
    constexpr size_t nBuckets = 10;

    auto data = genFixedValueArray(nElems, 1.0, 0.0);
    auto opCtx = makeOperationContext();
    const size_t actualCard = getActualCard(opCtx.get(), data, "[{$match: {a: {$lt: 10}}}]");
    ASSERT_EQ(36, actualCard);

    sortValueVector(data);
    const DataDistribution& dataDistrib = getDataDistribution(data);

    const ScalarHistogram& hist = genMaxDiffHistogram(dataDistrib, nBuckets, stats::SortArg::kArea);
    LOGV2(8674801, "Generated histogram", "histogram"_attr = hist.toString());
    ASSERT_LTE(hist.getBuckets().size(), nBuckets);
    const double estimatedCard = estimateCard(hist, 10, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(36, estimatedCard, kTolerance);

    const ScalarHistogram& histAreaDiff = genMaxDiffHistogram(dataDistrib, nBuckets);
    LOGV2(8674802, "Generated histogram", "histogram"_attr = histAreaDiff.toString());
    ASSERT_LTE(histAreaDiff.getBuckets().size(), nBuckets);
    const double estimatedCardAreaDiff = estimateCard(histAreaDiff, 10, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(36, estimatedCardAreaDiff, kTolerance);
}

TEST_F(HistogramTest, MaxDiffTestIntMixedDistribution) {
    constexpr size_t nElems = 1'000'000;
    constexpr size_t nBuckets = 100;

    std::mt19937_64 seed(42);
    stats::MixedDistributionDescriptor mixed{{stats::DistrType::kUniform, 0.2},
                                             {stats::DistrType::kNormal, 0.8}};
    stats::TypeDistrVector td;
    td.push_back(std::make_unique<stats::IntDistribution>(mixed, 1.0, 2000, 0, 10'000));
    stats::DatasetDescriptorNew desc{std::move(td), seed};
    std::vector<stats::SBEValue> data = desc.genRandomDataset(nElems);

    auto opCtx = makeOperationContext();
    const size_t actualCard = getActualCard(opCtx.get(), data, "[{$match: {a: {$lt: 3500}}}]");

    sortValueVector(data);
    const DataDistribution& dataDistrib = getDataDistribution(data);

    const ScalarHistogram& hist = genMaxDiffHistogram(dataDistrib, nBuckets, stats::SortArg::kArea);
    const ScalarHistogram& histAreaDiff = genMaxDiffHistogram(dataDistrib, nBuckets);
    ASSERT_LTE(hist.getBuckets().size(), nBuckets);
    ASSERT_LTE(histAreaDiff.getBuckets().size(), nBuckets);
    const double estimatedCard = estimateCard(hist, 3500, EstimationType::kLess);
    const double estimatedCardAreaDiff = estimateCard(histAreaDiff, 3500, EstimationType::kLess);

    ASSERT_APPROX_EQUAL(actualCard, estimatedCard, actualCard * 0.1);
    ASSERT_APPROX_EQUAL(actualCard, estimatedCardAreaDiff, actualCard * 0.1);
}

TEST_F(HistogramTest, MaxDiffTestString) {
    constexpr size_t nElems = 100;
    constexpr size_t nBuckets = 10;

    auto randData = genFixedValueArray(nElems, 0.0, 1.0);
    LOGV2(8674803,
          "Generated random values",
          "nElems"_attr = nElems,
          "randData"_attr = printValueArray(randData));
    auto opCtx = makeOperationContext();
    const size_t actualCard =
        getActualCard(opCtx.get(), randData, "[{$match: {a: {$lt: '91YgOvBB'}}}]");
    ASSERT_EQ(15, actualCard);

    sortValueVector(randData);
    const DataDistribution& dataDistrib = getDataDistribution(randData);
    const auto [tag, val] = value::makeNewString("91YgOvBB"_sd);
    value::ValueGuard vg(tag, val);

    const ScalarHistogram& hist = genMaxDiffHistogram(dataDistrib, nBuckets, stats::SortArg::kArea);
    LOGV2(8674804, "Generated histogram", "histogram"_attr = hist.toString());
    ASSERT_LTE(hist.getBuckets().size(), nBuckets);
    const double estimatedCard = estimateCardinality(hist, tag, val, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(15.9443, estimatedCard, kTolerance);

    const ScalarHistogram& histAreaDiff = genMaxDiffHistogram(dataDistrib, nBuckets);
    LOGV2(8674805, "Generated histogram", "histogram"_attr = histAreaDiff.toString());
    ASSERT_LTE(histAreaDiff.getBuckets().size(), nBuckets);
    const double estimatedCardAreaDiff =
        estimateCardinality(histAreaDiff, tag, val, EstimationType::kLess).card;
    ASSERT_APPROX_EQUAL(9.59627, estimatedCardAreaDiff, kTolerance);
}

// Converts the string values from MaxDiffTestString into integers using the ASCII values of the
// first two chars of each string to check the correctness of boundaries
TEST_F(HistogramTest, MaxDiffTestStringToInt) {
    constexpr size_t nElems = 100;
    constexpr size_t nBuckets = 10;

    std::vector<stats::SBEValue> data;
    auto rawData = std::vector<int>{
        5071,  5071,  5071,  5071,  5071,  5155,  5155,  5155,  5155,  5155,  5573,  5573,  5573,
        5573,  5573,  6582,  6582,  6582,  6582,  6582,  6597,  6597,  6597,  6597,  6597,  7088,
        7088,  7088,  7088,  7088,  7399,  7399,  7399,  7399,  7399,  7408,  7408,  7408,  7408,
        7408,  8215,  8215,  8215,  8215,  8215,  8220,  8220,  8220,  8220,  8220,  8454,  8454,
        8454,  8454,  8454,  8956,  8956,  8956,  8956,  8956,  9872,  9872,  9872,  9872,  9872,
        9881,  9881,  9881,  9881,  9881,  10683, 10683, 10683, 10683, 10683, 10698, 10698, 10698,
        10698, 10698, 11505, 11505, 11505, 11505, 11505, 11510, 11510, 11510, 11510, 11510, 11919,
        11919, 11919, 11919, 11919, 12250, 12250, 12250, 12250, 12250};
    for (size_t i = 0; i < nElems; i++) {
        const auto [tag, val] = makeInt64Value(rawData[i]);
        data.emplace_back(tag, val);
    }
    auto opCtx = makeOperationContext();
    const size_t actualCard = getActualCard(opCtx.get(), data, "[{$match: {a: {$lt: 5749}}}]");
    ASSERT_EQ(15, actualCard);

    sortValueVector(data);
    const DataDistribution& dataDistrib = getDataDistribution(data);

    auto result = generateTopKBuckets(dataDistrib, nBuckets);
    std::vector<int> expectedBounds = {0, 3, 7, 8, 12, 13, 14, 15, 16, 19};
    for (size_t i = 0; i < expectedBounds.size(); i++) {
        ASSERT_EQ(result[i]._idx, expectedBounds[i]);
    }
}

TEST_F(HistogramTest, MaxDiffTestMixedTypes) {
    constexpr size_t nElems = 100;
    constexpr size_t nBuckets = 10;

    auto randData = genFixedValueArray(nElems, 0.5, 0.5);
    LOGV2(8674806,
          "Generated random values",
          "nElems"_attr = nElems,
          "randData"_attr = printValueArray(randData));

    auto opCtx = makeOperationContext();
    const size_t actualCard = getActualCard(opCtx.get(), randData, "[{$match: {a: {$lt: 10}}}]");
    ASSERT_EQ(18, actualCard);

    sortValueVector(randData);
    const DataDistribution& dataDistrib = getDataDistribution(randData);

    const ScalarHistogram& hist = genMaxDiffHistogram(dataDistrib, nBuckets, stats::SortArg::kArea);
    LOGV2(8674807, "Generated histogram", "histogram"_attr = hist.toString());
    ASSERT_LTE(hist.getBuckets().size(), nBuckets);
    const double estimatedCard = estimateCard(hist, 10, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(18.0, estimatedCard, kTolerance);

    const ScalarHistogram& histAreaDiff = genMaxDiffHistogram(dataDistrib, nBuckets);
    LOGV2(8674808, "Generated histogram", "histogram"_attr = histAreaDiff.toString());
    ASSERT_LTE(histAreaDiff.getBuckets().size(), nBuckets);
    const double estimatedCardAreaDiff = estimateCard(histAreaDiff, 10, EstimationType::kLess);
    ASSERT_APPROX_EQUAL(17.3043, estimatedCardAreaDiff, kTolerance);
}

TEST_F(HistogramTest, MaxDiffIntArrays) {
    constexpr size_t nElems = 100;
    constexpr size_t nBuckets = 10;

    auto rawData = genFixedValueArray(nElems, 1.0, 0.0);
    auto arrayData = nestArrays(rawData, 0 /* No empty arrays */);

    auto estimator = createCEHistogram(arrayData, nBuckets, stats::SortArg::kArea);
    auto estimatorAreaDiff = createCEHistogram(arrayData, nBuckets);

    auto opCtx = makeOperationContext();

    {
        const size_t actualCard =
            getActualCard(opCtx.get(), arrayData, "[{$match: {a: {$eq: 2}}}]");

        const auto [tag, val] = makeInt64Value(2);
        value::ValueGuard vg(tag, val);
        const EstimationResult estimatedCard =
            estimateCardinalityEq(*estimator, tag, val, true /*includeScalar*/);
        const EstimationResult estimatedCardAreaDiff =
            estimateCardinalityEq(*estimatorAreaDiff, tag, val, true);

        ASSERT_EQ(4, actualCard);
        ASSERT_APPROX_EQUAL(4.0, estimatedCard.card, kTolerance);
        ASSERT_APPROX_EQUAL(4.0, estimatedCardAreaDiff.card, kTolerance);
    }

    {
        const size_t actualCard =
            getActualCard(opCtx.get(), arrayData, "[{$match: {a: {$lt: 3}}}]");

        const auto [tag, val] = makeInt64Value(3);
        value::ValueGuard vg(tag, val);
        const EstimationResult estimatedCard =
            estimateCardinalityRange(*estimator,
                                     false /*lowInclusive*/,
                                     value::TypeTags::MinKey,
                                     0,
                                     false /*highInclusive*/,
                                     tag,
                                     val,
                                     true /* includeScalar */,
                                     ArrayRangeEstimationAlgo::kConjunctArrayCE);
        const EstimationResult estimatedCardAreaDiff =
            estimateCardinalityRange(*estimatorAreaDiff,
                                     false /*lowInclusive*/,
                                     value::TypeTags::MinKey,
                                     0,
                                     false /*highInclusive*/,
                                     tag,
                                     val,
                                     true /* includeScalar */,
                                     ArrayRangeEstimationAlgo::kConjunctArrayCE);

        ASSERT_EQ(6, actualCard);
        ASSERT_APPROX_EQUAL(6.0, estimatedCard.card, kTolerance);
        ASSERT_APPROX_EQUAL(7.0, estimatedCardAreaDiff.card, kTolerance);
    }

    {
        const size_t actualCard = getActualCard(
            opCtx.get(), arrayData, "[{$match: {a: {$elemMatch: {$gt: 2, $lt: 5}}}}]");

        const auto [lowTag, lowVal] = makeInt64Value(2);
        value::ValueGuard vgLow(lowTag, lowVal);
        const auto [highTag, highVal] = makeInt64Value(5);
        value::ValueGuard vgHigh(highTag, highVal);

        const EstimationResult estimatedCard =
            estimateCardinalityRange(*estimator,
                                     false /*lowInclusive*/,
                                     lowTag,
                                     lowVal,
                                     false /*highInclusive*/,
                                     highTag,
                                     highVal,
                                     false /* includeScalar */,
                                     ArrayRangeEstimationAlgo::kExactArrayCE);
        const EstimationResult estimatedCardAreaDiff =
            estimateCardinalityRange(*estimatorAreaDiff,
                                     false /*lowInclusive*/,
                                     lowTag,
                                     lowVal,
                                     false /*highInclusive*/,
                                     highTag,
                                     highVal,
                                     false /* includeScalar */,
                                     ArrayRangeEstimationAlgo::kExactArrayCE);

        ASSERT_EQ(2, actualCard);
        ASSERT_APPROX_EQUAL(3.15479, estimatedCard.card, kTolerance);
        ASSERT_APPROX_EQUAL(2.52383, estimatedCardAreaDiff.card, kTolerance);
    }
}

TEST_F(HistogramTest, MaxDiffEmptyArrays) {
    constexpr size_t nElems = 21;
    constexpr size_t nBuckets = 5;
    constexpr size_t emptyArrayCount = 3;

    auto rawData = genFixedValueArray(nElems, 1.0, 0.0);
    auto arrayData = nestArrays(rawData, emptyArrayCount);
    LOGV2(8674809,
          "Generated arrayData",
          "nElems"_attr = nElems,
          "arrayData"_attr = printValueArray(arrayData));

    const auto ceHist = createCEHistogram(arrayData, nBuckets, stats::SortArg::kAreaDiff);
    const auto ceHistAreaDiff = createCEHistogram(arrayData, nBuckets);

    const auto histograms = {ceHist, ceHistAreaDiff};

    std::for_each(histograms.begin(), histograms.end(), [emptyArrayCount](auto&& histogram) {
        ASSERT_EQ(histogram->getEmptyArrayCount(), emptyArrayCount);
    });
}

TEST(SimpleHistogramTest, HistogramTopBucketsFreqDiffUniformInt) {
    constexpr size_t nBuckets = 10;

    std::vector<stats::SBEValue> data;
    auto rawData = std::vector<int>{
        5001,  5001,  5001,  5008,  5008,  5008,  5008,  5008,  5008,  5071,  5071,  5071,  5071,
        5071,  5071,  5071,  5071,  5071,  5077,  5077,  5077,  5077,  5077,  5077,  5077,  5077,
        5077,  5077,  5077,  5077,  5100,  5100,  5100,  5100,  5100,  5200,  5200,  5210,  5210,
        5210,  5210,  5210,  5210,  5210,  5210,  5210,  5210,  5210,  5210,  5210,  5210,  5210,
        5215,  5215,  5215,  5215,  5215,  5155,  5155,  5573,  5573,  5573,  5573,  5573,  5573,
        5573,  5573,  5573,  6582,  6582,  6597,  6597,  6597,  6597,  6597,  6597,  6597,  6597,
        6597,  6597,  7088,  7088,  7088,  7088,  7088,  7088,  7088,  7088,  7088,  7088,  7088,
        7088,  7088,  7399,  7408,  7408,  7408,  7408,  7408,  7408,  7408,  7408,  7408,  7408,
        7408,  7408,  7408,  7408,  7408,  8215,  8215,  8215,  8215,  8215,  8215,  8215,  8220,
        8220,  8454,  8454,  8454,  8454,  8454,  8454,  8454,  8454,  8454,  8454,  8956,  8956,
        8956,  8956,  8956,  8956,  8956,  8956,  8956,  9872,  9872,  9872,  9872,  9872,  9872,
        9872,  9872,  9872,  9872,  9872,  9872,  9881,  9881,  9881,  9881,  9881,  10683, 10683,
        10683, 10683, 10683, 10698, 10698, 10698, 10698, 10698, 10698, 10698, 10698, 10698, 10698,
        10698, 10698, 10698, 11505, 11505, 11505, 11505, 11505, 11510, 11510, 11510, 11510, 11510,
        11919, 11919, 11919, 11919, 11919, 11919, 11919, 11919, 11919, 11919, 11919, 11919, 11919,
        11919, 11919, 11919, 11919, 11919, 11919, 11919, 11919, 12250, 12250, 12250, 12250, 12250};
    for (size_t i = 0; i < rawData.size(); i++) {
        const auto [tag, val] = makeInt64Value(rawData[i]);
        data.emplace_back(tag, val);
    }

    sortValueVector(data);
    const DataDistribution& dataDistrib = getDataDistribution(data);

    auto result = generateTopKBuckets(dataDistrib, nBuckets, stats::SortArg::kFreqDiff);
    std::vector<size_t> expectedBounds = {0, 7, 8, 13, 14, 15, 17, 22, 23, 25};

    for (size_t i = 0; i < expectedBounds.size(); i++) {
        ASSERT_EQ(result[i]._idx, expectedBounds[i]);
    }
}

}  // namespace
}  // namespace mongo::ce

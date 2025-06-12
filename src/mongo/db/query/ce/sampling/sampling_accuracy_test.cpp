/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/ce/ce_test_utils.h"
#include "mongo/db/query/ce/sampling/sampling_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {

// Generated data settings.
constexpr size_t seedData = 17278214;
constexpr std::pair<size_t, size_t> dataInterval({0, 1000});
constexpr size_t size = 1000;
constexpr size_t ndv = 10;
constexpr size_t numOfFields = 1;
constexpr int arrayTypeLength = 100;

// Query settings.
constexpr size_t seedQueriesLow = seedData;
constexpr size_t seedQueriesHigh = 1012348998;
constexpr std::pair<size_t, size_t> queryInterval({0, 1000});
const std::vector<QueryType> queryTypes = {kPoint};
constexpr int numberOfQueries = 1;

// Sampling settings.
const std::vector<SamplingEstimationBenchmarkConfiguration::SampleSizeDef> sampleSizes{
    SamplingEstimationBenchmarkConfiguration::SampleSizeDef::ErrorSetting1};
const std::vector<std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>>
    samplingAlgoAndChunks{{SamplingEstimatorImpl::SamplingStyle::kRandom, boost::none}};

// Configuration for evaluation
// constexpr size_t size = 1000000;
// constexpr size_t ndv = 1000;
// constexpr int numberOfQueries = 100;
// const std::vector<QueryType> queryTypes = {kPoint, kRange};
// const std::vector<SamplingEstimationBenchmarkConfiguration::SampleSizeDef> sampleSizes{
//     SamplingEstimationBenchmarkConfiguration::SampleSizeDef::ErrorSetting1,
//     SamplingEstimationBenchmarkConfiguration::SampleSizeDef::ErrorSetting2};
// const std::vector<std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>>
//     samplingAlgoAndChunks{{SamplingEstimatorImpl::SamplingStyle::kRandom, boost::none},
//                           {SamplingEstimatorImpl::SamplingStyle::kChunk, 10},
//                           {SamplingEstimatorImpl::SamplingStyle::kChunk, 20}};

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataUniformInt64) {

    // Generated data settings.
    const TypeCombination typeCombinationData{{TypeTags::NumberInt64, 100}};
    constexpr DataDistributionEnum dataDistribution = kUniform;

    // Query settings.
    const TypeCombination typeCombinationsQueries{{TypeTags::NumberInt64, 100}};

    runSamplingEstimatorTestConfiguration(dataDistribution,
                                          typeCombinationData,
                                          typeCombinationsQueries,
                                          size,
                                          dataInterval,
                                          queryInterval,
                                          numberOfQueries,
                                          seedData,
                                          seedQueriesLow,
                                          seedQueriesHigh,
                                          arrayTypeLength,
                                          ndv,
                                          numOfFields,
                                          queryTypes,
                                          sampleSizes,
                                          samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataUniformDouble) {

    // Generated data settings.
    const TypeCombination typeCombinationData{{TypeTags::NumberDouble, 100}};
    constexpr DataDistributionEnum dataDistribution = kUniform;

    // Query settings.
    const TypeCombination typeCombinationsQueries{{TypeTags::NumberDouble, 100}};

    runSamplingEstimatorTestConfiguration(dataDistribution,
                                          typeCombinationData,
                                          typeCombinationsQueries,
                                          size,
                                          dataInterval,
                                          queryInterval,
                                          numberOfQueries,
                                          seedData,
                                          seedQueriesLow,
                                          seedQueriesHigh,
                                          arrayTypeLength,
                                          ndv,
                                          numOfFields,
                                          queryTypes,
                                          sampleSizes,
                                          samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataNormalInt64) {

    // Generated data settings.
    const TypeCombination typeCombinationData{{TypeTags::NumberInt64, 100}};
    constexpr DataDistributionEnum dataDistribution = kNormal;

    // Query settings.
    const TypeCombination typeCombinationsQueries{{TypeTags::NumberInt64, 100}};

    runSamplingEstimatorTestConfiguration(dataDistribution,
                                          typeCombinationData,
                                          typeCombinationsQueries,
                                          size,
                                          dataInterval,
                                          queryInterval,
                                          numberOfQueries,
                                          seedData,
                                          seedQueriesLow,
                                          seedQueriesHigh,
                                          arrayTypeLength,
                                          ndv,
                                          numOfFields,
                                          queryTypes,
                                          sampleSizes,
                                          samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataNormalDouble) {

    // Generated data settings.
    const TypeCombination typeCombinationData{{TypeTags::NumberDouble, 100}};
    constexpr DataDistributionEnum dataDistribution = kNormal;

    // Query settings.
    const TypeCombination typeCombinationsQueries{{TypeTags::NumberDouble, 100}};

    runSamplingEstimatorTestConfiguration(dataDistribution,
                                          typeCombinationData,
                                          typeCombinationsQueries,
                                          size,
                                          dataInterval,
                                          queryInterval,
                                          numberOfQueries,
                                          seedData,
                                          seedQueriesLow,
                                          seedQueriesHigh,
                                          arrayTypeLength,
                                          ndv,
                                          numOfFields,
                                          queryTypes,
                                          sampleSizes,
                                          samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataZipfianInt64) {

    // Generated data settings.
    const TypeCombination typeCombinationData{{TypeTags::NumberInt64, 100}};
    constexpr DataDistributionEnum dataDistribution = kZipfian;

    // Query settings.
    const TypeCombination typeCombinationsQueries{{TypeTags::NumberInt64, 100}};

    runSamplingEstimatorTestConfiguration(dataDistribution,
                                          typeCombinationData,
                                          typeCombinationsQueries,
                                          size,
                                          dataInterval,
                                          queryInterval,
                                          numberOfQueries,
                                          seedData,
                                          seedQueriesLow,
                                          seedQueriesHigh,
                                          arrayTypeLength,
                                          ndv,
                                          numOfFields,
                                          queryTypes,
                                          sampleSizes,
                                          samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataZipfianDouble) {

    // Generated data settings.
    const TypeCombination typeCombinationData{{TypeTags::NumberDouble, 100}};
    constexpr DataDistributionEnum dataDistribution = kZipfian;

    // Query settings.
    const TypeCombination typeCombinationsQueries{{TypeTags::NumberDouble, 100}};

    runSamplingEstimatorTestConfiguration(dataDistribution,
                                          typeCombinationData,
                                          typeCombinationsQueries,
                                          size,
                                          dataInterval,
                                          queryInterval,
                                          numberOfQueries,
                                          seedData,
                                          seedQueriesLow,
                                          seedQueriesHigh,
                                          arrayTypeLength,
                                          ndv,
                                          numOfFields,
                                          queryTypes,
                                          sampleSizes,
                                          samplingAlgoAndChunks);
}

}  // namespace mongo::ce

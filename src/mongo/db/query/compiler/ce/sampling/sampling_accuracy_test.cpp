// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ce_test_utils.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {

constexpr size_t size = 1000;  // NOTE: Increase when testing locally.
std::vector<std::pair<std::string, int>> fieldNamesAndPos = {{"a", 0}};
constexpr int arrayTypeLength = 100;
std::vector<size_t> seedData = {2341534534};
std::vector<size_t> ndvs = {1000};

constexpr int numberOfQueries = 100;       // NOTE: Increase when testing locally.
constexpr int numberOfNDVIterations = 10;  // NOTE: Increase when testing locally.
std::vector<std::string> queryFields = {"a", "a"};
const std::vector<QueryType> queryTypes = {kPoint, kRange};
std::vector<std::pair<size_t, size_t>> seed_queries = {{1724178, 1724178}, {1724178, 2154698}};
std::vector<size_t> queryndvs = {1000, 1000};

const std::vector<SampleSizeDef> sampleSizes{SampleSizeDef::ErrorSetting1,
                                             SampleSizeDef::ErrorSetting2};
const std::vector<std::pair<SamplingCEMethodEnum, boost::optional<int>>> samplingAlgoAndChunks{
    {SamplingCEMethodEnum::kRandom, boost::none},
    {SamplingCEMethodEnum::kChunk, 10},
    {SamplingCEMethodEnum::kChunk, 20}};

namespace {
std::pair<DataConfiguration, WorkloadConfiguration> buildConfigs(
    std::vector<stats::DistrType> dataDistributions,
    std::vector<sbe::value::TypeTags> fieldDataTypes,
    std::vector<sbe::value::TypeTags> queryFieldDataTypes) {
    std::vector<CollectionFieldConfiguration> dataFieldConfiguration;
    for (size_t fieldIdx = 0; fieldIdx < fieldNamesAndPos.size(); fieldIdx++) {
        dataFieldConfiguration.push_back(
            CollectionFieldConfiguration(fieldNamesAndPos[fieldIdx].first,
                                         fieldNamesAndPos[fieldIdx].second,
                                         fieldDataTypes[fieldIdx],
                                         ndvs[fieldIdx],
                                         dataDistributions[fieldIdx],
                                         /*seed (optional)*/ seedData[fieldIdx]));
    }
    DataConfiguration dataConfig(size, dataFieldConfiguration);

    std::vector<DataFieldDefinition> queryFieldDefinitions;
    for (size_t fieldIdx = 0; fieldIdx < queryFields.size(); fieldIdx++) {
        queryFieldDefinitions.push_back(DataFieldDefinition(
            /*fieldName*/ queryFields[fieldIdx],
            /*fieldType*/ queryFieldDataTypes[fieldIdx],
            /*ndv*/ queryndvs[fieldIdx],
            /*dataDistribution*/ stats::DistrType::kUniform,
            /*seed (optional)*/ seed_queries[fieldIdx]));
    }
    WorkloadConfiguration queryConfig(numberOfQueries,
                                      QueryConfiguration(queryFieldDefinitions, queryTypes));
    return {dataConfig, queryConfig};
}
}  // namespace

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataUniformInt64) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kUniform};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberInt64};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberInt64,
                                                             TypeTags::NumberInt64};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateNDVAccuracyOnDataUniformInt64) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kUniform};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberInt64};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberInt64,
                                                             TypeTags::NumberInt64};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runNDVSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, numberOfNDVIterations, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataUniformDouble) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kUniform};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberDouble};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberDouble,
                                                             TypeTags::NumberDouble};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateNDVAccuracyOnDataUniformDouble) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kUniform};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberDouble};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberDouble,
                                                             TypeTags::NumberDouble};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runNDVSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, numberOfNDVIterations, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataNormalInt64) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kNormal};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberInt64};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberInt64,
                                                             TypeTags::NumberInt64};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateNDVAccuracyOnDataNormalInt64) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kNormal};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberInt64};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberInt64,
                                                             TypeTags::NumberInt64};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runNDVSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, numberOfNDVIterations, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataNormalDouble) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kNormal};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberDouble};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberDouble,
                                                             TypeTags::NumberDouble};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateNDVAccuracyOnDataNormalDouble) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kNormal};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberDouble};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberDouble,
                                                             TypeTags::NumberDouble};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runNDVSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, numberOfNDVIterations, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataZipfianInt64) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kZipfian};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberInt64};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberInt64,
                                                             TypeTags::NumberInt64};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateNDVAccuracyOnDataZipfianInt64) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kZipfian};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberInt64};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberInt64,
                                                             TypeTags::NumberInt64};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runNDVSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, numberOfNDVIterations, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataZipfianDouble) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kZipfian};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberDouble};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberDouble,
                                                             TypeTags::NumberDouble};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateNDVAccuracyOnDataZipfianDouble) {
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kZipfian};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberDouble};
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberDouble,
                                                             TypeTags::NumberDouble};
    auto [dataConfig, queryConfig] =
        buildConfigs(dataDistributions, fieldDataTypes, queryFieldDataTypes);
    runNDVSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, numberOfNDVIterations, sampleSizes, samplingAlgoAndChunks);
}

}  // namespace mongo::ce

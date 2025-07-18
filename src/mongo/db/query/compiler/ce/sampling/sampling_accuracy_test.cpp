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

#include "mongo/db/query/compiler/ce/ce_test_utils.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {

constexpr size_t size = 1000;
std::vector<std::pair<std::string, int>> fieldNamesAndPos = {{"a", 0}};
constexpr int arrayTypeLength = 100;
std::vector<size_t> seedData = {2341534534};
std::vector<size_t> ndvs = {1000};

constexpr int numberOfQueries = 100;
std::vector<std::string> queryFields = {"a", "a"};
const std::vector<QueryType> queryTypes = {kPoint, kRange};
std::vector<std::pair<size_t, size_t>> seed_queries = {{1724178, 1724178}, {1724178, 2154698}};
std::vector<size_t> queryndvs = {1000, 1000};

const std::vector<SampleSizeDef> sampleSizes{SampleSizeDef::ErrorSetting1,
                                             SampleSizeDef::ErrorSetting2};
const std::vector<std::pair<SamplingEstimatorImpl::SamplingStyle, boost::optional<int>>>
    samplingAlgoAndChunks{{SamplingEstimatorImpl::SamplingStyle::kRandom, boost::none},
                          {SamplingEstimatorImpl::SamplingStyle::kChunk, 10},
                          {SamplingEstimatorImpl::SamplingStyle::kChunk, 20}};

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataUniformInt64) {
    // Initialize data configuration.
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kUniform};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberInt64};
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

    // Initialize query configuration.
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberInt64,
                                                             TypeTags::NumberInt64};
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
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataUniformDouble) {
    // Initialize data configuration.
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kUniform};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberDouble};
    std::vector<CollectionFieldConfiguration> dataFieldConfiguration;
    for (size_t fieldIdx = 0; fieldIdx < fieldNamesAndPos.size(); fieldIdx++) {
        dataFieldConfiguration.push_back(
            CollectionFieldConfiguration(fieldNamesAndPos[fieldIdx].first,
                                         fieldNamesAndPos[fieldIdx].second,
                                         fieldDataTypes[fieldIdx],
                                         ndvs[fieldIdx],
                                         dataDistributions[fieldIdx],
                                         seedData[fieldIdx]));
    }
    DataConfiguration dataConfig(size, dataFieldConfiguration);

    // Initialize query configuration.
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberDouble,
                                                             TypeTags::NumberDouble};
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
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataNormalInt64) {
    // Initialize data configuration.
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kNormal};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberInt64};
    std::vector<CollectionFieldConfiguration> dataFieldConfiguration;
    for (size_t fieldIdx = 0; fieldIdx < fieldNamesAndPos.size(); fieldIdx++) {
        dataFieldConfiguration.push_back(
            CollectionFieldConfiguration(fieldNamesAndPos[fieldIdx].first,
                                         fieldNamesAndPos[fieldIdx].second,
                                         fieldDataTypes[fieldIdx],
                                         ndvs[fieldIdx],
                                         dataDistributions[fieldIdx],
                                         seedData[fieldIdx]));
    }
    DataConfiguration dataConfig(size, dataFieldConfiguration);

    // Initialize query configuration.
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberInt64,
                                                             TypeTags::NumberInt64};
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
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataNormalDouble) {
    // Initialize data configuration.
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kNormal};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberDouble};
    std::vector<CollectionFieldConfiguration> dataFieldConfiguration;
    for (size_t fieldIdx = 0; fieldIdx < fieldNamesAndPos.size(); fieldIdx++) {
        dataFieldConfiguration.push_back(
            CollectionFieldConfiguration(fieldNamesAndPos[fieldIdx].first,
                                         fieldNamesAndPos[fieldIdx].second,
                                         fieldDataTypes[fieldIdx],
                                         ndvs[fieldIdx],
                                         dataDistributions[fieldIdx],
                                         seedData[fieldIdx]));
    }
    DataConfiguration dataConfig(size, dataFieldConfiguration);

    // Initialize query configuration.
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberDouble,
                                                             TypeTags::NumberDouble};
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
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataZipfianInt64) {
    // Initialize data configuration.
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kZipfian};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberInt64};
    std::vector<CollectionFieldConfiguration> dataFieldConfiguration;
    for (size_t fieldIdx = 0; fieldIdx < fieldNamesAndPos.size(); fieldIdx++) {
        dataFieldConfiguration.push_back(
            CollectionFieldConfiguration(fieldNamesAndPos[fieldIdx].first,
                                         fieldNamesAndPos[fieldIdx].second,
                                         fieldDataTypes[fieldIdx],
                                         ndvs[fieldIdx],
                                         dataDistributions[fieldIdx],
                                         seedData[fieldIdx]));
    }
    DataConfiguration dataConfig(size, dataFieldConfiguration);

    // Initialize query configuration.
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberInt64,
                                                             TypeTags::NumberInt64};
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
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

TEST_F(SamplingAccuracyTest, CalculateAccuracyOnDataZipfianDouble) {
    // Initialize data configuration.
    std::vector<stats::DistrType> dataDistributions = {stats::DistrType::kZipfian};
    std::vector<sbe::value::TypeTags> fieldDataTypes = {TypeTags::NumberDouble};
    std::vector<CollectionFieldConfiguration> dataFieldConfiguration;
    for (size_t fieldIdx = 0; fieldIdx < fieldNamesAndPos.size(); fieldIdx++) {
        dataFieldConfiguration.push_back(
            CollectionFieldConfiguration(fieldNamesAndPos[fieldIdx].first,
                                         fieldNamesAndPos[fieldIdx].second,
                                         fieldDataTypes[fieldIdx],
                                         ndvs[fieldIdx],
                                         dataDistributions[fieldIdx],
                                         seedData[fieldIdx]));
    }
    DataConfiguration dataConfig(size, dataFieldConfiguration);

    // Initialize query configuration.
    std::vector<sbe::value::TypeTags> queryFieldDataTypes = {TypeTags::NumberDouble,
                                                             TypeTags::NumberDouble};
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
    runSamplingEstimatorTestConfiguration(
        dataConfig, queryConfig, sampleSizes, samplingAlgoAndChunks);
}

}  // namespace mongo::ce

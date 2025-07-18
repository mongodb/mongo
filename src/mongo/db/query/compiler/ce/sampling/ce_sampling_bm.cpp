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

#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/ce/ce_test_utils.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/query/multiple_collection_accessor.h"

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::ce {

// Defining a constant seed value for data generation, the use of this ensure that point queries
// will match against at least one generated document in the dataset.
const size_t seed_value1 = 1724178;

/**
 * This map defines a set of configurations for collection generation.
 * The keys of the map are used as part of the benchmark state inputs to create a variety of base
 * collection to test against.
 * Each configuration requires a vector of CollectionFieldConfigurations which represent the set of
 * "user defined" fields. Each field requires configuring its name, position in the collection as
 * well as type and distribution information.
 * The collection will contain as many fields as the maximum position of the user defined fields.
 * The remaining in-between fields are copies of the user defined fields with names with suffix an
 * underscore and a number.
 * When defining the fields in the collection, order the fields in increasing position order.
 */
std::map<int, std::vector<CollectionFieldConfiguration>> collectionFieldConfigurations = {
    {1,
     {CollectionFieldConfiguration(
          /*fieldName*/ "a",
          /*fieldPosition*/ 0,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 500,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1),
      CollectionFieldConfiguration(
          /*fieldName*/ "b",
          /*fieldPosition*/ 5,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 500,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1),
      CollectionFieldConfiguration(
          /*fieldName*/ "c",
          /*fieldPosition*/ 20,
          /*fieldType*/ sbe::value::TypeTags::NumberInt64,
          /*ndv*/ 500,
          /*dataDistribution*/ stats::DistrType::kUniform,
          /*seed*/ seed_value1)}},
    {2,
     {CollectionFieldConfiguration({/*fieldName*/ "a",
                                    /*fieldPosition*/ 0,
                                    /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                                    /*ndv*/ 500,
                                    /*dataDistribution*/ stats::DistrType::kUniform,
                                    /*seed*/ seed_value1})}},
    {3,
     {CollectionFieldConfiguration(
         /*fieldName*/ "a",
         /*fieldPosition*/ 5,
         /*fieldType*/ sbe::value::TypeTags::NumberInt64,
         /*ndv*/ 500,
         /*dataDistribution*/ stats::DistrType::kUniform,
         /*seed*/ seed_value1)}}};

auto queryConfig1 = WorkloadConfiguration(
    /*numberOfQueries*/ 1000,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "a",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ 500,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value1, 1724178})},
                       /*queryTypes*/ {kPoint}));

auto queryConfig2 = WorkloadConfiguration(
    /*numberOfQueries*/ 1000,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "b",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ 500,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            {seed_value1, 1724178}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ 500,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1, 1724178})},
                       /*queryTypes*/ {kPoint, kPoint}));

auto queryConfig3 = WorkloadConfiguration(
    /*numberOfQueries*/ 1000,
    QueryConfiguration({DataFieldDefinition(
                            /*fieldName*/ "a",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ 500,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            {seed_value1, 1724178}),
                        DataFieldDefinition(
                            /*fieldName*/ "c",
                            /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                            /*ndv*/ 500,
                            /*dataDistribution*/ stats::DistrType::kUniform,
                            /*seed*/ {seed_value1, 1724178})},
                       /*queryTypes*/ {kPoint, kPoint}));

auto queryConfig4 = WorkloadConfiguration(
    /*numberOfQueries*/ 1000,
    QueryConfiguration({DataFieldDefinition(
                           /*fieldName*/ "a",
                           /*fieldType*/ sbe::value::TypeTags::NumberInt64,
                           /*ndv*/ 500,
                           /*dataDistribution*/ stats::DistrType::kUniform,
                           /*seed*/ {seed_value1, 1724178})},
                       /*queryTypes*/ {kPoint}));

std::vector<std::pair<int, WorkloadConfiguration&>> queryConfigVector{
    {1, queryConfig1}, {2, queryConfig2}, {3, queryConfig3}, {4, queryConfig4}};


/**
 * This map defines a set of configurations for workload generation.
 * The keys of the map are used as part of the benchmark state inputs to create a variety of the
 * workload to run against the defined collection. Each configuration requires a vector of
 * QueryConfiguration represents the set of fields to be queried against. Each field
 * requires configuring its name as well as type and distribution information. The implementation
 * currently supports only conjunction (AND) correlation between fields.
 * IMPORTANT: To ensure that point queries will have matches, ensure that the seeds provided for
 * query value generation to be identical with the generation of the dataset.
 */
std::map<int, WorkloadConfiguration> queryFieldsConfigurations(queryConfigVector.begin(),
                                                               queryConfigVector.end());

void BM_CreateSample(benchmark::State& state) {
    // Translate the fields and positions configurations based on the defined map.
    auto fieldsConfig = collectionFieldConfigurations[state.range(1)];

    DataConfiguration dataConfig(
        /*dataSize*/ state.range(0),
        /*dataFieldConfig*/ fieldsConfig);

    // Translate the sample size definition to corresponding sample size.
    auto sampleSize = translateSampleDefToActualSampleSize(
        /*sampleSizeDef*/ static_cast<SampleSizeDef>(state.range(2)));

    // Translate the number of chunks variable to both number of chunks and sampling algo.
    // This benchmark given as input numOfChunks <= 0 will use kRandom.
    auto sampling =
        iniitalizeSamplingAlgoBasedOnChunks(/*samplingAlgo-numOfChunks*/ state.range(3));

    // Generate data and populate source collection
    SamplingEstimatorTest samplingEstimatorTest;
    initializeSamplingEstimator(dataConfig, samplingEstimatorTest);

    // Initialize collection accessor
    AutoGetCollection collPtr(samplingEstimatorTest.getOperationContext(),
                              samplingEstimatorTest._kTestNss,
                              LockMode::MODE_IX);
    MultipleCollectionAccessor collection =
        MultipleCollectionAccessor(samplingEstimatorTest.getOperationContext(),
                                   &collPtr.getCollection(),
                                   samplingEstimatorTest._kTestNss,
                                   /*isAnySecondaryNamespaceAViewOrNotFullyLocal*/ false,
                                   /*secondaryExecNssList*/ {});

    for (auto _ : state) {
        // Create sample from the provided collection
        SamplingEstimatorImpl samplingEstimator(
            samplingEstimatorTest.getOperationContext(),
            collection,
            sampleSize,
            sampling.first,
            sampling.second,
            SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size));
    }
}

void BM_RunCardinalityEstimationOnSample(benchmark::State& state) {

    // queryFieldsConfigurations.emplace(1, config1);

    // Translate the fields and positions configurations based on the defined map.
    auto fieldsConfig = collectionFieldConfigurations[state.range(1)];

    DataConfiguration dataConfig(
        /*dataSize*/ state.range(0),
        /*dataFieldConfig*/ fieldsConfig);

    // Setup query types.
    WorkloadConfiguration& workloadConfig = queryFieldsConfigurations[state.range(5)];

    // Generate data and populate source collection
    SamplingEstimatorTest samplingEstimatorTest;
    initializeSamplingEstimator(dataConfig, samplingEstimatorTest);

    // Initialize collection accessor
    AutoGetCollection collPtr(samplingEstimatorTest.getOperationContext(),
                              samplingEstimatorTest._kTestNss,
                              LockMode::MODE_IX);
    MultipleCollectionAccessor collection =
        MultipleCollectionAccessor(samplingEstimatorTest.getOperationContext(),
                                   &collPtr.getCollection(),
                                   samplingEstimatorTest._kTestNss,
                                   /*isAnySecondaryNamespaceAViewOrNotFullyLocal*/ false,
                                   /*secondaryExecNssList*/ {});

    // Translate the sample size definition to corresponding sample size.
    auto sampleSize = translateSampleDefToActualSampleSize(
        /*sampleSizeDef*/ static_cast<SampleSizeDef>(state.range(2)));

    // Translate the number of chunks variable to both number of chunks and sampling algo.
    // This benchmark given as input numOfChunks <= 0 will use kRandom.
    auto sampling =
        iniitalizeSamplingAlgoBasedOnChunks(/*samplingAlgo-numOfChunks*/ state.range(3));

    // Create sample from the provided collection
    SamplingEstimatorImpl samplingEstimator(
        samplingEstimatorTest.getOperationContext(),
        collection,
        sampleSize,
        sampling.first,
        sampling.second,
        SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size));

    // Generate queries.
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> queryFieldsIntervals =
        generateMultiFieldIntervals(workloadConfig);

    std::vector<std::unique_ptr<MatchExpression>> allMatchExpressionQueries =
        createQueryMatchExpressionOnMultipleFields(workloadConfig, queryFieldsIntervals);

    size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            samplingEstimator.estimateCardinality(allMatchExpressionQueries[i].get()));
        i = (i + 1) % allMatchExpressionQueries.size();
    }
    state.SetItemsProcessed(state.iterations());
}

/**
 * Evaluate the performance of preparing the sampling CE estimator which mainly concentrates on
 * creating samples using a variety of Sampling strategies. This invocation will vary the number
 * documents and number of fields in the base collection as well as the sample size.
 */
BENCHMARK(BM_CreateSample)
    ->ArgNames({"dataSize", "dataFieldsConfiguration", "sampleSizeDef", "samplingAlgo-numChunks"})
    ->ArgsProduct({/*dataSize*/ {100},
                   /*dataFieldsConfiguration*/ {1},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting1)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1}})
    ->Unit(benchmark::kMillisecond);

/**
 * Evaluate the performance of estimating CE using an already populated sample. The
 * estimation mainly concentrates on processing the already existing sample and
 * extrapolating the cardinality results. This invocation will vary the number documents and
 * number of fields in the base collection, the type of queries (point and range), as well
 * as the sample size.
 */
BENCHMARK(BM_RunCardinalityEstimationOnSample)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "numberOfQueries",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {100},
                   /*dataFieldsConfiguration*/ {1},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting1)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*numberOfQueries*/ {50},
                   /*queryFieldConfig*/ {1}})
    ->Unit(benchmark::kMillisecond);
}  // namespace mongo::ce

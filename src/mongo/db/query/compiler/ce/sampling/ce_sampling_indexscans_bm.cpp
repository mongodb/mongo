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
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_rewrites.h"
#include "mongo/db/query/multiple_collection_accessor.h"

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::ce {

// Defining a constant seed value for data generation, the use of this ensure that point queries
// will match against at least one generated document in the dataset.
const size_t seed_value1 = 1724178;
const size_t seed_value2 = 8713211;

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
         /*fieldName*/ "f0",
         /*fieldPosition*/ 0,
         /*fieldType*/ sbe::value::TypeTags::NumberInt64,
         /*ndv*/ 1000,
         /*dataDistribution*/ stats::DistrType::kUniform,
         /*seed*/ seed_value1)}},
    {2,
     {CollectionFieldConfiguration(
         /*fieldName*/ "f0",
         /*fieldPosition*/ 50,
         /*fieldType*/ sbe::value::TypeTags::NumberInt64,
         /*ndv*/ 1000,
         /*dataDistribution*/ stats::DistrType::kUniform,
         /*seed*/ seed_value1)}},
    {3,
     {
         CollectionFieldConfiguration(
             /*fieldName*/ "f0",
             /*fieldPosition*/ 0,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
         CollectionFieldConfiguration(
             /*fieldName*/ "f1",
             /*fieldPosition*/ 1,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
         CollectionFieldConfiguration(
             /*fieldName*/ "f2",
             /*fieldPosition*/ 2,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
         CollectionFieldConfiguration(
             /*fieldName*/ "f3",
             /*fieldPosition*/ 3,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
         CollectionFieldConfiguration(
             /*fieldName*/ "f4",
             /*fieldPosition*/ 4,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
     }},
    {4,
     {
         CollectionFieldConfiguration(
             /*fieldName*/ "f0",
             /*fieldPosition*/ 50,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
         CollectionFieldConfiguration(
             /*fieldName*/ "f1",
             /*fieldPosition*/ 51,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
         CollectionFieldConfiguration(
             /*fieldName*/ "f2",
             /*fieldPosition*/ 52,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
         CollectionFieldConfiguration(
             /*fieldName*/ "f3",
             /*fieldPosition*/ 53,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
         CollectionFieldConfiguration(
             /*fieldName*/ "f4",
             /*fieldPosition*/ 54,
             /*fieldType*/ sbe::value::TypeTags::NumberInt64,
             /*ndv*/ 1000,
             /*dataDistribution*/ stats::DistrType::kUniform,
             /*seed*/ seed_value1),
     }}};

/**
 * This map defines the set of attributes queries will evaluate predicates on.
 * The seeds represent the seeds for the lower and upper values for a range.
 * For a point query only the first seed is used whereas for range query both seeds are relevant.
 * For range queries, ensure that the two seed values differ, otherwise the queries become in
 * essence point queries.
 */

// Single field point queries.
auto queryConfig1 = WorkloadConfiguration(
    /* numberOfQueries */ 20,
    QueryConfiguration({DataFieldDefinition(
                           /* fieldName*/ "f0",
                           /* fieldType */ sbe::value::TypeTags::NumberInt64,
                           /* ndv */ 1000,
                           stats::DistrType::kUniform,
                           {seed_value1, 1724178})},
                       /* queryTypes */ {kPoint}));

// Single field range queries.
auto queryConfig2 = WorkloadConfiguration(
    /* numberOfQueries */ 20,
    QueryConfiguration({DataFieldDefinition(
                           /* fieldName*/ "f0",
                           /* fieldType */ sbe::value::TypeTags::NumberInt64,
                           /* ndv */ 1000,
                           stats::DistrType::kUniform,
                           {seed_value1, seed_value2})},
                       /* queryTypes */ {kRange}));

// Multi-field point queries.
auto queryConfig3 = WorkloadConfiguration(
    /*numberOfQueries*/ 20,
    QueryConfiguration(
        {
            DataFieldDefinition(
                /* fieldName*/ "f0",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
            DataFieldDefinition(
                /* fieldName*/ "f1",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
            DataFieldDefinition(
                /* fieldName*/ "f2",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
            DataFieldDefinition(
                /* fieldName*/ "f3",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
            DataFieldDefinition(
                /* fieldName*/ "f4",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, 1724178}),
        },
        /* queryTypes */ {kPoint, kPoint, kPoint, kPoint, kPoint}));

// Multi-field range queries.
auto queryConfig4 = WorkloadConfiguration(
    /* numberOfQueries */ 20,
    QueryConfiguration(
        {
            DataFieldDefinition(
                /* fieldName*/ "f0",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f1",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f2",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f3",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
            DataFieldDefinition(
                /* fieldName*/ "f4",
                /* fieldType */ sbe::value::TypeTags::NumberInt64,
                /* ndv */ 1000,
                stats::DistrType::kUniform,
                {seed_value1, seed_value2}),
        },
        /* queryTypes */
        {kRange, kRange, kRange, kRange, kRange}));

std::vector<std::pair<int, WorkloadConfiguration&>> queryConfigVector{
    {1, queryConfig1},
    {2, queryConfig2},
    {3, queryConfig3},
    {4, queryConfig4},
};

/**
 * This map defines a set of configurations for workload generation.
 */
std::map<int, WorkloadConfiguration> queryFieldsConfigurations(queryConfigVector.begin(),
                                                               queryConfigVector.end());

/**
 * Evaluate the performance of estimating IndexScanNode CE with index bounds.
 */
void BM_RunCardinalityEstimationOnSampleWithIndexBounds(benchmark::State& state) {
    auto fieldsConfig = collectionFieldConfigurations[state.range(1)];
    DataConfiguration dataConfig(
        /*dataSize*/ state.range(0),
        /*dataFieldConfig*/ fieldsConfig);

    // Setup query types.
    WorkloadConfiguration& workloadConfig = queryFieldsConfigurations[state.range(4)];

    // Generate data and populate source collection
    SamplingEstimatorTest samplingEstimatorTest;
    initializeSamplingEstimator(dataConfig, samplingEstimatorTest);

    // Initialize collection accessor
    auto opCtx = samplingEstimatorTest.getOperationContext();
    auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, samplingEstimatorTest._kTestNss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
    MultipleCollectionAccessor collection = MultipleCollectionAccessor(acquisition);

    // Translate the sample size definition to corresponding sample size.
    auto sampleSize = translateSampleDefToActualSampleSize(
        /*sampleSizeDef*/ static_cast<SampleSizeDef>(state.range(2)));

    // Translate the number of chunks variable to both number of chunks and sampling algo.
    // This benchmark given as input numOfChunks <= 0 will use kRandom.
    auto samplingStyle =
        iniitalizeSamplingAlgoBasedOnChunks(/*samplingAlgo-numOfChunks*/ state.range(3));

    // Create sample from the provided collection.
    SamplingEstimatorImpl samplingEstimator(
        samplingEstimatorTest.getOperationContext(),
        collection,
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        sampleSize,
        samplingStyle.first,
        samplingStyle.second,
        SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size));
    samplingEstimator.generateSample(ce::NoProjection{});

    // Generate query intervals.
    // One vector element per query field, each element is a vector of numberOfQueries intervals.
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> allQueryIntervals =
        generateMultiFieldIntervals(workloadConfig);

    size_t i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        // Create index bounds combining intervals for all query fields.
        std::vector<std::pair<stats::SBEValue, stats::SBEValue>> queryIntervals;
        for (size_t fieldIdx = 0; fieldIdx < workloadConfig.queryConfig.queryFields.size();
             fieldIdx++) {
            queryIntervals.push_back(allQueryIntervals[fieldIdx][i]);
        }
        IndexBounds bounds = getIndexBounds(workloadConfig.queryConfig, queryIntervals);
        state.ResumeTiming();

        benchmark::DoNotOptimize(samplingEstimator.estimateRIDs(bounds, nullptr));
        i = (i + 1) % workloadConfig.numberOfQueries;
    }
    state.SetItemsProcessed(state.iterations());
}

/**
 * Evaluate the performance of estimating IndexScanNode CE with match expression.
 */
void BM_RunCardinalityEstimationOnSampleWithMatchExpressions(benchmark::State& state) {
    auto fieldsConfig = collectionFieldConfigurations[state.range(1)];
    DataConfiguration dataConfig(
        /*dataSize*/ state.range(0),
        /*dataFieldConfig*/ fieldsConfig);

    // Setup query types.
    WorkloadConfiguration& workloadConfig = queryFieldsConfigurations[state.range(4)];

    // Generate data and populate source collection
    SamplingEstimatorTest samplingEstimatorTest;
    initializeSamplingEstimator(dataConfig, samplingEstimatorTest);

    // Initialize collection accessor
    auto opCtx = samplingEstimatorTest.getOperationContext();
    auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, samplingEstimatorTest._kTestNss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);
    MultipleCollectionAccessor collection = MultipleCollectionAccessor(acquisition);

    // Translate the sample size definition to corresponding sample size.
    auto sampleSize = translateSampleDefToActualSampleSize(
        /*sampleSizeDef*/ static_cast<SampleSizeDef>(state.range(2)));

    // Translate the number of chunks variable to both number of chunks and sampling algo.
    // This benchmark given as input numOfChunks <= 0 will use kRandom.
    auto samplingStyle =
        iniitalizeSamplingAlgoBasedOnChunks(/*samplingAlgo-numOfChunks*/ state.range(3));

    // Create sample from the provided collection.
    SamplingEstimatorImpl samplingEstimator(
        samplingEstimatorTest.getOperationContext(),
        collection,
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        sampleSize,
        samplingStyle.first,
        samplingStyle.second,
        SamplingEstimatorTest::makeCardinalityEstimate(dataConfig.size));
    samplingEstimator.generateSample(ce::NoProjection{});

    // Generate query intervals.
    // One vector element per query field, each element is a vector of numberOfQueries intervals.
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> allQueryIntervals =
        generateMultiFieldIntervals(workloadConfig);

    size_t i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        // Create index bounds combining intervals for all query fields.
        std::vector<std::pair<stats::SBEValue, stats::SBEValue>> queryIntervals;
        for (size_t fieldIdx = 0; fieldIdx < workloadConfig.queryConfig.queryFields.size();
             fieldIdx++) {
            queryIntervals.push_back(allQueryIntervals[fieldIdx][i]);
        }
        IndexBounds bounds = getIndexBounds(workloadConfig.queryConfig, queryIntervals);
        state.ResumeTiming();

        auto matchExpr = cost_based_ranker::getMatchExpressionFromBounds(bounds, nullptr);
        benchmark::DoNotOptimize(samplingEstimator.estimateCardinality(matchExpr.get()));
        i = (i + 1) % workloadConfig.numberOfQueries;
    }
    state.SetItemsProcessed(state.iterations());
}

// Single field queries.
BENCHMARK(BM_RunCardinalityEstimationOnSampleWithIndexBounds)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {1, 2},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {1, 2}})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_RunCardinalityEstimationOnSampleWithMatchExpressions)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {1, 2},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {1, 2}})
    ->Unit(benchmark::kMillisecond);

// Multi-field queries.
BENCHMARK(BM_RunCardinalityEstimationOnSampleWithIndexBounds)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {3, 4},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {3, 4}})
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_RunCardinalityEstimationOnSampleWithMatchExpressions)
    ->ArgNames({
        "dataSize",
        "dataFieldsConfiguration",
        "sampleSizeDef",
        "samplingAlgo-numChunks",
        "queryFieldConfig",
    })
    ->ArgsProduct({/*dataSize*/ {10000},
                   /*dataFieldsConfiguration*/ {3, 4},
                   /*sampleSizeDef*/
                   {static_cast<int>(SampleSizeDef::ErrorSetting5),
                    static_cast<int>(SampleSizeDef::ErrorSetting2)},
                   /*samplingAlgo-numChunks*/ {/*random*/ -1},
                   /*queryFieldConfig*/ {3, 4}})
    ->Unit(benchmark::kMillisecond);
}  // namespace mongo::ce

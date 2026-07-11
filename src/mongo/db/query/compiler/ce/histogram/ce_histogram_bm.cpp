// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/histogram/histogram_test_utils.h"
#include "mongo/db/query/compiler/stats/max_diff.h"

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::ce {

void BM_CreateHistogram(benchmark::State& state) {
    auto dataType = static_cast<sbe::value::TypeTags>(state.range(3));
    auto ndv = 1000;
    size_t seed = 1354754;

    CollectionFieldConfiguration field(
        /*fieldName*/ "a",
        /*fieldPositionInCollection*/ 0,
        /*dataType*/ dataType,
        /*ndv*/ ndv,
        /*dataDistribution*/ static_cast<stats::DistrType>(state.range(2)),
        /*seed*/ seed);

    DataConfiguration dataConfig(
        /*size*/ static_cast<int>(state.range(1)),
        /*dataFieldConfig*/ {field});

    auto numberOfBuckets = static_cast<int>(state.range(0));

    std::vector<std::vector<stats::SBEValue>> data;
    generateDataBasedOnConfig(dataConfig, data);

    for (auto curState : state) {
        // Built histogram (there is only one field thus we use data[0])
        auto ceHist = stats::createCEHistogram(data[0], numberOfBuckets);
    }
}

void BM_RunHistogramEstimations(benchmark::State& state) {
    auto dataType = static_cast<sbe::value::TypeTags>(state.range(3));
    auto ndv = 1000;
    size_t seed = 1354754;
    size_t seed2 = 1354754;

    CollectionFieldConfiguration field(
        /*fieldName*/ "a",
        /*fieldPositionInCollection*/ 0,
        /*dataType*/ dataType,
        /*ndv*/ ndv,
        /*dataDistribution*/ static_cast<stats::DistrType>(state.range(2)),
        /*seed*/ seed);

    DataConfiguration dataConfig(
        /*size*/ static_cast<int>(state.range(1)),
        /*dataFieldConfig*/ {field});

    std::vector<std::vector<stats::SBEValue>> data;
    generateDataBasedOnConfig(dataConfig, data);

    // Define Queries.
    const int numberOfQueries = 10000;

    sbe::value::TypeTags typeQuery = dataType;
    if (typeQuery == sbe::value::TypeTags::Array) {
        // The array data generation currently only supports integer elements as implemented in
        // populateTypeDistrVectorAccordingToInputConfig.
        typeQuery = sbe::value::TypeTags::NumberInt64;
    }

    QueryConfiguration queryConfig(
        {DataFieldDefinition(
            /*fieldName*/ "a",
            /*fieldType*/ typeQuery,
            /*ndv*/ ndv,
            /*dataDistribution*/ static_cast<stats::DistrType>(state.range(2)),
            /*seed (optional)*/ {seed, seed2})},
        /*queryTypes*/ {static_cast<QueryType>(state.range(5))});

    WorkloadConfiguration workloadConfig(/*numberOfQueries*/ numberOfQueries,
                                         /*queryConfig*/ queryConfig);

    // We assume there is only one field i.e., we get always the index 0 in all vectors.
    auto queryIntervals =
        generateIntervals(workloadConfig.queryConfig.queryTypes[0],
                          workloadConfig.queryConfig.queryFields[0].dataInterval,
                          workloadConfig.numberOfQueries,
                          workloadConfig.queryConfig.queryFields[0].typeCombinationData,
                          workloadConfig.queryConfig.queryFields[0].seed1,
                          workloadConfig.queryConfig.queryFields[0].seed2,
                          workloadConfig.queryConfig.queryFields[0].ndv);

    auto numberOfBuckets = static_cast<int>(state.range(0));
    auto useE2EAPI = static_cast<bool>(state.range(1));

    // Build histogram.
    auto ceHist = stats::createCEHistogram(data[0], numberOfBuckets);

    tassert(9787300, "queryIntervals should have at least one interval", queryIntervals.size() > 0);
    size_t i = 0;
    for (auto curState : state) {
        benchmark::DoNotOptimize(runSingleQuery(queryConfig.queryTypes[0],
                                                queryIntervals[i].first,
                                                queryIntervals[i].second,
                                                ceHist,
                                                /*includeScalar*/ true,
                                                ArrayRangeEstimationAlgo::kConjunctArrayCE,
                                                /*useE2EAPI*/ useE2EAPI,
                                                dataConfig.size));
        i = (i + 1) % queryIntervals.size();
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_CreateHistogram)
    ->ArgNames({"numberOfBuckets", "size", "dataDistribution", "dataType", "useE2EAPI"})
    ->ArgsProduct({/*numberOfBuckets*/ {10, 100, 300},
                   /*size*/ {50'000, 100'000},
                   /*dataDistribution*/
                   {static_cast<int>(stats::DistrType::kUniform),
                    static_cast<int>(stats::DistrType::kNormal),
                    static_cast<int>(stats::DistrType::kZipfian)},
                   /*dataType*/
                   {static_cast<int>(sbe::value::TypeTags::NumberInt64),
                    static_cast<int>(sbe::value::TypeTags::StringBig),
                    static_cast<int>(sbe::value::TypeTags::Boolean),
                    static_cast<int>(sbe::value::TypeTags::Null),
                    // kNan,
                    static_cast<int>(sbe::value::TypeTags::Array)},
                   /*useE2EAPI*/ {0}});

BENCHMARK(BM_RunHistogramEstimations)
    ->ArgNames(
        {"numberOfBuckets", "size", "dataDistribution", "dataType", "useE2EAPI", "queryType"})
    ->ArgsProduct({/*numberOfBuckets*/ {10, 100, 300},
                   /*size*/ {50'000},
                   /*dataDistribution*/ {static_cast<int>(stats::DistrType::kUniform)},
                   /*dataType*/
                   {static_cast<int>(sbe::value::TypeTags::NumberInt64),
                    static_cast<int>(sbe::value::TypeTags::StringSmall),
                    static_cast<int>(sbe::value::TypeTags::StringBig),
                    static_cast<int>(sbe::value::TypeTags::Boolean),
                    static_cast<int>(sbe::value::TypeTags::Null),
                    // kNan,
                    static_cast<int>(sbe::value::TypeTags::Array)},
                   /*useE2EAPI*/ {0, 1},
                   /*queryType*/ {kPoint}});

BENCHMARK(BM_RunHistogramEstimations)
    ->ArgNames(
        {"numberOfBuckets", "size", "dataDistribution", "dataType", "useE2EAPI", "queryType"})
    ->ArgsProduct({
        /*numberOfBuckets*/ {10, 100, 300},
        /*size*/ {50'000},
        /*dataDistribution*/ {static_cast<int>(stats::DistrType::kUniform)},
        /*dataType*/
        {static_cast<int>(sbe::value::TypeTags::NumberInt64),
         static_cast<int>(sbe::value::TypeTags::StringSmall),
         static_cast<int>(sbe::value::TypeTags::StringBig),
         static_cast<int>(sbe::value::TypeTags::Boolean),
         static_cast<int>(sbe::value::TypeTags::Array)},
        /*useE2EAPI*/ {0, 1},
        /*queryType*/ {kRange},
    });

}  // namespace mongo::ce

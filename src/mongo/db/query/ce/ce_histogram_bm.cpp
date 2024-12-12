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

#include <benchmark/benchmark.h>

#include "mongo/db/query/ce/histogram_accuracy_test_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::ce {

enum DataType { kInt, kStringSmall, kString, kDouble, kBoolean, kNull, kNan, kArray };

struct HistogramEstimationBenchmarkConfiguration {
    int numberOfBuckets;
    size_t size;
    DataDistributionEnum dataDistribution;
    DataType dataType;
    QueryType queryType;

    // Inclusive minimum and maximum bounds for randomly generated data, ensuring each data falls
    // within these limits.
    std::pair<size_t, size_t> dataInterval;
    sbe::value::TypeTags sbeDataType;
    double nanProb = 0;
    size_t arrayTypeLength = 0;
    bool useE2EAPI = false;

    HistogramEstimationBenchmarkConfiguration(benchmark::State& state)
        : numberOfBuckets(state.range(0)),
          size(state.range(1)),
          dataDistribution(static_cast<DataDistributionEnum>(state.range(2))),
          dataType(static_cast<DataType>(state.range(3))),
          queryType(static_cast<QueryType>(state.range(4))),
          useE2EAPI(static_cast<bool>(state.range(5))) {
        switch (dataType) {
            case kInt:
                sbeDataType = sbe::value::TypeTags::NumberInt64;
                dataInterval = {0, 1000};
                break;
            case kStringSmall:
                sbeDataType = sbe::value::TypeTags::StringSmall;
                dataInterval = {1, 8};
                break;
            case kString:
                sbeDataType = sbe::value::TypeTags::StringBig;
                dataInterval = {16, 32};
                break;
            case kDouble:
                sbeDataType = sbe::value::TypeTags::NumberDouble;
                dataInterval = {0, 1000};
                break;
            case kBoolean:
                sbeDataType = sbe::value::TypeTags::Boolean;
                dataInterval = {0, 2};
                break;
            case kNull:
                sbeDataType = sbe::value::TypeTags::Null;
                dataInterval = {0, 1000};
                break;
            case kNan:
                sbeDataType = sbe::value::TypeTags::NumberDouble;
                dataInterval = {0, 1000};
                nanProb = 1;
                break;
            case kArray:
                sbeDataType = sbe::value::TypeTags::Array;
                dataInterval = {0, 1000};
                arrayTypeLength = 10;
                break;
        }
    }
};

void BM_CreateHistogram(benchmark::State& state) {

    HistogramEstimationBenchmarkConfiguration configuration(state);

    const TypeCombination typeCombinationData{TypeCombination{{configuration.sbeDataType, 100}}};

    std::vector<stats::SBEValue> data;
    const size_t seed = 1724178214;

    auto ndv = (configuration.dataInterval.second - configuration.dataInterval.first) / 2;
    // Create one by one the values.
    switch (configuration.dataDistribution) {
        case kUniform:
            // For ndv we set half the number of values in the provided data interval.
            generateDataUniform(configuration.size,
                                configuration.dataInterval,
                                typeCombinationData,
                                seed,
                                ndv,
                                data,
                                configuration.arrayTypeLength);
            break;
        case kNormal:
            // For ndv we set half the number of values in the provided data interval.
            generateDataNormal(configuration.size,
                               configuration.dataInterval,
                               typeCombinationData,
                               seed,
                               ndv,
                               data,
                               configuration.arrayTypeLength);
            break;
        case kZipfian:
            // For ndv we set half the number of values in the provided data interval.
            generateDataZipfian(configuration.size,
                                configuration.dataInterval,
                                typeCombinationData,
                                seed,
                                ndv,
                                data,
                                configuration.arrayTypeLength);
            break;
    }

    for (auto curState : state) {
        // Built histogram.
        auto ceHist = stats::createCEHistogram(data, configuration.numberOfBuckets);
    }
}

void BM_RunHistogramEstimations(benchmark::State& state) {

    HistogramEstimationBenchmarkConfiguration configuration(state);

    const TypeCombination typeCombinationData{
        TypeCombination{{configuration.sbeDataType, 100, configuration.nanProb}}};

    std::vector<stats::SBEValue> data;
    const size_t seed = 1724178214;
    const int numberOfQueries = 100;

    auto ndv = (configuration.dataInterval.second - configuration.dataInterval.first) / 2;

    // Create one by one the values.
    switch (configuration.dataDistribution) {
        case kUniform:
            // For ndv we set half the number of values in the provided data interval.
            generateDataUniform(configuration.size,
                                configuration.dataInterval,
                                typeCombinationData,
                                seed,
                                ndv,
                                data,
                                configuration.arrayTypeLength);
            break;
        case kNormal:
            // For ndv we set half the number of values in the provided data interval.
            generateDataNormal(configuration.size,
                               configuration.dataInterval,
                               typeCombinationData,
                               seed,
                               ndv,
                               data,
                               configuration.arrayTypeLength);
            break;
        case kZipfian:
            // For ndv we set half the number of values in the provided data interval.
            generateDataZipfian(configuration.size,
                                configuration.dataInterval,
                                typeCombinationData,
                                seed,
                                ndv,
                                data,
                                configuration.arrayTypeLength);
            break;
    }

    // Build histogram.
    auto ceHist = stats::createCEHistogram(data, configuration.numberOfBuckets);

    TypeProbability typeCombinationQuery{configuration.sbeDataType, 100, configuration.nanProb};
    if (configuration.dataType == kArray) {
        // The array data generation currently only supports integer elements as implemented in
        // populateTypeDistrVectorAccordingToInputConfig.
        typeCombinationQuery.typeTag = sbe::value::TypeTags::NumberInt64;
    }

    auto queryIntervals = generateIntervals(configuration.queryType,
                                            configuration.dataInterval,
                                            numberOfQueries,
                                            typeCombinationQuery,
                                            seed);
    tassert(9787300, "queryIntervals should have at least one interval", queryIntervals.size() > 0);
    size_t i = 0;
    for (auto curState : state) {
        benchmark::DoNotOptimize(runSingleQuery(configuration.queryType,
                                                queryIntervals[i].first,
                                                queryIntervals[i].second,
                                                ceHist,
                                                true /*includeScalar*/,
                                                ArrayRangeEstimationAlgo::kConjunctArrayCE,
                                                configuration.useE2EAPI /*useE2EAPI*/,
                                                seed));
        i = (i + 1) % queryIntervals.size();
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_CreateHistogram)
    ->ArgNames({"buckets", "size", "distrib", "dataType", "query", "useE2EAPI"})
    ->ArgsProduct({/*numberOfBuckets*/ {10, 100, 300},
                   /*size*/ {50'000, 100'000},
                   /*dataDistribution*/ {kUniform, kNormal, kZipfian},
                   /*dataType*/ {kInt, kString, kBoolean, kNull, kNan, kArray},
                   /*queryType*/ {kPoint},
                   /*useE2EAPI*/ {0}});

BENCHMARK(BM_RunHistogramEstimations)
    ->ArgNames({"buckets", "size", "distrib", "dataType", "query", "useE2EAPI"})
    ->ArgsProduct({/*numberOfBuckets*/ {10, 100, 300},
                   /*size*/ {50'000},
                   /*dataDistribution*/ {kUniform},
                   /*dataType*/
                   {kInt, kStringSmall, kString, kBoolean, kNull, kNan, kArray},
                   /*queryType*/ {kPoint},
                   /*useE2EAPI*/ {0, 1}});

BENCHMARK(BM_RunHistogramEstimations)
    ->ArgNames({"buckets", "size", "distrib", "dataType", "query", "useE2EAPI"})
    ->ArgsProduct({/*numberOfBuckets*/ {10, 100, 300},
                   /*size*/ {50'000},
                   /*dataDistribution*/ {kUniform},
                   /*dataType*/ {kInt, kStringSmall, kString, kBoolean, kArray},
                   /*queryType*/ {kRange},
                   /*useE2EAPI*/ {0, 1}});

}  // namespace mongo::ce

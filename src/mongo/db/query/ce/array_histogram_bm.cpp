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
#include <vector>

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/array_histogram_helpers.h"
#include "mongo/db/query/ce/histogram_accuracy_test_utils.h"
#include "mongo/db/query/ce/histogram_common.h"
#include "mongo/db/query/ce/histogram_predicate_estimation.h"
#include "mongo/db/query/ce/test_utils.h"
#include "mongo/db/query/stats/array_histogram.h"
#include "mongo/db/query/stats/max_diff.h"
#include "mongo/db/query/stats/maxdiff_test_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::ce {

enum DataType { kInt, kStringSmall, kString, kDouble };

using mongo::stats::TypeCounts;
using TypeProbability = std::pair<sbe::value::TypeTags, size_t>;
using TypeCombination = std::vector<TypeProbability>;

struct HistogramEstimationBenchmarkConfiguration {
    int numberOfBuckets;
    size_t size;
    DataDistributionEnum dataDistribution;
    DataType dataType;
    QueryType queryType;
    std::pair<size_t, size_t> dataInterval;
    sbe::value::TypeTags sbeDataType;

    HistogramEstimationBenchmarkConfiguration(benchmark::State& state)
        : numberOfBuckets(state.range(0)),
          size(state.range(1)),
          dataDistribution(static_cast<DataDistributionEnum>(state.range(2))),
          dataType(static_cast<DataType>(state.range(3))),
          queryType(static_cast<QueryType>(state.range(4))) {
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
                dataInterval = {8, 15};
                break;
            case kDouble:
                sbeDataType = sbe::value::TypeTags::NumberDouble;
                dataInterval = {0, 1000};
                break;
        }
    }
};

void BM_CreateHistogram(benchmark::State& state) {

    HistogramEstimationBenchmarkConfiguration configuration(state);

    const TypeCombination typeCombinationData{TypeCombination{{configuration.sbeDataType, 100}}};

    std::vector<stats::SBEValue> data;
    const size_t seed = 1724178214;
    TypeCounts typeCounts;

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
                                data);
            break;
        case kNormal:
            // For ndv we set half the number of values in the provided data interval.
            generateDataNormal(configuration.size,
                               configuration.dataInterval,
                               typeCombinationData,
                               seed,
                               ndv,
                               data);
            break;
        case kZipfian:
            // For ndv we set half the number of values in the provided data interval.
            generateDataZipfian(configuration.size,
                                configuration.dataInterval,
                                typeCombinationData,
                                seed,
                                ndv,
                                data);
            break;
    }

    for (auto curState : state) {
        // Built histogram.
        auto arrHist = stats::createArrayEstimator(data, configuration.numberOfBuckets);
    }
}

void BM_RunHistogramEstimations(benchmark::State& state) {

    HistogramEstimationBenchmarkConfiguration configuration(state);

    const TypeCombination typeCombinationData{TypeCombination{{configuration.sbeDataType, 100}}};

    std::vector<stats::SBEValue> data;
    const size_t seed = 1724178214;
    TypeCounts typeCounts;
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
                                data);
            break;
        case kNormal:
            // For ndv we set half the number of values in the provided data interval.
            generateDataNormal(configuration.size,
                               configuration.dataInterval,
                               typeCombinationData,
                               seed,
                               ndv,
                               data);
            break;
        case kZipfian:
            // For ndv we set half the number of values in the provided data interval.
            generateDataZipfian(configuration.size,
                                configuration.dataInterval,
                                typeCombinationData,
                                seed,
                                ndv,
                                data);
            break;
    }

    // Build histogram.
    auto arrHist = stats::createArrayEstimator(data, configuration.numberOfBuckets);

    TypeProbability typeCombinationQuery{configuration.sbeDataType, 100};

    for (auto curState : state) {
        runQueries(configuration.size,
                   numberOfQueries,
                   configuration.queryType,
                   configuration.dataInterval,
                   typeCombinationQuery,
                   data,
                   arrHist,
                   true /*includeScalar*/,
                   false /*useE2EAPI*/,
                   seed);
    }
}


BENCHMARK(BM_CreateHistogram)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kUniform,
            /*dataType*/ kInt,
            /*queryType*/ kPoint});

BENCHMARK(BM_CreateHistogram)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kNormal,
            /*dataType*/ kInt,
            /*queryType*/ kPoint});

BENCHMARK(BM_CreateHistogram)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kZipfian,
            /*dataType*/ kInt,
            /*queryType*/ kPoint});

BENCHMARK(BM_CreateHistogram)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kUniform,
            /*dataType*/ kString,
            /*queryType*/ kPoint});

BENCHMARK(BM_CreateHistogram)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kNormal,
            /*dataType*/ kString,
            /*queryType*/ kPoint});

BENCHMARK(BM_CreateHistogram)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kZipfian,
            /*dataType*/ kString,
            /*queryType*/ kPoint});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kUniform,
            /*dataType*/ kInt,
            /*queryType*/ kPoint});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kUniform,
            /*dataType*/ kString,
            /*queryType*/ kPoint});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kUniform,
            /*dataType*/ kInt,
            /*queryType*/ kRange});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kUniform,
            /*dataType*/ kString,
            /*queryType*/ kRange});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kNormal,
            /*dataType*/ kInt,
            /*queryType*/ kPoint});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kNormal,
            /*dataType*/ kString,
            /*queryType*/ kPoint});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kNormal,
            /*dataType*/ kInt,
            /*queryType*/ kRange});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kNormal,
            /*dataType*/ kString,
            /*queryType*/ kRange});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kZipfian,
            /*dataType*/ kInt,
            /*queryType*/ kPoint});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kZipfian,
            /*dataType*/ kString,
            /*queryType*/ kPoint});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kZipfian,
            /*dataType*/ kInt,
            /*queryType*/ kRange});

BENCHMARK(BM_RunHistogramEstimations)
    ->Args({/*numberOfBuckets*/ 10,
            /*size*/ 50000,
            /*dataDistribution*/ kZipfian,
            /*dataType*/ kString,
            /*queryType*/ kRange});

}  // namespace mongo::ce

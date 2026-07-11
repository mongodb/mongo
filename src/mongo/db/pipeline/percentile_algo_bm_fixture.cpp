// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/percentile_algo_bm_fixture.h"

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/percentile_algo_accurate.h"
#include "mongo/db/pipeline/percentile_algo_tdigest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <random>

#include <benchmark/benchmark.h>
#include <boost/random/normal_distribution.hpp>

namespace mongo {
using std::vector;

// We'd like to test with "realistic" data so that tdigest has to do sorting and merging on a
// regular basis. The particular distribution of data shouldn't matter much.
vector<double> generateNormal(size_t n, bool presorted) {
    std::mt19937 generator(2023u);
    boost::random::normal_distribution<double> dist(0.0 /* mean */, 1.0 /* sigma */);

    vector<double> inputs;
    inputs.reserve(n);
    for (size_t i = 0; i < n; i++) {
        inputs.push_back(dist(generator));
    }

    if (presorted) {
        std::sort(inputs.begin(), inputs.end());
    }
    return inputs;
}

void PercentileAlgoBenchmarkFixture::tdigest_normalData(benchmark::State& state,
                                                        TDigest::ScalingFunction k_limit,
                                                        double delta,
                                                        int dataSize,
                                                        bool presorted,
                                                        const std::vector<double>& ps) {
    const vector<double> inputs = generateNormal(dataSize, presorted);

    for (auto keepRunning : state) {
        auto d = std::make_unique<TDigest>(k_limit, delta);
        d->incorporate(inputs);
        benchmark::DoNotOptimize(d->computePercentiles(ps));
        benchmark::ClobberMemory();
    }
}

void PercentileAlgoBenchmarkFixture::discrete_normalData(benchmark::State& state,
                                                         int dataSize,
                                                         bool presorted,
                                                         const std::vector<double>& ps) {
    const vector<double> inputs = generateNormal(dataSize, presorted);
    for (auto keepRunning : state) {
        auto expCtx = ExpressionContextForTest();
        auto d = createDiscretePercentile(&expCtx);
        d->incorporate(inputs);
        benchmark::DoNotOptimize(d->computePercentiles(ps));
        benchmark::ClobberMemory();
    }
}

void PercentileAlgoBenchmarkFixture::tdigest_normalData_batched(benchmark::State& state,
                                                                TDigest::ScalingFunction k_limit,
                                                                double delta) {
    const vector<double> inputs = generateNormal(nLarge, false /* presorted */);

    for (auto keepRunning : state) {
        auto d = std::make_unique<TDigest>(k_limit, delta);
        vector<double> batch;
        batch.reserve(delta);
        for (double input : inputs) {
            if (batch.size() == 5 * delta) {
                d->incorporate(batch);
                batch.clear();
            }
            batch.push_back(input);
        }
        if (!batch.empty()) {
            d->incorporate(batch);
        }
        benchmark::DoNotOptimize(d->computePercentile(0.5));
        benchmark::ClobberMemory();
    }
}

BENCHMARK_PERCENTILE_ALGO(PercentileAlgoBenchmarkFixture);
}  // namespace mongo

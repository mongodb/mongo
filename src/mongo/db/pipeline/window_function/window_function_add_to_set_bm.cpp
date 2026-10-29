// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_add_to_set.h"

#include "mongo/db/pipeline/expression_context_for_test.h"

#include <random>

#include <benchmark/benchmark.h>
#include <boost/random/normal_distribution.hpp>

namespace mongo {
namespace {

std::vector<Value> generateData(size_t numValues) {
    std::mt19937 generator{2025};
    boost::random::normal_distribution<double> dist{145.0, 30.0};

    std::vector<Value> data;
    data.reserve(numValues);
    for (size_t i = 0; i < numValues; ++i) {
        data.push_back(Value{dist(generator)});
    }

    return data;
}

static void runWindowFunctionAddToSet(benchmark::State& state) {
    const size_t dataSize = state.range(0);
    const size_t windowSize = state.range(1);
    std::vector<Value> data = generateData(state.range(0));
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto windowFunction = WindowFunctionAddToSet::create(expCtx.get());

    for (auto keepRunning : state) {
        for (size_t i = 0; i < dataSize; ++i) {
            if (i > windowSize) {
                windowFunction->remove(data[i - windowSize]);
            }

            windowFunction->add(data[i]);
        }

        state.PauseTiming();
        windowFunction->reset();
        state.ResumeTiming();
        benchmark::ClobberMemory();
    }
}

BENCHMARK(runWindowFunctionAddToSet)
    ->ArgPair(200 /* data size */, 50 /* window size */)
    ->ArgPair(2000 /* data size */, 500 /* window size */);

}  // namespace
}  // namespace mongo

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

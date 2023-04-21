/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/random/normal_distribution.hpp>

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/window_function/window_function_percentile_bm_fixture.h"

namespace mongo {
using std::vector;
vector<double> generateNormalData(size_t n) {
    std::mt19937 generator(curTimeMillis64());
    boost::random::normal_distribution<double> dist(0.0 /* mean */, 1.0 /* sigma */);

    vector<double> inputs;
    inputs.reserve(n);
    for (size_t i = 0; i < n; i++) {
        inputs.push_back(dist(generator));
    }

    return inputs;
}

// This benchmark is mimicking the behavior of computing $percentile for a [0, unbounded]
// window. In a [0, unbounded] window the first window will add all of the inputs in the window
// function. Then for each following window, the element before the current element will be removed
// and the percentile will be recalculated.
void WindowFunctionPercentileBenchmarkFixture::removable_unbounded_percentile(
    benchmark::State& state, std::vector<double> ps) {
    // Generate the data.
    const vector<double> inputs = generateNormalData(dataSizeLarge);
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Run the test.
    for (auto keepRunning : state) {
        auto w = WindowFunctionPercentile::create(expCtx.get(), ps);

        // Calculate the percentile for a [0, unbounded] window for each input.
        for (size_t i = 0; i < dataSizeLarge; i++) {
            // All of the values are in the first window.
            if (i == 0) {
                for (double input : inputs) {
                    w->add(Value(input));
                }
                benchmark::DoNotOptimize(w->getValue());
            } else {
                // Remove the previous value for the next window.
                double valToRemove = inputs[i - 1];
                w->remove(Value(valToRemove));
                benchmark::DoNotOptimize(w->getValue());
            }
        }
        benchmark::ClobberMemory();
    }
}

// This benchmark is mimicking the behavior of computing $percentile for a ["current", 100]
// window. In a ["current", 100] window, the first window will add itself and the next 100 elements
// in 'inputs' to the window function. Then for each following window, the previous current element
// will be removed, and a new element (100 indexes away from the new current element) will be added.
// Then the percentile will be recalculated. We will not add any elements if the index is out of
// bounds, resulting in smaller windows towards the end of 'inputs'.
void WindowFunctionPercentileBenchmarkFixture::removable_bounded_percentile(
    benchmark::State& state, std::vector<double> ps) {
    // Generate the data.
    const vector<double> inputs = generateNormalData(dataSizeLarge);
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Run the test.
    for (auto keepRunning : state) {
        auto w = WindowFunctionPercentile::create(expCtx.get(), ps);

        // Calculate the percentile for a ["current", 100] window for each input.
        for (size_t i = 0; i < dataSizeLarge; i++) {
            // Add the first value and the next 100 to the window.
            if (i == 0) {
                for (size_t j = 0; j < 101; j++) {
                    w->add(Value(inputs[j]));
                }
                benchmark::DoNotOptimize(w->getValue());
            } else {
                // Remove the previous current value.
                double valToRemove = inputs[i - 1];
                w->remove(Value(valToRemove));
                // If possible, add the new value.
                if (i + 100 < dataSizeLarge - 1) {
                    w->add(Value(inputs[i + 100]));
                }
                benchmark::DoNotOptimize(w->getValue());
            }
        }
        benchmark::ClobberMemory();
    }
}

BENCHMARK_WINDOW_PERCENTILE(WindowFunctionPercentileBenchmarkFixture);
}  // namespace mongo

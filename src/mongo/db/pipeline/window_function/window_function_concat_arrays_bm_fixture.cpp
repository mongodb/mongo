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

#include "mongo/db/pipeline/window_function/window_function_concat_arrays_bm_fixture.h"

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/window_function/window_function_concat_arrays.h"

namespace mongo {

std::vector<std::vector<Value>> generateData(size_t n_arrays, size_t n_values_per_array) {

    return std::vector<std::vector<Value>>(
        n_arrays, std::vector<Value>(n_values_per_array, Value(123'456'789)));
}

// This benchmark is mimicking the behavior of computing $concatArrays for a [unbounded, unbounded]
// window. In this window, all of the inputs will be added, and nothing will be removed.
void WindowFunctionConcatArraysBenchmarkFixture::no_removal_concat_arrays(benchmark::State& state,
                                                                          int n_documents,
                                                                          int n_vals_per_array) {
    // Generate the data
    const std::vector<std::vector<Value>> inputs = generateData(n_documents, n_vals_per_array);
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Run the test
    for (auto keepRunning : state) {
        auto w = WindowFunctionConcatArrays::create(expCtx.get());

        // All of the values are added in the first window.
        for (auto& inputArray : inputs) {
            w->add(Value{inputArray});
        }

        // We get the value for each document produced.
        for (int i = 0; i < n_documents; ++i) {
            benchmark::DoNotOptimize(w->getValue());
        }

        benchmark::ClobberMemory();
    }
}

// This benchmark is mimicking the behavior of computing $concatArrays for a [0, unbounded] window.
// The first window will add all of the inputs, then for each following window, the element before
// the current element will be removed.
void WindowFunctionConcatArraysBenchmarkFixture::removable_unbounded_concat_arrays(
    benchmark::State& state, int n_documents, int n_vals_per_array) {
    // Generate the data
    const std::vector<std::vector<Value>> inputs = generateData(n_documents, n_vals_per_array);
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Run the test
    for (auto keepRunning : state) {
        auto w = WindowFunctionConcatArrays::create(expCtx.get());

        // All of the values are in the first window.
        for (auto& inputArray : inputs) {
            w->add(Value{inputArray});
        }
        benchmark::DoNotOptimize(w->getValue());

        // Each window after the first removes the previous value.
        for (auto itr = inputs.begin(); itr + 1 != inputs.end(); ++itr) {
            w->remove(Value{*itr});
            benchmark::DoNotOptimize(w->getValue());
        }

        benchmark::ClobberMemory();
    }
}

// This benchmark is mimicking the behavior of computing $concatArrays for a ["current", 100]
// window. The first window will add itself and the next 100 elements. Then, for each following
// window, the oldest element will be removed and a new element will be added.
void WindowFunctionConcatArraysBenchmarkFixture::removable_bounded_concat_arrays(
    benchmark::State& state, int n_documents, int n_vals_per_array) {
    // Generate the data
    const std::vector<std::vector<Value>> inputs = generateData(n_documents, n_vals_per_array);
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Run the test
    for (auto keepRunning : state) {
        auto w = WindowFunctionConcatArrays::create(expCtx.get());

        // The first window will add the first value and the next 100 values.
        for (auto itr = inputs.begin(); itr != inputs.end() && itr != inputs.begin() + 101; ++itr) {
            w->add(Value{*itr});
        }
        benchmark::DoNotOptimize(w->getValue());

        // Each window after the first removes the previous value and adds the new value.
        for (int i = 1; i < n_documents; ++i) {
            w->remove(Value{inputs[i - 1]});
            // If possible, add the new value.
            if (i + 100 < n_documents) {
                w->add(Value{inputs[i + 100]});
            }
            benchmark::DoNotOptimize(w->getValue());
        }

        benchmark::ClobberMemory();
    }
}

// This benchmark is mimicking the behavior of computing $concatArrays for a ["current", 4]
// window. The first window will add itself and the next 4 elements. Then, for each following
// window, the oldest element will be removed and a new element will be added.
void WindowFunctionConcatArraysBenchmarkFixture::five_value_concat_arrays(benchmark::State& state,
                                                                          int n_documents,
                                                                          int n_vals_per_array) {
    // Generate the data
    const std::vector<std::vector<Value>> inputs = generateData(n_documents, n_vals_per_array);
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Run the test
    for (auto keepRunning : state) {
        auto w = WindowFunctionConcatArrays::create(expCtx.get());

        // The first window will add the first value and the next 100 values.
        for (int i = 0; i < 5; ++i) {
            w->add(Value{inputs[i]});
        }
        benchmark::DoNotOptimize(w->getValue());

        // Each window after the first removes the previous value and adds the new value.
        for (int i = 1; i < n_documents; ++i) {
            w->remove(Value{inputs[i - 1]});
            // If possible, add the new value.
            if (i + 4 < n_documents) {
                w->add(Value{inputs[i + 4]});
            }
            benchmark::DoNotOptimize(w->getValue());
        }

        benchmark::ClobberMemory();
    }
}


// This benchmark mimics the case where only one array is in the window at a time (i.e. we add and
// remove from every single window).
void WindowFunctionConcatArraysBenchmarkFixture::only_one_value_concat_arrays(
    benchmark::State& state, int n_documents, int n_vals_per_array) {
    // Generate the data
    const std::vector<std::vector<Value>> inputs = generateData(n_documents, n_vals_per_array);
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    // Run the test
    for (auto keepRunning : state) {
        auto w = WindowFunctionConcatArrays::create(expCtx.get());

        // The first window will add the first value.
        w->add(Value{inputs[0]});
        benchmark::DoNotOptimize(w->getValue());

        // Each window after the first removes the previous value and adds the new value.
        for (int i = 1; i < n_documents; ++i) {
            w->remove(Value{inputs[i - 1]});
            w->add(Value{inputs[i]});
            benchmark::DoNotOptimize(w->getValue());
        }

        benchmark::ClobberMemory();
    }
}

BENCHMARK_WINDOW_CONCAT_ARRAYS(WindowFunctionConcatArraysBenchmarkFixture);
}  // namespace mongo

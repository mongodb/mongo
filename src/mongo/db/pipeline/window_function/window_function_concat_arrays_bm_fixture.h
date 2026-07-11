// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/window_function/window_function_concat_arrays.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/util/modules.h"

#include <benchmark/benchmark.h>

namespace mongo {

class WindowFunctionConcatArraysBenchmarkFixture : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) final {
        QueryFCVEnvironmentForTest::setUp();
    }

    void no_removal_concat_arrays(benchmark::State& state, int n_documents, int n_vals_per_array);
    void removable_unbounded_concat_arrays(benchmark::State& state,
                                           int n_documents,
                                           int n_vals_per_array);
    void removable_bounded_concat_arrays(benchmark::State& state,
                                         int n_documents,
                                         int n_vals_per_array);
    void five_value_concat_arrays(benchmark::State& state, int n_documents, int n_vals_per_array);
    void only_one_value_concat_arrays(benchmark::State& state,
                                      int n_documents,
                                      int n_vals_per_array);
};

#define BENCHMARK_WINDOW_CONCAT_ARRAYS(Fixture)                                                   \
    BENCHMARK_F(Fixture, no_removal_concat_arrays_1000x10)(benchmark::State & state) {            \
        no_removal_concat_arrays(state, 1'000, 10);                                               \
    }                                                                                             \
    BENCHMARK_F(Fixture, no_removal_concat_arrays_1000x100)(benchmark::State & state) {           \
        no_removal_concat_arrays(state, 1'000, 100);                                              \
    }                                                                                             \
    BENCHMARK_F(Fixture, no_removal_concat_arrays_1000x1000)(benchmark::State & state) {          \
        no_removal_concat_arrays(state, 1'000, 1'000);                                            \
    }                                                                                             \
                                                                                                  \
    BENCHMARK_F(Fixture, removable_unbounded_concat_arrays_1000x10)(benchmark::State & state) {   \
        removable_unbounded_concat_arrays(state, 1'000, 10);                                      \
    }                                                                                             \
    BENCHMARK_F(Fixture, removable_unbounded_concat_arrays_1000x100)(benchmark::State & state) {  \
        removable_unbounded_concat_arrays(state, 1'000, 100);                                     \
    }                                                                                             \
    BENCHMARK_F(Fixture, removable_unbounded_concat_arrays_1000x1000)(benchmark::State & state) { \
        removable_unbounded_concat_arrays(state, 1'000, 1'000);                                   \
    }                                                                                             \
                                                                                                  \
    BENCHMARK_F(Fixture, removable_bounded_concat_arrays_1000x10)(benchmark::State & state) {     \
        removable_bounded_concat_arrays(state, 1'000, 10);                                        \
    }                                                                                             \
    BENCHMARK_F(Fixture, removable_bounded_concat_arrays_1000x100)(benchmark::State & state) {    \
        removable_bounded_concat_arrays(state, 1'000, 100);                                       \
    }                                                                                             \
    BENCHMARK_F(Fixture, removable_bounded_concat_arrays_1000x1000)(benchmark::State & state) {   \
        removable_bounded_concat_arrays(state, 1'000, 1'000);                                     \
    }                                                                                             \
                                                                                                  \
    BENCHMARK_F(Fixture, five_value_concat_arrays_1000x10)(benchmark::State & state) {            \
        five_value_concat_arrays(state, 1'000, 10);                                               \
    }                                                                                             \
    BENCHMARK_F(Fixture, five_value_concat_arrays_1000x100)(benchmark::State & state) {           \
        five_value_concat_arrays(state, 1'000, 100);                                              \
    }                                                                                             \
    BENCHMARK_F(Fixture, five_value_concat_arrays_1000x1000)(benchmark::State & state) {          \
        five_value_concat_arrays(state, 1'000, 1'000);                                            \
    }                                                                                             \
                                                                                                  \
    BENCHMARK_F(Fixture, only_one_value_concat_arrays_1000x10)(benchmark::State & state) {        \
        only_one_value_concat_arrays(state, 1'000, 10);                                           \
    }                                                                                             \
    BENCHMARK_F(Fixture, only_one_value_concat_arrays_1000x100)(benchmark::State & state) {       \
        only_one_value_concat_arrays(state, 1'000, 100);                                          \
    }                                                                                             \
    BENCHMARK_F(Fixture, only_one_value_concat_arrays_1000x1000)(benchmark::State & state) {      \
        only_one_value_concat_arrays(state, 1'000, 1'000);                                        \
    }

}  // namespace mongo

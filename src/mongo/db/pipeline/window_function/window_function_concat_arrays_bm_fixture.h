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

#pragma once

#include "mongo/db/pipeline/window_function/window_function_concat_arrays.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"

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

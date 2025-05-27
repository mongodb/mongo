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
#pragma once
#include "mongo/db/pipeline/accumulator_percentile_enum_gen.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"

#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {

class WindowFunctionPercentileBenchmarkFixture : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) final {
        QueryFCVEnvironmentForTest::setUp();
    }

    void removable_unbounded_percentile(benchmark::State& state,
                                        PercentileMethodEnum method,
                                        std::vector<double> ps);
    void removable_bounded_percentile(benchmark::State& state,
                                      PercentileMethodEnum method,
                                      std::vector<double> ps);

    static constexpr int dataSizeLarge = 100'000;
};

#define BENCHMARK_WINDOW_PERCENTILE(Fixture)                                                \
                                                                                            \
    BENCHMARK_F(Fixture, percentile_unbounded_low_p)(benchmark::State & state) {            \
        removable_unbounded_percentile(state, PercentileMethodEnum::kApproximate, {0.001}); \
    }                                                                                       \
    BENCHMARK_F(Fixture, percentile_unbounded_high_p)(benchmark::State & state) {           \
        removable_unbounded_percentile(state, PercentileMethodEnum::kApproximate, {.999});  \
    }                                                                                       \
    BENCHMARK_F(Fixture, percentile_unbounded_mid_p)(benchmark::State & state) {            \
        removable_unbounded_percentile(state, PercentileMethodEnum::kApproximate, {.55});   \
    }                                                                                       \
    BENCHMARK_F(Fixture, percentile_unbounded_multi_p)(benchmark::State & state) {          \
        removable_unbounded_percentile(state,                                               \
                                       PercentileMethodEnum::kApproximate,                  \
                                       {.1, .47, .88, .05, .33, .999, .2, .59, .9, .7});    \
    }                                                                                       \
    BENCHMARK_F(Fixture, percentile_bounded_low_p)(benchmark::State & state) {              \
        removable_bounded_percentile(state, PercentileMethodEnum::kApproximate, {.001});    \
    }                                                                                       \
    BENCHMARK_F(Fixture, percentile_bounded_high_p)(benchmark::State & state) {             \
        removable_bounded_percentile(state, PercentileMethodEnum::kApproximate, {.999});    \
    }                                                                                       \
    BENCHMARK_F(Fixture, percentile_bounded_mid_p)(benchmark::State & state) {              \
        removable_bounded_percentile(state, PercentileMethodEnum::kApproximate, {.55});     \
    }                                                                                       \
    BENCHMARK_F(Fixture, percentile_bounded_multi_p)(benchmark::State & state) {            \
        removable_bounded_percentile(state,                                                 \
                                     PercentileMethodEnum::kApproximate,                    \
                                     {.1, .47, .88, .05, .33, .999, .2, .59, .9, .7});      \
    }

}  // namespace mongo

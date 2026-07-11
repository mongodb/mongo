// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/db/pipeline/accumulator_percentile_enum_gen.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/util/modules.h"

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

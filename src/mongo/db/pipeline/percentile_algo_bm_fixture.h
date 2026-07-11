// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/db/pipeline/percentile_algo_tdigest.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/util/modules.h"

#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {

class PercentileAlgoBenchmarkFixture : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) final {
        QueryFCVEnvironmentForTest::setUp();
    }

    void tdigest_normalData(benchmark::State& state,
                            TDigest::ScalingFunction k_limit,
                            double delta,
                            int dataSize,
                            bool presorted,
                            const std::vector<double>& ps);

    // This test assesses the overhead of sending to t-digest one input at a time vs batching them.
    void tdigest_normalData_batched(benchmark::State& state,
                                    TDigest::ScalingFunction k_limit,
                                    double delta);

    void discrete_normalData(benchmark::State& state,
                             int dataSize,
                             bool presorted,
                             const std::vector<double>& ps);

    static constexpr int nLarge = 10'000'000;
};

#define BENCHMARK_PERCENTILE_ALGO(Fixture)                                                         \
                                                                                                   \
    BENCHMARK_F(Fixture, tdigest_k0_delta1000)(benchmark::State & state) {                         \
        tdigest_normalData(state, TDigest::k0_limit, 1000, nLarge, false /*presorted*/, {0.5});    \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_k1_delta1000)(benchmark::State & state) {                         \
        tdigest_normalData(state, TDigest::k1_limit, 1000, nLarge, false /*presorted*/, {0.5});    \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta500)(benchmark::State & state) {                          \
        tdigest_normalData(state, TDigest::k2_limit, 500, nLarge, false /*presorted*/, {0.5});     \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta1000)(benchmark::State & state) {                         \
        tdigest_normalData(state, TDigest::k2_limit, 1000, nLarge, false /*presorted*/, {0.5});    \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta5000)(benchmark::State & state) {                         \
        tdigest_normalData(state, TDigest::k2_limit, 5000, nLarge, false /*presorted*/, {0.5});    \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta1000_sorted)(benchmark::State & state) {                  \
        tdigest_normalData(state, TDigest::k2_limit, 1000, nLarge, false /*presorted*/, {0.5});    \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta1000_batched)(benchmark::State & state) {                 \
        tdigest_normalData_batched(state, TDigest::k2_limit, 1000);                                \
    }                                                                                              \
                                                                                                   \
    BENCHMARK_F(Fixture, tdigest_mid_10)(benchmark::State & state) {                               \
        tdigest_normalData(                                                                        \
            state, TDigest::k2_limit, 1000 /*delta*/, 10, false /*presorted*/, {0.5});             \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_mid_100)(benchmark::State & state) {                              \
        tdigest_normalData(                                                                        \
            state, TDigest::k2_limit, 1000 /*delta*/, 100, false /*presorted*/, {0.5});            \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_mid_1000)(benchmark::State & state) {                             \
        tdigest_normalData(                                                                        \
            state, TDigest::k2_limit, 1000 /*delta*/, 1000, false /*presorted*/, {0.5});           \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_multi_1000)(benchmark::State & state) {                           \
        tdigest_normalData(state,                                                                  \
                           TDigest::k2_limit,                                                      \
                           1000 /*delta*/,                                                         \
                           1000,                                                                   \
                           false /*presorted*/,                                                    \
                           {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                   \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_mid_10000)(benchmark::State & state) {                            \
        tdigest_normalData(state, TDigest::k2_limit, 1000, 10000, false /*presorted*/, {0.5});     \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_multi_10000)(benchmark::State & state) {                          \
        tdigest_normalData(state,                                                                  \
                           TDigest::k2_limit,                                                      \
                           1000 /*delta*/,                                                         \
                           10000,                                                                  \
                           false /*presorted*/,                                                    \
                           {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                   \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_multi101_10000)(benchmark::State & state) {                       \
        tdigest_normalData(                                                                        \
            state,                                                                                 \
            TDigest::k2_limit,                                                                     \
            1000 /*delta*/,                                                                        \
            10000,                                                                                 \
            false /*presorted*/,                                                                   \
            {0,    0.01, 0.02, 0.03, 0.04, 0.05, 0.06, 0.07, 0.08, 0.09, 0.1,  0.11, 0.12,         \
             0.13, 0.14, 0.15, 0.16, 0.17, 0.18, 0.19, 0.2,  0.21, 0.22, 0.23, 0.24, 0.25,         \
             0.26, 0.27, 0.28, 0.29, 0.3,  0.31, 0.32, 0.33, 0.34, 0.35, 0.36, 0.37, 0.38,         \
             0.39, 0.4,  0.41, 0.42, 0.43, 0.44, 0.45, 0.46, 0.47, 0.48, 0.49, 0.5,  0.51,         \
             0.52, 0.53, 0.54, 0.55, 0.56, 0.57, 0.58, 0.59, 0.6,  0.61, 0.62, 0.63, 0.64,         \
             0.65, 0.66, 0.67, 0.68, 0.69, 0.7,  0.71, 0.72, 0.73, 0.74, 0.75, 0.76, 0.77,         \
             0.78, 0.79, 0.8,  0.81, 0.82, 0.83, 0.84, 0.85, 0.86, 0.87, 0.88, 0.89, 0.9,          \
             0.91, 0.92, 0.93, 0.94, 0.95, 0.96, 0.97, 0.98, 0.99, 1});                            \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_mid_100000)(benchmark::State & state) {                           \
        tdigest_normalData(                                                                        \
            state, TDigest::k2_limit, 1000 /*delta*/, 100'000, false /*presorted*/, {0.5});        \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_multi_100000)(benchmark::State & state) {                         \
        tdigest_normalData(state,                                                                  \
                           TDigest::k2_limit,                                                      \
                           1000 /*delta*/,                                                         \
                           100'000,                                                                \
                           false /*presorted*/,                                                    \
                           {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                   \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_mid_1000000)(benchmark::State & state) {                          \
        tdigest_normalData(                                                                        \
            state, TDigest::k2_limit, 1000 /*delta*/, 1'000'000, false /*presorted*/, {0.5});      \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_multi_1000000)(benchmark::State & state) {                        \
        tdigest_normalData(state,                                                                  \
                           TDigest::k2_limit,                                                      \
                           1000 /*delta*/,                                                         \
                           1'000'000,                                                              \
                           false /*presorted*/,                                                    \
                           {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                   \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_mid_10000000)(benchmark::State & state) {                         \
        tdigest_normalData(                                                                        \
            state, TDigest::k2_limit, 1000 /*delta*/, 10'000'000, false /*presorted*/, {0.5});     \
    }                                                                                              \
    BENCHMARK_F(Fixture, tdigest_multi_10000000)(benchmark::State & state) {                       \
        tdigest_normalData(state,                                                                  \
                           TDigest::k2_limit,                                                      \
                           1000 /*delta*/,                                                         \
                           10'000'000,                                                             \
                           false /*presorted*/,                                                    \
                           {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                   \
    }                                                                                              \
                                                                                                   \
    BENCHMARK_F(Fixture, discrete_mid_10)(benchmark::State & state) {                              \
        discrete_normalData(state, 10, false /*presorted*/, {0.05});                               \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_mid_100)(benchmark::State & state) {                             \
        discrete_normalData(state, 100, false /*presorted*/, {0.5});                               \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_multi_100)(benchmark::State & state) {                           \
        discrete_normalData(                                                                       \
            state, 100, false /*presorted*/, {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95}); \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_low_1000)(benchmark::State & state) {                            \
        discrete_normalData(state, 1000, false /*presorted*/, {0.01});                             \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_mid_1000)(benchmark::State & state) {                            \
        discrete_normalData(state, 1000, false /*presorted*/, {0.5});                              \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_mid_1000_presorted)(benchmark::State & state) {                  \
        discrete_normalData(state, 1000, true /*presorted*/, {0.99});                              \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_high_1000)(benchmark::State & state) {                           \
        discrete_normalData(state, 1000, false /*presorted*/, {0.99});                             \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_multi_1000)(benchmark::State & state) {                          \
        discrete_normalData(state,                                                                 \
                            1000,                                                                  \
                            false /*presorted*/,                                                   \
                            {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                  \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_low_10000)(benchmark::State & state) {                           \
        discrete_normalData(state, 10000, false /*presorted*/, {0.01});                            \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_mid_10000)(benchmark::State & state) {                           \
        discrete_normalData(state, 10000, false /*presorted*/, {0.5});                             \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_mid_10000_presorted)(benchmark::State & state) {                 \
        discrete_normalData(state, 10000, true /*presorted*/, {0.5});                              \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_high_10000)(benchmark::State & state) {                          \
        discrete_normalData(state, 10000, false /*presorted*/, {0.99});                            \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_multi_10000)(benchmark::State & state) {                         \
        discrete_normalData(state,                                                                 \
                            10000,                                                                 \
                            false /*presorted*/,                                                   \
                            {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                  \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_multi18_10000)(benchmark::State & state) {                       \
        discrete_normalData(state,                                                                 \
                            10000,                                                                 \
                            false /*presorted*/,                                                   \
                            {0,                                                                    \
                             0.001,                                                                \
                             0.005,                                                                \
                             0.01,                                                                 \
                             0.05,                                                                 \
                             0.1,                                                                  \
                             0.2,                                                                  \
                             0.3,                                                                  \
                             0.4,                                                                  \
                             0.5,                                                                  \
                             0.6,                                                                  \
                             0.7,                                                                  \
                             0.8,                                                                  \
                             0.9,                                                                  \
                             0.95,                                                                 \
                             0.99,                                                                 \
                             0.995,                                                                \
                             0.999});                                                              \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_multi51_10000)(benchmark::State & state) {                       \
        discrete_normalData(state,                                                                 \
                            10000,                                                                 \
                            false /*presorted*/,                                                   \
                            {0,    0.02, 0.04, 0.06, 0.08, 0.1,  0.12, 0.14, 0.16, 0.18, 0.2,      \
                             0.22, 0.24, 0.26, 0.28, 0.3,  0.32, 0.34, 0.36, 0.38, 0.4,  0.42,     \
                             0.44, 0.46, 0.48, 0.5,  0.52, 0.54, 0.56, 0.58, 0.6,  0.62, 0.64,     \
                             0.66, 0.68, 0.7,  0.72, 0.74, 0.76, 0.78, 0.8,  0.82, 0.84, 0.86,     \
                             0.88, 0.9,  0.92, 0.94, 0.96, 0.98, 1});                              \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_multi101_10000)(benchmark::State & state) {                      \
        discrete_normalData(                                                                       \
            state,                                                                                 \
            10000,                                                                                 \
            false /*presorted*/,                                                                   \
            {0,    0.01, 0.02, 0.03, 0.04, 0.05, 0.06, 0.07, 0.08, 0.09, 0.1,  0.11, 0.12,         \
             0.13, 0.14, 0.15, 0.16, 0.17, 0.18, 0.19, 0.2,  0.21, 0.22, 0.23, 0.24, 0.25,         \
             0.26, 0.27, 0.28, 0.29, 0.3,  0.31, 0.32, 0.33, 0.34, 0.35, 0.36, 0.37, 0.38,         \
             0.39, 0.4,  0.41, 0.42, 0.43, 0.44, 0.45, 0.46, 0.47, 0.48, 0.49, 0.5,  0.51,         \
             0.52, 0.53, 0.54, 0.55, 0.56, 0.57, 0.58, 0.59, 0.6,  0.61, 0.62, 0.63, 0.64,         \
             0.65, 0.66, 0.67, 0.68, 0.69, 0.7,  0.71, 0.72, 0.73, 0.74, 0.75, 0.76, 0.77,         \
             0.78, 0.79, 0.8,  0.81, 0.82, 0.83, 0.84, 0.85, 0.86, 0.87, 0.88, 0.89, 0.9,          \
             0.91, 0.92, 0.93, 0.94, 0.95, 0.96, 0.97, 0.98, 0.99, 1});                            \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_mid_100000)(benchmark::State & state) {                          \
        discrete_normalData(state, 100'000, false /*presorted*/, {0.5});                           \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_multi_100000)(benchmark::State & state) {                        \
        discrete_normalData(state,                                                                 \
                            100'000,                                                               \
                            false /*presorted*/,                                                   \
                            {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                  \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_mid_1000000)(benchmark::State & state) {                         \
        discrete_normalData(state, 1'000'000, false /*presorted*/, {0.5});                         \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_multi_1000000)(benchmark::State & state) {                       \
        discrete_normalData(state,                                                                 \
                            1'000'000,                                                             \
                            false /*presorted*/,                                                   \
                            {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                  \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_mid_10000000)(benchmark::State & state) {                        \
        discrete_normalData(state, 10'000'000, false /*presorted*/, {0.5});                        \
    }                                                                                              \
    BENCHMARK_F(Fixture, discrete_multi_10000000)(benchmark::State & state) {                      \
        discrete_normalData(state,                                                                 \
                            10'000'000,                                                            \
                            false /*presorted*/,                                                   \
                            {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95});                  \
    }

}  // namespace mongo

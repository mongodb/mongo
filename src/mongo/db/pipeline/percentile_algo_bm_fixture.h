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
#include "mongo/platform/basic.h"
#include <iostream>

#include <benchmark/benchmark.h>

#include "mongo/db/pipeline/percentile_algo_tdigest.h"

namespace mongo {

class PercentileAlgoBenchmarkFixture : public benchmark::Fixture {
public:
    void tdigest_normalData(benchmark::State& state,
                            TDigest::ScalingFunction k_limit,
                            double delta);

    // When already sorted data is fed into t-digest, it should be more accurate and also run
    // faster. This test allows to assess, how much faster.
    void tdigest_normalData_sorted(benchmark::State& state,
                                   TDigest::ScalingFunction k_limit,
                                   double delta);

    // This test assesses the overhead of sending to t-digest one input at a time vs batching them.
    void tdigest_normalData_batched(benchmark::State& state,
                                    TDigest::ScalingFunction k_limit,
                                    double delta);

    // Using small data size allows the benchmark to run many more times, imitating use of the
    // algorithm in an expression. The test assumes "batched" execution.
    void tdigest_normalData_expr(benchmark::State& state,
                                 TDigest::ScalingFunction k_limit,
                                 double delta,
                                 int dataSize,
                                 double p);
    void sortAndRank_normalData_expr(benchmark::State& state, int dataSize, double p);

    // To ensure comparability of benchmarks' results use the same data size unless explicitly
    // specified otherwise.
    static constexpr int dataSizeLarge = 1000000;
};

#define BENCHMARK_PERCENTILE_ALGO(Fixture)                                         \
                                                                                   \
    BENCHMARK_F(Fixture, tdigest_k0_delta1000)(benchmark::State & state) {         \
        tdigest_normalData(state, TDigest::k0_limit, 1000);                        \
    }                                                                              \
    BENCHMARK_F(Fixture, tdigest_k1_delta1000)(benchmark::State & state) {         \
        tdigest_normalData(state, TDigest::k1_limit, 1000);                        \
    }                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta500)(benchmark::State & state) {          \
        tdigest_normalData(state, TDigest::k2_limit, 500);                         \
    }                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta1000)(benchmark::State & state) {         \
        tdigest_normalData(state, TDigest::k2_limit, 1000);                        \
    }                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta5000)(benchmark::State & state) {         \
        tdigest_normalData(state, TDigest::k2_limit, 5000);                        \
    }                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta1000_sorted)(benchmark::State & state) {  \
        tdigest_normalData_sorted(state, TDigest::k2_limit, 1000);                 \
    }                                                                              \
    BENCHMARK_F(Fixture, tdigest_k2_delta1000_batched)(benchmark::State & state) { \
        tdigest_normalData_batched(state, TDigest::k2_limit, 1000);                \
    }                                                                              \
    BENCHMARK_F(Fixture, tdigest_expr_99_100)(benchmark::State & state) {          \
        tdigest_normalData_expr(state, TDigest::k2_limit, 1000, 100, 0.99);        \
    }                                                                              \
    BENCHMARK_F(Fixture, tdigest_expr_01_100)(benchmark::State & state) {          \
        tdigest_normalData_expr(state, TDigest::k2_limit, 1000, 100, 0.01);        \
    }                                                                              \
    BENCHMARK_F(Fixture, tdigest_expr_01_1000)(benchmark::State & state) {         \
        tdigest_normalData_expr(state, TDigest::k2_limit, 1000, 1000, 0.01);       \
    }                                                                              \
    BENCHMARK_F(Fixture, sortAndRank_expr_100)(benchmark::State & state) {         \
        sortAndRank_normalData_expr(state, 100, 0.01);                             \
    }                                                                              \
    BENCHMARK_F(Fixture, sortAndRank_expr_1000)(benchmark::State & state) {        \
        sortAndRank_normalData_expr(state, 1000, 0.01);                            \
    }

}  // namespace mongo

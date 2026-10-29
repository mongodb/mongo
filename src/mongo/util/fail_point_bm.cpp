// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/fail_point.h"

#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"

#include <string>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failPointBench);
Atomic<bool> atomic;

void BM_AtomicLoadAndBranch(benchmark::State& state) {
    atomic.store(!!state.range(0));
    for (auto _ : state) {
        if (MONGO_unlikely(atomic.load())) {
            benchmark::DoNotOptimize(0);
        }
    }
}
BENCHMARK(BM_AtomicLoadAndBranch)->Range(0, 1);

void BM_FailPointShouldFail(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state) {
        if (MONGO_unlikely(failPointBench.shouldFail())) {
            benchmark::DoNotOptimize(0);
        }
    }
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointShouldFail)->Range(0, 1);

void BM_FailPointExecute(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state)
        failPointBench.execute([](auto&&) { benchmark::DoNotOptimize(0); });
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointExecute)->Range(0, 1);

void BM_FailPointExecuteIf(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state)
        failPointBench.executeIf([](auto&&) { benchmark::DoNotOptimize(0); },
                                 [](auto&&) {
                                     bool b;
                                     benchmark::DoNotOptimize(b);
                                     return b;
                                 });
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointExecuteIf)->Range(0, 1);

void BM_FailPointScoped(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state) {
        auto scoped = failPointBench.scoped();
        if (MONGO_unlikely(scoped.isActive()))
            benchmark::DoNotOptimize(0);
    }
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointScoped)->Range(0, 1);

void BM_FailPointScopedIf(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state) {
        auto scoped = failPointBench.scopedIf([](auto&&) { return false; });
        if (MONGO_unlikely(scoped.isActive()))
            benchmark::DoNotOptimize(0);
    }
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointScopedIf)->Range(0, 1);

}  // namespace
}  // namespace mongo

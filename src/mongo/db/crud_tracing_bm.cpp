// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/crud_bm_fixture.h"
#include "mongo/otel/traces/benchmark_tracing.h"

namespace mongo {
namespace {

using otel::traces::BenchmarkTracingSession;
using otel::traces::kSmallestSamplingFactor;
using otel::traces::kUnlimitedRateLimit;
using otel::traces::SamplingParameters;

const std::vector<std::string> kTracedSpanNames = {"find", "insert", "aggregate"};

/**
 * Represents the worst realistic cost of an untraced command on mongod, i.e. the gap this fixture
 * reports from CrudBenchmarkFixture is overhead tracing readiness adds to a command that isn't
 * actually traced, and should be driven toward zero over time rather than treated as a necessary
 * cost.
 */
class CrudTraceMissBenchmarkFixture : public CrudBenchmarkFixture {
public:
    std::unique_ptr<BenchmarkTracingSession> setupTracingSession() const override {
        // These command spans are registered with the sampler at the smallest possible
        // nonzero rate, so nearly every one is rejected by the sampler's roll-and-compare logic
        // rather than skipped before it (a "trace miss"), with a hit astronomically unlikely.
        return std::make_unique<BenchmarkTracingSession>(
            SamplingParameters{.factor = kSmallestSamplingFactor}, kTracedSpanNames);
    }
};

class CrudTracedBenchmarkFixture : public CrudBenchmarkFixture {
public:
    std::unique_ptr<BenchmarkTracingSession> setupTracingSession() const override {
        return std::make_unique<BenchmarkTracingSession>(
            SamplingParameters{.factor = 1.0, .rateLimits = kUnlimitedRateLimit}, kTracedSpanNames);
    }
};

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_FIND_ONE)
(benchmark::State& state) {
    _runFindOne(state);
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_INSERT_ONE)
(benchmark::State& state) {
    _runInsertOne(state);
}

BENCHMARK_DEFINE_F(CrudTraceMissBenchmarkFixture, BM_FIND_ONE)
(benchmark::State& state) {
    _runFindOne(state);
}

BENCHMARK_DEFINE_F(CrudTraceMissBenchmarkFixture, BM_INSERT_ONE)
(benchmark::State& state) {
    _runInsertOne(state);
}

BENCHMARK_DEFINE_F(CrudTracedBenchmarkFixture, BM_FIND_ONE)
(benchmark::State& state) {
    _runFindOne(state);
}

BENCHMARK_DEFINE_F(CrudTracedBenchmarkFixture, BM_INSERT_ONE)
(benchmark::State& state) {
    _runInsertOne(state);
}

BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_FIND_ONE)->Threads(1)->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_INSERT_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTraceMissBenchmarkFixture, BM_FIND_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTraceMissBenchmarkFixture, BM_INSERT_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTracedBenchmarkFixture, BM_FIND_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTracedBenchmarkFixture, BM_INSERT_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);

// Heavier query shapes, to show how the fixed per-span cost of tracing shrinks relative to work.
BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_FIND_INDEXED_RANGE)
(benchmark::State& state) {
    _runFindIndexedRange(state);
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_FIND_COLLSCAN)
(benchmark::State& state) {
    _runFindCollscan(state);
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_AGG_GROUP)
(benchmark::State& state) {
    _runAggGroup(state);
}

BENCHMARK_DEFINE_F(CrudTraceMissBenchmarkFixture, BM_FIND_INDEXED_RANGE)
(benchmark::State& state) {
    _runFindIndexedRange(state);
}

BENCHMARK_DEFINE_F(CrudTraceMissBenchmarkFixture, BM_FIND_COLLSCAN)
(benchmark::State& state) {
    _runFindCollscan(state);
}

BENCHMARK_DEFINE_F(CrudTraceMissBenchmarkFixture, BM_AGG_GROUP)
(benchmark::State& state) {
    _runAggGroup(state);
}

BENCHMARK_DEFINE_F(CrudTracedBenchmarkFixture, BM_FIND_INDEXED_RANGE)
(benchmark::State& state) {
    _runFindIndexedRange(state);
}

BENCHMARK_DEFINE_F(CrudTracedBenchmarkFixture, BM_FIND_COLLSCAN)
(benchmark::State& state) {
    _runFindCollscan(state);
}

BENCHMARK_DEFINE_F(CrudTracedBenchmarkFixture, BM_AGG_GROUP)
(benchmark::State& state) {
    _runAggGroup(state);
}

BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_FIND_INDEXED_RANGE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_FIND_COLLSCAN)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_AGG_GROUP)->Threads(1)->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTraceMissBenchmarkFixture, BM_FIND_INDEXED_RANGE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTraceMissBenchmarkFixture, BM_FIND_COLLSCAN)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTraceMissBenchmarkFixture, BM_AGG_GROUP)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTracedBenchmarkFixture, BM_FIND_INDEXED_RANGE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTracedBenchmarkFixture, BM_FIND_COLLSCAN)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudTracedBenchmarkFixture, BM_AGG_GROUP)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);

}  // namespace
}  // namespace mongo

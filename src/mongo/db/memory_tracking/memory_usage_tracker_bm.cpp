// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/memory_tracking/memory_usage_tracker.h"

#include "mongo/db/memory_tracking/memory_usage_limit.h"

#include <cstdint>
#include <limits>
#include <memory>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

// Benchmarks for the SimpleMemoryUsageTracker update path, which blocking query stages call on
// every input document.
//
// The cases below are chosen to separate the two things that determine what this path costs in
// practice:
//
//  - How often an update is a no-op. A stage calls add() once per document with
//    'accumulator->getMemUsage() - prevMemUsage', which is 0 for fixed-size accumulators ($sum,
//    $count, $min / $max over scalars). add() filters those inline, so they never reach
//    addInternal(). In a $group over a fixed-size accumulator the non-zero updates are outnumbered
//    by roughly two orders of magnitude, which is what 'BM_GroupAccumulatePattern' reproduces.
//  - Whether a non-zero update crosses a chunk boundary. Only a level with chunking enabled does
//    the division, and only a boundary crossing reaches the CurOp write.
//
// Deltas are laundered through 'DoNotOptimize' so the compiler cannot fold add()'s inline zero
// check against a compile-time constant; in production the delta is a runtime value.

// Exposes the protected setWriteToCurOp() so a benchmark can install a reporting callback the way
// OperationMemoryUsageTracker does for the root of the chain.
class RootTracker : public SimpleMemoryUsageTracker {
public:
    using SimpleMemoryUsageTracker::setWriteToCurOp;
    using SimpleMemoryUsageTracker::SimpleMemoryUsageTracker;
};

constexpr int64_t kNoLimit = std::numeric_limits<int64_t>::max();

// Default of 'internalQueryMaxWriteToCurOpMemoryUsageBytes'.
constexpr int64_t kDefaultChunkSize = 1024 * 1024;

// Keeps usage far enough from zero that oscillating around a working set never trips the
// return-to-zero reporting rule, which would otherwise mix a CurOp write into cases meant to
// measure the plain update.
constexpr int64_t kBaseUsage = 64 * 1024;

/**
 * The three-level chain a $group builds: a per-accumulator child tracker, the stage's own base
 * tracker (the only level with chunking enabled), and the operation-wide root that owns the CurOp
 * callback. Levels are heap-allocated so the parent pointers stay valid.
 */
struct TrackerChain {
    explicit TrackerChain(int64_t chunkSize) {
        root = std::make_unique<RootTracker>(MemoryUsageLimit{kNoLimit}, 0 /* chunkSize */);
        // Stands in for the real CurOp write, which is more expensive than this counter. The
        // benchmarks that report on every update measure the call and the surrounding branch, not
        // the cost of touching CurOp's atomics.
        root->setWriteToCurOp([counter = reportCount.get()](int64_t, int64_t) { ++(*counter); });
        stage = std::make_unique<SimpleMemoryUsageTracker>(
            root.get(), MemoryUsageLimit{kNoLimit}, chunkSize);
        child = std::make_unique<SimpleMemoryUsageTracker>(stage.get(), MemoryUsageLimit{kNoLimit});
    }

    std::unique_ptr<int64_t> reportCount = std::make_unique<int64_t>(0);
    std::unique_ptr<RootTracker> root;
    std::unique_ptr<SimpleMemoryUsageTracker> stage;
    std::unique_ptr<SimpleMemoryUsageTracker> child;
};

/**
 * Zero-delta updates, the common case for a $group over a fixed-size accumulator. Measures add()'s
 * inline early return, which never calls into addInternal().
 */
void BM_AddZeroDelta(benchmark::State& state) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kNoLimit}, kDefaultChunkSize};
    tracker.add(kBaseUsage);
    for (auto _ : state) {
        int64_t delta = 0;
        benchmark::DoNotOptimize(delta);
        tracker.add(delta);
    }
    benchmark::DoNotOptimize(tracker.inUseTrackedMemoryBytes());
}
BENCHMARK(BM_AddZeroDelta);

/**
 * Same, but through the full three-level chain, to show that a zero delta costs the same regardless
 * of how deep the chain is: it is filtered before the walk starts.
 */
void BM_AddZeroDeltaChain(benchmark::State& state) {
    TrackerChain chain{kDefaultChunkSize};
    chain.child->add(kBaseUsage);
    for (auto _ : state) {
        int64_t delta = 0;
        benchmark::DoNotOptimize(delta);
        chain.child->add(delta);
    }
    benchmark::DoNotOptimize(chain.child->inUseTrackedMemoryBytes());
}
BENCHMARK(BM_AddZeroDeltaChain);

/**
 * Non-zero updates on a single tracker with chunking disabled: the update itself with no division
 * and no chain walk.
 */
void BM_AddNonZeroNoChunking(benchmark::State& state) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kNoLimit}, 0 /* chunkSize */};
    tracker.add(kBaseUsage);
    int64_t delta = 64;
    for (auto _ : state) {
        benchmark::DoNotOptimize(delta);
        tracker.add(delta);
        delta = -delta;
    }
    benchmark::DoNotOptimize(tracker.inUseTrackedMemoryBytes());
}
BENCHMARK(BM_AddNonZeroNoChunking);

/**
 * Non-zero updates that stay inside the chunk already reported, through the full chain. This is the
 * shape of a real non-zero update: the working set moves but rarely crosses a 1 MiB boundary, so
 * the chunk check takes its fast path and nothing is written to CurOp.
 */
void BM_AddNonZeroWithinChunk(benchmark::State& state) {
    TrackerChain chain{kDefaultChunkSize};
    chain.child->add(kBaseUsage);
    int64_t delta = 64;
    for (auto _ : state) {
        benchmark::DoNotOptimize(delta);
        chain.child->add(delta);
        delta = -delta;
    }
    benchmark::DoNotOptimize(*chain.reportCount);
}
BENCHMARK(BM_AddNonZeroWithinChunk);

/**
 * Non-zero updates that cross a chunk boundary every time, through the full chain. A small chunk
 * size turns every update into a boundary crossing, so this measures the division plus the CurOp
 * report -- the worst case for a single update, and the case chunking exists to make rare.
 */
void BM_AddNonZeroCrossingChunk(benchmark::State& state) {
    constexpr int64_t kSmallChunk = 128;
    TrackerChain chain{kSmallChunk};
    chain.child->add(kBaseUsage);
    int64_t delta = kSmallChunk;
    for (auto _ : state) {
        benchmark::DoNotOptimize(delta);
        chain.child->add(delta);
        delta = -delta;
    }
    benchmark::DoNotOptimize(*chain.reportCount);
}
BENCHMARK(BM_AddNonZeroCrossingChunk);

/**
 * The mix a $group actually produces: 'state.range(0)' zero deltas for every update that changes
 * the total. The ratio is the reason the aggregate cost of this path is dominated by
 * add()'s inline check rather than by anything addInternal() does -- compare the per-iteration time
 * here against 'BM_AddNonZeroWithinChunk'.
 */
void BM_GroupAccumulatePattern(benchmark::State& state) {
    const int64_t zeroDeltasPerUpdate = state.range(0);
    TrackerChain chain{kDefaultChunkSize};
    chain.child->add(kBaseUsage);
    int64_t countdown = zeroDeltasPerUpdate;
    int64_t nonZero = 64;
    for (auto _ : state) {
        int64_t delta = 0;
        if (--countdown < 0) {
            countdown = zeroDeltasPerUpdate;
            delta = nonZero;
            nonZero = -nonZero;
        }
        benchmark::DoNotOptimize(delta);
        chain.child->add(delta);
    }
    benchmark::DoNotOptimize(chain.child->inUseTrackedMemoryBytes());
}
// 0 = every update is non-zero (a growing accumulator such as $push), through to 250, roughly the
// ratio measured for a $sum in a $group over an unwound array.
BENCHMARK(BM_GroupAccumulatePattern)->Arg(0)->Arg(10)->Arg(250);

/**
 * set() expressed as a delta against the running total. Stages that recompute an absolute size use
 * this instead of add(); when the size is unchanged it degenerates to a zero delta.
 */
void BM_SetUnchangedTotal(benchmark::State& state) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kNoLimit}, kDefaultChunkSize};
    tracker.add(kBaseUsage);
    for (auto _ : state) {
        int64_t total = kBaseUsage;
        benchmark::DoNotOptimize(total);
        tracker.set(total);
    }
    benchmark::DoNotOptimize(tracker.inUseTrackedMemoryBytes());
}
BENCHMARK(BM_SetUnchangedTotal);

}  // namespace
}  // namespace mongo

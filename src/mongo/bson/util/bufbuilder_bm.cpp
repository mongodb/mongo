// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/util/builder.h"

#include <benchmark/benchmark.h>

namespace mongo {

template <typename BufBuilderType>
void BM_growBuffer(benchmark::State& state) {
    BufBuilderType buf;
    // The first argument determines if the buffer should be preallocated to stay on the fast path.
    if (state.range(0) == 1) {
        // Allocate max space for the buffer.
        buf.grow(BufferMaxSize);
        buf.reset();
    }
    for (auto _ : state) {
        // Grow the buffer by the second argument.
        benchmark::DoNotOptimize(buf.grow(state.range(1)));
    }
}

// Set the number of iterations explicitly so that the buffer does not grow beyond the limit.
// When the first argument is 1, increasing the second argument does not matter since the space is
// preallocated. When the first argument is 0, increasing the second argument increases the chance
// of falling back to slow path.
BENCHMARK_TEMPLATE(BM_growBuffer, BufBuilder)
    ->Ranges({{0, 1}, {1, 256}})
    ->Iterations(BufferMaxSize / 256);
BENCHMARK_TEMPLATE(BM_growBuffer, UniqueBufBuilder)
    ->Ranges({{0, 1}, {1, 256}})
    ->Iterations(BufferMaxSize / 256);
BENCHMARK_TEMPLATE(BM_growBuffer, StackBufBuilder)
    ->Ranges({{0, 1}, {1, 256}})
    ->Iterations(BufferMaxSize / 256);

}  // namespace mongo

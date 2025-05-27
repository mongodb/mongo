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

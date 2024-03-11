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

#include <benchmark/benchmark.h>

#include "mongo/util/tracking_allocator.h"

namespace mongo {

namespace {

constexpr size_t numIncsPerRead = 1000;

}  // namespace

class AllocatorFixture : public benchmark::Fixture {
protected:
    std::unique_ptr<TrackingAllocatorStats> _stats;
};

BENCHMARK_DEFINE_F(AllocatorFixture, counter)(benchmark::State& state) {
    if (state.thread_index == 0) {
        // Setup code
        // state.range(0) --> Number of partitions
        _stats = std::make_unique<TrackingAllocatorStats>(state.range(0));
    }

    for (auto _ : state) {
        for (size_t i = 0; i < numIncsPerRead; i++) {
            _stats->bytesAllocated(1);
        }
        benchmark::DoNotOptimize(_stats->allocated());
    }

    if (state.thread_index == 0) {
        // Teardown code
        state.counters["numIncrements"] = _stats->allocated();
    }
}

BENCHMARK_REGISTER_F(AllocatorFixture, counter)
    ->RangeMultiplier(2)
    ->Ranges({{1, 64}})
    ->ThreadRange(1, 64)
    ->UseRealTime();

}  // namespace mongo

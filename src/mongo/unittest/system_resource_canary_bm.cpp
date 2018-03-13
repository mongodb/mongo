/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the prograxm with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>

#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

// This is a trivial test case to sanity check that "benchmark" runs.
// For more information on how benchmarks should be written, please refer to Google Benchmark's
// excellent README: https://github.com/google/benchmark/blob/v1.3.0/README.md
void BM_Empty(benchmark::State& state) {
    for (auto keepRunning : state) {
        // The code inside this for-loop is what's being timed.
        benchmark::DoNotOptimize(state.iterations());
    }
}

// Register two benchmarks, one runs the "BM_Empty" function in a single thread, the other runs a
// copy per CPU core.
BENCHMARK(BM_Empty);
BENCHMARK(BM_Empty)->ThreadPerCpu();

void BM_CpuLoad(benchmark::State& state) {
    for (auto keepRunning : state) {
        uint64_t limit = 10000;
        uint64_t lresult = 0;
        uint64_t x = 100;
        for (uint64_t i = 0; i < limit; i++) {
            benchmark::DoNotOptimize(x *= 13);
        }
        benchmark::DoNotOptimize(lresult = x);
    }
}

// This Benchmark is adapted from the `cpuload` command:
// https://github.com/mongodb/mongo/blob/r3.7.2/src/mongo/db/commands/cpuload.cpp
BENCHMARK(BM_CpuLoad)->Threads(1)->ThreadPerCpu();


void BM_Sleep(benchmark::State& state) {
    for (auto keepRunning : state) {
        sleepmillis(100);
    }
}

BENCHMARK(BM_Sleep)->Threads(1)->ThreadPerCpu();


// Generate a loop with macros.
#define ONE ptrToNextLinkedListNode = reinterpret_cast<char**>(*ptrToNextLinkedListNode);
#define FIVE ONE ONE ONE ONE ONE
#define TEN FIVE FIVE
#define FIFTY TEN TEN TEN TEN TEN
#define HUNDRED FIFTY FIFTY

// Stride is the number of elements to skip when traversing the array.
// It should ideally be >= the cache line to avoid side-effects from pre-fetching.
const int kStrideBytes = 64;

class CacheLatencyTest : public benchmark::Fixture {
    // Fixture for CPU Cache and RAM latency test. Adapted from lmbench's lat_mem_rd test.
public:
    // Array of pointers used as a linked list.
    std::unique_ptr<char* []> data;

    void SetUp(benchmark::State& state) override {
        if (state.thread_index == 0) {
            fassert(data.get() == nullptr, "'data' is not null");

            /*
             * Create a circular list of pointers using a simple striding
             * algorithm.
             */
            const int arrLength = state.range(0);
            int counter = 0;

            data = std::make_unique<char* []>(arrLength);

            char** arr = data.get();

            /*
             * This access pattern corresponds to many array/matrix algorithms.
             * It should be easily and correctly predicted by any decent hardware
             * prefetch algorithm.
             */
            for (counter = 0; counter < arrLength - kStrideBytes; counter += kStrideBytes) {
                arr[counter] = reinterpret_cast<char*>(&arr[counter + kStrideBytes]);
            }
            arr[counter] = reinterpret_cast<char*>(&arr[0]);
        }
    }

    void TearDown(benchmark::State& state) override {
        if (state.thread_index == 0) {
            fassert(data.get() != nullptr, "'data' is null");
            data.reset();
        }
    }
};


BENCHMARK_DEFINE_F(CacheLatencyTest, BM_CacheLatency)(benchmark::State& state) {
    size_t arrLength = state.range(0);
    size_t counter = arrLength / (kStrideBytes * 100) + 1;

    for (auto keepRunning : state) {
        char** dummyResult = 0;  // Dummy result to prevent the loop from being optimized out.
        char** ptrToNextLinkedListNode = reinterpret_cast<char**>(data.get()[0]);

        for (size_t i = 0; i < counter; ++i) {
            HUNDRED;
        }
        benchmark::DoNotOptimize(dummyResult = ptrToNextLinkedListNode);
    }

    // Record the number of times we accessed the cache so Benchmark can compute the average latency
    // of each access. This allows comparing access latency across caches of different sizes.
    state.SetItemsProcessed(state.iterations() * counter * 100);
}

BENCHMARK_REGISTER_F(CacheLatencyTest, BM_CacheLatency)
    ->RangeMultiplier(2 * 1024)
    // Loop over arrays of different sizes to test the L2, L3, and RAM latency.
    ->Range(256 * 1024, 4096 * 1024)
    ->ThreadRange(1, ProcessInfo::getNumAvailableCores());

}  // namespace
}  // namespace mongo

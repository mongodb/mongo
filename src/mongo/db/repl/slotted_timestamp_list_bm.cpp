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

#include "mongo/db/repl/slotted_timestamp_list.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace repl {
namespace {

namespace {

/**
 * Executes a function and returns the duration of the execution time.
 */
auto measureOperationTime(std::function<void()> operation) {
    auto start = mongo::stdx::chrono::high_resolution_clock::now();
    operation();
    auto end = mongo::stdx::chrono::high_resolution_clock::now();
    return mongo::stdx::chrono::duration_cast<mongo::stdx::chrono::duration<double>>(end - start);
}

/**
 * Inserts n timestamps with values starting at the startValue and incrementing by 1.
 * Adds to positions vector after each insert.
 * Precondition: startValue has to be unique because all Timestamps are unique
 */
void insertN(SlottedTimestampList& list,
             size_t startValue,
             int N,
             std::vector<std::list<Timestamp>::const_iterator>& positions) {
    for (int i = 0; i < N; i++) {
        auto it = list.insert(Timestamp(startValue + i));
        positions.push_back(it);
    }
}

/**
 * Erases n timestamps starting at index startIndex.
 */
void eraseN(SlottedTimestampList& list,
            int startIndex,
            int N,
            const std::vector<std::list<Timestamp>::const_iterator>& positions) {
    for (int i = 0; i < N; i++) {
        list.erase(positions[startIndex + i]);
    }
}

}  // namespace

void BM_InsertMany(benchmark::State& state) {
    for (auto _ : state) {
        SlottedTimestampList list;
        const int64_t numTimestamps = state.range(0);

        auto timeForInsert = measureOperationTime([&] {
            for (int i = 0; i < numTimestamps; i++) {
                list.insert(Timestamp(i));
            }
        });

        state.SetIterationTime(timeForInsert.count());
    }
}

void BM_ReusingHalfSlots(benchmark::State& state) {
    for (auto _ : state) {
        SlottedTimestampList list;
        std::vector<std::list<Timestamp>::const_iterator> iters;

        const int64_t timestampsPerRound = state.range(0);
        const int64_t round = state.range(1);

        iters.reserve(timestampsPerRound * round);

        auto timeForHelpers = mongo::stdx::chrono::duration<double>(0);

        for (int i = 0; i < round; i++) {
            timeForHelpers += measureOperationTime([&] {
                insertN(
                    list, timestampsPerRound * i /*startValue*/, timestampsPerRound /*N*/, iters);
                eraseN(list, 0 /*startIndex*/, (timestampsPerRound / 2) /*N/2*/, iters);
            });
            iters.erase(iters.begin(), iters.begin() + (timestampsPerRound / 2));
        }

        state.SetIterationTime(timeForHelpers.count());
    }
}

void BM_EraseFIFO(benchmark::State& state) {
    for (auto _ : state) {
        SlottedTimestampList list;
        std::vector<SlottedTimestampList::const_iterator> iters;
        const int64_t count = state.range(0);

        iters.reserve(count);
        insertN(list, 0 /* startIndex */, count /* N */, iters);

        auto duration =
            measureOperationTime([&]() { eraseN(list, 0 /* startIndex */, count /* N */, iters); });

        state.SetIterationTime(duration.count());
    }
}

void BM_EraseLIFO(benchmark::State& state) {
    for (auto _ : state) {
        SlottedTimestampList list;
        std::vector<SlottedTimestampList::const_iterator> iters;
        const int64_t count = state.range(0);

        iters.reserve(count);
        insertN(list, 0 /* startIndex */, count /* N */, iters);

        std::reverse(iters.begin(), iters.end());

        auto duration =
            measureOperationTime([&] { eraseN(list, 0 /* startIndex */, count /* N */, iters); });

        state.SetIterationTime(duration.count());
    }
}

void BM_InsertAfterEraseFIFO(benchmark::State& state) {
    for (auto _ : state) {
        SlottedTimestampList list;
        std::vector<SlottedTimestampList::const_iterator> iters;
        const int64_t count = state.range(0);

        iters.reserve(count);

        insertN(list, 0 /* startIndex */, count /* N */, iters);
        eraseN(list, 0 /* startIndex */, count /* N */, iters);
        iters.clear();

        auto duration =
            measureOperationTime([&] { insertN(list, 0 /* startIndex */, count /* N */, iters); });

        state.SetIterationTime(duration.count());
    }
}

void BM_InsertAfterEraseLIFO(benchmark::State& state) {
    for (auto _ : state) {
        SlottedTimestampList list;
        std::vector<SlottedTimestampList::const_iterator> iters;
        const int64_t count = state.range(0);

        iters.reserve(count);

        insertN(list, 0 /* startIndex */, count /* N */, iters);
        std::reverse(iters.begin(), iters.end());
        eraseN(list, 0 /* startIndex */, count /* N */, iters);
        iters.clear();

        auto duration =
            measureOperationTime([&] { insertN(list, 0 /* startIndex */, count /* N */, iters); });

        state.SetIterationTime(duration.count());
    }
}

BENCHMARK(BM_InsertMany)->RangeMultiplier(2)->Range(1, 1 << 10)->UseManualTime()->MinTime(0.01);
BENCHMARK(BM_ReusingHalfSlots)
    ->RangeMultiplier(2)
    ->Ranges({{2, 1 << 7}, {1, 1 << 3}})
    ->UseManualTime()
    ->MinTime(0.01);
BENCHMARK(BM_EraseFIFO)->RangeMultiplier(2)->Range(1, 1 << 10)->UseManualTime()->MinTime(0.01);
BENCHMARK(BM_EraseLIFO)->RangeMultiplier(2)->Range(1, 1 << 10)->UseManualTime()->MinTime(0.01);
BENCHMARK(BM_InsertAfterEraseFIFO)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 10)
    ->UseManualTime()
    ->MinTime(0.01);
BENCHMARK(BM_InsertAfterEraseLIFO)
    ->RangeMultiplier(2)
    ->Range(1, 1 << 10)
    ->UseManualTime()
    ->MinTime(0.01);

}  // namespace
}  // namespace repl
}  // namespace mongo

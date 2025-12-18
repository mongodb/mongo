/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/data_with_lock_free_reads.h"

#include <random>

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace repl {

AtomicWord<long> mismatches;
template <size_t N>
struct Data {
    static constexpr size_t serializationForLockFreeReadsU64Count = N;
    using Buffer = DataWithLockFreeReadsBuffer<serializationForLockFreeReadsU64Count>;
    Buffer data;

    Data() {
        data.fill(0);
    }

    explicit Data(uint64_t value) {
        data.fill(value);
    }

    static Data parseForLockFreeReads(Buffer& data) {
        auto d = Data();
        d.data = data;
        return d;
    }

    Buffer serializeForLockFreeReads() const {
        return data;
    }
};

static std::random_device rd;
static std::mt19937_64 gen(rd());
static std::uniform_int_distribution<uint64_t> dist(0, 99);

template <typename sync>
void BM_LockFreeReads(benchmark::State& state, sync& test, uint64_t conflictChance) {
    if (state.thread_index == 0) {
        mismatches.store(0);
        test.store(WithLock::withoutLock(), Data<sync::N>(0));
    }

    long i = 1;
    for (auto keepRunning : state) {
        auto res = dist(gen);
        if (state.thread_index == 0 && res < conflictChance) {
            test.store(WithLock::withoutLock(), Data<sync::N>(i));
        } else {
            auto data = test.load();
            if (data.data[0] != data.data[data.data.size() - 1]) {
                mismatches.addAndFetch(1);
            }
            benchmark::DoNotOptimize(data);
        }
        i++;
    }

    if (state.thread_index == 0) {
        auto m = mismatches.load();
        // Remove the following line to see how common or rare mismatches are.
        invariant(m == 0);
        if (m != 0) {
            benchmark::Counter counter;
            counter.value = mismatches.load();
            state.counters["mismatches"] = counter;
        }
    }
}

DataWithLockFreeReads<Data<2>> seqlock_2;
DataWithLockFreeReads<Data<4>> seqlock_4;
DataWithLockFreeReads<Data<8>> seqlock_8;

void BM_LockFreeReadsSeqLock(benchmark::State& state) {
    auto dataWidth = state.range(0);
    auto conflictChance = state.range(1);
    switch (dataWidth) {
        case 2:
            BM_LockFreeReads(state, seqlock_2, conflictChance);
            break;
        case 4:
            BM_LockFreeReads(state, seqlock_4, conflictChance);
            break;
        case 8:
            BM_LockFreeReads(state, seqlock_8, conflictChance);
            break;
    }
}

BENCHMARK(BM_LockFreeReadsSeqLock)
    ->ArgsProduct({
        {2, 4, 8},
        {1, 5, 10, 20, 50, 80, 100},
    })
    ->ArgNames({"dataWidth", "conflictChance"})
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->Threads(32)
    ->Threads(64);

}  // namespace repl
}  // namespace mongo

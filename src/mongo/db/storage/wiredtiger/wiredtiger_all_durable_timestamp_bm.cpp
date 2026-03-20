/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include <benchmark/benchmark.h>

/**
 * Benchmarks getAllDurableTimestamp() on a real WiredTigerKVEngine instance,
 * measuring the end-to-end cost including WT's query_timestamp call and the
 * pin check, under varying thread counts and pin states.
 */

namespace mongo {
namespace {

class WiredTigerBenchmarkHelper : public MongoDScopedGlobalServiceContextForTest {
public:
    WiredTigerBenchmarkHelper()
        : MongoDScopedGlobalServiceContextForTest(Options{}.useReplSettings(true),
                                                  /*shouldSetupTL=*/false) {
        _engine =
            static_cast<WiredTigerKVEngine*>(getServiceContext()->getStorageEngine()->getEngine());
    }

    WiredTigerKVEngine* engine() {
        return _engine;
    }

private:
    WiredTigerKVEngine* _engine = nullptr;
};

// Baseline: pure WT query_timestamp cost with no pin check.
void BM_GetRawAllDurableTimestamp(benchmark::State& state) {
    static WiredTigerBenchmarkHelper* helper;
    static WiredTigerKVEngine* engine;

    if (state.thread_index == 0) {
        helper = new WiredTigerBenchmarkHelper();
        engine = helper->engine();
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(engine->getRawAllDurableTimestamp());
    }

    if (state.thread_index == 0) {
        delete helper;
    }
}

// No pins held.
void BM_GetAllDurableTimestamp(benchmark::State& state) {
    static WiredTigerBenchmarkHelper* helper;
    static WiredTigerKVEngine* engine;

    if (state.thread_index == 0) {
        helper = new WiredTigerBenchmarkHelper();
        engine = helper->engine();
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(engine->getAllDurableTimestamp());
    }

    if (state.thread_index == 0) {
        delete helper;
    }
}

// With a pin held.
void BM_GetAllDurableTimestamp_WithPin(benchmark::State& state) {
    static WiredTigerBenchmarkHelper* helper;
    static WiredTigerKVEngine* engine;

    if (state.thread_index == 0) {
        helper = new WiredTigerBenchmarkHelper();
        engine = helper->engine();
        engine->pinAllDurableTimestamp(100);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(engine->getAllDurableTimestamp());
    }

    if (state.thread_index == 0) {
        engine->unpinAllDurableTimestamp(100);
        delete helper;
    }
}

// One writer thread pins/unpins while readers call getAllDurableTimestamp().
void BM_GetAllDurableTimestamp_ConcurrentPinUnpin(benchmark::State& state) {
    static WiredTigerBenchmarkHelper* helper;
    static WiredTigerKVEngine* engine;

    if (state.thread_index == 0) {
        helper = new WiredTigerBenchmarkHelper();
        engine = helper->engine();
    }

    if (state.thread_index == 0) {
        uint64_t ts = 100;
        for (auto _ : state) {
            engine->pinAllDurableTimestamp(ts);
            engine->unpinAllDurableTimestamp(ts);
            ts++;
        }
    } else {
        for (auto _ : state) {
            benchmark::DoNotOptimize(engine->getAllDurableTimestamp());
        }
    }

    if (state.thread_index == 0) {
        delete helper;
    }
}

BENCHMARK(BM_GetRawAllDurableTimestamp)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16);
BENCHMARK(BM_GetAllDurableTimestamp)->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16);
BENCHMARK(BM_GetAllDurableTimestamp_WithPin)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16);
BENCHMARK(BM_GetAllDurableTimestamp_ConcurrentPinUnpin)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16);

}  // namespace
}  // namespace mongo

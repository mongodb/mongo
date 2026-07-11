// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

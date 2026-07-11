// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_cpu_timer.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

class Fixture {
public:
    Fixture() {
        setGlobalServiceContext(ServiceContext::make());
        _client = getGlobalServiceContext()->getService()->makeClient("test");
    }

    auto makeOpCtx() {
        return _client->makeOperationContext();
    }

private:
    ServiceContext::UniqueClient _client;
};

void BM_CPUTimerStartStop(benchmark::State& state) {
    Fixture fixture;
    auto opCtx = fixture.makeOpCtx();
    auto timer = OperationCPUTimers::get(opCtx.get())->makeTimer();
    for (auto _ : state) {
        timer.start();
        timer.stop();
    }
}

void BM_CPUTimerLifetime(benchmark::State& state) {
    Fixture fixture;
    for (auto _ : state) {
        auto opCtx = fixture.makeOpCtx();
        auto timer = OperationCPUTimers::get(opCtx.get())->makeTimer();
        timer.start();
        timer.stop();

        // Don't account for destruction since that is not accounted for in our command path.
        state.PauseTiming();
        opCtx.reset();
        state.ResumeTiming();
    }
}

void BM_MultipleCPUTimer(benchmark::State& state) {
    Fixture fixture;
    for (auto _ : state) {
        auto opCtx = fixture.makeOpCtx();
        for (auto i = 0; i < 4; i++) {
            auto timer = OperationCPUTimers::get(opCtx.get())->makeTimer();
            timer.start();
            timer.stop();
        }
    }
}

BENCHMARK(BM_CPUTimerStartStop);
BENCHMARK(BM_CPUTimerLifetime);
BENCHMARK(BM_MultipleCPUTimer);

}  // namespace
}  // namespace mongo

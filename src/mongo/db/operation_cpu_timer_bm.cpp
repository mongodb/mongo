/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/locker_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_cpu_timer.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/basic.h"


namespace mongo {
namespace {

class LockerClientObserver : public ServiceContext::ClientObserver {
public:
    LockerClientObserver() = default;
    ~LockerClientObserver() = default;

    void onCreateClient(Client* client) final {}
    void onDestroyClient(Client* client) final {}
    void onCreateOperationContext(OperationContext* opCtx) final {
        auto service = opCtx->getServiceContext();

        opCtx->setLockState(std::make_unique<LockerImpl>(service));
    }
    void onDestroyOperationContext(OperationContext* opCtx) final {}
};

class Fixture {
public:
    Fixture() {
        setGlobalServiceContext(ServiceContext::make());
        getGlobalServiceContext()->registerClientObserver(std::make_unique<LockerClientObserver>());
        _client = getGlobalServiceContext()->makeClient("test");
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
        timer->start();
        timer->stop();
    }
}

void BM_CPUTimerLifetime(benchmark::State& state) {
    Fixture fixture;
    for (auto _ : state) {
        auto opCtx = fixture.makeOpCtx();
        auto timer = OperationCPUTimers::get(opCtx.get())->makeTimer();
        timer->start();
        timer->stop();

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
            timer->start();
            timer->stop();
        }
    }
}

BENCHMARK(BM_CPUTimerStartStop);
BENCHMARK(BM_CPUTimerLifetime);
BENCHMARK(BM_MultipleCPUTimer);

}  // namespace
}  // namespace mongo

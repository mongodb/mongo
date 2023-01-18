/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/unittest/barrier.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kTest

namespace mongo::transport {
namespace {

struct Notification {
    void set() {
        stdx::unique_lock lk{mu};
        notified = true;
        cv.notify_all();
    }

    void get() {
        stdx::unique_lock lk{mu};
        cv.wait(lk, [&] { return notified; });
    }

    stdx::mutex mu;  // NOLINT
    stdx::condition_variable cv;
    bool notified = false;
};

class ServiceExecutorSynchronousBm : public benchmark::Fixture {
public:
    void firstSetup() {
        auto usc = ServiceContext::make();
        sc = usc.get();
        usc->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
        setGlobalServiceContext(std::move(usc));
        (void)executor()->start();
    }

    ServiceExecutorSynchronous* executor() {
        return ServiceExecutorSynchronous::get(sc);
    }

    void lastTearDown() {
        (void)executor()->shutdown(Hours{1});
        setGlobalServiceContext({});
    }

    void SetUp(benchmark::State& state) override {
        stdx::lock_guard lk{mu};
        if (nThreads++)
            return;
        firstSetup();
    }

    void TearDown(benchmark::State& state) override {
        stdx::lock_guard lk{mu};
        if (--nThreads)
            return;
        lastTearDown();
    }

    stdx::mutex mu;  // NOLINT
    int nThreads = 0;
    ServiceContext* sc;
};

/** 2x to benchmark the case of more threads than cores for curiosity's sake. */
auto maxThreads = 2 * ProcessInfo::getNumCores();

BENCHMARK_DEFINE_F(ServiceExecutorSynchronousBm, ScheduleTask)(benchmark::State& state) {
    for (auto _ : state) {
        executor()->makeTaskRunner()->schedule([](Status) {});
    }
}
BENCHMARK_REGISTER_F(ServiceExecutorSynchronousBm, ScheduleTask)->ThreadRange(1, maxThreads);

/** A simplified ChainedSchedule with only one task. */
BENCHMARK_DEFINE_F(ServiceExecutorSynchronousBm, ScheduleAndWait)(benchmark::State& state) {
    for (auto _ : state) {
        auto runner = executor()->makeTaskRunner();
        Notification done;
        runner->schedule([&](Status) { done.set(); });
        done.get();
    }
}
BENCHMARK_REGISTER_F(ServiceExecutorSynchronousBm, ScheduleAndWait)->ThreadRange(1, maxThreads);

/** Like ScheduleAndWait, but kill worker lease from within task like SessionWorkflow does. */
BENCHMARK_DEFINE_F(ServiceExecutorSynchronousBm, ScheduleAndWaitReleaseInTask)
(benchmark::State& state) {
    for (auto _ : state) {
        auto runner = executor()->makeTaskRunner();
        Notification done;
        auto raw = runner.get();
        raw->schedule([&, trp = std::move(runner)](Status) mutable {
            trp = {};
            done.set();
        });
        done.get();
    }
}
BENCHMARK_REGISTER_F(ServiceExecutorSynchronousBm, ScheduleAndWaitReleaseInTask)
    ->ThreadRange(1, maxThreads);

BENCHMARK_DEFINE_F(ServiceExecutorSynchronousBm, ChainedSchedule)(benchmark::State& state) {
    int chainDepth = state.range(0);
    struct LoopState {
        std::shared_ptr<ServiceExecutor::Executor> runner;
        Notification done;
        unittest::Barrier startingLine{2};
    };
    LoopState* loopStatePtr = nullptr;
    std::function<void(Status)> chainedTask = [&](Status) { loopStatePtr->done.set(); };
    for (int step = 0; step != chainDepth; ++step)
        chainedTask = [this, chainedTask, &loopStatePtr](Status) {
            loopStatePtr->runner->schedule(chainedTask);
        };

    // The first scheduled task starts the worker thread. This test is
    // specifically measuring the per-task schedule and run overhead. So startup
    // costs are moved outside the loop. But it's tricky because that started
    // thread will die if its task returns without scheduling a successor task.
    // So we start the worker thread with a task that will pause until the
    // benchmark loop resumes it.
    for (auto _ : state) {
        state.PauseTiming();
        LoopState loopState{
            executor()->makeTaskRunner(),
            {},
        };
        loopStatePtr = &loopState;
        loopStatePtr->runner->schedule([&](Status s) {
            loopState.startingLine.countDownAndWait();
            loopStatePtr->runner->schedule(chainedTask);
        });
        state.ResumeTiming();
        loopState.startingLine.countDownAndWait();
        loopState.done.get();
    }
}
BENCHMARK_REGISTER_F(ServiceExecutorSynchronousBm, ChainedSchedule)
    ->Range(1, 1 << 8)
    ->ThreadRange(1, maxThreads);

}  // namespace
}  // namespace mongo::transport

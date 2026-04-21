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

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/ticketing/ordered_ticket_semaphore.h"
#include "mongo/db/admission/ticketing/unordered_ticket_semaphore.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/tick_source_mock.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <random>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

using benchmark::internal::Benchmark;

constexpr int kMaxWaiters = 4096;
// Fixed amount of work done while holding a ticket, representing a realistic MongoDB critical
// section (~120 ns on Neoverse-N1).
constexpr int64_t kHoldIters = 300;

std::mutex mtx;
stdx::condition_variable cv;
bool ready = false;
int numRemaining = 0;
std::unique_ptr<TicketSemaphore> sem;
ServiceContext::UniqueServiceContext svcCtx;

/**
 * Allows the initialization/destruction of the semaphore and the service context safely.
 */
void barrierSetup(benchmark::State& state, std::unique_ptr<TicketSemaphore> s) {
    std::unique_lock lk(mtx);
    if (state.thread_index == 0) {
        numRemaining = state.threads;
        svcCtx = ServiceContext::make(std::make_unique<SystemClockSource>(),
                                      std::make_unique<SystemClockSource>(),
                                      std::make_unique<TickSourceMock<Microseconds>>());
        sem = std::move(s);
        ready = true;
        cv.notify_all();
    } else {
        cv.wait(lk, [] { return ready; });
    }
}

void barrierTeardown(benchmark::State& state) {
    if (state.thread_index == 0) {
        sem.reset();
        svcCtx.reset();
        ready = false;
    }
}

void barrierTeardownWithServiceContext(benchmark::State& state,
                                       ServiceContext::UniqueOperationContext& opCtx,
                                       ServiceContext::UniqueClient& client) {
    {
        std::unique_lock lk(mtx);
        opCtx.reset();
        client.reset();
        numRemaining--;
        if (numRemaining > 0) {
            cv.wait(lk, [] { return numRemaining == 0; });
        } else {
            cv.notify_all();
        }
    }
    barrierTeardown(state);
}

void setCounters(benchmark::State& state, int64_t ops) {
    state.counters["Ops"] = benchmark::Counter(ops, benchmark::Counter::kIsRate);
    state.counters["OpsPerThread"] = benchmark::Counter(ops, benchmark::Counter::kAvgThreadsRate);
}

void burnCycles(int64_t iters, int* data) {
    for (int64_t i = 0; i < iters; i++) {
        ++(*data);
        benchmark::DoNotOptimize(*data);
    }
}

/**
 * Measures throughput at a target semaphore utilization.
 * Inspired in mutex_benchmark.cc.
 *
 * The out-of-critical-section work is derived from the utilization formula so that, on average,
 * (utilization_pct/100 * permits) tickets are in use at any given time:
 *
 *   outsideIters = kHoldIters * (threads / (utilization * permits) - 1)
 *
 * When threads <= utilization * permits (the semaphore is underloaded for the target),
 * outsideIters clamps to 0 and every thread is always trying to hold a ticket.
 *
 * Outside work is randomised ±50% per iteration to break the thundering-herd effect that arises
 * when all threads start in lockstep from the barrier.
 *
 * Thread counts deliberately span both undersubscription (< core count) and oversubscription
 * (> core count) to expose scheduler-induced latency in addition to semaphore overhead.
 */
template <typename SemType>
void BM_ContendedAcquireRelease(benchmark::State& state) {
    const int permits = static_cast<int>(state.range(0));
    const double utilization = state.range(1) / 100.0;

    // Derived outside-work iteration count. Clamped to 0 when threads <= utilization * permits
    // (i.e. the semaphore cannot be saturated to the target level with this many threads).
    const double loadRatio = state.threads / (utilization * permits);
    const int64_t outsideIters =
        loadRatio > 1.0 ? static_cast<int64_t>(kHoldIters * (loadRatio - 1.0)) : 0;

    barrierSetup(state, std::make_unique<SemType>(permits, kMaxWaiters));

    auto client =
        svcCtx->getService()->makeClient(str::stream() << "bm_thread_" << state.thread_index);
    auto opCtx = client->makeOperationContext();
    MockAdmissionContext admCtx;

    // Local RNG to jitter outside work and break synchronization from the barrier.
    std::minstd_rand rng(std::random_device{}());
    std::uniform_real_distribution<double> jitter(0.5, 1.5);

    int local = 0;
    int64_t ops = 0;
    for (auto _ : state) {
        if (outsideIters > 0)
            burnCycles(static_cast<int64_t>(outsideIters * jitter(rng)), &local);
        sem->acquire(opCtx.get(), &admCtx, Date_t::max(), false);
        burnCycles(kHoldIters, &local);
        sem->release();
        ops++;
    }

    setCounters(state, ops);
    barrierTeardownWithServiceContext(state, opCtx, client);
}

// Thread counts: 8 and 16 are under/at the 16-core machine count; 32, 64, 128 are 2×/4×/8×
// oversubscribed. Utilization levels: 1% (barely loaded), 20%, 60%, 90% (near-saturation).
void contendedArgs(benchmark::internal::Benchmark* bm) {
    bm->ArgNames({"permits", "util_pct"});
    for (auto util : {1, 20, 60, 90})
        bm->Args({32, util});
    for (auto t : {8, 16, 32, 64, 128})
        bm->Threads(t);
}

BENCHMARK_TEMPLATE(BM_ContendedAcquireRelease, UnorderedTicketSemaphore)->Apply(contendedArgs);
BENCHMARK_TEMPLATE(BM_ContendedAcquireRelease, OrderedTicketSemaphore)->Apply(contendedArgs);

/*
 * Benchmarks the semaphore queueing path by keeping the wait queue at maximal depth. Uses a single
 * permit so that all non-holding threads must go through the slow (blocking) acquisition path
 * rather than the tryAcquire fast path. For OrderedTicketSemaphore, the multiple_admissions variant
 * mixes threads with different admission counts to benchmark the overhead of priority-queue
 * ordering when a high-priority waiter joins a queue of lower-priority ones.
 */
template <typename SemType>
void BM_SemaphoreEnqueue(benchmark::State& state) {
    const bool multiple_admissions = static_cast<bool>(state.range(0));

    struct Shared {
        std::atomic<int> looping_threads{0};
        std::atomic<int> blocked_threads{0};
        std::atomic<bool> permit_held{false};
    };
    static Shared shared;

    barrierSetup(state, std::make_unique<SemType>(1, kMaxWaiters));

    auto client = svcCtx->getService()->makeClient(fmt::format("bm_thread_()", state.thread_index));
    auto opCtx = client->makeOperationContext();
    MockAdmissionContext admCtx;

    if (multiple_admissions) {
        admCtx.setAdmission_forTest(state.thread_index == 0 ? 0 : 100);
    }

    static constexpr int kBatchSize = 1000;
    while (state.KeepRunningBatch(kBatchSize)) {
        shared.looping_threads.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < kBatchSize; i++) {
            shared.blocked_threads.fetch_add(1, std::memory_order_relaxed);
            sem->acquire(opCtx.get(), &admCtx, Date_t::max(), false);
            shared.blocked_threads.fetch_add(-1, std::memory_order_relaxed);

            shared.permit_held.store(true, std::memory_order_relaxed);
            // Busy wait to ensure the queue is at maximal depth to benchmark the cost of queueing
            // on a highly contended semaphore.
            while (shared.looping_threads.load(std::memory_order_relaxed) -
                       shared.blocked_threads.load(std::memory_order_relaxed) !=
                   1) {
            }
            shared.permit_held.store(false, std::memory_order_relaxed);
            sem->release();

            // Busy wait before we attempt to re-acquire to ensure we always go through the slow
            // (queueing) path rather than stealing the permit we just released via the tryAcquire
            // fast path.
            while (!shared.permit_held.load(std::memory_order_relaxed) &&
                   shared.looping_threads.load(std::memory_order_relaxed) > 1) {
            }
        }
        // Prevent other threads to spin indefinitely waiting for us to call acquire().
        shared.looping_threads.fetch_add(-1, std::memory_order_relaxed);
    }

    barrierTeardownWithServiceContext(state, opCtx, client);
}

void enqueueArgs(Benchmark* bm, bool ordered) {
    bm->ArgName("multiple_admissions");
    bm->Arg(ordered);
    for (auto t : {4, 32, 64, 128}) {
        bm->Threads(t);
    }
}

BENCHMARK_TEMPLATE(BM_SemaphoreEnqueue, UnorderedTicketSemaphore)->Apply([](Benchmark* bm) {
    enqueueArgs(bm, false);
});

BENCHMARK_TEMPLATE(BM_SemaphoreEnqueue, OrderedTicketSemaphore)->Apply([](Benchmark* bm) {
    enqueueArgs(bm, true);
});

}  // namespace
}  // namespace mongo

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

#include <vector>

#include "mongo/util/concurrency/ticketholder.h"


namespace mongo {
namespace {

static int kTickets = 128;
static int kThreadMin = 8;
static int kThreadMax = 1024;

template <class TicketHolderImpl>
void BM_tryAcquire(benchmark::State& state) {
    static std::unique_ptr<TicketHolder> ticketHolder;
    if (state.thread_index == 0) {
        ticketHolder = std::make_unique<TicketHolderImpl>(kTickets);
    }
    double attempted = 0, acquired = 0;
    for (auto _ : state) {
        auto hasAcquired = ticketHolder->tryAcquire();
        state.PauseTiming();
        sleepmicros(1);
        attempted++;
        if (hasAcquired) {
            acquired++;
            ticketHolder->release();
        }
        state.ResumeTiming();
    }
    state.counters["Attempted"] = attempted;
    state.counters["Acquired"] = acquired;
}

BENCHMARK_TEMPLATE(BM_tryAcquire, SemaphoreTicketHolder)->ThreadRange(kThreadMin, kThreadMax);

BENCHMARK_TEMPLATE(BM_tryAcquire, FifoTicketHolder)->ThreadRange(kThreadMin, kThreadMax);

template <class TicketHolderImpl>
void BM_acquire(benchmark::State& state) {
    static std::unique_ptr<TicketHolder> ticketHolder;
    if (state.thread_index == 0) {
        ticketHolder = std::make_unique<TicketHolderImpl>(kTickets);
    }
    double acquired = 0;
    for (auto _ : state) {
        ticketHolder->waitForTicket();
        state.PauseTiming();
        sleepmicros(1);
        ticketHolder->release();
        acquired++;
        state.ResumeTiming();
    }
    state.counters["Acquired"] = benchmark::Counter(acquired, benchmark::Counter::kIsRate);
    state.counters["AcquiredPerThread"] =
        benchmark::Counter(acquired, benchmark::Counter::kAvgThreadsRate);
}

BENCHMARK_TEMPLATE(BM_acquire, SemaphoreTicketHolder)->ThreadRange(kThreadMin, kThreadMax);

BENCHMARK_TEMPLATE(BM_acquire, FifoTicketHolder)->ThreadRange(kThreadMin, kThreadMax);

template <class TicketHolderImpl>
void BM_release(benchmark::State& state) {
    static std::unique_ptr<TicketHolder> ticketHolder;
    if (state.thread_index == 0) {
        ticketHolder = std::make_unique<TicketHolderImpl>(kTickets);
    }
    double acquired = 0;
    for (auto _ : state) {
        state.PauseTiming();
        ticketHolder->waitForTicket();
        sleepmicros(1);
        state.ResumeTiming();
        ticketHolder->release();
        acquired++;
    }
    state.counters["Acquired"] = benchmark::Counter(acquired, benchmark::Counter::kIsRate);
    state.counters["AcquiredPerThread"] =
        benchmark::Counter(acquired, benchmark::Counter::kAvgThreadsRate);
}

BENCHMARK_TEMPLATE(BM_release, SemaphoreTicketHolder)->ThreadRange(kThreadMin, kThreadMax);

BENCHMARK_TEMPLATE(BM_release, FifoTicketHolder)->ThreadRange(kThreadMin, kThreadMax);


template <class H>
void BM_acquireAndRelease(benchmark::State& state) {
    static std::unique_ptr<TicketHolder> ticketHolder;
    if (state.thread_index == 0) {
        ticketHolder = std::make_unique<H>(kTickets);
    }
    double acquired = 0;
    for (auto _ : state) {
        ticketHolder->waitForTicket();
        state.PauseTiming();
        sleepmicros(1);
        state.ResumeTiming();
        ticketHolder->release();
        acquired++;
    }
    state.counters["Acquired"] = benchmark::Counter(acquired, benchmark::Counter::kIsRate);
    state.counters["AcquiredPerThread"] =
        benchmark::Counter(acquired, benchmark::Counter::kAvgThreadsRate);
}

BENCHMARK_TEMPLATE(BM_acquireAndRelease, SemaphoreTicketHolder)
    ->ThreadRange(kThreadMin, kThreadMax);

BENCHMARK_TEMPLATE(BM_acquireAndRelease, FifoTicketHolder)->ThreadRange(kThreadMin, kThreadMax);

}  // namespace
}  // namespace mongo

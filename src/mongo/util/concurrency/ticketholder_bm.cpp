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
// IWYU pragma: no_include "cxxabi.h"
#include <map>
#include <memory>

#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/duration.h"
#include "mongo/util/latency_distribution.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

static int kTickets = 32;
static int kThreadMin = 16;
static int kThreadMax = 1024;

class TicketHolderFixture {
public:
    std::unique_ptr<TicketHolder> ticketHolder;

    TicketHolderFixture(int threads, ServiceContext* serviceContext) {
        ticketHolder =
            std::make_unique<TicketHolder>(serviceContext, kTickets, true /* track peakUsed */);
    }
};


static stdx::mutex isReadyMutex;
static stdx::condition_variable isReadyCv;
static bool isReady = false;

void BM_acquireAndRelease(benchmark::State& state) {
    static std::unique_ptr<TicketHolderFixture> ticketHolder;
    static ServiceContext::UniqueServiceContext serviceContext;
    static constexpr auto resolution = Microseconds{100};
    static LatencyPercentileDistribution resultingDistribution(resolution);
    static int numRemainingToMerge;
    {
        stdx::unique_lock lk(isReadyMutex);
        if (state.thread_index == 0) {
            resultingDistribution = LatencyPercentileDistribution{resolution};
            numRemainingToMerge = state.threads;
            serviceContext = ServiceContext::make(std::make_unique<SystemClockSource>(),
                                                  std::make_unique<SystemClockSource>(),
                                                  std::make_unique<TickSourceMock<Microseconds>>());
            ticketHolder =
                std::make_unique<TicketHolderFixture>(state.threads, serviceContext.get());
            isReady = true;
            isReadyCv.notify_all();
        } else {
            isReadyCv.wait(lk, [&] { return isReady; });
        }
    }
    double acquired = 0;

    ServiceContext::UniqueClient client = serviceContext->getService()->makeClient(
        str::stream() << "test client for thread " << state.thread_index);
    ServiceContext::UniqueOperationContext opCtx = client->makeOperationContext();

    TicketHolderFixture* fixture = ticketHolder.get();
    // We build the latency distribution locally in order to avoid synchronizing with other threads.
    // All of them will be merged at the end instead.
    LatencyPercentileDistribution localDistribution{resolution};

    for (auto _ : state) {
        Timer timer;
        Microseconds timeForAcquire;
        {
            auto& admCtx = ExecutionAdmissionContext::get(opCtx.get());
            auto ticket =
                fixture->ticketHolder->waitForTicketUntil(opCtx.get(), &admCtx, Date_t::max());
            timeForAcquire = timer.elapsed();
            state.PauseTiming();
            sleepmicros(1);
            acquired++;
            state.ResumeTiming();
            // We reset the timer here to ignore the time spent doing artificial sleeping for time
            // spent doing acquire and release. Release will be performed as part of the ticket
            // destructor.
            timer.reset();
        }
        localDistribution.addEntry(timeForAcquire + timer.elapsed());
    }
    state.counters["Acquired"] = benchmark::Counter(acquired, benchmark::Counter::kIsRate);
    state.counters["AcquiredPerThread"] =
        benchmark::Counter(acquired, benchmark::Counter::kAvgThreadsRate);
    // Merge all latency distributions in order to get the full view of all threads.
    {
        stdx::unique_lock lk(isReadyMutex);
        opCtx.reset();
        client.reset();
        resultingDistribution = resultingDistribution.mergeWith(localDistribution);
        numRemainingToMerge--;
        if (numRemainingToMerge > 0) {
            isReadyCv.wait(lk, [&] { return numRemainingToMerge == 0; });
        } else {
            isReadyCv.notify_all();
        }
    }
    if (state.thread_index == 0) {
        ticketHolder.reset();
        serviceContext.reset();
        isReady = false;
        state.counters["AcqRel50"] =
            benchmark::Counter(resultingDistribution.getPercentile(0.5f).count());
        state.counters["AcqRel95"] =
            benchmark::Counter(resultingDistribution.getPercentile(0.95f).count());
        state.counters["AcqRel99"] =
            benchmark::Counter(resultingDistribution.getPercentile(0.99f).count());
        state.counters["AcqRel99.9"] =
            benchmark::Counter(resultingDistribution.getPercentile(0.999f).count());
        state.counters["AcqRelMax"] = benchmark::Counter(resultingDistribution.getMax().count());
    }
}

BENCHMARK(BM_acquireAndRelease)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
    ->Threads(128)
    ->Threads(kThreadMax);

}  // namespace
}  // namespace mongo

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

#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/priority_ticketholder.h"
#include "mongo/util/concurrency/semaphore_ticketholder.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/duration.h"
#include "mongo/util/latency_distribution.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

static int kTickets = 32;
static int kThreadMin = 16;
static int kThreadMax = 1024;
static int kLowPriorityAdmissionBypassThreshold = 100;

// For a given benchmark, specifies the AdmissionContext::Priority of ticket admissions
enum class AdmissionsPriority {
    // All admissions must be AdmissionContext::Priority::kNormal.
    kNormal,
    // Admissions may vary between AdmissionContext::Priority::kNormal and
    // AdmissionContext::Priority::kLow.
    kNormalAndLow,
    // All admissions must be AdmissionContext::Priority::kLow.
    kLow,
};

template <typename TicketHolderImpl>
class TicketHolderFixture {
public:
    std::unique_ptr<TicketHolder> ticketHolder;

    TicketHolderFixture(int threads, ServiceContext* serviceContext) {
        if constexpr (std::is_same_v<PriorityTicketHolder, TicketHolderImpl>) {
            ticketHolder = std::make_unique<TicketHolderImpl>(serviceContext,
                                                              kTickets,
                                                              kLowPriorityAdmissionBypassThreshold,
                                                              true /* track peakUsed */);
        } else {
            ticketHolder = std::make_unique<TicketHolderImpl>(
                serviceContext, kTickets, true /* track peakUsed */);
        }
    }
};


static Mutex isReadyMutex;
static stdx::condition_variable isReadyCv;
static bool isReady = false;

template <class TicketHolderImpl, AdmissionsPriority admissionsPriority>
void BM_acquireAndRelease(benchmark::State& state) {
    static std::unique_ptr<TicketHolderFixture<TicketHolderImpl>> ticketHolder;
    static ServiceContext::UniqueServiceContext serviceContext;
    static constexpr auto resolution = Microseconds{100};
    static LatencyPercentileDistribution resultingDistribution(resolution);
    static int numRemainingToMerge;
    {
        stdx::unique_lock lk(isReadyMutex);
        if (state.thread_index == 0) {
            resultingDistribution = LatencyPercentileDistribution{resolution};
            numRemainingToMerge = state.threads;
            serviceContext = ServiceContext::make();
            serviceContext->setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
            ticketHolder = std::make_unique<TicketHolderFixture<TicketHolderImpl>>(
                state.threads, serviceContext.get());
            isReady = true;
            isReadyCv.notify_all();
        } else {
            isReadyCv.wait(lk, [&] { return isReady; });
        }
    }
    double acquired = 0;

    auto client = getGlobalServiceContext()->getService()->makeClient(
        str::stream() << "test client for thread " << state.thread_index);
    auto opCtx = client->makeOperationContext();

    ScopedAdmissionPriority admissionPriority(opCtx.get(), [&] {
        switch (admissionsPriority) {
            case AdmissionsPriority::kNormal:
                return AdmissionContext::Priority::kNormal;
            case AdmissionsPriority::kLow:
                return AdmissionContext::Priority::kLow;
            case AdmissionsPriority::kNormalAndLow: {
                return (state.thread_index % 2) == 0 ? AdmissionContext::Priority::kNormal
                                                     : AdmissionContext::Priority::kLow;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }());

    TicketHolderFixture<TicketHolderImpl>* fixture = ticketHolder.get();
    // We build the latency distribution locally in order to avoid synchronizing with other threads.
    // All of them will be merged at the end instead.
    LatencyPercentileDistribution localDistribution{resolution};

    for (auto _ : state) {
        Timer timer;
        Microseconds timeForAcquire;
        Microseconds timeInQueue(0);
        {
            auto admCtx = AdmissionContext::get(opCtx.get());
            auto ticket = fixture->ticketHolder->waitForTicketUntil(
                *Interruptible::notInterruptible(), &admCtx, Date_t::max(), timeInQueue);
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

// The 'AdmissionsPriority' has no effect on SemaphoreTicketHolder performance because the
// SemaphoreTicketHolder treaats all operations the same, regardless of their specified priority.
// However, the benchmarks between the SemaphoreTicketHolder and the PriorityTicketHolder are only
// comparable when all admissions are of normal priority.
BENCHMARK_TEMPLATE(BM_acquireAndRelease, SemaphoreTicketHolder, AdmissionsPriority::kNormal)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
    ->Threads(128)
    ->Threads(kThreadMax);

// TODO SERVER-72616: Remove ifdefs once PriorityTicketHolder is available cross-platform.
#ifdef __linux__

BENCHMARK_TEMPLATE(BM_acquireAndRelease, PriorityTicketHolder, AdmissionsPriority::kNormal)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
    ->Threads(128)
    ->Threads(kThreadMax);

// Low priority operations are expected to take longer to acquire a ticket because they are forced
// to take a slower path than normal priority operations.
BENCHMARK_TEMPLATE(BM_acquireAndRelease, PriorityTicketHolder, AdmissionsPriority::kLow)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
    ->Threads(128)
    ->Threads(kThreadMax);

// This benchmark is intended for comparisons between different iterations of the
// PriorityTicketHolder over time.
//
// Since it is known low priority operations will be less performant than normal priority
// operations, the aggregate performance over operations will be lower, and cannot be accurately
// compared to TicketHolderImpl benchmarks with only normal priority operations.
BENCHMARK_TEMPLATE(BM_acquireAndRelease, PriorityTicketHolder, AdmissionsPriority::kNormalAndLow)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
    ->Threads(128)
    ->Threads(kThreadMax);

#endif

}  // namespace
}  // namespace mongo

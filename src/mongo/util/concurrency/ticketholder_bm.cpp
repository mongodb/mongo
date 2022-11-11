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

#include <string>
#include <vector>

#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

static int kTickets = 128;
static int kThreadMin = 16;
static int kThreadMax = 1024;
static int kLowPriorityAdmissionBypassThreshold = 100;
static TicketHolder::WaitMode waitMode = TicketHolder::WaitMode::kUninterruptible;

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
    std::vector<ServiceContext::UniqueClient> clients;
    std::vector<ServiceContext::UniqueOperationContext> opCtxs;


    TicketHolderFixture(int threads, ServiceContext* serviceContext) {
        if constexpr (std::is_same_v<PriorityTicketHolder, TicketHolderImpl>) {
            ticketHolder = std::make_unique<TicketHolderImpl>(
                kTickets, kLowPriorityAdmissionBypassThreshold, serviceContext);
        } else {
            ticketHolder = std::make_unique<TicketHolderImpl>(kTickets, serviceContext);
        }

        for (int i = 0; i < threads; ++i) {
            clients.push_back(
                serviceContext->makeClient(str::stream() << "test client for thread " << i));
            opCtxs.push_back(clients[i]->makeOperationContext());
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
    {
        stdx::unique_lock lk(isReadyMutex);
        if (state.thread_index == 0) {
            serviceContext = ServiceContext::make();
            serviceContext->setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
            serviceContext->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
            ticketHolder = std::make_unique<TicketHolderFixture<TicketHolderImpl>>(
                state.threads, serviceContext.get());
            isReady = true;
            isReadyCv.notify_all();
        } else {
            isReadyCv.wait(lk, [&] { return isReady; });
        }
    }
    double acquired = 0;

    AdmissionContext::Priority priority = [&] {
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
    }();

    TicketHolderFixture<TicketHolderImpl>* fixture = ticketHolder.get();
    for (auto _ : state) {
        AdmissionContext admCtx;
        admCtx.setPriority(priority);
        auto opCtx = fixture->opCtxs[state.thread_index].get();
        {
            auto ticket = fixture->ticketHolder->waitForTicket(opCtx, &admCtx, waitMode);
            state.PauseTiming();
            sleepmicros(1);
            acquired++;
            state.ResumeTiming();
        }
    }
    state.counters["Acquired"] = benchmark::Counter(acquired, benchmark::Counter::kIsRate);
    state.counters["AcquiredPerThread"] =
        benchmark::Counter(acquired, benchmark::Counter::kAvgThreadsRate);
    if (state.thread_index == 0) {
        ticketHolder.reset();
        serviceContext.reset();
        isReady = false;
    }
}

// The 'AdmissionsPriority' has no effect on SemaphoreTicketHolder performance because the
// SemaphoreTicketHolder treaats all operations the same, regardless of their specified priority.
// However, the benchmarks between the SemaphoreTicketHolder and the PriorityTicketHolder are only
// comparable when all admissions are of normal priority.
BENCHMARK_TEMPLATE(BM_acquireAndRelease, SemaphoreTicketHolder, AdmissionsPriority::kNormal)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
    ->Threads(kThreadMax);

BENCHMARK_TEMPLATE(BM_acquireAndRelease, PriorityTicketHolder, AdmissionsPriority::kNormal)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
    ->Threads(kThreadMax);

// Low priority operations are expected to take longer to acquire a ticket because they are forced
// to take a slower path than normal priority operations.
BENCHMARK_TEMPLATE(BM_acquireAndRelease, PriorityTicketHolder, AdmissionsPriority::kLow)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
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
    ->Threads(kThreadMax);


}  // namespace
}  // namespace mongo

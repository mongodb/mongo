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
static TicketHolder::WaitMode waitMode = TicketHolder::WaitMode::kUninterruptible;

template <typename TicketHolderImpl>
class TicketHolderFixture {
public:
    std::unique_ptr<TicketHolder> ticketHolder;
    std::vector<ServiceContext::UniqueClient> clients;
    std::vector<ServiceContext::UniqueOperationContext> opCtxs;


    TicketHolderFixture(int threads, ServiceContext* serviceContext) {
        ticketHolder = std::make_unique<TicketHolderImpl>(kTickets, serviceContext);
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

template <class TicketHolderImpl>
void BM_acquireAndRelease(benchmark::State& state) {
    static std::unique_ptr<TicketHolderFixture<TicketHolderImpl>> readTicketHolder;
    static std::unique_ptr<TicketHolderFixture<TicketHolderImpl>> writeTicketHolder;
    static ServiceContext::UniqueServiceContext serviceContext;
    {
        stdx::unique_lock lk(isReadyMutex);
        if (state.thread_index == 0) {
            serviceContext = ServiceContext::make();
            serviceContext->setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
            serviceContext->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
            readTicketHolder = std::make_unique<TicketHolderFixture<TicketHolderImpl>>(
                state.threads, serviceContext.get());
            writeTicketHolder = std::make_unique<TicketHolderFixture<TicketHolderImpl>>(
                state.threads, serviceContext.get());
            isReady = true;
            isReadyCv.notify_all();
        } else {
            isReadyCv.wait(lk, [&] { return isReady; });
        }
    }
    double acquired = 0;
    auto mode = (state.thread_index % 2) == 0 ? MODE_IS : MODE_IX;
    auto priority = (state.thread_index % 2) == 0 ? AdmissionContext::Priority::kLow
                                                  : AdmissionContext::Priority::kNormal;
    TicketHolderFixture<TicketHolderImpl>* fixture;
    fixture = (mode == MODE_IS ? readTicketHolder : writeTicketHolder).get();
    for (auto _ : state) {
        AdmissionContext admCtx;
        admCtx.setLockMode(mode);
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
        readTicketHolder.reset();
        writeTicketHolder.reset();
        serviceContext.reset();
        isReady = false;
    }
}

BENCHMARK_TEMPLATE(BM_acquireAndRelease, SemaphoreTicketHolder)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
    ->Threads(kThreadMax);

BENCHMARK_TEMPLATE(BM_acquireAndRelease, PriorityTicketHolder)
    ->Threads(kThreadMin)
    ->Threads(kTickets)
    ->Threads(kThreadMax);

}  // namespace
}  // namespace mongo

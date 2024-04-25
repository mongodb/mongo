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

#include "mongo/util/concurrency/semaphore_ticketholder.h"

#include "mongo/util/concurrency/ticketholder.h"
#include <memory>

#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/ticketholder_test_fixture.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#define ASSERT_SOON_EXP(exp)                         \
    if (!(exp)) {                                    \
        LOGV2_WARNING(8896601,                       \
                      "Expression failed, retrying", \
                      "exp"_attr = #exp,             \
                      "file"_attr = __FILE__,        \
                      "line"_attr = __LINE__);       \
        return false;                                \
    }

namespace {
using namespace mongo;

// Windows test variants are sometimes slow so we have a relatively large timeout here for the
// assertSoon.
static inline const Seconds kWaitTimeout{20};
static inline const Milliseconds kSleepTime{1};

/**
 * Asserts that eventually the predicate does not throw an exception.
 */
void assertSoon(std::function<bool()> predicate, Milliseconds timeout = kWaitTimeout) {
    Timer t;
    while (!predicate()) {
        if (t.elapsed() >= timeout) {
            LOGV2_ERROR(8896602,
                        "assertSoon failed, please check the logs for the reason all attempts have "
                        "failed.");
            ASSERT_TRUE(false);
        }
        sleepFor(kSleepTime);
    }
}

class SemaphoreTicketHolderTest : public TicketHolderTestFixture {
public:
    void setUp() override {
        TicketHolderTestFixture::setUp();

        auto tickSource = std::make_unique<TickSourceMock<Microseconds>>();
        _tickSource = tickSource.get();
        getServiceContext()->setTickSource(std::move(tickSource));
    }

    TickSourceMock<Microseconds>* getTickSource() {
        return _tickSource;
    }

private:
    TickSourceMock<Microseconds>* _tickSource;
};

TEST_F(SemaphoreTicketHolderTest, BasicTimeoutSemaphore) {
    basicTimeout(
        _opCtx.get(),
        std::make_unique<SemaphoreTicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */));
}

TEST_F(SemaphoreTicketHolderTest, ResizeStatsSemaphore) {
    resizeTest(
        _opCtx.get(),
        std::make_unique<SemaphoreTicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */),
        getTickSource());
}

TEST_F(SemaphoreTicketHolderTest, Interruption) {
    interruptTest(
        _opCtx.get(),
        std::make_unique<SemaphoreTicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */));
}

TEST_F(SemaphoreTicketHolderTest, InterruptResize) {
    auto ticketHolder =
        std::make_unique<SemaphoreTicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */);

    _opCtx->markKilled(ErrorCodes::ClientMarkedKilled);
    ASSERT_THROWS_CODE(
        ticketHolder->resize(_opCtx.get(), 0), DBException, ErrorCodes::ClientMarkedKilled);
}

TEST_F(SemaphoreTicketHolderTest, PriorityBookkeeping) {
    priorityBookkeepingTest(
        _opCtx.get(),
        std::make_unique<SemaphoreTicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */),
        AdmissionContext::Priority::kNormal,
        AdmissionContext::Priority::kExempt,
        [](auto statsWhileProcessing, auto statsWhenFinished) {
            ASSERT_EQ(statsWhileProcessing.getObjectField("normalPriority")
                          .getIntField("startedProcessing"),
                      0);
            ASSERT_EQ(
                statsWhileProcessing.getObjectField("exempt").getIntField("startedProcessing"), 1);
            ASSERT_EQ(statsWhenFinished.getObjectField("normalPriority")
                          .getIntField("finishedProcessing"),
                      0);
            ASSERT_EQ(statsWhenFinished.getObjectField("exempt").getIntField("finishedProcessing"),
                      1);
        });
}

TEST_F(SemaphoreTicketHolderTest, ImmediateResize) {
    auto holder =
        std::make_unique<SemaphoreTicketHolder>(getServiceContext(),
                                                1,
                                                false /* trackPeakUsed */,
                                                SemaphoreTicketHolder::ResizePolicy::kImmediate);

    Stats stats(holder.get());
    MockAdmissionContext admCtx{};

    // This mutex is to avoid data race conditions between checking for the ticket state and setting
    // it in the worker threads.
    Mutex ticketCheckMutex;

    // expand the pool to 10 tickets
    ASSERT_TRUE(holder->resize(_opCtx.get(), 10));
    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 10);
    ASSERT_EQ(holder->outof(), 10);

    // acquire 5 tickets
    std::vector<Ticket> tickets;
    for (size_t i = 0; i < 5; ++i) {
        auto ticket = holder->waitForTicket(_opCtx.get(), &admCtx);
        tickets.push_back(std::move(ticket));
    }
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->available(), 5);
    ASSERT_EQ(holder->outof(), 10);

    // shrink the pool to 3 tickets
    ASSERT_TRUE(holder->resize(_opCtx.get(), 3));
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->available(), -2);
    ASSERT_EQ(holder->outof(), 3);

    // try to acquire a ticket
    ASSERT_FALSE(holder->tryAcquire(&admCtx));

    // start a thread and make it wait on a ticket
    MockAdmission releaseWaiterAdmission{getServiceContext(), AdmissionContext::Priority::kNormal};
    stdx::thread releaseWaiter([&] {
        auto ticket = holder->waitForTicket(releaseWaiterAdmission.opCtx.get(),
                                            &releaseWaiterAdmission.admCtx);
        stdx::lock_guard lk(ticketCheckMutex);
        releaseWaiterAdmission.ticket = std::move(ticket);
    });
    assertSoon([&]() { return holder->queued() >= 1; });

    // release 3 tickets
    tickets.erase(tickets.end() - 3, tickets.end());

    // check that the waiter acquired a ticket
    assertSoon([&] {
        stdx::lock_guard lk(ticketCheckMutex);
        ASSERT_SOON_EXP(releaseWaiterAdmission.ticket);
        return true;
    });
    releaseWaiter.join();
    ASSERT_EQ(holder->used(), 3);
    ASSERT_EQ(holder->available(), 0);
    ASSERT_EQ(holder->outof(), 3);

    // shrink the pool to 1 ticket
    ASSERT_TRUE(holder->resize(_opCtx.get(), 1));
    ASSERT_EQ(holder->used(), 3);
    ASSERT_EQ(holder->available(), -2);
    ASSERT_EQ(holder->outof(), 1);

    // start 3 threads and make them wait on tickets
    MockAdmission resizeWaiterAdmission1{getServiceContext(), AdmissionContext::Priority::kNormal};
    stdx::thread resizeWaiter1([&] {
        auto ticket = holder->waitForTicket(resizeWaiterAdmission1.opCtx.get(),
                                            &resizeWaiterAdmission1.admCtx);
        stdx::lock_guard lk(ticketCheckMutex);
        resizeWaiterAdmission1.ticket = std::move(ticket);
    });
    MockAdmission resizeWaiterAdmission2{getServiceContext(), AdmissionContext::Priority::kNormal};
    stdx::thread resizeWaiter2([&] {
        auto ticket = holder->waitForTicket(resizeWaiterAdmission2.opCtx.get(),
                                            &resizeWaiterAdmission2.admCtx);
        stdx::lock_guard lk(ticketCheckMutex);
        resizeWaiterAdmission2.ticket = std::move(ticket);
    });
    MockAdmission resizeWaiterAdmission3{getServiceContext(), AdmissionContext::Priority::kNormal};
    stdx::thread resizeWaiter3([&] {
        auto ticket = holder->waitForTicket(resizeWaiterAdmission3.opCtx.get(),
                                            &resizeWaiterAdmission3.admCtx);
        stdx::lock_guard lk(ticketCheckMutex);
        resizeWaiterAdmission3.ticket = std::move(ticket);
    });
    assertSoon([&]() { return holder->queued() >= 3; });

    // grow the pool to 5
    ASSERT_TRUE(holder->resize(_opCtx.get(), 5));

    auto countAcquiredTickets = [&] {
        stdx::lock_guard lk(ticketCheckMutex);
        size_t numAcquired = 0;
        if (resizeWaiterAdmission1.ticket)
            numAcquired++;
        if (resizeWaiterAdmission2.ticket)
            numAcquired++;
        if (resizeWaiterAdmission3.ticket)
            numAcquired++;
        return numAcquired;
    };

    // check that 2 of the 3 waiters acquired a ticket
    assertSoon([&] {
        auto numAcquired = countAcquiredTickets();
        ASSERT_SOON_EXP(numAcquired == 2);
        return true;
    });
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->available(), 0);
    ASSERT_EQ(holder->outof(), 5);
    ASSERT_EQ(holder->queued(), 1);

    // release a ticket
    releaseWaiterAdmission.ticket.reset();

    // check that all 3 waiters acquired a ticket
    assertSoon([&] {
        auto numAcquired = countAcquiredTickets();
        ASSERT_SOON_EXP(numAcquired == 3);
        return true;
    });

    // clean up waiters
    resizeWaiter1.join();
    resizeWaiter2.join();
    resizeWaiter3.join();
}

TEST_F(SemaphoreTicketHolderTest, ReleaseToPoolWakesWaiters) {
    // We had a bug where releasing a ticket back to the ticket holder would only waker waiters when
    // adding a ticket would result in 0 available tickets (a case only reachable after resize).
    // This test is meant to prove that we always wake waiters when a ticket is returned, if there
    // are available tickets

    auto holder =
        std::make_unique<SemaphoreTicketHolder>(getServiceContext(),
                                                2,
                                                false /* trackPeakUsed */,
                                                SemaphoreTicketHolder::ResizePolicy::kImmediate);

    // Here's the approach: We need to have a SemaphoreTicketHolder of size >1 in order to meet the
    // condition that we possibly have a non-zero number of tickets when returning a ticket to the
    // pool. Initially acquire two tickets, and spin up two waiters which will queue. A third
    // waiting thread waits for the initial waiters to queue before enqueueing itself. Back on the
    // main thread, wait for all three queued waiters before returning two tickets to the pool
    // immediately.

    MockAdmissionContext admCtx{};
    std::vector<Ticket> tickets;
    for (size_t i = 0; i < 2; ++i) {
        auto ticket = holder->waitForTicket(_opCtx.get(), &admCtx);
        tickets.push_back(std::move(ticket));
    }

    std::vector<stdx::thread> threads;
    for (size_t i = 0; i < 3; ++i) {
        threads.emplace_back([&] {
            MockAdmission admission{getServiceContext(), AdmissionContext::Priority::kNormal};
            auto ticket = holder->waitForTicket(admission.opCtx.get(), &admission.admCtx);
        });
    }

    // await 3 queued waiters, and then return 2 tickets to the pool
    assertSoon([&] { return holder->queued() == 3; });
    tickets.erase(tickets.end() - 2, tickets.end());

    // join all threads and drain the waiters one-by-one
    for (auto& t : threads) {
        t.join();
    }
}

}  // namespace

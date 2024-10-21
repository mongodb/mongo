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
#include <memory>

#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/semaphore_ticketholder.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/concurrency/ticketholder_test_fixture.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_util.h"
#include "mongo/util/packaged_task.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace {
using namespace mongo;

// Timeout to use to ensure that waiters get queued and/or receive tickets.
// We use this timeout so we can bail-out early and fail with a better diagnostic when we appear to
// be hanging on such a wait, rather than waiting for the test infrastructure to kill us. Windows
// test variants are sometimes slow so we have a relatively large timeout here.
constexpr auto kDefaultTimeout = Minutes{1};

Date_t getNextDeadline() {
    return Date_t::now() + kDefaultTimeout;
}

class SemaphoreTicketHolderTest : public TicketHolderTestFixture {
public:
    void setUp() override {
        TicketHolderTestFixture::setUp();

        ThreadPool::Options opts;
        _pool = std::make_unique<ThreadPool>(opts);
        _pool->startup();
    }

    void tearDown() override {
        TicketHolderTestFixture::tearDown();
        _pool->shutdown();
        //_pool->join();
    }

    /**
     * Utility that schedules `cb` to run on an executor thread managed by the fixture.
     * Returns a Future that is a handle to the return-value of the callback.
     * The fixture's executor threads are joined at fixture tear-down.
     */
    template <typename Callable>
    auto spawn(Callable&& cb) -> Future<typename std::invoke_result<Callable>::type> {
        using ReturnType = typename std::invoke_result<Callable>::type;
        auto task = PackagedTask([cb = std::move(cb)] { return cb(); });
        auto taskFuture = task.getFuture();
        _pool->schedule([runTask = std::move(task)](Status s) mutable {
            invariant(s);
            runTask();
        });
        return taskFuture;
    }

    /** Utility to make a immediate-resize-policy SemaphoreTicketHolder. */
    std::unique_ptr<SemaphoreTicketHolder> makeImmediateResizeHolder(int initialNumTickets) {
        return std::make_unique<SemaphoreTicketHolder>(
            getServiceContext(),
            initialNumTickets,
            false /* trackPeakUsed */,
            SemaphoreTicketHolder::ResizePolicy::kImmediate);
    }

private:
    std::unique_ptr<ThreadPool> _pool;
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

TEST_F(SemaphoreTicketHolderTest, QueuedWaiterGetsTicketWhenMadeAvailable) {
    constexpr int initialNumTickets = 3;
    auto holder = makeImmediateResizeHolder(initialNumTickets);

    // acquire all 3 tickets
    std::array<MockAdmissionContext, 6> admCtxs;
    std::vector<Ticket> tickets;
    for (int i = 0; i < initialNumTickets; ++i) {
        tickets.push_back(holder->waitForTicket(_opCtx.get(), &admCtxs[i + 1]));
    }

    // try (and fail) to acquire a ticket - none are availabe.
    ASSERT_FALSE(holder->tryAcquire(&admCtxs[0]));

    // start a thread and make it wait on a ticket
    MockAdmission releaseWaiterAdmission{getServiceContext(), AdmissionContext::Priority::kNormal};
    Future<Ticket> ticketFuture = spawn([&]() {
        return holder->waitForTicket(releaseWaiterAdmission.opCtx.get(),
                                     &releaseWaiterAdmission.admCtx);
    });

    ASSERT_TRUE(releaseWaiterAdmission.waitUntilQueued(kDefaultTimeout));

    // release the 3 tickets
    tickets.erase(tickets.begin(), tickets.end());

    // check that the waiter acquired a ticket
    boost::optional<Ticket> ticket;
    _opCtx->runWithDeadline(getNextDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        ticket = std::move(ticketFuture).get(_opCtx.get());
    });
    ASSERT_EQ(holder->used(), 1);
    ASSERT_EQ(holder->available(), 2);
    ASSERT_EQ(holder->outof(), 3);
}

using SemaphoreTicketHolderImmediateResizeTest = SemaphoreTicketHolderTest;

TEST_F(SemaphoreTicketHolderImmediateResizeTest, CanResizePool) {
    constexpr int initialNumTickets = 1;
    auto holder = makeImmediateResizeHolder(initialNumTickets);

    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 1);
    ASSERT_EQ(holder->outof(), 1);

    // expand the pool to 10 tickets
    ASSERT_TRUE(holder->resize(_opCtx.get(), 10));
    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 10);
    ASSERT_EQ(holder->outof(), 10);

    // shrink the pool to 5 tickets
    ASSERT_TRUE(holder->resize(_opCtx.get(), 5));
    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 5);
    ASSERT_EQ(holder->outof(), 5);
}

TEST_F(SemaphoreTicketHolderImmediateResizeTest, ResizeDownTicketsStillAvailable) {
    constexpr int initialNumTickets = 10;
    auto holder = makeImmediateResizeHolder(initialNumTickets);

    // acquire 5 tickets
    std::array<MockAdmissionContext, 6> admCtxs;
    std::vector<Ticket> tickets;
    for (int i = 0; i < 5; ++i) {
        tickets.push_back(holder->waitForTicket(_opCtx.get(), &admCtxs[i + 1]));
    }

    // ensure 5 are now in-use and 5 are left available
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->available(), 5);
    ASSERT_EQ(holder->outof(), 10);

    // shrink the pool to 8 tickets
    // available should still be positive, because we have more capacity (8) than tickets in use (5)
    ASSERT_TRUE(holder->resize(_opCtx.get(), 8));
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->available(), 3);
    ASSERT_EQ(holder->outof(), 8);

    // We can get a ticket as some are still available
    ASSERT_TRUE(holder->tryAcquire(&admCtxs[0]));
}

TEST_F(SemaphoreTicketHolderImmediateResizeTest, ResizeDownSoNoTicketsAvailable) {
    constexpr int initialNumTickets = 10;
    auto holder = makeImmediateResizeHolder(initialNumTickets);

    // acquire 5 tickets
    std::array<MockAdmissionContext, 6> admCtxs;
    std::vector<Ticket> tickets;
    for (int i = 0; i < 5; ++i) {
        tickets.push_back(holder->waitForTicket(_opCtx.get(), &admCtxs[i + 1]));
    }

    // ensure 5 are now in-use and 5 are left available
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->available(), 5);
    ASSERT_EQ(holder->outof(), 10);

    // We can get a ticket when one is available
    ASSERT_TRUE(holder->tryAcquire(&admCtxs[0]));

    // shrink the pool to 3 tickets
    // available should now be negative because more are in use (5) than the total capacity (3)
    ASSERT_TRUE(holder->resize(_opCtx.get(), 3));
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->available(), -2);
    ASSERT_EQ(holder->outof(), 3);

    // When < 0 tickets are availabe, we shouldn't be able to acquire one.
    ASSERT_FALSE(holder->tryAcquire(&admCtxs[0]));
}

TEST_F(SemaphoreTicketHolderImmediateResizeTest, ResizeUpMakesTicketsAvailableToWaiters) {
    constexpr int initialNumTickets = 3;
    auto holder = makeImmediateResizeHolder(initialNumTickets);

    // acquire all 3 tickets
    std::array<MockAdmissionContext, 6> admCtxs;
    std::vector<Ticket> tickets;
    for (int i = 0; i < initialNumTickets; ++i) {
        tickets.push_back(holder->waitForTicket(_opCtx.get(), &admCtxs[i]));
    }

    // try (and fail) to acquire a ticket - none are availabe.
    ASSERT_FALSE(holder->tryAcquire(&admCtxs[0]));

    // start a thread and make it wait on a ticket
    MockAdmission releaseWaiterAdmission{getServiceContext(), AdmissionContext::Priority::kNormal};
    Future<Ticket> ticketFuture = spawn([&]() {
        return holder->waitForTicket(releaseWaiterAdmission.opCtx.get(),
                                     &releaseWaiterAdmission.admCtx);
    });

    ASSERT_TRUE(releaseWaiterAdmission.waitUntilQueued(kDefaultTimeout));

    // grow the pool to 5
    ASSERT_TRUE(holder->resize(_opCtx.get(), 5));

    // check that the waiter acquired a ticket
    boost::optional<Ticket> ticket;
    _opCtx->runWithDeadline(getNextDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        ticket = std::move(ticketFuture).get(_opCtx.get());
    });

    ASSERT_EQ(holder->used(), 4);
    ASSERT_EQ(holder->available(), 1);
    ASSERT_EQ(holder->outof(), 5);
}

TEST_F(SemaphoreTicketHolderImmediateResizeTest,
       QueuedWaiterRemainsNegativeAfterReleaseToPoolWithNegativeAvailable) {
    constexpr int initialNumTickets = 10;
    auto holder = makeImmediateResizeHolder(initialNumTickets);

    // acquire 5 tickets
    std::array<MockAdmissionContext, 5> admCtxs;
    std::vector<Ticket> tickets;
    for (int i = 0; i < 5; ++i) {
        tickets.push_back(holder->waitForTicket(_opCtx.get(), &admCtxs[i]));
    }

    // ensure 5 are now in-use and 5 are left available
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->available(), 5);
    ASSERT_EQ(holder->outof(), 10);

    // shrink the pool to 3 tickets
    // available should now be negative because more are in use (5) than the total capacity (3)
    ASSERT_TRUE(holder->resize(_opCtx.get(), 3));
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->available(), -2);
    ASSERT_EQ(holder->outof(), 3);

    // start a thread and make it wait on a ticket
    MockAdmission releaseWaiterAdmission{getServiceContext(), AdmissionContext::Priority::kNormal};
    Future<Ticket> ticketFuture = spawn([&]() {
        return holder->waitForTicket(releaseWaiterAdmission.opCtx.get(),
                                     &releaseWaiterAdmission.admCtx);
    });

    ASSERT_TRUE(releaseWaiterAdmission.waitUntilQueued(kDefaultTimeout));
    // If we return a ticket, the waiter should stay asleep becuase the availabe count is still < 0
    tickets.erase(tickets.end() - 1, tickets.end());
    ASSERT_FALSE(ticketFuture.isReady());
    // Same should be true when availabe count == 0 after the release .
    tickets.erase(tickets.end() - 1, tickets.end());
    ASSERT_FALSE(ticketFuture.isReady());
    // But once it becomes positive, the waiter should wake up .
    tickets.erase(tickets.end() - 1, tickets.end());
    _opCtx->runWithDeadline(getNextDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        std::move(ticketFuture).get(_opCtx.get());
    });
}

TEST_F(SemaphoreTicketHolderTest, ReleaseToPoolWakesWaiters) {
    // We had a bug where releasing a ticket back to the ticket holder would only waker waiters when
    // adding a ticket would result in 0 available tickets (a case only reachable after resize).
    // This test is meant to prove that we always wake waiters when a ticket is returned, if there
    // are available tickets
    constexpr int initialNumTickets = 2;
    auto holder = makeImmediateResizeHolder(initialNumTickets);

    // Here's the approach: We need to have a SemaphoreTicketHolder of size >1 in order to meet the
    // condition that we possibly have a non-zero number of tickets when returning a ticket to the
    // pool. Initially acquire two tickets, and spin up two waiters which will queue. A third
    // waiting thread waits for the initial waiters to queue before enqueueing itself. Back on the
    // main thread, wait for all three queued waiters before returning two tickets to the pool
    // immediately.
    std::array<MockAdmissionContext, 5> admCtxs;
    std::vector<Ticket> tickets;
    for (int i = 0; i < initialNumTickets; ++i) {
        tickets.push_back(holder->waitForTicket(_opCtx.get(), &admCtxs[i]));
    }

    std::vector<Future<Ticket>> ticketFutures;
    // Prepare 3 admisisons that will queue.
    std::vector<std::unique_ptr<MockAdmission>> admissions;
    for (size_t i = 0; i < 3; ++i) {
        auto admission = std::make_unique<MockAdmission>(getServiceContext(),
                                                         AdmissionContext::Priority::kNormal);
        admissions.push_back(std::move(admission));
    }

    //  Enqueue the 3 admissions.
    for (size_t i = 0; i < 3; ++i) {
        auto* admission = admissions[i].get();
        AdmissionContext* admCtx = &admission->admCtx;
        Future<Ticket> ticketFuture =
            spawn([holder = holder.get(), opCtx = admission->opCtx.get(), admCtx = admCtx]() {
                Timer t;
                // TODO(SERVER-89297): SemaphoreTicketHolder currently does timed waits in the 500ms
                // range, which prevents deadlock with the bug this test is meant to test. Remove
                // this assertion when we switch to the NotifyableParkingLot. Choose a value of 400
                // because the timed waiters introduce jitter for each wait using a base value of
                // 500ms.
                auto ticket = holder->waitForTicket(opCtx, admCtx);
                ASSERT_LTE(t.millis(), 400);
                return ticket;
            });
        ticketFutures.push_back(std::move(ticketFuture));
    }

    // Wait for 3 queued waiters, and then return 2 tickets to the pool
    for (auto& admission : admissions) {
        ASSERT_TRUE(admission->waitUntilQueued(kDefaultTimeout));
    }

    tickets.erase(tickets.end() - 2, tickets.end());

    // Ensure that the released tickets wake waiters.
    std::vector<Future<void>> eachDone;
    for (auto&& ticket : ticketFutures) {
        auto done = std::move(ticket).then([](Ticket ticket) {
            // Let the ticket get destroyed, returning it to the pool
            return;
        });
        eachDone.push_back(std::move(done));
    }
    SemiFuture<void> allDone = whenAllSucceed(std::move(eachDone));
    _opCtx->runWithDeadline(
        getNextDeadline(), ErrorCodes::ExceededTimeLimit, [&] { allDone.get(_opCtx.get()); });
}
}  // namespace

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
#include "mongo/stdx/thread.h"
#include <concepts>
#include <memory>

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_util.h"
#include "mongo/util/packaged_task.h"
#include "mongo/util/tick_source_mock.h"

#include "mongo/unittest/assert.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace {
using namespace mongo;
using namespace std::literals;

// Timeout to use to ensure that waiters get queued and/or receive tickets.
// We use this timeout so we can bail-out early and fail with a better diagnostic when we appear to
// be hanging on such a wait, rather than waiting for the test infrastructure to kill us. Windows
// test variants are sometimes slow so we have a relatively large timeout here.
constexpr auto kDefaultTimeout = Minutes{1};

Date_t getNextDeadline() {
    return Date_t::now() + kDefaultTimeout;
}

class TicketHolderTest : public ServiceContextTest {
public:
    TicketHolderTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
                  nullptr, nullptr, std::make_unique<TickSourceMock<Microseconds>>()))) {}

    static inline const Milliseconds kSleepTime{1};

    void setUp() override {
        ServiceContextTest::setUp();
        _client = getServiceContext()->getService()->makeClient("test");
        _opCtx = _client->makeOperationContext();

        ThreadPool::Options opts;
        _pool = std::make_unique<ThreadPool>(opts);
        _pool->startup();
    }

    void tearDown() override {
        _pool->shutdown();
        ServiceContextTest::tearDown();
    }

    TickSourceMock<Microseconds>* getTickSource() {
        return checked_cast<decltype(getTickSource())>(getServiceContext()->getTickSource());
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

    /** Utility to make a immediate-resize-policy TicketHolder. */
    std::unique_ptr<TicketHolder> makeImmediateResizeHolder(int initialNumTickets) {
        return std::make_unique<TicketHolder>(getServiceContext(),
                                              initialNumTickets,
                                              false /* trackPeakUsed */,
                                              TicketHolder::ResizePolicy::kImmediate);
    }

    template <std::invocable Predicate>
    void waitUntilCanceled(OperationContext& opCtx, Predicate predicate) {
        while (opCtx.checkForInterruptNoAssert() == Status::OK()) {
            if (predicate())
                return;
            sleepFor(kSleepTime);
        }
        ASSERT(false);
    }

protected:
    class Stats;
    struct MockAdmission;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;

private:
    std::unique_ptr<ThreadPool> _pool;
};

/**
 * Provides easy access to instantaneous statistics of the TicketHolder.
 */
class TicketHolderTest::Stats {
public:
    Stats(TicketHolder* holder) : _holder(holder){};

    long long operator[](StringData field) const {
        BSONObjBuilder bob;
        _holder->appendStats(bob);
        auto stats = bob.obj();
        return stats[field].numberLong();
    }

    BSONObj getStats() const {
        BSONObjBuilder bob;
        _holder->appendStats(bob);
        return bob.obj();
    }

    BSONObj getNonTicketStats() const {
        return getStats().removeField("out").removeField("available").removeField("totalTickets");
    }

private:
    TicketHolder* _holder;
};

/**
 * Constructs the context necessary to submit a for-test admission to a TicketHolder.
 */
struct TicketHolderTest::MockAdmission {
    MockAdmission(ServiceContext* serviceContext, AdmissionContext::Priority priority) {
        client = serviceContext->getService()->makeClient("");
        opCtx = client->makeOperationContext();
        admissionPriority.emplace(opCtx.get(), admCtx, priority);
    }

    // Block until this Admission attempt is queued waiting on a ticket.
    bool waitUntilQueued(Nanoseconds timeout) {
        return admCtx.waitUntilQueued(timeout);
    }

    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
    MockAdmissionContext admCtx;
    boost::optional<ScopedAdmissionPriorityBase> admissionPriority;
    boost::optional<Ticket> ticket;
};

TEST_F(TicketHolderTest, BasicTimeout) {
    auto holder = std::make_unique<TicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */);
    OperationContext* opCtx = _opCtx.get();
    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 1);
    ASSERT_EQ(holder->outof(), 1);

    MockAdmissionContext admCtx{};
    {
        // Ignores deadline if there is a ticket instantly available.
        auto ticket = holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() - Milliseconds(100));
        ASSERT(ticket);
        ASSERT_EQ(holder->used(), 1);
        ASSERT_EQ(holder->available(), 0);
        ASSERT_EQ(holder->outof(), 1);

        // Respects there are none available.
        ASSERT_FALSE(holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now()));
        ASSERT_FALSE(holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds(42)));
    }

    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 1);
    ASSERT_EQ(holder->outof(), 1);
}

/**
 * Tests that TicketHolder::resize() does not impact metrics outside of those related to the
 * number of tickets available(), used(), and outof().
 */
TEST_F(TicketHolderTest, ResizeStats) {
    auto holder = std::make_unique<TicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */);
    OperationContext* opCtx = _opCtx.get();
    auto tickSource = getTickSource();
    Stats stats(holder.get());

    std::array<MockAdmissionContext, 6> admCtxs;
    auto ticket = holder->waitForTicketUntil(opCtx, &admCtxs[0], Date_t::now() + Milliseconds{500});

    ASSERT_EQ(holder->used(), 1);
    ASSERT_EQ(holder->available(), 0);
    ASSERT_EQ(holder->outof(), 1);

    auto currentStats = stats.getNonTicketStats();

    tickSource->advance(Microseconds{100});
    ASSERT_TRUE(holder->resize(opCtx, 10));

    ASSERT_EQ(holder->available(), 9);
    ASSERT_EQ(holder->outof(), 10);

    auto newStats = stats.getNonTicketStats();

    ASSERT_EQ(currentStats.woCompare(newStats), 0);

    tickSource->advance(Microseconds{100});
    ticket.reset();

    currentStats = stats.getNonTicketStats();

    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 10);
    ASSERT_EQ(stats["totalTickets"], 10);

    ASSERT_TRUE(holder->resize(opCtx, 1));
    newStats = stats.getNonTicketStats();
    ASSERT_EQ(currentStats.woCompare(newStats), 0);

    tickSource->advance(Microseconds{100});
    ASSERT_TRUE(holder->resize(opCtx, 10));
    currentStats = stats.getNonTicketStats();
    ASSERT_EQ(currentStats.woCompare(newStats), 0);

    ASSERT_TRUE(holder->resize(opCtx, 6));
    std::array<boost::optional<Ticket>, 5> tickets;
    {
        auto ticket = holder->waitForTicket(opCtx, &admCtxs[0]);
        ASSERT_EQ(holder->used(), 1);
        ASSERT_EQ(holder->outof(), 6);

        for (int i = 0; i < 5; ++i) {
            tickets[i] = holder->waitForTicket(opCtx, &admCtxs[i + 1]);
            ASSERT_EQ(holder->used(), 2 + i);
            ASSERT_EQ(holder->outof(), 6);
        }
        ASSERT_FALSE(
            holder->waitForTicketUntil(opCtx, &admCtxs[0], Date_t::now() + Milliseconds(1)));
    }

    ASSERT_TRUE(holder->resize(opCtx, 5));
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->outof(), 5);
    ASSERT_FALSE(holder->waitForTicketUntil(opCtx, &admCtxs[0], Date_t::now() + Milliseconds(1)));

    ASSERT_FALSE(holder->resize(opCtx, 4, Date_t::now() + Milliseconds(1)));
}

TEST_F(TicketHolderTest, Interruption) {
    auto holder = std::make_unique<TicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */);
    OperationContext* opCtx = _opCtx.get();

    ASSERT_TRUE(holder->resize(opCtx, 0));

    auto waiter = stdx::thread([&]() {
        MockAdmissionContext admCtx{};
        ASSERT_THROWS_CODE(holder->waitForTicketUntil(opCtx, &admCtx, Date_t::max()),
                           DBException,
                           ErrorCodes::Interrupted);
    });

    while (!holder->queued()) {
    }

    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 0);

    opCtx->markKilled();
    waiter.join();
}

TEST_F(TicketHolderTest, InterruptResize) {
    auto ticketHolder =
        std::make_unique<TicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */);

    _opCtx->markKilled(ErrorCodes::ClientMarkedKilled);
    ASSERT_THROWS_CODE(
        ticketHolder->resize(_opCtx.get(), 0), DBException, ErrorCodes::ClientMarkedKilled);
}

TEST_F(TicketHolderTest, PriorityBookkeeping) {
    auto holder = std::make_unique<TicketHolder>(getServiceContext(), 1, false /* trackPeakUsed */);
    OperationContext* opCtx = _opCtx.get();
    MockAdmissionContext admCtx{};
    ScopedAdmissionPriorityBase initialPriority{opCtx, admCtx, AdmissionContext::Priority::kNormal};

    Stats stats(holder.get());

    boost::optional<ScopedAdmissionPriorityBase> priorityOverride;
    priorityOverride.emplace(opCtx, admCtx, AdmissionContext::Priority::kExempt);

    boost::optional<Ticket> ticket = holder->waitForTicket(opCtx, &admCtx);

    priorityOverride.reset();

    auto statsWhileProcessing = stats.getStats();
    ticket.reset();
    auto statsWhenFinished = stats.getStats();

    // The ticket must be released with the same priority with which it was acquired.
    ASSERT_EQ(
        statsWhileProcessing.getObjectField("normalPriority").getIntField("startedProcessing"), 0);
    ASSERT_EQ(statsWhileProcessing.getObjectField("exempt").getIntField("startedProcessing"), 1);
    ASSERT_EQ(statsWhenFinished.getObjectField("normalPriority").getIntField("finishedProcessing"),
              0);

    ASSERT_EQ(statsWhenFinished.getObjectField("exempt").getIntField("finishedProcessing"), 1);
}

TEST_F(TicketHolderTest, QueuedWaiterGetsTicketWhenMadeAvailable) {
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

using TicketHolderImmediateResizeTest = TicketHolderTest;

TEST_F(TicketHolderImmediateResizeTest, CanResizePool) {
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

TEST_F(TicketHolderImmediateResizeTest, ResizeDownTicketsStillAvailable) {
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

TEST_F(TicketHolderImmediateResizeTest, ResizeDownSoNoTicketsAvailable) {
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

TEST_F(TicketHolderImmediateResizeTest, ResizeUpMakesTicketsAvailableToWaiters) {
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

TEST_F(TicketHolderImmediateResizeTest,
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

TEST_F(TicketHolderTest, ReleaseToPoolWakesWaiters) {
    // We had a bug where releasing a ticket back to the ticket holder would only waker waiters when
    // adding a ticket would result in 0 available tickets (a case only reachable after resize).
    // This test is meant to prove that we always wake waiters when a ticket is returned, if there
    // are available tickets
    constexpr int initialNumTickets = 2;
    auto holder = makeImmediateResizeHolder(initialNumTickets);

    // Here's the approach: We need to have a TicketHolder of size >1 in order to meet the
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
                // TODO(SERVER-89297): TicketHolder currently does timed waits in the 500ms
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

TEST_F(TicketHolderImmediateResizeTest, WaitQueueMax0) {
    constexpr int initialNumTickets = 4;
    constexpr int maxNumberOfWaiters = 0;
    auto holder = std::make_unique<TicketHolder>(getServiceContext(),
                                                 initialNumTickets,
                                                 false /* trackPeakUsed */,
                                                 TicketHolder::ResizePolicy::kImmediate,
                                                 maxNumberOfWaiters);

    // acquire 4 tickets
    std::array<MockAdmissionContext, 6> admCtxs;
    std::array<boost::optional<Ticket>, 4> tickets;
    for (int i = 0; i < 4; ++i) {
        tickets[i] = holder->waitForTicket(_opCtx.get(), &admCtxs[i + 1]);
    }

    // ensure 4 are now in-use and 0 are left available
    ASSERT_EQ(holder->used(), 4);
    ASSERT_EQ(holder->available(), 0);
    ASSERT_EQ(holder->outof(), 4);

    // Since no thread can be waiting for a ticket, it will cause waiting
    ASSERT_THROWS(holder->waitForTicket(_opCtx.get(), &admCtxs[0]),
                  ExceptionFor<ErrorCodes::AdmissionQueueOverflow>);

    // Releasing the tickets, making all tickets available in the process
    tickets = {};

    // ensure 0 are now in-use and 4 are left available
    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 4);
    ASSERT_EQ(holder->outof(), 4);
}

TEST_F(TicketHolderImmediateResizeTest, WaitQueueMax1) {
    constexpr int initialNumTickets = 4;
    constexpr int maxNumberOfWaiters = 1;
    auto holder = std::make_unique<TicketHolder>(getServiceContext(),
                                                 initialNumTickets,
                                                 false /* trackPeakUsed */,
                                                 TicketHolder::ResizePolicy::kImmediate,
                                                 maxNumberOfWaiters);

    // acquire 4 tickets
    std::array<MockAdmissionContext, 6> admCtxs;
    std::array<boost::optional<Ticket>, 4> tickets;
    for (int i = 0; i < 4; ++i) {
        tickets[i] = holder->waitForTicket(_opCtx.get(), &admCtxs[i + 1]);
    }

    // ensure 4 are now in-use and 0 are left available
    ASSERT_EQ(holder->used(), 4);
    ASSERT_EQ(holder->available(), 0);
    ASSERT_EQ(holder->outof(), 4);

    // We aquire a ticket in another thread.
    // Since no ticket available, it will cause a blocking wait on that thread.
    Future<Ticket> ticketFuture =
        spawn([&]() { return holder->waitForTicket(_opCtx.get(), &admCtxs[5]); });

    // We wait until ticketFuture is actually waiting for the ticket or until timeout exceeded
    _opCtx->runWithDeadline(getNextDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        waitUntilCanceled(*_opCtx, [&] { return holder->waiting_forTest() == 1; });
    });

    // Since the maximum amount of ticket is one, and one is already waiting, it will throw
    ASSERT_THROWS(holder->waitForTicketUntil(_opCtx.get(), &admCtxs[0], getNextDeadline()),
                  ExceptionFor<ErrorCodes::AdmissionQueueOverflow>);

    // Releasing the tickets, resuming the waiter
    tickets = {};

    // The fifth ticket is getting aquired after waiting
    boost::optional<Ticket> ticket;
    _opCtx->runWithDeadline(getNextDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        // We can gst a ticket when one is available
        ticket = std::move(ticketFuture).get(_opCtx.get());
    });

    // ensure 3 are now available and 1 are in-use
    // The one in use was the waiting ticket
    ASSERT_EQ(holder->used(), 1);
    ASSERT_EQ(holder->available(), 3);
    ASSERT_EQ(holder->outof(), 4);

    // Releasing the fifth ticket
    ticket.reset();

    // ensure 4 are now available and 0 are in-use
    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 4);
    ASSERT_EQ(holder->outof(), 4);
}
}  // namespace

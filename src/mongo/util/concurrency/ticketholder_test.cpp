/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/util/duration.h"
#include <condition_variable>
#include <mutex>

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {
using namespace mongo;

class TicketHolderTest : public ServiceContextTest {
    void setUp() override {
        ServiceContextTest::setUp();

        getServiceContext()->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
        _client = getServiceContext()->makeClient("test");
        _opCtx = _client->makeOperationContext();
    }

protected:
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
};

static inline const Seconds kWaitTimeout{2};
static inline const Milliseconds kSleepTime{1};

/**
 * Asserts that eventually the predicate does not throw an exception.
 */
void assertSoon(std::function<void()> predicate, Milliseconds timeout = kWaitTimeout) {
    Timer t;
    while (true) {
        try {
            predicate();
            break;
        } catch (...) {
            if (t.elapsed() >= timeout) {
                throw;
            }
            sleepFor(kSleepTime);
        }
    }
}

template <class H>
void basicTimeout(OperationContext* opCtx) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto mode = TicketHolder::WaitMode::kInterruptible;
    std::unique_ptr<TicketHolderWithQueueingStats> holder = std::make_unique<H>(1, &serviceContext);
    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 1);
    ASSERT_EQ(holder->outof(), 1);

    AdmissionContext admCtx;
    admCtx.setPriority(AdmissionContext::Priority::kNormal);
    {
        auto ticket = holder->waitForTicket(opCtx, &admCtx, mode);
        ASSERT_EQ(holder->used(), 1);
        ASSERT_EQ(holder->available(), 0);
        ASSERT_EQ(holder->outof(), 1);

        ASSERT_FALSE(holder->tryAcquire(&admCtx));
        ASSERT_FALSE(holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now(), mode));
        ASSERT_FALSE(
            holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds(1), mode));
        ASSERT_FALSE(
            holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds(42), mode));
    }

    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 1);
    ASSERT_EQ(holder->outof(), 1);

    {
        auto ticket = holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now(), mode);
        ASSERT(ticket);
    }

    ASSERT_EQ(holder->used(), 0);

    {
        auto ticket =
            holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds(20), mode);
        ASSERT(ticket);
        ASSERT_EQ(holder->used(), 1);
        ASSERT_FALSE(
            holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds(2), mode));
    }

    ASSERT_EQ(holder->used(), 0);

    //
    // Test resize
    //
    holder->resize(6);
    std::array<boost::optional<Ticket>, 5> tickets;
    {
        auto ticket = holder->waitForTicket(opCtx, &admCtx, mode);
        ASSERT_EQ(holder->used(), 1);
        ASSERT_EQ(holder->outof(), 6);

        for (int i = 0; i < 5; ++i) {
            tickets[i] = holder->waitForTicket(opCtx, &admCtx, mode);
            ASSERT_EQ(holder->used(), 2 + i);
            ASSERT_EQ(holder->outof(), 6);
        }

        ASSERT_FALSE(
            holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds(1), mode));
    }

    holder->resize(5);
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->outof(), 5);
    ASSERT_FALSE(holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds(1), mode));
}

TEST_F(TicketHolderTest, BasicTimeoutSemaphore) {
    basicTimeout<SemaphoreTicketHolder>(_opCtx.get());
}
TEST_F(TicketHolderTest, BasicTimeoutPriority) {
    basicTimeout<PriorityTicketHolder>(_opCtx.get());
}

class Stats {
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

// Mocks an operation submitting for ticket admission.
struct MockAdmission {
    MockAdmission(std::string name,
                  ServiceContext* serviceContext,
                  AdmissionContext::Priority priority) {
        client = serviceContext->makeClient(name);
        opCtx = client->makeOperationContext();
        admCtx.setPriority(priority);
    }

    AdmissionContext admCtx;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
    boost::optional<Ticket> ticket;
};

template <class H>
void resizeTest(OperationContext* opCtx, bool testWithOutstandingImmediateOperation = false) {
    // Verify that resize operations don't alter metrics outside of those linked to the number of
    // tickets.
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());
    auto mode = TicketHolder::WaitMode::kInterruptible;
    std::unique_ptr<TicketHolderWithQueueingStats> holder = std::make_unique<H>(1, &serviceContext);
    Stats stats(holder.get());

    // An outstanding kImmediate priority operation should not impact resize statistics.
    MockAdmission immediatePriorityAdmission("immediatePriorityAdmission",
                                             getGlobalServiceContext(),
                                             AdmissionContext::Priority::kImmediate);
    if (testWithOutstandingImmediateOperation) {
        immediatePriorityAdmission.ticket =
            holder->acquireImmediateTicket(&immediatePriorityAdmission.admCtx);
        ASSERT(immediatePriorityAdmission.ticket);
    }

    AdmissionContext admCtx;
    admCtx.setPriority(AdmissionContext::Priority::kNormal);

    auto ticket =
        holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds{500}, mode);

    ASSERT_EQ(holder->used(), 1);
    ASSERT_EQ(holder->available(), 0);
    ASSERT_EQ(holder->outof(), 1);

    auto currentStats = stats.getNonTicketStats();

    tickSource->advance(Microseconds{100});
    holder->resize(10);

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

    holder->resize(1);
    newStats = stats.getNonTicketStats();
    ASSERT_EQ(currentStats.woCompare(newStats), 0);

    tickSource->advance(Microseconds{100});
    holder->resize(10);
    currentStats = stats.getNonTicketStats();
    ASSERT_EQ(currentStats.woCompare(newStats), 0);
}

TEST_F(TicketHolderTest, ResizeStatsSemaphore) {
    resizeTest<SemaphoreTicketHolder>(_opCtx.get());
}
TEST_F(TicketHolderTest, ResizeStatsPriority) {
    resizeTest<PriorityTicketHolder>(_opCtx.get());
}
TEST_F(TicketHolderTest, ResizeStatsSemaphoreWithOutstandingImmediatePriority) {
    resizeTest<SemaphoreTicketHolder>(_opCtx.get(), true);
}
TEST_F(TicketHolderTest, ResizeStatsPriorityWithOutstandingImmediatePriority) {
    resizeTest<PriorityTicketHolder>(_opCtx.get(), true);
}

TEST_F(TicketHolderTest, PriorityTwoQueuedOperations) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    PriorityTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);

    {
        // Allocate the only available ticket. Priority is irrelevant when there are tickets
        // available.
        MockAdmission initialAdmission(
            "initialAdmission", this->getServiceContext(), AdmissionContext::Priority::kLow);
        initialAdmission.ticket = holder.waitForTicket(initialAdmission.opCtx.get(),
                                                       &initialAdmission.admCtx,
                                                       TicketHolder::WaitMode::kInterruptible);
        ASSERT(initialAdmission.ticket);

        MockAdmission lowPriorityAdmission(
            "lowPriority", this->getServiceContext(), AdmissionContext::Priority::kLow);
        stdx::thread lowPriorityThread([&]() {
            lowPriorityAdmission.ticket =
                holder.waitForTicket(lowPriorityAdmission.opCtx.get(),
                                     &lowPriorityAdmission.admCtx,
                                     TicketHolder::WaitMode::kUninterruptible);
        });

        MockAdmission normalPriorityAdmission(
            "normalPriority", this->getServiceContext(), AdmissionContext::Priority::kNormal);
        stdx::thread normalPriorityThread([&]() {
            normalPriorityAdmission.ticket =
                holder.waitForTicket(normalPriorityAdmission.opCtx.get(),
                                     &normalPriorityAdmission.admCtx,
                                     TicketHolder::WaitMode::kUninterruptible);
        });

        // Wait for the threads to to queue for a ticket.
        while (holder.queued() < 2) {
        }

        initialAdmission.ticket.reset();

        // Normal priority thread takes the ticket.
        normalPriorityThread.join();
        ASSERT_TRUE(normalPriorityAdmission.ticket);
        normalPriorityAdmission.ticket.reset();

        // Low priority thread takes the ticket.
        lowPriorityThread.join();
        ASSERT_TRUE(lowPriorityAdmission.ticket);
        lowPriorityAdmission.ticket.reset();
    }

    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 1);
    ASSERT_EQ(stats["totalTickets"], 1);

    auto currentStats = stats.getStats();
    auto lowPriorityStats = currentStats.getObjectField("lowPriority");
    // Low priority operations always go to the queue to avoid optimistic acquisition.
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 2);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 2);
    ASSERT_EQ(lowPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("startedProcessing"), 2);
    ASSERT_EQ(lowPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("finishedProcessing"), 2);
    ASSERT_EQ(lowPriorityStats.getIntField("newAdmissions"), 2);
    ASSERT_EQ(lowPriorityStats.getIntField("canceled"), 0);

    auto normalPriorityStats = currentStats.getObjectField("normalPriority");
    ASSERT_EQ(normalPriorityStats.getIntField("addedToQueue"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("removedFromQueue"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("startedProcessing"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("finishedProcessing"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("newAdmissions"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("canceled"), 0);

    auto immediatePriorityStats = currentStats.getObjectField("immediatePriority");
    ASSERT_EQ(immediatePriorityStats.getIntField("startedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("finishedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("totalTimeProcessingMicros"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("newAdmissions"), 0);
}


TEST_F(TicketHolderTest, OnlyLowPriorityOps) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    PriorityTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);

    {
        // Allocate the only available ticket. Priority is irrelevant when there are tickets
        // available.
        MockAdmission initialAdmission(
            "initialAdmission", this->getServiceContext(), AdmissionContext::Priority::kLow);
        initialAdmission.ticket = holder.waitForTicket(initialAdmission.opCtx.get(),
                                                       &initialAdmission.admCtx,
                                                       TicketHolder::WaitMode::kInterruptible);
        ASSERT(initialAdmission.ticket);

        MockAdmission low1PriorityAdmission(
            "low1Priority", this->getServiceContext(), AdmissionContext::Priority::kLow);
        stdx::thread low1PriorityThread([&]() {
            low1PriorityAdmission.ticket =
                holder.waitForTicket(low1PriorityAdmission.opCtx.get(),
                                     &low1PriorityAdmission.admCtx,
                                     TicketHolder::WaitMode::kUninterruptible);
        });


        MockAdmission low2PriorityAdmission(
            "low2Priority", this->getServiceContext(), AdmissionContext::Priority::kLow);
        stdx::thread low2PriorityThread([&]() {
            low2PriorityAdmission.ticket =
                holder.waitForTicket(low2PriorityAdmission.opCtx.get(),
                                     &low2PriorityAdmission.admCtx,
                                     TicketHolder::WaitMode::kUninterruptible);
        });


        // Wait for threads on the queue
        while (holder.queued() < 2) {
        }

        // Release the ticket.
        initialAdmission.ticket.reset();

        sleepFor(Milliseconds{100});
        assertSoon([&] {
            // Other low priority thread takes the ticket
            ASSERT_TRUE(low1PriorityAdmission.ticket || low2PriorityAdmission.ticket);
        });

        MockAdmission low3PriorityAdmission(
            "low3Priority", this->getServiceContext(), AdmissionContext::Priority::kLow);
        stdx::thread low3PriorityThread([&]() {
            low3PriorityAdmission.ticket =
                holder.waitForTicket(low3PriorityAdmission.opCtx.get(),
                                     &low3PriorityAdmission.admCtx,
                                     TicketHolder::WaitMode::kUninterruptible);
        });

        // Wait for the new thread on the queue.
        while (holder.queued() < 2) {
        }

        auto releaseCurrentTicket = [&] {
            ASSERT_TRUE(low1PriorityAdmission.ticket || low2PriorityAdmission.ticket ||
                        low3PriorityAdmission.ticket);
            // Ensure only one ticket is present.
            ASSERT_EQ(static_cast<int>(low1PriorityAdmission.ticket.has_value()) +
                          static_cast<int>(low2PriorityAdmission.ticket.has_value()) +
                          static_cast<int>(low3PriorityAdmission.ticket.has_value()),
                      1);
            if (low1PriorityAdmission.ticket.has_value()) {
                low1PriorityAdmission.ticket.reset();
            } else if (low2PriorityAdmission.ticket.has_value()) {
                low2PriorityAdmission.ticket.reset();
            } else {
                low3PriorityAdmission.ticket.reset();
            }
        };

        // Release the ticket.
        assertSoon(releaseCurrentTicket);

        // The other low priority thread takes the ticket.
        assertSoon(releaseCurrentTicket);

        // Third low priority thread takes the ticket.
        assertSoon(releaseCurrentTicket);

        low1PriorityThread.join();
        low2PriorityThread.join();
        low3PriorityThread.join();
    }

    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 1);
    ASSERT_EQ(stats["totalTickets"], 1);

    auto currentStats = stats.getStats();
    auto lowPriorityStats = currentStats.getObjectField("lowPriority");
    // Low priority operations always go to the queue to avoid optimistic acquisition.
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 4);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 4);
    ASSERT_EQ(lowPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("startedProcessing"), 4);
    ASSERT_EQ(lowPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("finishedProcessing"), 4);

    auto normalPriorityStats = currentStats.getObjectField("normalPriority");
    ASSERT_EQ(normalPriorityStats.getIntField("addedToQueue"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("removedFromQueue"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("startedProcessing"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("finishedProcessing"), 0);

    auto immediatePriorityStats = currentStats.getObjectField("immediatePriority");
    ASSERT_EQ(immediatePriorityStats.getIntField("startedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("finishedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("totalTimeProcessingMicros"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("newAdmissions"), 0);
}

TEST_F(TicketHolderTest, PriorityTwoNormalOneLowQueuedOperations) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    PriorityTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);

    {
        // Allocate the only available ticket. Priority is irrelevant when there are tickets
        // available.
        MockAdmission initialAdmission(
            "initialAdmission", this->getServiceContext(), AdmissionContext::Priority::kLow);
        initialAdmission.ticket = holder.waitForTicket(initialAdmission.opCtx.get(),
                                                       &initialAdmission.admCtx,
                                                       TicketHolder::WaitMode::kInterruptible);
        ASSERT(initialAdmission.ticket);

        MockAdmission lowPriorityAdmission(
            "lowPriority", this->getServiceContext(), AdmissionContext::Priority::kLow);
        stdx::thread lowPriorityThread([&]() {
            lowPriorityAdmission.ticket =
                holder.waitForTicket(lowPriorityAdmission.opCtx.get(),
                                     &lowPriorityAdmission.admCtx,
                                     TicketHolder::WaitMode::kUninterruptible);
        });


        MockAdmission normal1PriorityAdmission(
            "normal1Priority", this->getServiceContext(), AdmissionContext::Priority::kNormal);
        stdx::thread normal1PriorityThread([&]() {
            normal1PriorityAdmission.ticket =
                holder.waitForTicket(normal1PriorityAdmission.opCtx.get(),
                                     &normal1PriorityAdmission.admCtx,
                                     TicketHolder::WaitMode::kUninterruptible);
        });


        // Wait for threads on the queue
        while (holder.queued() < 2) {
        }

        // Release the ticket.
        initialAdmission.ticket.reset();

        // Normal priority thread takes the ticket
        normal1PriorityThread.join();
        ASSERT_TRUE(normal1PriorityAdmission.ticket);

        MockAdmission normal2PriorityAdmission(
            "normal2Priority", this->getServiceContext(), AdmissionContext::Priority::kNormal);
        stdx::thread normal2PriorityThread([&]() {
            normal2PriorityAdmission.ticket =
                holder.waitForTicket(normal2PriorityAdmission.opCtx.get(),
                                     &normal2PriorityAdmission.admCtx,
                                     TicketHolder::WaitMode::kUninterruptible);
        });

        // Wait for the new thread on the queue.
        while (holder.queued() < 2) {
        }

        // Release the ticket.
        normal1PriorityAdmission.ticket.reset();

        // The other normal priority thread takes the ticket.
        normal2PriorityThread.join();
        ASSERT_TRUE(normal2PriorityAdmission.ticket);
        normal2PriorityAdmission.ticket.reset();

        // Low priority thread takes the ticket.
        lowPriorityThread.join();
        ASSERT_TRUE(lowPriorityAdmission.ticket);
        lowPriorityAdmission.ticket.reset();
    }

    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 1);
    ASSERT_EQ(stats["totalTickets"], 1);

    auto currentStats = stats.getStats();
    auto lowPriorityStats = currentStats.getObjectField("lowPriority");
    // Low priority operations always go to the queue to avoid optimistic acquisition.
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 2);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 2);
    ASSERT_EQ(lowPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("startedProcessing"), 2);
    ASSERT_EQ(lowPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("finishedProcessing"), 2);

    auto normalPriorityStats = currentStats.getObjectField("normalPriority");
    ASSERT_EQ(normalPriorityStats.getIntField("addedToQueue"), 2);
    ASSERT_EQ(normalPriorityStats.getIntField("removedFromQueue"), 2);
    ASSERT_EQ(normalPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("startedProcessing"), 2);
    ASSERT_EQ(normalPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("finishedProcessing"), 2);

    auto immediatePriorityStats = currentStats.getObjectField("immediatePriority");
    ASSERT_EQ(immediatePriorityStats.getIntField("startedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("finishedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("totalTimeProcessingMicros"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("newAdmissions"), 0);
}

TEST_F(TicketHolderTest, PriorityBasicMetrics) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());
    PriorityTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);

    MockAdmission lowPriorityAdmission(
        "lowPriorityAdmission", this->getServiceContext(), AdmissionContext::Priority::kLow);
    lowPriorityAdmission.ticket = holder.waitForTicket(lowPriorityAdmission.opCtx.get(),
                                                       &lowPriorityAdmission.admCtx,
                                                       TicketHolder::WaitMode::kInterruptible);

    unittest::Barrier barrierAcquiredTicket(2);
    unittest::Barrier barrierReleaseTicket(2);

    stdx::thread waiting([&]() {
        // The ticket assigned to this admission is tied to the scope of the thread. Once the thread
        // joins, the ticket is released back to the TicketHolder.
        MockAdmission normalPriorityAdmission(
            "normalPriority", this->getServiceContext(), AdmissionContext::Priority::kNormal);

        normalPriorityAdmission.ticket =
            holder.waitForTicket(normalPriorityAdmission.opCtx.get(),
                                 &normalPriorityAdmission.admCtx,
                                 TicketHolder::WaitMode::kUninterruptible);
        barrierAcquiredTicket.countDownAndWait();
        barrierReleaseTicket.countDownAndWait();
    });

    while (holder.queued() == 0) {
        // Wait for thread to start waiting.
    }

    {
        // Test that the metrics eventually converge to the following set of values. There can be
        // cases where the values are incorrect for brief periods of time due to optimistic
        // concurrency.
        assertSoon([&] { ASSERT_EQ(stats["available"], 0); });
    }

    tickSource->advance(Microseconds(100));
    lowPriorityAdmission.ticket.reset();

    while (holder.queued() > 0) {
        // Wait for thread to take ticket.
    }

    barrierAcquiredTicket.countDownAndWait();
    tickSource->advance(Microseconds(200));
    barrierReleaseTicket.countDownAndWait();

    waiting.join();
    ASSERT_EQ(lowPriorityAdmission.admCtx.getAdmissions(), 1);

    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 1);
    ASSERT_EQ(stats["totalTickets"], 1);

    auto currentStats = stats.getStats();
    auto lowPriorityStats = currentStats.getObjectField("lowPriority");
    // Low priority operations always go to the queue to avoid optimistic acquisition.
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("startedProcessing"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("finishedProcessing"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("totalTimeProcessingMicros"), 100);
    ASSERT_EQ(lowPriorityStats.getIntField("totalTimeQueuedMicros"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("newAdmissions"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("canceled"), 0);

    auto normalPriorityStats = currentStats.getObjectField("normalPriority");
    ASSERT_EQ(normalPriorityStats.getIntField("addedToQueue"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("removedFromQueue"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("startedProcessing"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("finishedProcessing"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("totalTimeProcessingMicros"), 200);
    ASSERT_EQ(normalPriorityStats.getIntField("totalTimeQueuedMicros"), 100);
    ASSERT_EQ(normalPriorityStats.getIntField("newAdmissions"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("canceled"), 0);

    auto immediatePriorityStats = currentStats.getObjectField("immediatePriority");
    ASSERT_EQ(immediatePriorityStats.getIntField("startedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("finishedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("totalTimeProcessingMicros"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("newAdmissions"), 0);

    // Retake ticket.
    holder.waitForTicket(
        _opCtx.get(), &lowPriorityAdmission.admCtx, TicketHolder::WaitMode::kInterruptible);

    ASSERT_EQ(lowPriorityAdmission.admCtx.getAdmissions(), 2);

    currentStats = stats.getStats();
    lowPriorityStats = currentStats.getObjectField("lowPriority");
    ASSERT_EQ(lowPriorityStats.getIntField("newAdmissions"), 1);

    normalPriorityStats = currentStats.getObjectField("normalPriority");
    ASSERT_EQ(normalPriorityStats.getIntField("newAdmissions"), 1);
}

TEST_F(TicketHolderTest, PrioritImmediateMetrics) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());
    PriorityTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);

    MockAdmission lowPriorityAdmission(
        "lowPriorityAdmission", this->getServiceContext(), AdmissionContext::Priority::kLow);
    lowPriorityAdmission.ticket = holder.waitForTicket(lowPriorityAdmission.opCtx.get(),
                                                       &lowPriorityAdmission.admCtx,
                                                       TicketHolder::WaitMode::kInterruptible);
    ASSERT(lowPriorityAdmission.ticket);
    {
        // Test that the metrics eventually converge to the following set of values. There can be
        // cases where the values are incorrect for brief periods of time due to optimistic
        // concurrency.
        assertSoon([&] {
            ASSERT_EQ(stats["available"], 0);
            ASSERT_EQ(stats["out"], 1);
            ASSERT_EQ(stats["totalTickets"], 1);
        });
    }

    MockAdmission immediatePriorityAdmission("immediatePriorityAdmission",
                                             this->getServiceContext(),
                                             AdmissionContext::Priority::kImmediate);
    immediatePriorityAdmission.ticket =
        holder.acquireImmediateTicket(&immediatePriorityAdmission.admCtx);
    ASSERT(immediatePriorityAdmission.ticket);

    {
        // Test that the metrics eventually converge to the following set of values. There can be
        // cases where the values are incorrect for brief periods of time due to optimistic
        // concurrency.
        assertSoon([&]() {
            // only reported in the priority specific statistics.
            ASSERT_EQ(stats["available"], 0);
            ASSERT_EQ(stats["out"], 1);
            ASSERT_EQ(stats["totalTickets"], 1);

            auto currentStats = stats.getStats();
            auto immediatePriorityStats = currentStats.getObjectField("immediatePriority");
            ASSERT_EQ(immediatePriorityStats.getIntField("newAdmissions"), 1);

            ASSERT_EQ(immediatePriorityStats.getIntField("startedProcessing"), 1);
            ASSERT_EQ(immediatePriorityStats.getIntField("processing"), 1);
            ASSERT_EQ(immediatePriorityStats.getIntField("finishedProcessing"), 0);
        });
    }

    lowPriorityAdmission.ticket.reset();

    tickSource->advance(Microseconds(200));

    assertSoon([&] {
        ASSERT_EQ(stats["out"], 0);
        ASSERT_EQ(stats["available"], 1);
        ASSERT_EQ(stats["totalTickets"], 1);
    });

    immediatePriorityAdmission.ticket.reset();

    auto currentStats = stats.getStats();
    auto lowPriorityStats = currentStats.getObjectField("lowPriority");
    // Low priority operations always go to the queue to avoid optimistic acquisition.
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("startedProcessing"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("finishedProcessing"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("totalTimeProcessingMicros"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("totalTimeQueuedMicros"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("newAdmissions"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("canceled"), 0);

    auto immediatePriorityStats = currentStats.getObjectField("immediatePriority");
    ASSERT_EQ(immediatePriorityStats.getIntField("startedProcessing"), 1);
    ASSERT_EQ(immediatePriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("finishedProcessing"), 1);
    ASSERT_EQ(immediatePriorityStats.getIntField("totalTimeProcessingMicros"), 200);
    ASSERT_EQ(immediatePriorityStats.getIntField("newAdmissions"), 1);
}

TEST_F(TicketHolderTest, PriorityCanceled) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());
    PriorityTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);
    {
        MockAdmission lowPriorityAdmission(
            "lowPriorityAdmission", this->getServiceContext(), AdmissionContext::Priority::kLow);
        lowPriorityAdmission.ticket = holder.waitForTicket(lowPriorityAdmission.opCtx.get(),
                                                           &lowPriorityAdmission.admCtx,
                                                           TicketHolder::WaitMode::kInterruptible);
        stdx::thread waiting([&]() {
            MockAdmission normalPriorityAdmission(
                "normalPriority", this->getServiceContext(), AdmissionContext::Priority::kNormal);

            auto deadline = Date_t::now() + Milliseconds(100);
            normalPriorityAdmission.ticket =
                holder.waitForTicketUntil(normalPriorityAdmission.opCtx.get(),
                                          &normalPriorityAdmission.admCtx,
                                          deadline,
                                          TicketHolder::WaitMode::kInterruptible);
            ASSERT_FALSE(normalPriorityAdmission.ticket);
        });

        while (holder.queued() == 0) {
            // Wait for thread to take ticket.
        }

        tickSource->advance(Microseconds(100));
        waiting.join();
    }

    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 1);
    ASSERT_EQ(stats["totalTickets"], 1);

    auto currentStats = stats.getStats();
    auto lowPriorityStats = currentStats.getObjectField("lowPriority");
    // Low priority operations always go to the queue to avoid optimistic acquisition.
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("startedProcessing"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("finishedProcessing"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("totalTimeProcessingMicros"), 100);
    ASSERT_EQ(lowPriorityStats.getIntField("totalTimeQueuedMicros"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("newAdmissions"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("canceled"), 0);

    auto normalPriorityStats = currentStats.getObjectField("normalPriority");
    ASSERT_EQ(normalPriorityStats.getIntField("addedToQueue"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("removedFromQueue"), 1);
    ASSERT_EQ(normalPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("startedProcessing"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("finishedProcessing"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("totalTimeProcessingMicros"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("totalTimeQueuedMicros"), 100);
    ASSERT_EQ(normalPriorityStats.getIntField("newAdmissions"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("canceled"), 1);

    auto immediatePriorityStats = currentStats.getObjectField("immediatePriority");
    ASSERT_EQ(immediatePriorityStats.getIntField("startedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("finishedProcessing"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("totalTimeProcessingMicros"), 0);
    ASSERT_EQ(immediatePriorityStats.getIntField("newAdmissions"), 0);
}
}  // namespace

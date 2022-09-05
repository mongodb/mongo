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
    admCtx.setPriority(AdmissionContext::AcquisitionPriority::kNormal);
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

TEST_F(TicketHolderTest, BasicTimeoutFifo) {
    basicTimeout<FifoTicketHolder>(_opCtx.get());
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

template <class H>
void resizeTest(OperationContext* opCtx) {
    // Verify that resize operations don't alter metrics outside of those linked to the number of
    // tickets.
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());
    auto mode = TicketHolder::WaitMode::kInterruptible;
    std::unique_ptr<TicketHolderWithQueueingStats> holder = std::make_unique<H>(1, &serviceContext);
    Stats stats(holder.get());

    AdmissionContext admCtx;
    admCtx.setPriority(AdmissionContext::AcquisitionPriority::kNormal);

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
    ASSERT_EQ(stats["totalTimeProcessingMicros"], 200);

    holder->resize(1);
    newStats = stats.getNonTicketStats();
    ASSERT_EQ(currentStats.woCompare(newStats), 0);

    tickSource->advance(Microseconds{100});
    holder->resize(10);
    currentStats = stats.getNonTicketStats();
    ASSERT_EQ(currentStats.woCompare(newStats), 0);
}

TEST_F(TicketHolderTest, ResizeStatsFifo) {
    resizeTest<FifoTicketHolder>(_opCtx.get());
}
TEST_F(TicketHolderTest, ResizeStatsSemaphore) {
    resizeTest<SemaphoreTicketHolder>(_opCtx.get());
}
TEST_F(TicketHolderTest, ResizeStatsPriority) {
    resizeTest<PriorityTicketHolder>(_opCtx.get());
}

TEST_F(TicketHolderTest, FifoBasicMetrics) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());
    FifoTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);
    AdmissionContext admCtx;

    boost::optional<Ticket> ticket =
        holder.waitForTicket(_opCtx.get(), &admCtx, TicketHolder::WaitMode::kInterruptible);

    unittest::Barrier barrierAcquiredTicket(2);
    unittest::Barrier barrierReleaseTicket(2);
    stdx::thread waiting([&]() {
        auto client = this->getServiceContext()->makeClient("waiting");
        auto opCtx = client->makeOperationContext();
        AdmissionContext admCtx;

        auto ticket =
            holder.waitForTicket(opCtx.get(), &admCtx, TicketHolder::WaitMode::kInterruptible);
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
        auto deadline = Date_t::now() + Milliseconds{100};
        while (true) {
            try {
                ASSERT_EQ(stats["out"], 1);
                ASSERT_EQ(stats["available"], 0);
                ASSERT_EQ(stats["addedToQueue"], 1);
                ASSERT_EQ(stats["queueLength"], 1);
                break;
            } catch (...) {
                if (Date_t::now() > deadline) {
                    throw;
                }
                // Sleep to allow other threads to process and converge the metrics.
                stdx::this_thread::sleep_for(Milliseconds{1}.toSystemDuration());
            }
        }
    }
    tickSource->advance(Microseconds(100));
    ticket.reset();

    while (holder.queued() > 0) {
        // Wait for thread to take ticket.
    }

    barrierAcquiredTicket.countDownAndWait();
    tickSource->advance(Microseconds(200));
    barrierReleaseTicket.countDownAndWait();

    waiting.join();

    ASSERT_EQ(admCtx.getAdmissions(), 1);
    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 1);
    ASSERT_EQ(stats["addedToQueue"], 1);
    ASSERT_EQ(stats["removedFromQueue"], 1);
    ASSERT_EQ(stats["queueLength"], 0);
    ASSERT_EQ(stats["totalTimeQueuedMicros"], 100);
    ASSERT_EQ(stats["startedProcessing"], 2);
    ASSERT_EQ(stats["finishedProcessing"], 2);
    ASSERT_EQ(stats["processing"], 0);
    ASSERT_EQ(stats["totalTimeProcessingMicros"], 300);
    ASSERT_EQ(stats["canceled"], 0);
    ASSERT_EQ(stats["newAdmissions"], 2);

    // Retake ticket.
    holder.waitForTicket(_opCtx.get(), &admCtx, TicketHolder::WaitMode::kInterruptible);

    ASSERT_EQ(admCtx.getAdmissions(), 2);
    ASSERT_EQ(stats["newAdmissions"], 2);
}

TEST_F(TicketHolderTest, FifoCanceled) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());
    FifoTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);
    AdmissionContext admCtx;

    {
        auto ticket =
            holder.waitForTicket(_opCtx.get(), &admCtx, TicketHolder::WaitMode::kInterruptible);

        stdx::thread waiting([&]() {
            auto client = this->getServiceContext()->makeClient("waiting");
            auto opCtx = client->makeOperationContext();

            AdmissionContext admCtx;
            auto deadline = Date_t::now() + Milliseconds(100);
            ASSERT_FALSE(holder.waitForTicketUntil(
                opCtx.get(), &admCtx, deadline, TicketHolder::WaitMode::kInterruptible));
        });

        while (holder.queued() == 0) {
            // Wait for thread to take ticket.
        }

        tickSource->advance(Microseconds(100));
        waiting.join();
    }

    ASSERT_EQ(stats["addedToQueue"], 1);
    ASSERT_EQ(stats["removedFromQueue"], 1);
    ASSERT_EQ(stats["queueLength"], 0);
    ASSERT_EQ(stats["totalTimeQueuedMicros"], 100);
    ASSERT_EQ(stats["startedProcessing"], 1);
    ASSERT_EQ(stats["finishedProcessing"], 1);
    ASSERT_EQ(stats["processing"], 0);
    ASSERT_EQ(stats["totalTimeProcessingMicros"], 100);
    ASSERT_EQ(stats["canceled"], 1);
}

TEST_F(TicketHolderTest, PriorityTwoQueuedOperations) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    PriorityTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);

    {
        // Allocate the only available ticket. Priority is irrelevant when there are tickets
        // available.
        AdmissionContext admCtx;
        admCtx.setPriority(AdmissionContext::AcquisitionPriority::kLow);
        boost::optional<Ticket> ticket =
            holder.waitForTicket(_opCtx.get(), &admCtx, TicketHolder::WaitMode::kInterruptible);
        ASSERT(ticket);

        // Create a client corresponding to a low priority operation that will queue.
        boost::optional<Ticket> ticketLowPriority;
        auto clientLowPriority = this->getServiceContext()->makeClient("clientLowPriority");
        auto opCtxLowPriority = clientLowPriority->makeOperationContext();
        // Each ticket is assigned with a pointer to an AdmissionContext. The AdmissionContext must
        // survive the lifetime of the ticket.
        AdmissionContext admCtxLowPriority;
        admCtxLowPriority.setPriority(AdmissionContext::AcquisitionPriority::kLow);

        stdx::thread lowPriorityThread([&]() {
            ticketLowPriority = holder.waitForTicket(opCtxLowPriority.get(),
                                                     &admCtxLowPriority,
                                                     TicketHolder::WaitMode::kUninterruptible);
        });

        // Create a client corresponding to a normal priority operation that will queue.
        boost::optional<Ticket> ticketNormalPriority;
        auto clientNormalPriority = this->getServiceContext()->makeClient("clientNormalPriority");
        auto opCtxNormalPriority = clientNormalPriority->makeOperationContext();
        // Each ticket is assigned with a pointer to an AdmissionContext. The AdmissionContext must
        // survive the lifetime of the ticket.
        AdmissionContext admCtxNormalPriority;
        admCtxNormalPriority.setPriority(AdmissionContext::AcquisitionPriority::kNormal);

        stdx::thread normalPriorityThread([&]() {
            ticketNormalPriority = holder.waitForTicket(opCtxNormalPriority.get(),
                                                        &admCtxNormalPriority,
                                                        TicketHolder::WaitMode::kUninterruptible);
        });

        // Wait for the threads to to queue for a ticket.
        while (holder.queued() < 2) {
        }

        ASSERT_EQ(stats["queueLength"], 2);
        ticket.reset();

        // Normal priority thread takes the ticket.
        normalPriorityThread.join();
        ASSERT_TRUE(ticketNormalPriority);
        ASSERT_EQ(stats["removedFromQueue"], 1);
        ticketNormalPriority.reset();

        // Low priority thread takes the ticket.
        lowPriorityThread.join();
        ASSERT_TRUE(ticketLowPriority);
        ASSERT_EQ(stats["removedFromQueue"], 2);
        ticketLowPriority.reset();
    }

    ASSERT_EQ(stats["addedToQueue"], 2);
    ASSERT_EQ(stats["removedFromQueue"], 2);
    ASSERT_EQ(stats["queueLength"], 0);
}

TEST_F(TicketHolderTest, PriorityTwoNormalOneLowQueuedOperations) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    PriorityTicketHolder holder(1, &serviceContext);
    Stats stats(&holder);

    {
        // Allocate the only available ticket. Priority is irrelevant when there are tickets
        // available.
        AdmissionContext admCtx;
        admCtx.setPriority(AdmissionContext::AcquisitionPriority::kLow);
        boost::optional<Ticket> ticket =
            holder.waitForTicket(_opCtx.get(), &admCtx, TicketHolder::WaitMode::kInterruptible);
        ASSERT(ticket);

        // Create a client corresponding to a low priority operation that will queue.
        boost::optional<Ticket> ticketLowPriority;
        auto clientLowPriority = this->getServiceContext()->makeClient("clientLowPriority");
        auto opCtxLowPriority = clientLowPriority->makeOperationContext();
        // Each ticket is assigned with a pointer to an AdmissionContext. The AdmissionContext must
        // survive the lifetime of the ticket.
        AdmissionContext admCtxLowPriority;
        admCtxLowPriority.setPriority(AdmissionContext::AcquisitionPriority::kLow);

        stdx::thread lowPriorityThread([&]() {
            ticketLowPriority = holder.waitForTicket(opCtxLowPriority.get(),
                                                     &admCtxLowPriority,
                                                     TicketHolder::WaitMode::kUninterruptible);
        });

        // Create a client corresponding to a normal priority operation that will queue.
        boost::optional<Ticket> ticketNormal1Priority;
        auto clientNormal1Priority = this->getServiceContext()->makeClient("clientNormal1Priority");
        auto opCtxNormal1Priority = clientNormal1Priority->makeOperationContext();
        // Each ticket is assigned with a pointer to an AdmissionContext. The AdmissionContext must
        // survive the lifetime of the ticket.
        AdmissionContext admCtxNormal1Priority;
        admCtxNormal1Priority.setPriority(AdmissionContext::AcquisitionPriority::kNormal);

        stdx::thread normal1PriorityThread([&]() {
            ticketNormal1Priority = holder.waitForTicket(opCtxNormal1Priority.get(),
                                                         &admCtxNormal1Priority,
                                                         TicketHolder::WaitMode::kUninterruptible);
        });

        // Wait for threads on the queue
        while (holder.queued() < 2) {
        }

        // Release the ticket.
        ticket.reset();

        // Normal priority thread takes the ticket
        normal1PriorityThread.join();
        ASSERT_TRUE(ticketNormal1Priority);
        ASSERT_EQ(stats["removedFromQueue"], 1);

        // Create a client corresponding to a second normal priority operation that will be
        // prioritized over the queued low priority operation.
        boost::optional<Ticket> ticketNormal2Priority;
        auto clientNormal2Priority = this->getServiceContext()->makeClient("clientNormal2Priority");
        auto opCtxNormal2Priority = clientNormal2Priority->makeOperationContext();
        // Each ticket is assigned with a pointer to an AdmissionContext. The AdmissionContext must
        // survive the lifetime of the ticket.
        AdmissionContext admCtxNormal2Priority;
        admCtxNormal2Priority.setPriority(AdmissionContext::AcquisitionPriority::kNormal);
        stdx::thread normal2PriorityThread([&]() {
            ticketNormal2Priority = holder.waitForTicket(opCtxNormal2Priority.get(),
                                                         &admCtxNormal2Priority,
                                                         TicketHolder::WaitMode::kUninterruptible);
        });

        // Wait for the new thread on the queue.
        while (holder.queued() < 2) {
        }

        // Release the ticket.
        ticketNormal1Priority.reset();

        // The other normal priority thread takes the ticket.
        normal2PriorityThread.join();
        ASSERT_TRUE(ticketNormal2Priority);
        ASSERT_EQ(stats["removedFromQueue"], 2);
        ticketNormal2Priority.reset();

        // Low priority thread takes the ticket.
        lowPriorityThread.join();
        ASSERT_TRUE(ticketLowPriority);
        ASSERT_EQ(stats["removedFromQueue"], 3);
        ticketLowPriority.reset();
    }
    ASSERT_EQ(stats["addedToQueue"], 3);
    ASSERT_EQ(stats["removedFromQueue"], 3);
    ASSERT_EQ(stats["queueLength"], 0);
}

}  // namespace

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

#include <boost/move/utility_core.hpp>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/priority_ticketholder.h"
#include "mongo/util/concurrency/ticketholder_test_fixture.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#define ASSERT_SOON_EXP(exp)                         \
    if (!(exp)) {                                    \
        LOGV2_WARNING(7153501,                       \
                      "Expression failed, retrying", \
                      "exp"_attr = #exp,             \
                      "file"_attr = __FILE__,        \
                      "line"_attr = __LINE__);       \
        return false;                                \
    }

namespace {
using namespace mongo;

// By default, tests will create a PriorityTicketHolder where low priority admissions can be
// bypassed an unlimited amount of times in favor of normal priority admissions.
static constexpr int kDefaultLowPriorityAdmissionBypassThreshold = 0;

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
            LOGV2_ERROR(7153502,
                        "assertSoon failed, please check the logs for the reason all attempts have "
                        "failed.");
            ASSERT_TRUE(false);
        }
        sleepFor(kSleepTime);
    }
}

class PriorityTicketHolderTest : public TicketHolderTestFixture {
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

TEST_F(PriorityTicketHolderTest, BasicTimeoutPriority) {
    basicTimeout(_opCtx.get(),
                 std::make_unique<PriorityTicketHolder>(getServiceContext(),
                                                        1 /* tickets */,
                                                        kDefaultLowPriorityAdmissionBypassThreshold,
                                                        false /* trackPeakUsed */
                                                        ));
}

TEST_F(PriorityTicketHolderTest, ResizeStatsPriority) {
    resizeTest(_opCtx.get(),
               std::make_unique<PriorityTicketHolder>(getServiceContext(),
                                                      1 /* tickets */,
                                                      kDefaultLowPriorityAdmissionBypassThreshold,
                                                      false /* trackPeakUsed */
                                                      ),
               getTickSource());
}

TEST_F(PriorityTicketHolderTest, PriorityTwoQueuedOperations) {
    PriorityTicketHolder holder(getServiceContext(),
                                1 /* tickets */,
                                kDefaultLowPriorityAdmissionBypassThreshold,
                                false /* trackPeakUsed*/);

    Stats stats(&holder);
    Microseconds timeInQueue;

    {
        // Allocate the only available ticket. Priority is irrelevant when there are tickets
        // available.
        MockAdmission initialAdmission(this->getServiceContext(), AdmissionContext::Priority::kLow);
        initialAdmission.ticket =
            holder.waitForTicket(*initialAdmission.opCtx.get(),
                                 &AdmissionContext::get(initialAdmission.opCtx.get()),
                                 timeInQueue);
        ASSERT(initialAdmission.ticket);

        MockAdmission lowPriorityAdmission(this->getServiceContext(),
                                           AdmissionContext::Priority::kLow);
        stdx::thread lowPriorityThread([&]() {
            lowPriorityAdmission.ticket =
                holder.waitForTicket(*Interruptible::notInterruptible(),
                                     &AdmissionContext::get(lowPriorityAdmission.opCtx.get()),
                                     timeInQueue);
        });

        MockAdmission normalPriorityAdmission(this->getServiceContext(),
                                              AdmissionContext::Priority::kNormal);
        stdx::thread normalPriorityThread([&]() {
            normalPriorityAdmission.ticket =
                holder.waitForTicket(*Interruptible::notInterruptible(),
                                     &AdmissionContext::get(normalPriorityAdmission.opCtx.get()),
                                     timeInQueue);
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
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 1);
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
}


TEST_F(PriorityTicketHolderTest, OnlyLowPriorityOps) {
    PriorityTicketHolder holder(getServiceContext(),
                                1 /* tickets */,
                                kDefaultLowPriorityAdmissionBypassThreshold,
                                false /* trackPeakUsed*/);
    Stats stats(&holder);
    Microseconds timeInQueue;

    // This mutex is to avoid data race conditions between checking for the ticket state and setting
    // it in the worker threads.
    Mutex ticketCheckMutex;

    {
        // Allocate the only available ticket. Priority is irrelevant when there are tickets
        // available.
        MockAdmission initialAdmission(this->getServiceContext(), AdmissionContext::Priority::kLow);
        initialAdmission.ticket =
            holder.waitForTicket(*initialAdmission.opCtx,
                                 &AdmissionContext::get(initialAdmission.opCtx.get()),
                                 timeInQueue);
        ASSERT(initialAdmission.ticket);

        MockAdmission low1PriorityAdmission(this->getServiceContext(),
                                            AdmissionContext::Priority::kLow);
        stdx::thread low1PriorityThread([&]() {
            auto ticket =
                holder.waitForTicket(*Interruptible::notInterruptible(),
                                     &AdmissionContext::get(low1PriorityAdmission.opCtx.get()),
                                     timeInQueue);
            stdx::lock_guard lk(ticketCheckMutex);
            low1PriorityAdmission.ticket = std::move(ticket);
        });


        MockAdmission low2PriorityAdmission(this->getServiceContext(),
                                            AdmissionContext::Priority::kLow);
        stdx::thread low2PriorityThread([&]() {
            auto ticket =
                holder.waitForTicket(*Interruptible::notInterruptible(),
                                     &AdmissionContext::get(low2PriorityAdmission.opCtx.get()),
                                     timeInQueue);
            stdx::lock_guard lk(ticketCheckMutex);
            low2PriorityAdmission.ticket = std::move(ticket);
        });


        // Wait for threads on the queue
        while (holder.queued() < 2) {
        }

        // Release the ticket.
        initialAdmission.ticket.reset();

        sleepFor(Milliseconds{100});
        assertSoon([&] {
            stdx::lock_guard lk(ticketCheckMutex);
            // Other low priority thread takes the ticket
            ASSERT_SOON_EXP(low1PriorityAdmission.ticket || low2PriorityAdmission.ticket);
            return true;
        });

        MockAdmission low3PriorityAdmission(this->getServiceContext(),
                                            AdmissionContext::Priority::kLow);
        stdx::thread low3PriorityThread([&]() {
            auto ticket =
                holder.waitForTicket(*Interruptible::notInterruptible(),
                                     &AdmissionContext::get(low3PriorityAdmission.opCtx.get()),
                                     timeInQueue);
            stdx::lock_guard lk(ticketCheckMutex);
            low3PriorityAdmission.ticket = std::move(ticket);
        });

        // Wait for the new thread on the queue.
        while (holder.queued() < 2) {
        }

        auto releaseCurrentTicket = [&] {
            stdx::lock_guard lk(ticketCheckMutex);
            ASSERT_SOON_EXP(low1PriorityAdmission.ticket || low2PriorityAdmission.ticket ||
                            low3PriorityAdmission.ticket);
            // Ensure only one ticket is present.
            auto numTickets = static_cast<int>(low1PriorityAdmission.ticket.has_value()) +
                static_cast<int>(low2PriorityAdmission.ticket.has_value()) +
                static_cast<int>(low3PriorityAdmission.ticket.has_value());
            ASSERT_SOON_EXP(numTickets == 1);

            if (low1PriorityAdmission.ticket.has_value()) {
                low1PriorityAdmission.ticket.reset();
            } else if (low2PriorityAdmission.ticket.has_value()) {
                low2PriorityAdmission.ticket.reset();
            } else {
                low3PriorityAdmission.ticket.reset();
            }
            return true;
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
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 3);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 3);
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
}

TEST_F(PriorityTicketHolderTest, PriorityTwoNormalOneLowQueuedOperations) {
    PriorityTicketHolder holder(getServiceContext(),
                                1 /* tickets */,
                                kDefaultLowPriorityAdmissionBypassThreshold,
                                false /* trackPeakUsed*/);
    Stats stats(&holder);
    Microseconds timeInQueue;

    {
        // Allocate the only available ticket. Priority is irrelevant when there are tickets
        // available.
        MockAdmission initialAdmission(this->getServiceContext(), AdmissionContext::Priority::kLow);
        initialAdmission.ticket =
            holder.waitForTicket(*initialAdmission.opCtx,
                                 &AdmissionContext::get(initialAdmission.opCtx.get()),
                                 timeInQueue);
        ASSERT(initialAdmission.ticket);

        MockAdmission lowPriorityAdmission(this->getServiceContext(),
                                           AdmissionContext::Priority::kLow);
        stdx::thread lowPriorityThread([&]() {
            lowPriorityAdmission.ticket =
                holder.waitForTicket(*Interruptible::notInterruptible(),
                                     &AdmissionContext::get(lowPriorityAdmission.opCtx.get()),
                                     timeInQueue);
        });


        MockAdmission normal1PriorityAdmission(this->getServiceContext(),
                                               AdmissionContext::Priority::kNormal);
        stdx::thread normal1PriorityThread([&]() {
            normal1PriorityAdmission.ticket =
                holder.waitForTicket(*Interruptible::notInterruptible(),
                                     &AdmissionContext::get(normal1PriorityAdmission.opCtx.get()),
                                     timeInQueue);
        });


        // Wait for threads on the queue
        while (holder.queued() < 2) {
        }

        // Release the ticket.
        initialAdmission.ticket.reset();

        // Normal priority thread takes the ticket
        normal1PriorityThread.join();
        ASSERT_TRUE(normal1PriorityAdmission.ticket);

        MockAdmission normal2PriorityAdmission(this->getServiceContext(),
                                               AdmissionContext::Priority::kNormal);
        stdx::thread normal2PriorityThread([&]() {
            normal2PriorityAdmission.ticket =
                holder.waitForTicket(*Interruptible::notInterruptible(),
                                     &AdmissionContext::get(normal2PriorityAdmission.opCtx.get()),
                                     timeInQueue);
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
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 1);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 1);
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
}

TEST_F(PriorityTicketHolderTest, PriorityBasicMetrics) {
    PriorityTicketHolder holder(getServiceContext(),
                                1 /* tickets */,
                                kDefaultLowPriorityAdmissionBypassThreshold,
                                false /* trackPeakUsed*/);
    Stats stats(&holder);
    Microseconds timeInQueue(0);

    MockAdmission lowPriorityAdmission(this->getServiceContext(), AdmissionContext::Priority::kLow);
    lowPriorityAdmission.ticket =
        holder.waitForTicket(*lowPriorityAdmission.opCtx,
                             &AdmissionContext::get(lowPriorityAdmission.opCtx.get()),
                             timeInQueue);

    unittest::Barrier barrierAcquiredTicket(2);
    unittest::Barrier barrierReleaseTicket(2);

    stdx::thread waiting([&]() {
        // The ticket assigned to this admission is tied to the scope of the thread. Once the thread
        // joins, the ticket is released back to the TicketHolder.
        MockAdmission normalPriorityAdmission(this->getServiceContext(),
                                              AdmissionContext::Priority::kNormal);

        normalPriorityAdmission.ticket =
            holder.waitForTicket(*Interruptible::notInterruptible(),
                                 &AdmissionContext::get(normalPriorityAdmission.opCtx.get()),
                                 timeInQueue);
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
        assertSoon([&] {
            ASSERT_SOON_EXP(stats["available"] == 0);
            return true;
        });
    }

    getTickSource()->advance(Microseconds(100));
    lowPriorityAdmission.ticket.reset();

    while (holder.queued() > 0) {
        // Wait for thread to take ticket.
    }

    barrierAcquiredTicket.countDownAndWait();
    getTickSource()->advance(Microseconds(200));
    barrierReleaseTicket.countDownAndWait();

    waiting.join();
    ASSERT_EQ(AdmissionContext::get(lowPriorityAdmission.opCtx.get()).getAdmissions(), 1);
    ASSERT_EQ(timeInQueue, Microseconds(100));

    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 1);
    ASSERT_EQ(stats["totalTickets"], 1);

    auto currentStats = stats.getStats();
    auto lowPriorityStats = currentStats.getObjectField("lowPriority");
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 0);
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

    // Retake ticket.
    holder.waitForTicket(
        *_opCtx, &AdmissionContext::get(lowPriorityAdmission.opCtx.get()), timeInQueue);

    ASSERT_EQ(AdmissionContext::get(lowPriorityAdmission.opCtx.get()).getAdmissions(), 2);

    currentStats = stats.getStats();
    lowPriorityStats = currentStats.getObjectField("lowPriority");
    ASSERT_EQ(lowPriorityStats.getIntField("newAdmissions"), 1);

    normalPriorityStats = currentStats.getObjectField("normalPriority");
    ASSERT_EQ(normalPriorityStats.getIntField("newAdmissions"), 1);
}

TEST_F(PriorityTicketHolderTest, PriorityCanceled) {
    PriorityTicketHolder holder(getServiceContext(),
                                1 /* tickets */,
                                kDefaultLowPriorityAdmissionBypassThreshold,
                                false /* trackPeakUsed*/);
    Stats stats(&holder);
    Microseconds timeInQueue(0);
    {
        MockAdmission lowPriorityAdmission(this->getServiceContext(),
                                           AdmissionContext::Priority::kLow);
        lowPriorityAdmission.ticket =
            holder.waitForTicket(*lowPriorityAdmission.opCtx,
                                 &AdmissionContext::get(lowPriorityAdmission.opCtx.get()),
                                 timeInQueue);
        stdx::thread waiting([&]() {
            MockAdmission normalPriorityAdmission(this->getServiceContext(),
                                                  AdmissionContext::Priority::kNormal);

            auto deadline = Date_t::now() + Milliseconds(100);
            normalPriorityAdmission.ticket = holder.waitForTicketUntil(
                *normalPriorityAdmission.opCtx,
                &AdmissionContext::get(normalPriorityAdmission.opCtx.get()),
                deadline,
                timeInQueue);
            ASSERT_FALSE(normalPriorityAdmission.ticket);
            ASSERT_EQ(timeInQueue, Microseconds(100));
        });

        while (holder.queued() == 0) {
            // Wait for thread to take ticket.
        }

        getTickSource()->advance(Microseconds(100));
        waiting.join();
    }

    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 1);
    ASSERT_EQ(stats["totalTickets"], 1);

    auto currentStats = stats.getStats();
    auto lowPriorityStats = currentStats.getObjectField("lowPriority");
    ASSERT_EQ(lowPriorityStats.getIntField("addedToQueue"), 0);
    ASSERT_EQ(lowPriorityStats.getIntField("removedFromQueue"), 0);
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
}

TEST_F(PriorityTicketHolderTest, LowPriorityExpedited) {
    auto lowPriorityBypassThreshold = 2;
    PriorityTicketHolder holder(
        getServiceContext(), 1 /* tickets */, lowPriorityBypassThreshold, false /* trackPeakUsed*/);
    Stats stats(&holder);
    Microseconds timeInQueue;

    // Use the GlobalServiceContext to create MockAdmissions.
    auto svcCtx = this->getServiceContext();

    // Allocate the only available ticket. Priority is irrelevant when there are tickets
    // available.
    MockAdmission initialAdmission(svcCtx, AdmissionContext::Priority::kNormal);
    initialAdmission.ticket = holder.waitForTicket(
        *initialAdmission.opCtx, &AdmissionContext::get(initialAdmission.opCtx.get()), timeInQueue);
    ASSERT(initialAdmission.ticket);

    std::vector<stdx::thread> threads;
    MockAdmission lowPriorityAdmission(svcCtx, AdmissionContext::Priority::kLow);
    // This mutex protects the lowPriorityAdmission ticket
    Mutex ticketMutex;

    threads.emplace_back([&]() {
        auto ticket = holder.waitForTicket(*Interruptible::notInterruptible(),
                                           &AdmissionContext::get(lowPriorityAdmission.opCtx.get()),
                                           timeInQueue);
        stdx::lock_guard lk(ticketMutex);
        lowPriorityAdmission.ticket = std::move(ticket);
    });

    auto queuedNormalAdmissionsCount = 4;
    for (int i = 0; i < queuedNormalAdmissionsCount; i++) {
        threads.emplace_back([&]() {
            MockAdmission adm(svcCtx, AdmissionContext::Priority::kNormal);
            adm.ticket = holder.waitForTicket(*Interruptible::notInterruptible(),
                                              &AdmissionContext::get(adm.opCtx.get()),
                                              timeInQueue);
            adm.ticket.reset();
        });
    }

    while (holder.queued() < queuedNormalAdmissionsCount + 1) {
        // holder.queued() accounts for both normal and low priority admissions queued.
    }

    initialAdmission.ticket.reset();

    assertSoon([&] {
        stdx::lock_guard lk(ticketMutex);
        return holder.expedited() == 1 && lowPriorityAdmission.ticket;
    });

    lowPriorityAdmission.ticket.reset();

    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(stats["out"], 0);
    ASSERT_EQ(stats["available"], 1);
    ASSERT_EQ(stats["totalTickets"], 1);

    auto currentStats = stats.getStats();
    auto lowPriorityStats = currentStats.getObjectField("lowPriority");
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
    ASSERT_EQ(lowPriorityStats.getIntField("expedited"), 1);

    auto normalPriorityStats = currentStats.getObjectField("normalPriority");
    ASSERT_EQ(normalPriorityStats.getIntField("addedToQueue"), queuedNormalAdmissionsCount);
    ASSERT_EQ(normalPriorityStats.getIntField("removedFromQueue"), queuedNormalAdmissionsCount);
    ASSERT_EQ(normalPriorityStats.getIntField("queueLength"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("startedProcessing"),
              queuedNormalAdmissionsCount + 1);
    ASSERT_EQ(normalPriorityStats.getIntField("processing"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("finishedProcessing"),
              queuedNormalAdmissionsCount + 1);
    ASSERT_EQ(normalPriorityStats.getIntField("totalTimeProcessingMicros"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("totalTimeQueuedMicros"), 0);
    ASSERT_EQ(normalPriorityStats.getIntField("newAdmissions"), queuedNormalAdmissionsCount + 1);
    ASSERT_EQ(normalPriorityStats.getIntField("canceled"), 0);
}

TEST_F(PriorityTicketHolderTest, Interruption) {
    interruptTest(_opCtx.get(),
                  std::make_unique<PriorityTicketHolder>(
                      getServiceContext(), 1 /* tickets */, 0, false /* trackPeakUsed */));
}

TEST_F(PriorityTicketHolderTest, PriorityQueuedTimeTracker) {
    PriorityTicketHolder holder(getServiceContext(),
                                1 /* tickets */,
                                kDefaultLowPriorityAdmissionBypassThreshold,
                                false /* trackPeakUsed*/);
    Stats stats(&holder);
    Microseconds timeInQueue(0);

    MockAdmission admissionInitial(this->getServiceContext(), AdmissionContext::Priority::kNormal);
    admissionInitial.ticket = holder.waitForTicket(
        *admissionInitial.opCtx, &AdmissionContext::get(admissionInitial.opCtx.get()), timeInQueue);
    // No need to queue for a ticket.
    ASSERT_EQ(timeInQueue, Microseconds(0));

    stdx::thread waiting([&]() {
        // The ticket assigned to this admission is tied to the scope of the thread. Once the thread
        // joins, the ticket is released back to the TicketHolder.
        MockAdmission admission1(this->getServiceContext(), AdmissionContext::Priority::kNormal);
        admission1.ticket = holder.waitForTicket(*Interruptible::notInterruptible(),
                                                 &AdmissionContext::get(admission1.opCtx.get()),
                                                 timeInQueue);
    });

    while (holder.queued() == 0) {
        // Wait for thread to start waiting.
    }

    getTickSource()->advance(Microseconds(100));
    admissionInitial.ticket.reset();

    while (holder.queued() > 0) {
        // Wait for thread to take ticket.
    }

    waiting.join();
    // Check that time waiting for a ticket is updated when ticket is acquired.
    ASSERT_EQ(timeInQueue, Microseconds(100));

    // Retake ticket.
    admissionInitial.ticket = holder.waitForTicket(
        *_opCtx, &AdmissionContext::get(admissionInitial.opCtx.get()), timeInQueue);
    // No need to queue for a ticket.
    ASSERT_EQ(timeInQueue, Microseconds(100));

    stdx::thread waitButFail([&]() {
        MockAdmission admission2(this->getServiceContext(), AdmissionContext::Priority::kNormal);
        auto deadline = Date_t::now() + Milliseconds(50);
        admission2.ticket =
            holder.waitForTicketUntil(*admission2.opCtx,
                                      &AdmissionContext::get(admission2.opCtx.get()),
                                      deadline,
                                      timeInQueue);

        ASSERT_FALSE(admission2.ticket);
        // Check that time waiting for ticket is updated even if ticket is not acquired.
        ASSERT_EQ(timeInQueue, Microseconds(150));
    });

    while (holder.queued() == 0) {
        // Wait for thread to take ticket.
    }

    getTickSource()->advance(Microseconds(50));
    waitButFail.join();
}
}  // namespace

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
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/semaphore_ticketholder.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#define ASSERT_SOON_EXP(exp)                         \
    if (!(exp)) {                                    \
        LOGV2_WARNING(7207203,                       \
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
            LOGV2_ERROR(7207202,
                        "assertSoon failed, please check the logs for the reason all attempts have "
                        "failed.");
            ASSERT_TRUE(false);
        }
        sleepFor(kSleepTime);
    }
}

void basicTimeout(OperationContext* opCtx, std::unique_ptr<TicketHolderWithQueueingStats> holder) {
    auto mode = TicketHolder::WaitMode::kInterruptible;
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
    holder->resize(opCtx, 6);
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

    holder->resize(opCtx, 5);
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->outof(), 5);
    ASSERT_FALSE(holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds(1), mode));
}

TEST_F(TicketHolderTest, BasicTimeoutSemaphore) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    basicTimeout(_opCtx.get(), std::make_unique<SemaphoreTicketHolder>(1, &serviceContext));
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
    MockAdmission(ServiceContext* serviceContext, AdmissionContext::Priority priority) {
        client = serviceContext->makeClient("");
        opCtx = client->makeOperationContext();
        admCtx.setPriority(priority);
    }

    AdmissionContext admCtx;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
    boost::optional<Ticket> ticket;
};

// Verify that resize operations don't alter metrics outside of those linked to the number of
// tickets.
void resizeTest(OperationContext* opCtx,
                std::unique_ptr<TicketHolderWithQueueingStats> holder,
                TickSourceMock<Microseconds>* tickSource,
                bool testWithOutstandingImmediateOperation = false) {
    Stats stats(holder.get());

    // An outstanding kImmediate priority operation should not impact resize statistics.
    MockAdmission immediatePriorityAdmission(getGlobalServiceContext(),
                                             AdmissionContext::Priority::kImmediate);
    if (testWithOutstandingImmediateOperation) {
        immediatePriorityAdmission.ticket =
            holder->acquireImmediateTicket(&immediatePriorityAdmission.admCtx);
        ASSERT(immediatePriorityAdmission.ticket);
    }

    AdmissionContext admCtx;
    admCtx.setPriority(AdmissionContext::Priority::kNormal);
    auto mode = TicketHolder::WaitMode::kInterruptible;

    auto ticket =
        holder->waitForTicketUntil(opCtx, &admCtx, Date_t::now() + Milliseconds{500}, mode);

    ASSERT_EQ(holder->used(), 1);
    ASSERT_EQ(holder->available(), 0);
    ASSERT_EQ(holder->outof(), 1);

    auto currentStats = stats.getNonTicketStats();

    tickSource->advance(Microseconds{100});
    holder->resize(opCtx, 10);

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

    holder->resize(opCtx, 1);
    newStats = stats.getNonTicketStats();
    ASSERT_EQ(currentStats.woCompare(newStats), 0);

    tickSource->advance(Microseconds{100});
    holder->resize(opCtx, 10);
    currentStats = stats.getNonTicketStats();
    ASSERT_EQ(currentStats.woCompare(newStats), 0);
}

TEST_F(TicketHolderTest, ResizeStatsSemaphore) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());

    resizeTest(
        _opCtx.get(), std::make_unique<SemaphoreTicketHolder>(1, &serviceContext), tickSource);
}

TEST_F(TicketHolderTest, ResizeStatsSemaphoreWithOutstandingImmediatePriority) {
    ServiceContext serviceContext;
    serviceContext.setTickSource(std::make_unique<TickSourceMock<Microseconds>>());
    auto tickSource = dynamic_cast<TickSourceMock<Microseconds>*>(serviceContext.getTickSource());

    resizeTest(_opCtx.get(),
               std::make_unique<SemaphoreTicketHolder>(1, &serviceContext),
               tickSource,
               true);
}
}  // namespace

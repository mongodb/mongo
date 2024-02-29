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

#include "mongo/util/concurrency/ticketholder_test_fixture.h"

#include <array>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

namespace mongo {

void TicketHolderTestFixture::setUp() {
    ServiceContextTest::setUp();
    _client = getServiceContext()->getService()->makeClient("test");
    _opCtx = _client->makeOperationContext();
}

void TicketHolderTestFixture::basicTimeout(OperationContext* opCtx,
                                           std::unique_ptr<TicketHolder> holder) {
    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 1);
    ASSERT_EQ(holder->outof(), 1);

    AdmissionContext admCtx;
    admCtx.setPriority(AdmissionContext::Priority::kNormal);
    Microseconds timeInQueue(0);
    {
        // Ignores deadline if there is a ticket instantly available.
        auto ticket = holder->waitForTicketUntil(
            *opCtx, &admCtx, Date_t::now() - Milliseconds(100), timeInQueue);
        ASSERT(ticket);
        ASSERT_EQ(holder->used(), 1);
        ASSERT_EQ(holder->available(), 0);
        ASSERT_EQ(holder->outof(), 1);

        // Respects there are none available.
        ASSERT_FALSE(holder->waitForTicketUntil(*opCtx, &admCtx, Date_t::now(), timeInQueue));
        ASSERT_FALSE(holder->waitForTicketUntil(
            *opCtx, &admCtx, Date_t::now() + Milliseconds(42), timeInQueue));
    }

    ASSERT_EQ(holder->used(), 0);
    ASSERT_EQ(holder->available(), 1);
    ASSERT_EQ(holder->outof(), 1);
}

void TicketHolderTestFixture::resizeTest(OperationContext* opCtx,
                                         std::unique_ptr<TicketHolder> holder,
                                         TickSourceMock<Microseconds>* tickSource) {
    Stats stats(holder.get());

    AdmissionContext admCtx;
    admCtx.setPriority(AdmissionContext::Priority::kNormal);
    Microseconds timeInQueue(0);
    auto ticket =
        holder->waitForTicketUntil(*opCtx, &admCtx, Date_t::now() + Milliseconds{500}, timeInQueue);

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

    holder->resize(6);
    std::array<boost::optional<Ticket>, 5> tickets;
    {
        auto ticket = holder->waitForTicket(*opCtx, &admCtx, timeInQueue);
        ASSERT_EQ(holder->used(), 1);
        ASSERT_EQ(holder->outof(), 6);

        for (int i = 0; i < 5; ++i) {
            tickets[i] = holder->waitForTicket(*opCtx, &admCtx, timeInQueue);
            ASSERT_EQ(holder->used(), 2 + i);
            ASSERT_EQ(holder->outof(), 6);
        }
        ASSERT_FALSE(holder->waitForTicketUntil(
            *opCtx, &admCtx, Date_t::now() + Milliseconds(1), timeInQueue));
    }

    holder->resize(5);
    ASSERT_EQ(holder->used(), 5);
    ASSERT_EQ(holder->outof(), 5);
    ASSERT_FALSE(
        holder->waitForTicketUntil(*opCtx, &admCtx, Date_t::now() + Milliseconds(1), timeInQueue));

    ASSERT_FALSE(holder->resize(4, Date_t::now() + Milliseconds(1)));
}

void TicketHolderTestFixture::interruptTest(OperationContext* opCtx,
                                            std::unique_ptr<TicketHolder> holder) {
    holder->resize(0);
    Microseconds timeInQueue(0);

    auto waiter = stdx::thread([&]() {
        AdmissionContext admCtx;
        ASSERT_THROWS_CODE(holder->waitForTicketUntil(*opCtx, &admCtx, Date_t::max(), timeInQueue),
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

}  // namespace mongo

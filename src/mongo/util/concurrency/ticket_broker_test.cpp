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

#include <condition_variable>
#include <mutex>

#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/ticket_broker.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#define ASSERT_SOON_EXP(exp)                         \
    if (!(exp)) {                                    \
        LOGV2_WARNING(7206702,                       \
                      "Expression failed, retrying", \
                      "exp"_attr = #exp,             \
                      "file"_attr = __FILE__,        \
                      "line"_attr = __LINE__);       \
        return false;                                \
    }

namespace {
using namespace mongo;

static inline const Seconds kWaitTimeout{20};
static inline const Milliseconds kSleepTime{1};

/**
 * Asserts that eventually the predicate does not throw an exception.
 */
#define assertSoon(predicate)                                                                    \
    {                                                                                            \
        Timer t;                                                                                 \
        while (!predicate()) {                                                                   \
            if (t.elapsed() >= kWaitTimeout) {                                                   \
                LOGV2_ERROR(                                                                     \
                    7206701,                                                                     \
                    "assertSoon failed, please check the logs for the reason all attempts have " \
                    "failed.",                                                                   \
                    "file"_attr = __FILE__,                                                      \
                    "line"_attr = __LINE__);                                                     \
                FAIL("assertSoon failed");                                                       \
            }                                                                                    \
        }                                                                                        \
    }

TEST(TicketBrokerTest, BasicTimeout) {
    TicketBroker broker;

    stdx::mutex brokerMutex;  // NOLINT

    {
        stdx::unique_lock lk(brokerMutex);
        auto result =
            broker.attemptWaitForTicketUntil(std::move(lk), Date_t::now() + Milliseconds{10});
        ASSERT_FALSE(result.hasTicket);
        ASSERT_TRUE(result.hasTimedOut);
    }

    {
        stdx::unique_lock lk(brokerMutex);
        auto result = broker.attemptWaitForTicketUntil(std::move(lk), Date_t::min());
        ASSERT_FALSE(result.hasTicket);
        ASSERT_TRUE(result.hasTimedOut);
    }
}

TEST(TicketBrokerTest, HandOverWorks) {
    TicketBroker broker;

    stdx::mutex brokerMutex;  // NOLINT

    {
        {
            stdx::unique_lock growthLock(brokerMutex);
            ASSERT_FALSE(broker.attemptToTransferTicket(growthLock));
        }

        stdx::thread waitingThread([&] {
            stdx::unique_lock lk(brokerMutex);
            auto result =
                broker.attemptWaitForTicketUntil(std::move(lk), Date_t::now() + Seconds{10});
            ASSERT_TRUE(result.hasTicket);
            ASSERT_FALSE(result.hasTimedOut);
        });

        assertSoon([&] {
            ASSERT_SOON_EXP(broker.waitingThreadsRelaxed() == 1);
            return true;
        });

        {
            stdx::unique_lock growthLock(brokerMutex);
            ASSERT_TRUE(broker.attemptToTransferTicket(growthLock));
        }

        waitingThread.join();
    }

    {
        static constexpr auto threadsToTest = 10;
        AtomicWord<int32_t> pendingThreads{threadsToTest};
        std::vector<stdx::thread> threads;

        for (int i = 0; i < threadsToTest; i++) {
            threads.emplace_back([&] {
                stdx::unique_lock lk(brokerMutex);
                auto result =
                    broker.attemptWaitForTicketUntil(std::move(lk), Date_t::now() + Seconds{10});
                ASSERT_TRUE(result.hasTicket);
                ASSERT_FALSE(result.hasTimedOut);
                pendingThreads.subtractAndFetch(1);
            });
        }

        assertSoon([&] {
            ASSERT_SOON_EXP(broker.waitingThreadsRelaxed() == 10);
            return true;
        });

        for (int i = 1; i <= threadsToTest; i++) {
            stdx::unique_lock growthLock(brokerMutex);
            ASSERT_TRUE(broker.attemptToTransferTicket(growthLock));
            assertSoon([&] {
                ASSERT_SOON_EXP(pendingThreads.load() == threadsToTest - i);
                return true;
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }
    }
}
}  // namespace

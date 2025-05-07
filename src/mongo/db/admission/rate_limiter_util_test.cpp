/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/admission/rate_limiter.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/notification.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::admission {

namespace {

class RateLimiterTest : public ClockSourceMockServiceContextTest {
public:
    auto makeClientsWithOpCtxs(size_t numOps) {
        std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
            clientsWithOps;
        for (size_t i = 0; i < numOps; i++) {
            auto client =
                getServiceContext()->getService()->makeClient(fmt::format("test client {}", i));
            auto opCtx = client->makeOperationContext();
            clientsWithOps.emplace_back(std::move(client), std::move(opCtx));
        }

        return clientsWithOps;
    }

    void advanceTime(Milliseconds d) {
        static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource())->advance(d);
    }

private:
    unittest::MinimumLoggedSeverityGuard logSeverityGuard{logv2::LogComponent::kDefault,
                                                          logv2::LogSeverity::Debug(4)};
};

TEST_F(RateLimiterTest, BasicTokenAcquisition) {
    RateLimiter rateLimiter(DBL_MAX, DBL_MAX, INT_MAX);
    auto opCtx = makeOperationContext();
    ASSERT_OK(rateLimiter.acquireToken(opCtx.get()));
}

TEST_F(RateLimiterTest, InvalidBurstSize) {
    ASSERT_THROWS_CODE(RateLimiter(1.0, 0, 0), DBException, ErrorCodes::InvalidOptions);

    RateLimiter rateLimiter(DBL_MAX, DBL_MAX, INT_MAX);
    ASSERT_THROWS_CODE(rateLimiter.setBurstSize(0), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(RateLimiterTest, ConcurrentTokenAcquisitionWithQueueing) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        const int maxTokens = 5;
        const int refreshRate = 4;
        const size_t numThreads = 20;
        const Milliseconds tokenInterval = Milliseconds(1002) / refreshRate;

        RateLimiter rateLimiter(refreshRate, maxTokens, INT_MAX);

        std::vector<unittest::JoinThread> threads;
        auto clientsWithOps = makeClientsWithOpCtxs(numThreads);

        stdx::mutex mutex;
        stdx::condition_variable cv;
        size_t acquiredTokens = 0;

        std::vector<double> tokenAcquisitionTimes;

        for (size_t i = 0; i < numThreads; i++) {
            threads.emplace_back(monitor.spawn([&, threadNum = i]() {
                ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[threadNum].second.get()));
                stdx::lock_guard lg(mutex);
                LOGV2(10440801,
                      "Acquired token",
                      "threadNum"_attr = threadNum,
                      "mockClockNow"_attr =
                          getServiceContext()->getFastClockSource()->now().toMillisSinceEpoch());
                tokenAcquisitionTimes.emplace_back(
                    getServiceContext()->getFastClockSource()->now().toMillisSinceEpoch());
                acquiredTokens++;
                cv.notify_one();
            }));
        }

        auto client = getServiceContext()->getService()->makeClient("test client");
        auto opCtx = client->makeOperationContext();

        // Make sure the initial burstRate fulfills the first requests
        {
            stdx::unique_lock<stdx::mutex> lk(mutex);
            ASSERT_DOES_NOT_THROW(opCtx->waitForConditionOrInterrupt(
                cv, mutex, [&] { return acquiredTokens == maxTokens; }));
        }

        // Until we start moving the mock clock forward, no other requests will be fulfilled.
        ASSERT_EQ(acquiredTokens, maxTokens);

        // Advancing time less than tokenInterval doesn't cause a token to be acquired.
        auto smallAdvance = Milliseconds{10};
        ASSERT_LT(smallAdvance, tokenInterval);
        advanceTime(smallAdvance);
        ASSERT_EQ(acquiredTokens, maxTokens);

        // For each remaining token, ensure that the rate limiter gives out a token every 1000 /
        // refreshRate milliseconds.
        for (size_t i = 1; i <= numThreads - maxTokens; i++) {
            stdx::unique_lock<stdx::mutex> lk(mutex);
            advanceTime(tokenInterval);
            ASSERT_DOES_NOT_THROW(opCtx->waitForConditionOrInterrupt(
                cv, mutex, [&] { return acquiredTokens == maxTokens + i; }));
        }

        ASSERT_EQ(acquiredTokens, numThreads);

        // Assert that the tokens were acquired at the correct intervals.
        for (size_t i = 0; i < numThreads; i++) {
            if (i < maxTokens) {
                ASSERT_APPROX_EQUAL(tokenAcquisitionTimes[i], 1, 1e-3);
            } else {
                ASSERT_APPROX_EQUAL(
                    tokenAcquisitionTimes[i],
                    1 + durationCount<Milliseconds>(smallAdvance) +
                        (((i + 1) - maxTokens) * durationCount<Milliseconds>(tokenInterval)),
                    1e-3);
            }
        }
    });
}

TEST_F(RateLimiterTest, RejectOverMaxWaiters) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        RateLimiter rateLimiter(.01, 1.0, 1);
        auto clientsWithOps = makeClientsWithOpCtxs(3);
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        Notification<void> firstTokenAcquired;
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The next token acquisition is queued because the refresh rate is low and is eventually
        // cancelled.
        Notification<void> clientEnqueued;
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            clientEnqueued.set();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::InterruptedAtShutdown, ""));
        }));

        // The final token acquisition is rejected because it is above the maximum queue depth.
        Notification<void> hasFailed;
        threads.emplace_back(monitor.spawn([&]() {
            clientEnqueued.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[2].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::TemporarilyUnavailable, ""));
            hasFailed.set();
        }));

        hasFailed.get();
        getServiceContext()->setKillAllOperations();
    });
}

TEST_F(RateLimiterTest, QueueingDisabled) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        RateLimiter rateLimiter(.01, 1.0, 0);
        auto clientsWithOps = makeClientsWithOpCtxs(2);
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        Notification<void> firstTokenAcquired;
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The next token acquisition attempt fails because it would need to queue.
        Notification<void> clientEnqueued;
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            clientEnqueued.set();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::TemporarilyUnavailable, ""));
        }));
    });
}

TEST_F(RateLimiterTest, InterruptedDueToOperationKilled) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        RateLimiter rateLimiter(.01, 1.0, INT_MAX);
        auto clientsWithOps = makeClientsWithOpCtxs(2);
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        Notification<void> firstTokenAcquired;
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.set();
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
        }));

        // The second token acquisition queues until its opCtx is killed below.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::ClientDisconnect, ""));
        }));

        firstTokenAcquired.get();
        clientsWithOps[1].second->markKilled(ErrorCodes::ClientDisconnect);
    });
}

TEST_F(RateLimiterTest, InterruptedDueToOperationDeadline) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        RateLimiter rateLimiter(.01, 1.0, INT_MAX);
        auto clientsWithOps = makeClientsWithOpCtxs(2);
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        Notification<void> firstTokenAcquired;
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.set();
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
        }));

        // The second token acquisition queues until its opCtx deadline passes.
        clientsWithOps[1].second->setDeadlineByDate(
            getServiceContext()->getFastClockSource()->now() + Milliseconds(5),
            ErrorCodes::MaxTimeMSExpired);
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::MaxTimeMSExpired, ""));
        }));

        firstTokenAcquired.get();
        advanceTime(Milliseconds(5));
    });
}

TEST_F(RateLimiterTest, InterruptedDueToKillAllOperations) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        RateLimiter rateLimiter(.01, 1.0, INT_MAX);
        auto clientsWithOps = makeClientsWithOpCtxs(2);
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        Notification<void> firstTokenAcquired;
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The second token acquisition queues until its opCtx is killed.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::InterruptedAtShutdown, ""));
        }));

        firstTokenAcquired.get();
        getServiceContext()->setKillAllOperations();

        // Acquiring a token after shutdown also fails.
        auto client = getServiceContext()->getService()->makeClient("test client");
        auto opCtx = client->makeOperationContext();
        Status token = rateLimiter.acquireToken(opCtx.get());
        ASSERT_EQ(token, Status(ErrorCodes::InterruptedAtShutdown, ""));
    });
}

}  // namespace
}  // namespace mongo::admission

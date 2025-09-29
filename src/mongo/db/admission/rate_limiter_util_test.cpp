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

auto makeClientsWithOpCtxs(ServiceContext* svcCtx, size_t numOps) {
    std::vector<std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>>
        clientsWithOps;
    for (size_t i = 0; i < numOps; i++) {
        auto client = svcCtx->getService()->makeClient(fmt::format("test client {}", i));
        auto opCtx = client->makeOperationContext();
        clientsWithOps.emplace_back(std::move(client), std::move(opCtx));
    }

    return clientsWithOps;
}

auto constexpr convertBurstSizeToBurstCapacitySecs(double refreshRate, double burstSize) {
    return burstSize / refreshRate;
}

auto assertApproxEqualUnevenBounds(double value, double lowerBound, double upperBound) {
    ASSERT_GTE(value, lowerBound);
    ASSERT_LT(value, upperBound);
}

class RateLimiterTest : public ServiceContextTest {
private:
    unittest::MinimumLoggedSeverityGuard logSeverityGuard{logv2::LogComponent::kDefault,
                                                          logv2::LogSeverity::Debug(4)};
};

// Verify that a RateLimiter with sufficient capacity will dispense a token.
TEST_F(RateLimiterTest, BasicTokenAcquisition) {
    RateLimiter rateLimiter(DBL_MAX, DBL_MAX, INT_MAX, "BasicTokenAcquisition");
    auto opCtx = makeOperationContext();
    ASSERT_OK(rateLimiter.acquireToken(opCtx.get()));

    ASSERT_EQ(rateLimiter.stats().successfulAdmissions.get(), 1);
    ASSERT_EQ(rateLimiter.stats().attemptedAdmissions.get(), 1);

    ASSERT_EQ(rateLimiter.stats().addedToQueue.get(), 0);
    ASSERT_EQ(rateLimiter.stats().removedFromQueue.get(), 0);
    ASSERT_EQ(rateLimiter.stats().rejectedAdmissions.get(), 0);
    // Immediate acquisition (without throttling, i.e. no sleep) still counts as a queue time of
    // zero, so the average timing metric will have that as its first value.
    ASSERT(rateLimiter.stats().averageTimeQueuedMicros.get().has_value());
    ASSERT_APPROX_EQUAL(*rateLimiter.stats().averageTimeQueuedMicros.get(), 0.0, 0.1);
}

// Verify that RateLimiter::setBurstSize range checks its input.
TEST_F(RateLimiterTest, InvalidBurstSize) {
    ASSERT_THROWS_CODE(
        RateLimiter(1.0, 0, 0, "InvalidBurstSize"), DBException, ErrorCodes::InvalidOptions);

    RateLimiter rateLimiter(DBL_MAX, DBL_MAX, INT_MAX, "InvalidBurstSize");
    ASSERT_THROWS_CODE(
        rateLimiter.updateRateParameters(1.0, 0), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(RateLimiterTest, RateLimitIsValidAfterQueueing) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = 2.0;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);
        constexpr int maxQueueDepth = 4;
        constexpr int numThreads = 10;
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), numThreads + 1);

        RateLimiter rateLimiter(
            refreshRate, burstCapacitySecs, maxQueueDepth, "RateLimitIsValidAfterQueueing");
        auto tickSource = getServiceContext()->getTickSource();

        auto startTicks = tickSource->getTicks();
        Atomic<size_t> numAdmitted = 0;
        Atomic<size_t> numRejected = 0;
        {
            std::vector<unittest::JoinThread> threads;
            for (size_t i = 0; i < numThreads; i++) {
                threads.emplace_back(monitor.spawn([&, threadNum = i]() {
                    if (rateLimiter.acquireToken(clientsWithOps[threadNum].second.get()).isOK()) {
                        LOGV2(10614903, "Acquired token", "threadNum"_attr = threadNum);
                        numAdmitted.fetchAndAdd(1);
                    } else {
                        LOGV2(10614904,
                              "Token acquisition request rejected",
                              "threadNum"_attr = threadNum);
                        numRejected.fetchAndAdd(1);
                    }
                }));
            }
        }

        // We assert that the approx. correct amount of time has passed.
        auto expectedElapsedMillis = (maxQueueDepth / refreshRate) * 1000;
        Milliseconds elapsed =
            tickSource->ticksTo<Milliseconds>(tickSource->getTicks() - startTicks);
        LOGV2(10614901,
              "Elapsed vs. expected elapsed millis",
              "elapsed"_attr = elapsed,
              "expected"_attr = expectedElapsedMillis);
        // The folly token bucket may dispense tokens at a slightly lower rate than specified,
        // but it must not dispense tokens at a higher rate.
        assertApproxEqualUnevenBounds(durationCount<Milliseconds>(elapsed),
                                      expectedElapsedMillis - 100,
                                      expectedElapsedMillis + 1000);

        // Once all the threads have joined, we expect that 3 threads were admitted and 2 were
        // rejected.
        ASSERT_EQ(numAdmitted.load(), burstSize + maxQueueDepth);
        ASSERT_EQ(numRejected.load(), numThreads - (burstSize + maxQueueDepth));

        // We also expect the token bucket balance to be between ~0-1, as the token was returned to
        // the bucket and some additional time may have passed due to the fuzziness of the token
        // bucket.
        assertApproxEqualUnevenBounds(rateLimiter.tokenBalance(), -0.1, 1.1);

        // And finally, a new token acquisition will take approx. 0 - 1/refreshRate seconds,
        // accounting for fuzziness of the token bucket.
        auto expectedFinalMillis = (1 / refreshRate) * 1000;
        auto finalStartTicks = tickSource->getTicks();
        ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[numThreads].second.get()));
        Milliseconds finalElapsed =
            tickSource->ticksTo<Milliseconds>(tickSource->getTicks() - finalStartTicks);
        LOGV2(10614902,
              "Elapsed vs. expected elapsed millis",
              "elapsed"_attr = finalElapsed,
              "expected"_attr = expectedFinalMillis);
        // We use uneven bounds here to account for the fact that the token bucket is imprecise-- if
        // it dispensed tokens at a lower rate than specified earlier in the test, there may already
        // be a token available now, and so sleep time is 0. The important assertion here is that we
        // didn't wait for longer than 1 token period (with fuzzy bounds due to possible slow
        // machines).
        assertApproxEqualUnevenBounds(
            durationCount<Milliseconds>(finalElapsed), 0, expectedFinalMillis + 500);
    });
}

// Verify that RateLimiter will reject a request for a token if:
// - the request would otherwise be enqueued (there are insufficent tokens),
// - but there are already the maximum number of threads enqueued.
TEST_F(RateLimiterTest, RejectOverMaxWaiters) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = 0.1;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter(refreshRate, burstCapacitySecs, 1, "RejectOverMaxWaiters");
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 3);
        Notification<void> firstTokenAcquired;
        Notification<void> hasFailed;
        Status status1 = Status::OK();
        Status status2 = Status::OK();
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // Enqueue two requests, both of which will queue due to the low refreshRate. Whichever
        // request comes in second will be rejected, because maxQueueDepth is 1.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            status1 = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            if (!hasFailed) {
                hasFailed.set();
            }
        }));
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            status2 = rateLimiter.acquireToken(clientsWithOps[2].second.get());
            if (!hasFailed) {
                hasFailed.set();
            }
        }));

        // Wait until one of the operations has failed.
        hasFailed.get();

        LOGV2(10574302,
              "At least one thread finished",
              "status1"_attr = status1,
              "status2"_attr = status2);

        // Assert that exactly one of the statuses is from rate limiter rejection.
        ASSERT((status1.code() == RateLimiter::kRejectedErrorCode) ^
               (status2.code() == RateLimiter::kRejectedErrorCode));
        ASSERT_EQ(rateLimiter.stats().rejectedAdmissions.get(), 1);

        // Assert that the token balance is between ~0 and -1, as only one token should have been
        // borrowed from the bucket. The bucket may have refilled slightly past 0 on slow machines,
        // and so we account for that in the assertion.
        ASSERT_LT(rateLimiter.tokenBalance(), 0.2);
        ASSERT_GT(rateLimiter.tokenBalance(), -1);

        // Interrupt the other token acquisition.
        getServiceContext()->setKillAllOperations();
    });
}

// Verify that if the maximum queue depth is configured to be zero, then any requests that would
// otherwise queue are instead rejected.
TEST_F(RateLimiterTest, QueueingDisabled) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = .01;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter(refreshRate, burstCapacitySecs, 0, "QueueingDisabled");
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 2);
        Notification<void> firstTokenAcquired;

        std::vector<unittest::JoinThread> threads;
        // Expect the first token acquisition to succeed. We have at least as many tokens
        // available as the configured burst size, which is 1.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The next token acquisition attempt fails because it would need to queue. We'd need to
        // wait another 100 seconds for the next token, and the max queue depth is zero, so the
        // request is rejected.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            LOGV2(10574301, "Final token acquisition result", "status"_attr = token);
            ASSERT_EQ(token, Status(RateLimiter::kRejectedErrorCode, ""));

            ASSERT_EQ(rateLimiter.stats().successfulAdmissions.get(), 1);
            ASSERT_EQ(rateLimiter.stats().rejectedAdmissions.get(), 1);
            ASSERT_EQ(rateLimiter.stats().attemptedAdmissions.get(), 2);

            // Assert that the token balance is ~0, as only one token should have been
            // consumed from the bucket.
            ASSERT_APPROX_EQUAL(rateLimiter.tokenBalance(), 0, .1);
        }));
    });
}

// Verify that if a client disconnects while their session thread is asleep in the rate limiter,
// the rate limiter wakes up the thread and returns the appropriate error status.
TEST_F(RateLimiterTest, InterruptedDueToOperationKilled) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = .01;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);


        RateLimiter rateLimiter(
            refreshRate, burstCapacitySecs, INT_MAX, "InterruptedDueToOperationKilled");
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 2);
        Notification<void> firstTokenAcquired;
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The second token acquisition queues until its opCtx is killed below.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::ClientDisconnect, ""));

            // The first thread was immediately admitted.
            // The second thread was enqueued, then was interrupted, and then was dequeued.
            // The second thread was enqueued for some finite time, so the average queue time
            // could be greater than zero.
            ASSERT_EQ(rateLimiter.stats().successfulAdmissions.get(), 1);
            ASSERT_EQ(rateLimiter.stats().interruptedInQueue.get(), 1);
            ASSERT_EQ(rateLimiter.stats().attemptedAdmissions.get(), 2);
            ASSERT_EQ(rateLimiter.stats().addedToQueue.get(), 1);
            ASSERT_EQ(rateLimiter.stats().removedFromQueue.get(), 1);
            ASSERT_NE(rateLimiter.stats().averageTimeQueuedMicros.get(), boost::none);
            ASSERT_GTE(*rateLimiter.stats().averageTimeQueuedMicros.get(), 0.0);
            // Assert that the token balance is ~0, as only one token should have been
            // consumed from the bucket.
            ASSERT_APPROX_EQUAL(rateLimiter.tokenBalance(), 0, .1);
        }));

        firstTokenAcquired.get();
        clientsWithOps[1].second->markKilled(ErrorCodes::ClientDisconnect);
    });
}


// This is like the previous two tests, but instead of a client disconnect or a timeout, it's a
// service shutdown.
TEST_F(RateLimiterTest, InterruptedDueToKillAllOperations) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = .01;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter(
            refreshRate, burstCapacitySecs, INT_MAX, "InterruptedDueToKillAllOperations");
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 2);
        Notification<void> firstTokenAcquired;
        Notification<void> secondTokenInterrupted;
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The second token acquisition queues until its opCtx is killed.
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::InterruptedAtShutdown, ""));
            secondTokenInterrupted.set();
        }));

        firstTokenAcquired.get();
        getServiceContext()->setKillAllOperations();

        // Acquiring a token after shutdown also fails.
        auto client = getServiceContext()->getService()->makeClient("test client");
        auto opCtx = client->makeOperationContext();
        Status token = rateLimiter.acquireToken(opCtx.get());
        ASSERT_EQ(token, Status(ErrorCodes::InterruptedAtShutdown, ""));

        secondTokenInterrupted.get();

        // The first thread was immediately admitted.
        // The second thread was enqueued, then was interrupted by shutdown, and then was
        // dequeued.
        // This thread was enqueued, then was interrupted by shutdown immediately, and then was
        // dequeued.
        // The second thread and this thread were enqueued for some finite time, so the average
        // queue time is not null.
        ASSERT_EQ(rateLimiter.stats().successfulAdmissions.get(), 1);
        ASSERT_EQ(rateLimiter.stats().interruptedInQueue.get(), 2);
        ASSERT_EQ(rateLimiter.stats().addedToQueue.get(), 2);
        ASSERT_EQ(rateLimiter.stats().removedFromQueue.get(), 2);
        ASSERT_EQ(rateLimiter.stats().attemptedAdmissions.get(), 3);
        ASSERT_NE(rateLimiter.stats().averageTimeQueuedMicros.get(), boost::none);
    });
}

// Verify that `RateLimiter::recordExemption()` increments the exemption metric but no others.
TEST_F(RateLimiterTest, RecordExemption) {
    RateLimiter rateLimiter(INT_MAX, INT_MAX, INT_MAX, "RecordExemption");
    rateLimiter.recordExemption();

    ASSERT_EQ(rateLimiter.stats().exemptedAdmissions.get(), 1);

    ASSERT_EQ(rateLimiter.stats().attemptedAdmissions.get(), 0);
    ASSERT_EQ(rateLimiter.stats().successfulAdmissions.get(), 0);
    ASSERT_EQ(rateLimiter.stats().interruptedInQueue.get(), 0);
    ASSERT_EQ(rateLimiter.stats().addedToQueue.get(), 0);
    ASSERT_EQ(rateLimiter.stats().removedFromQueue.get(), 0);
    ASSERT_EQ(rateLimiter.stats().averageTimeQueuedMicros.get(), boost::none);
}


class RateLimiterWithMockClockTest : public ClockSourceMockServiceContextTest {
public:
    /**
     * Note that the mocked clock in this test applies to sleeping on the opCtx rather than to the
     * internal folly::TokenBucket's calculations.
     */
    void advanceTime(Milliseconds d) {
        static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource())->advance(d);
    }

private:
    unittest::MinimumLoggedSeverityGuard logSeverityGuard{logv2::LogComponent::kDefault,
                                                          logv2::LogSeverity::Debug(4)};
};

// Verify that a newly initialized RateLimiter can immediately dispense up to its burst rate of
// tokens, and thereafter releases tokens at its configured rate.
// If multiple threads concurrently request tokens, then some of the threads will be admitted
// immediately, while the remainder will be admitted as tokens become available.
TEST_F(RateLimiterWithMockClockTest, ConcurrentTokenAcquisitionWithQueueing) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        const int maxTokens = 2;
        const int refreshRate = 4;
        const double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, maxTokens);
        const int64_t numThreads = 10;
        const Milliseconds tokenInterval = Milliseconds(1000) / refreshRate;

        RateLimiter rateLimiter(
            refreshRate, burstCapacitySecs, INT_MAX, "ConcurrentTokenAcquisitionWithQueueing");

        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), numThreads);

        stdx::mutex mutex;
        stdx::condition_variable cv;

        std::vector<double> tokenAcquisitionTimes;

        std::vector<unittest::JoinThread> threads;
        for (int64_t i = 0; i < numThreads; i++) {
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
                cv.notify_one();
            }));
        }

        // Make sure the initial burstRate fulfills the first requests
        {
            stdx::unique_lock<stdx::mutex> lk(mutex);
            ASSERT_DOES_NOT_THROW(
                cv.wait(lk, [&] { return (int)tokenAcquisitionTimes.size() == maxTokens; }));
        }

        // At this point, maxTokens threads have been allowed through. The rest of the threads
        // are about to be blocked in sleep (or have been already).
        // Queue timing metric samples have been collected, but they're also zero due to the
        // burst availability.
        // Some threads might have been enqueued, but none have been dequeued.
        ASSERT_EQ(rateLimiter.stats().successfulAdmissions.get(), maxTokens);
        ASSERT_EQ(rateLimiter.stats().removedFromQueue.get(), 0);
        ASSERT_EQ(rateLimiter.stats().rejectedAdmissions.get(), 0);
        ASSERT(rateLimiter.stats().averageTimeQueuedMicros.get().has_value());
        ASSERT_APPROX_EQUAL(*rateLimiter.stats().averageTimeQueuedMicros.get(), 0.0, .1);

        // Make sure we've enqueued all the remaining waiters so that we don't race with advancing
        // the mock clock.
        int64_t numRetries = 0;
        const int64_t maxRetries = 5;
        int64_t backoffTimeMillis{5};
        while (rateLimiter.queued() != numThreads - maxTokens && numRetries++ < maxRetries) {
            sleepmillis(backoffTimeMillis);
            backoffTimeMillis *= 5;
        }

        // Until we start moving the mock clock forward, no other requests will be fulfilled and all
        // other requests will be waiting.
        ASSERT_EQ(rateLimiter.queued(), numThreads - maxTokens);
        ASSERT_EQ((int)tokenAcquisitionTimes.size(), maxTokens);

        ASSERT_EQ(rateLimiter.stats().successfulAdmissions.get(), maxTokens);
        ASSERT_EQ(rateLimiter.stats().addedToQueue.get(), numThreads - maxTokens);
        ASSERT_EQ(rateLimiter.stats().removedFromQueue.get(), 0);
        ASSERT_EQ(rateLimiter.stats().rejectedAdmissions.get(), 0);
        ASSERT(rateLimiter.stats().averageTimeQueuedMicros.get().has_value());
        ASSERT_APPROX_EQUAL(*rateLimiter.stats().averageTimeQueuedMicros.get(), 0.0, .1);

        // Advancing time less than tokenInterval doesn't cause a token to be acquired.
        auto smallAdvance = Milliseconds{10};
        ASSERT_LT(smallAdvance, tokenInterval);
        advanceTime(smallAdvance);
        ASSERT_EQ((int)tokenAcquisitionTimes.size(), maxTokens);

        // For each remaining token, ensure that the rate limiter gives out a token every 1000 /
        // refreshRate milliseconds.
        // Time out overall after 20 seconds, though, so the test doesn't hang on failure.
        const auto deadline = (Date_t::now() + Seconds(20)).toSystemTimePoint();
        for (int64_t i = 1; i <= numThreads - maxTokens; i++) {
            stdx::unique_lock<stdx::mutex> lk(mutex);
            advanceTime(tokenInterval);
            // If the cv deadline, which is based on the system (rather than the mock) clock,
            // passes, we want to explicitly kill all operations to ensure that the test does not
            // hang and we can diagnose the issue.
            if (!cv.wait_until(lk, deadline, [&] {
                    return (int)tokenAcquisitionTimes.size() == maxTokens + i;
                })) {
                getServiceContext()->setKillAllOperations();
                // We re-run this assertion so that proper diagnostics are output.
                ASSERT_EQ((int)tokenAcquisitionTimes.size(), maxTokens + i);
            }

            // Metrics will reflect that an enqueued thread woke up and was dequeued, which
            // takes some measurable (but still possibly zero) amount of time.
            ASSERT_EQ(rateLimiter.stats().successfulAdmissions.get(), maxTokens + i);
            ASSERT_EQ(rateLimiter.stats().removedFromQueue.get(), i);
            ASSERT_NE(rateLimiter.stats().averageTimeQueuedMicros.get(), boost::none);
            ASSERT_GTE(*rateLimiter.stats().averageTimeQueuedMicros.get(), 0.0);
        }

        ASSERT_EQ((int)tokenAcquisitionTimes.size(), numThreads);

        // Assert that the tokens were acquired at the correct intervals.
        for (int64_t i = 0; i < numThreads; i++) {
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

        // By the end, there was one attempt per thread.
        ASSERT_EQ(rateLimiter.stats().attemptedAdmissions.get(), numThreads);
    });
}

// Verify that if the sleep within an enqueued thread is interrupted, then the rate limiter
// returns the error status corresponding to the reason for the interruption.
TEST_F(RateLimiterWithMockClockTest, InterruptedDueToOperationDeadline) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        constexpr double burstSize = 1.0;
        constexpr double refreshRate = .01;
        constexpr double burstCapacitySecs =
            convertBurstSizeToBurstCapacitySecs(refreshRate, burstSize);

        RateLimiter rateLimiter(
            refreshRate, burstCapacitySecs, INT_MAX, "InterruptedDueToOperationDeadline");
        auto clientsWithOps = makeClientsWithOpCtxs(getServiceContext(), 2);
        Notification<void> firstTokenAcquired;
        std::vector<unittest::JoinThread> threads;

        // Expect the first token acquisition to succeed.
        threads.emplace_back(monitor.spawn([&]() {
            ASSERT_OK(rateLimiter.acquireToken(clientsWithOps[0].second.get()));
            firstTokenAcquired.set();
        }));

        // The second token acquisition queues until its opCtx deadline passes.
        clientsWithOps[1].second->setDeadlineByDate(
            getServiceContext()->getFastClockSource()->now() + Milliseconds(5),
            ErrorCodes::MaxTimeMSExpired);
        threads.emplace_back(monitor.spawn([&]() {
            firstTokenAcquired.get();
            Status token = rateLimiter.acquireToken(clientsWithOps[1].second.get());
            ASSERT_EQ(token, Status(ErrorCodes::MaxTimeMSExpired, ""));

            // The first thread was immediately admitted.
            // The second thread was enqueued, then timed out, and then was dequeued.
            // The second thread was enqueued for some finite time, so the average queue time
            // could be greater than zero.
            ASSERT_EQ(rateLimiter.stats().successfulAdmissions.get(), 1);
            ASSERT_EQ(rateLimiter.stats().interruptedInQueue.get(), 1);
            ASSERT_EQ(rateLimiter.stats().addedToQueue.get(), 1);
            ASSERT_EQ(rateLimiter.stats().attemptedAdmissions.get(), 2);
            ASSERT_EQ(rateLimiter.stats().removedFromQueue.get(), 1);
            ASSERT_NE(rateLimiter.stats().averageTimeQueuedMicros.get(), boost::none);
            ASSERT_GTE(*rateLimiter.stats().averageTimeQueuedMicros.get(), 0.0);
        }));

        firstTokenAcquired.get();
        advanceTime(Milliseconds(5));
    });
}
}  // namespace
}  // namespace mongo::admission

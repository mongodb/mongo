/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/util/concurrency/ordered_ticket_semaphore.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/packaged_task.h"

#include <atomic>
#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

// Generous timeout so tests fail with a useful diagnostic instead of hanging.
constexpr auto kWaitTimeout = Milliseconds{500};

Date_t getDeadline() {
    return Date_t::now() + kWaitTimeout;
}

/**
 * Test fixture for OrderedTicketSemaphoreTest.
 */
class OrderedTicketSemaphoreTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _client = getServiceContext()->getService()->makeClient("test");
        _opCtx = _client->makeOperationContext();

        ThreadPool::Options opts;
        opts.maxThreads = 32;  // big enough for all blocked waiters.
        opts.minThreads = 0;
        _pool = std::make_unique<ThreadPool>(opts);
        _pool->startup();
    }

    template <typename Callable>
    auto launchAsync(Callable&& cb) -> Future<typename std::invoke_result<Callable>::type> {
        using ReturnType = typename std::invoke_result<Callable>::type;
        auto task = PackagedTask([cb = std::move(cb)] { return cb(); });
        auto taskFuture = task.getFuture();
        _pool->schedule([runTask = std::move(task)](Status s) mutable {
            invariant(s);
            runTask();
        });
        return taskFuture;
    }

    std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext> makeOpCtx() {
        auto client = getServiceContext()->getService()->makeClient("test");
        auto opCtx = client->makeOperationContext();
        return {std::move(client), std::move(opCtx)};
    }
    void tearDown() override {
        _pool->shutdown();
        _pool->join();
        ServiceContextTest::tearDown();
    }

    template <typename Condition>
    void waitWhile(Condition&& condition) {
        while (condition()) {
            sleepFor(Milliseconds{10});  // Give threads time to queue up.
        }
    }

    void waitForQueuedThreads(std::unique_ptr<OrderedTicketSemaphore>& ticketSemaphore,
                              int numWaiters) {
        waitWhile([&]() { return ticketSemaphore->waiters() != numWaiters; });
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    std::unique_ptr<OrderedTicketSemaphore> makeSemaphore(int numTickets, int maxQueueDepth) {
        return std::make_unique<OrderedTicketSemaphore>(numTickets, maxQueueDepth);
    }

protected:
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;

private:
    std::unique_ptr<ThreadPool> _pool;
};

/**
 * Test that verifies priority ordering: lower admission counts get served first.
 */
TEST_F(OrderedTicketSemaphoreTest, PrioritizesLowerAdmissionCounts) {
    auto sem = makeSemaphore(0, 10);  // Start with 0 tickets to force queueing.

    // Track the order in which threads acquire tickets.
    std::vector<int> orderByAdmissions(6);
    std::atomic<int32_t> acquireOrder{0};

    // Create admission contexts with different admission counts.
    MockAdmissionContext admCtx1, admCtx2, admCtx3, admCtx4, admCtx5;

    // Set different admission counts (higher numbers should have lower priority).
    int32_t kAdmCtx1Admissions = 10;
    int32_t kAdmCtx2Admissions = 1;
    int32_t kAdmCtx3Admissions = 5;
    int32_t kAdmCtx4Admissions = 3;
    int32_t kAdmCtx5Admissions = 7;
    admCtx1.setAdmission_forTest(kAdmCtx1Admissions);  // Low priority
    admCtx2.setAdmission_forTest(kAdmCtx2Admissions);  // Highest priority
    admCtx3.setAdmission_forTest(kAdmCtx3Admissions);  // Medium priority
    admCtx4.setAdmission_forTest(kAdmCtx4Admissions);  // High priority
    admCtx5.setAdmission_forTest(kAdmCtx5Admissions);  // Low-medium priority

    // Spawn 5 threads that will try to acquire tickets.
    auto future1 = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx1, Date_t::max(), true));
        orderByAdmissions[1] = acquireOrder.fetch_add(1);
    });

    auto future2 = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx2, Date_t::max(), true));
        orderByAdmissions[2] = acquireOrder.fetch_add(1);
    });

    auto future3 = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx3, Date_t::max(), true));
        orderByAdmissions[3] = acquireOrder.fetch_add(1);
    });

    auto future4 = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx4, Date_t::max(), true));
        orderByAdmissions[4] = acquireOrder.fetch_add(1);
    });

    auto future5 = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx5, Date_t::max(), true));
        orderByAdmissions[5] = acquireOrder.fetch_add(1);
    });

    // Wait for all threads to be queued.
    waitForQueuedThreads(sem, 5);

    // Release tickets one at a time to verify priority ordering.
    auto expectedWaiters = sem->waiters();
    for (int i = 0; i < 5; ++i) {
        sem->resize(1);
        expectedWaiters--;
        waitWhile([&]() {
            return sem->waiters() != expectedWaiters || acquireOrder != 5 - expectedWaiters;
        });
    }

    // Wait for all threads to complete
    future1.get();
    future2.get();
    future3.get();
    future4.get();
    future5.get();

    ASSERT_EQ(orderByAdmissions[2], 0)
        << "Admission count " << kAdmCtx2Admissions << " should acquire first but was acquired "
        << orderByAdmissions[2];
    ASSERT_EQ(orderByAdmissions[4], 1)
        << "Admission count " << kAdmCtx4Admissions << " should acquire second but was acquired "
        << orderByAdmissions[4];
    ASSERT_EQ(orderByAdmissions[3], 2)
        << "Admission count " << kAdmCtx3Admissions << " should acquire third but was acquired "
        << orderByAdmissions[3];
    ASSERT_EQ(orderByAdmissions[5], 3)
        << "Admission count " << kAdmCtx5Admissions << " should acquire fourth but was acquired "
        << orderByAdmissions[5];
    ASSERT_EQ(orderByAdmissions[1], 4)
        << "Admission count " << kAdmCtx1Admissions << " should acquire last but was acquired "
        << orderByAdmissions[1];
}

/**
 * Test that new high-priority requests jump ahead of low-priority waiting requests.
 */
TEST_F(OrderedTicketSemaphoreTest, HighPriorityJumpsQueue) {
    auto sem = makeSemaphore(0, 10);

    std::atomic<int> acquireOrder{0};
    std::vector<int> orderByAdmissions(3);

    struct AdmCtxAndIndex {
        MockAdmissionContext admCtx;
        int index;
        AdmCtxAndIndex(int admissions, int idx) {
            index = idx;
            admCtx.setAdmission_forTest(admissions);
        }
    };

    AdmCtxAndIndex lowPriority(100, 0), mediumPriority(50, 1), highPriority(1, 2);

    auto enqueueRequest = [&](AdmCtxAndIndex& admCtxAndIndex) {
        return launchAsync([&]() {
            auto [client, opCtx] = makeOpCtx();
            ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtxAndIndex.admCtx, Date_t::max(), true));
            orderByAdmissions[admCtxAndIndex.index] = acquireOrder.fetch_add(1);
        });
    };

    // First, queue a low priority request.
    auto lowPriorityFuture = enqueueRequest(lowPriority);

    waitForQueuedThreads(sem, 1);

    // Then queue a medium priority request.
    auto mediumPriorityFuture = enqueueRequest(mediumPriority);

    waitForQueuedThreads(sem, 2);

    // Finally, queue a high priority request (should jump ahead).
    auto highPriorityFuture = enqueueRequest(highPriority);

    waitForQueuedThreads(sem, 3);

    // Release tickets one at a time to verify priority ordering.
    auto expectedWaiters = sem->waiters();
    for (int i = 0; i < 3; ++i) {
        sem->resize(1);
        expectedWaiters--;
        waitWhile([&]() {
            return sem->waiters() != expectedWaiters || acquireOrder != 3 - expectedWaiters;
        });
    }

    lowPriorityFuture.get();
    mediumPriorityFuture.get();
    highPriorityFuture.get();

    // Verify high priority was served first, even though it queued last.
    ASSERT_EQ(orderByAdmissions[highPriority.index], 0)
        << "High priority (1 admission) should be first";
    ASSERT_EQ(orderByAdmissions[mediumPriority.index], 1)
        << "Medium priority (50 admissions) should be second";
    ASSERT_EQ(orderByAdmissions[lowPriority.index], 2)
        << "Low priority (100 admissions) should be last";
}

TEST_F(OrderedTicketSemaphoreTest, PriorityOrderingStillWorksAfterResize) {
    auto sem = makeSemaphore(0, 5);

    std::atomic<int> acquireOrder{0};
    std::vector<int> orderByPriority(3);
    std::vector<MockAdmissionContext> finalAdmCtxs(3);
    std::vector<Future<void>> finalFutures;

    finalAdmCtxs[0].setAdmission_forTest(10);  // Low priority.
    finalAdmCtxs[1].setAdmission_forTest(1);   // High priority.
    finalAdmCtxs[2].setAdmission_forTest(5);   // Medium priority.

    for (int i = 0; i < 3; ++i) {
        finalFutures.push_back(launchAsync([&, admCtx = &(finalAdmCtxs[i]), i]() {
            auto [client, opCtx] = makeOpCtx();
            ASSERT_TRUE(sem->acquire(opCtx.get(), admCtx, Date_t::max(), true));
            orderByPriority[i] = acquireOrder.fetch_add(1);
        }));
    }

    waitForQueuedThreads(sem, 3);

    // Release one at a time to verify priority ordering.
    for (int i = 0; i < 3; ++i) {
        sem->resize(1);
        sleepFor(Milliseconds{10});
    }

    for (auto& future : finalFutures) {
        future.get();
    }

    ASSERT_EQ(orderByPriority[1], 0) << "Admission 1 (high priority) should acquire first";
    ASSERT_EQ(orderByPriority[2], 1) << "Admission 5 (medium priority) should acquire second";
    ASSERT_EQ(orderByPriority[0], 2) << "Admission 10 (low priority) should acquire last";
}

TEST_F(OrderedTicketSemaphoreTest, PermitsAreNotLeaked) {
    auto sem = makeSemaphore(0, 2);
    MockAdmissionContext admCtx1, admCtx2;

    admCtx1.setAdmission_forTest(1);
    admCtx2.setAdmission_forTest(2);

    Future<void> future1, future2;
    auto [client1, opCtx1] = makeOpCtx();
    auto [client2, opCtx2] = makeOpCtx();
    future1 = launchAsync([&]() {
        ASSERT_THROWS_CODE(sem->acquire(opCtx1.get(), &admCtx1, Date_t::max(), true),
                           DBException,
                           ErrorCodes::Interrupted);
    });
    future2 = launchAsync(
        [&]() { ASSERT_TRUE(sem->acquire(opCtx2.get(), &admCtx2, Date_t::max(), false)); });

    waitForQueuedThreads(sem, 2);

    // Simulate the leak scenario, kill an opCtx and then give one permit.
    opCtx1->markKilled(ErrorCodes::Interrupted);
    sem->release();

    future1.get();
    future2.get();
}

/**
 * Tests that tryAcquire returns false when there are waiters in the queue, even if available > 0
 * (fair semantics).
 */
TEST_F(OrderedTicketSemaphoreTest, TryAcquireReturnsFalseWhenWaitersQueued) {
    auto sem = makeSemaphore(1, 1);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);

    auto future = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        MockAdmissionContext admCtx;
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx, Date_t::max(), true));
    });

    waitForQueuedThreads(sem, 1);

    sem->release();

    ASSERT_FALSE(sem->tryAcquire());

    future.get();
    ASSERT_EQ(sem->available(), 0);
}

/**
 * Test that operations with lowAdmissions > 0 can bypass the max waiters limit.
 */
TEST_F(OrderedTicketSemaphoreTest, LowAdmissionsCanBypassMaxWaitersLimit) {
    auto sem = makeSemaphore(0, 1);  // 0 tickets, max 1 waiter.

    MockAdmissionContext admCtx1, admCtx2WithLowAdmissions, admCtx3NoLowAdmissions;

    auto future1 = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx1, Date_t::max(), true));
    });

    waitForQueuedThreads(sem, 1);
    ASSERT_EQ(sem->waiters(), 1);

    ASSERT_THROWS_CODE(sem->acquire(opCtx(), &admCtx3NoLowAdmissions, getDeadline(), true),
                       DBException,
                       ErrorCodes::AdmissionQueueOverflow);
    ASSERT_EQ(sem->waiters(), 1);  // Queue unchanged.

    // An operation with lowAdmissions > 0 should be able to bypass the limit.
    admCtx2WithLowAdmissions.recordLowAdmission();
    ASSERT_GT(admCtx2WithLowAdmissions.getLowAdmissions(), 0);

    auto future2 = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx2WithLowAdmissions, Date_t::max(), true));
    });

    waitForQueuedThreads(sem, 2);
    ASSERT_EQ(sem->waiters(), 2);

    sem->resize(2);
    future1.get();
    future2.get();
}

/**
 * Test that operations marked as load-shed-exempt bypass the max waiters limit.
 */
TEST_F(OrderedTicketSemaphoreTest, LoadShedExemptOperationsBypassesMaxWaitersLimit) {
    auto sem = makeSemaphore(0, 1);  // 0 tickets, max 1 waiter.

    // Create a custom AdmissionContext that overrides isLoadShedExempt().
    class ExemptAdmissionContext : public MockAdmissionContext {
    public:
        bool isLoadShedExempt() const override {
            return true;
        }
    };

    MockAdmissionContext admCtx1, admCtx2NoExemption;
    ExemptAdmissionContext admCtx3Exempt;

    // First operation fills the queue.
    auto future1 = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx1, Date_t::max(), true));
    });

    waitForQueuedThreads(sem, 1);
    ASSERT_EQ(sem->waiters(), 1);

    // Non-exempt operation should be load-shed.
    ASSERT_THROWS_CODE(sem->acquire(opCtx(), &admCtx2NoExemption, getDeadline(), true),
                       DBException,
                       ErrorCodes::AdmissionQueueOverflow);
    ASSERT_EQ(sem->waiters(), 1);

    // Exempt operation should bypass the limit.
    auto future3 = launchAsync([&]() {
        auto [client, opCtx] = makeOpCtx();
        ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtx3Exempt, Date_t::max(), true));
    });

    waitForQueuedThreads(sem, 2);
    ASSERT_EQ(sem->waiters(), 2);

    sem->resize(2);
    future1.get();
    future3.get();
}

/**
 * Verifies that when the highest-priority blocked waiter is killed after being woken, the
 * permit passes to the next-highest priority waiter rather than to the lowest-priority one.  With
 * only two alternatives this property cannot be distinguished from a simple "permit is not lost"
 * check, so this test uses three waiters.
 */
TEST_F(OrderedTicketSemaphoreTest, PassTheBatonRoutesPermitToNextPriorityWaiter) {
    auto sem = makeSemaphore(0, 10);

    auto kHighPriorityAdmissions = 1;
    auto kMidPriorityAdmissions = 5;
    auto kLowPriorityAdmissions = 10;
    MockAdmissionContext highAdmCtx, midAdmCtx, lowAdmCtx;
    highAdmCtx.setAdmission_forTest(kHighPriorityAdmissions);  // highest priority (lowest count)
    midAdmCtx.setAdmission_forTest(kMidPriorityAdmissions);
    lowAdmCtx.setAdmission_forTest(kLowPriorityAdmissions);  // lowest priority

    auto [clientHigh, opCtxHigh] = makeOpCtx();
    auto [clientMid, opCtxMid] = makeOpCtx();
    auto [clientLow, opCtxLow] = makeOpCtx();

    // Track which admission count was served first and second.
    std::atomic<int> acquireOrder{0};
    std::vector<int32_t> acquiredByAdmission(2, -1);

    auto highFuture = launchAsync([&]() {
        ASSERT_THROWS_CODE(sem->acquire(opCtxHigh.get(), &highAdmCtx, Date_t::max(), true),
                           DBException,
                           ErrorCodes::Interrupted);
    });
    auto midFuture = launchAsync([&]() {
        ASSERT_TRUE(sem->acquire(opCtxMid.get(), &midAdmCtx, Date_t::max(), false));
        acquiredByAdmission[acquireOrder.fetch_add(1)] = kMidPriorityAdmissions;
        sem->release();
    });
    auto lowFuture = launchAsync([&]() {
        ASSERT_TRUE(sem->acquire(opCtxLow.get(), &lowAdmCtx, Date_t::max(), false));
        acquiredByAdmission[acquireOrder.fetch_add(1)] = kLowPriorityAdmissions;
        sem->release();
    });

    waitForQueuedThreads(sem, 3);

    // Kill the highest-priority waiter, then release one permit.
    // Whether the kill races ahead of or behind the wakeup, the ON_BLOCK_EXIT guard
    // ensures the permit reaches admissions=5 rather than admissions=10.
    opCtxHigh->markKilled(ErrorCodes::Interrupted);
    sem->release();

    highFuture.get();
    midFuture.get();

    ASSERT_EQ(acquiredByAdmission[0], kMidPriorityAdmissions)
        << "admissions=5 should receive the permit before admissions=10";

    // midFuture already released its permit, which wakes lowFuture.
    lowFuture.get();
    ASSERT_EQ(acquiredByAdmission[1], kLowPriorityAdmissions);
    ASSERT_EQ(sem->waiters(), 0);
}

/**
 * Verifies that N waiters sharing the same admission count are all eventually served with no
 * permits leaked. All-equal priorities stress the _waitQueue's handling of duplicate keys and the
 * numToWake = std::min(waitQueue.size(), available) path.
 */
TEST_F(OrderedTicketSemaphoreTest, EqualAdmissionCountsAllServed) {
    constexpr int n = 5;
    auto sem = makeSemaphore(0, n + 1);

    std::vector<MockAdmissionContext> admCtxs(n);
    for (auto& ctx : admCtxs)
        ctx.setAdmission_forTest(42);

    std::atomic<int> servedCount{0};
    std::vector<Future<void>> futures;
    futures.reserve(n);

    for (int i = 0; i < n; ++i) {
        futures.push_back(launchAsync([&, i]() {
            auto [client, opCtx] = makeOpCtx();
            ASSERT_TRUE(sem->acquire(opCtx.get(), &admCtxs[i], Date_t::max(), false));
            servedCount.fetch_add(1);
            // Do not release — permits are verified via available() below.
        }));
    }

    waitForQueuedThreads(sem, n);

    // Add exactly n permits: all n waiters should unblock and each consume one.
    sem->resize(n);

    for (auto& f : futures)
        f.get();

    ASSERT_EQ(servedCount.load(), n);
    ASSERT_EQ(sem->available(), 0);  // All n permits consumed.
    ASSERT_EQ(sem->waiters(), 0);

    // Release the n permits still held so the semaphore destructs cleanly.
    for (int i = 0; i < n; ++i)
        sem->release();
}
}  // namespace mongo

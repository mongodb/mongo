// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/ticketing/ticket_semaphore.h"

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/ticketing/ordered_ticket_semaphore.h"
#include "mongo/db/admission/ticketing/unordered_ticket_semaphore.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/packaged_task.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using SemaphoreFactory =
    std::function<std::unique_ptr<TicketSemaphore>(int, Milliseconds basePollInterval)>;

// Generous timeout so tests fail with a useful diagnostic instead of hanging.
constexpr auto kWaitTimeout = Minutes{1};
constexpr auto kDefaultPollInterval = Milliseconds{500};

Date_t getDeadline() {
    return Date_t::now() + kWaitTimeout;
}

// Mirrors UnorderedTicketSemaphore::acquire() poll-deadline selection.
Date_t pollDeadline(Date_t until, Date_t now, Milliseconds pollInterval) {
    return std::min(until, now + pollInterval);
}

/**
 * Parameterized test fixture for TicketSemaphore implementations.
 */
class TicketSemaphoreTest : public ServiceContextTest,
                            public testing::WithParamInterface<SemaphoreFactory> {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _client = getServiceContext()->getService()->makeClient("test");
        _opCtx = _client->makeOperationContext();

        _pool = ThreadPool::make({});
        _pool->startup();
    }

    void tearDown() override {
        _pool->shutdown();
        ServiceContextTest::tearDown();
    }

    std::unique_ptr<TicketSemaphore> makeSemaphore(
        int numPermits, Milliseconds basePollInterval = kDefaultPollInterval) {
        return GetParam()(numPermits, basePollInterval);
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    /**
     * Polls the semaphore's waiters() count until at least 'expectedWaiters' threads are
     * queued. This is deterministic: once waiters() reaches the target, any subsequent
     * release()/resize() is guaranteed to notify or leave permits for those threads.
     */
    void waitUntilBlocked(TicketSemaphore* sem, int expectedWaiters) {
        _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
            while (sem->waiters() < expectedWaiters) {
                std::this_thread::yield();
            }
        });
    }

    /**
     * Spawns a thread with its own client/opCtx that blocks in acquire() on 'sem' using
     * 'admCtx' until 'deadline'. 'admCtx' must outlive the returned future.
     */
    Future<bool> spawnWaiter(TicketSemaphore* sem,
                             std::string name,
                             AdmissionContext* admCtx,
                             Date_t deadline = getDeadline()) {
        return spawn(
            [sem, name = std::move(name), admCtx, deadline, svcCtx = getServiceContext()]() {
                auto client = svcCtx->getService()->makeClient(name);
                auto waiterOpCtx = client->makeOperationContext();
                return sem->acquire(waiterOpCtx.get(), admCtx, deadline, /*interruptible=*/true);
            });
    }

    /**
     * Same as above for waiters whose admission context the test never inspects.
     */
    Future<bool> spawnWaiter(TicketSemaphore* sem,
                             std::string name,
                             Date_t deadline = getDeadline()) {
        return spawn([sem, name = std::move(name), deadline, svcCtx = getServiceContext()]() {
            auto client = svcCtx->getService()->makeClient(name);
            auto waiterOpCtx = client->makeOperationContext();
            MockAdmissionContext admCtx;
            return sem->acquire(waiterOpCtx.get(), &admCtx, deadline, /*interruptible=*/true);
        });
    }

    /**
     * Spawns 'n' waiters named "waiter0".."waiter{n-1}" and blocks until all are queued.
     */
    std::vector<Future<bool>> spawnWaiters(TicketSemaphore* sem, int n) {
        std::vector<Future<bool>> futures;
        for (int i = 0; i < n; ++i) {
            futures.push_back(spawnWaiter(sem, "waiter" + std::to_string(i)));
        }
        waitUntilBlocked(sem, n);
        return futures;
    }

    /**
     * Asserts that acquiring 'sem' from the test opCtx with 'admCtx' is load shed with
     * AdmissionQueueOverflow and that the waiter count is unchanged.
     */
    void assertAcquireIsShed(TicketSemaphore* sem, AdmissionContext* admCtx) {
        auto waitersBefore = sem->waiters();
        ASSERT_THROWS_CODE(sem->acquire(opCtx(), admCtx, getDeadline(), /*interruptible=*/true),
                           DBException,
                           ErrorCodes::AdmissionQueueOverflow);
        ASSERT_TRUE(admCtx->getLoadShed());
        ASSERT_EQ(sem->waiters(), waitersBefore);
    }

    /**
     * Waits (bounded by getDeadline()) for a spawned waiter to finish and asserts it acquired.
     */
    void assertWaiterAcquired(Future<bool>&& waiter) {
        ASSERT_TRUE(waitForWaiter(std::move(waiter)));
    }

    void assertAllWaitersAcquired(std::vector<Future<bool>>& waiters) {
        for (auto& f : waiters) {
            assertWaiterAcquired(std::move(f));
        }
    }

    /**
     * Waits (bounded by getDeadline()) for a spawned waiter to finish and returns its result.
     */
    bool waitForWaiter(Future<bool>&& waiter) {
        bool result = false;
        _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
            result = std::move(waiter).get(_opCtx.get());
        });
        return result;
    }

    template <std::invocable Callable>
    auto spawn(Callable&& cb) -> Future<std::invoke_result_t<Callable>> {
        using ReturnType = std::invoke_result_t<Callable>;
        auto task = PackagedTask(std::forward<Callable>(cb));
        auto taskFuture = task.getFuture();
        _pool->schedule([runTask = std::move(task)](Status s) mutable {
            invariant(s);
            runTask();
        });
        return taskFuture;
    }

protected:
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;

private:
    std::unique_ptr<ThreadPool> _pool;
};

TEST_P(TicketSemaphoreTest, TryAcquireSucceedsWhenAvailable) {
    auto sem = makeSemaphore(3);
    ASSERT_EQ(sem->available(), 3);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 2);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 1);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);
}

TEST_P(TicketSemaphoreTest, TryAcquireFailsWhenNoneAvailable) {
    auto sem = makeSemaphore(1);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);

    ASSERT_FALSE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);
}

TEST_P(TicketSemaphoreTest, TryAcquireFailsWithZeroPermits) {
    auto sem = makeSemaphore(0);
    ASSERT_EQ(sem->available(), 0);
    ASSERT_FALSE(sem->tryAcquire());
}

TEST_P(TicketSemaphoreTest, ReleaseIncrementsAvailable) {
    auto sem = makeSemaphore(2);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);

    sem->release();
    ASSERT_EQ(sem->available(), 1);

    sem->release();
    ASSERT_EQ(sem->available(), 2);

    sem->release();
    ASSERT_EQ(sem->available(), 3);
}

TEST_P(TicketSemaphoreTest, Resize) {
    auto sem = makeSemaphore(5);
    ASSERT_EQ(sem->available(), 5);

    sem->resize(3);
    ASSERT_EQ(sem->available(), 8);

    sem->resize(-6);
    ASSERT_EQ(sem->available(), 2);

    sem->resize(-2);
    ASSERT_EQ(sem->available(), 0);
    ASSERT_FALSE(sem->tryAcquire());
}

TEST_P(TicketSemaphoreTest, ResizeNegativeBelowZero) {
    auto sem = makeSemaphore(2);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);

    sem->resize(-3);
    ASSERT_EQ(sem->available(), -3);

    ASSERT_FALSE(sem->tryAcquire());

    sem->release();
    ASSERT_EQ(sem->available(), -2);
    ASSERT_FALSE(sem->tryAcquire());

    sem->release();
    sem->release();
    sem->release();
    ASSERT_EQ(sem->available(), 1);
    ASSERT_TRUE(sem->tryAcquire());
}

TEST_P(TicketSemaphoreTest, AcquireSucceedsWhenAvailable) {
    auto sem = makeSemaphore(3);
    MockAdmissionContext admCtx;

    ASSERT_TRUE(sem->acquire(opCtx(), &admCtx, Date_t::max(), /*interruptible=*/true));
    ASSERT_EQ(sem->available(), 2);
}

TEST_P(TicketSemaphoreTest, AcquireTimesOut) {
    auto sem = makeSemaphore(0);
    MockAdmissionContext admCtx;

    auto deadline = Date_t::now() + Milliseconds{50};
    ASSERT_FALSE(sem->acquire(opCtx(), &admCtx, deadline, /*interruptible=*/true));

    MockAdmissionContext admCtx2;
    auto deadline2 = Date_t::now() + Milliseconds{50};
    ASSERT_FALSE(sem->acquire(opCtx(), &admCtx2, deadline2, /*interruptible=*/false));
}

TEST_P(TicketSemaphoreTest, AcquireOpCtxDeadlineThrows) {
    auto sem = makeSemaphore(0);

    auto client = getServiceContext()->getService()->makeClient("deadlineTest");
    auto deadlineOpCtx = client->makeOperationContext();
    deadlineOpCtx->setDeadlineAfterNowBy(Milliseconds{50}, ErrorCodes::MaxTimeMSExpired);

    MockAdmissionContext admCtx;
    ASSERT_THROWS_CODE(
        sem->acquire(deadlineOpCtx.get(), &admCtx, Date_t::max(), /*interruptible=*/true),
        DBException,
        ErrorCodes::MaxTimeMSExpired);
}

TEST_P(TicketSemaphoreTest, AcquireExplicitDeadlineReturnsFalse) {
    auto sem = makeSemaphore(0);

    auto client = getServiceContext()->getService()->makeClient("deadlineTest");
    auto deadlineOpCtx = client->makeOperationContext();
    deadlineOpCtx->setDeadlineAfterNowBy(Minutes{5}, ErrorCodes::MaxTimeMSExpired);

    MockAdmissionContext admCtx;
    auto shortDeadline = Date_t::now() + Milliseconds{50};
    ASSERT_FALSE(sem->acquire(deadlineOpCtx.get(), &admCtx, shortDeadline, /*interruptible=*/true));
}

TEST_P(TicketSemaphoreTest, AcquireBlocksAndSucceedsWhenReleased) {
    auto sem = makeSemaphore(1);

    // Exhaust the single permit.
    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);

    // Spawn a thread that waits to acquire.
    auto waiter = spawnWaiter(sem.get(), "waiter");
    waitUntilBlocked(sem.get(), 1);

    // Release the permit -- the waiter should wake up and succeed.
    sem->release();
    assertWaiterAcquired(std::move(waiter));
}

TEST_P(TicketSemaphoreTest, AcquireBeforeShortDeadlineSucceedsWhenReleased) {
    constexpr auto kBasePollInterval = Seconds{10};
    auto sem = makeSemaphore(1, kBasePollInterval);

    // Exhaust the single permit.
    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);

    // The minimum jittered poll interval is 8 seconds. Use a deadline 5 seconds away so this
    // is the final wait, with a multi-second margin for slow test hosts. Waking because a
    // permit becomes available must not be mistaken for that deadline expiring.
    auto waiter = spawnWaiter(sem.get(), "shortDeadlineWaiter", Date_t::now() + Seconds{5});
    waitUntilBlocked(sem.get(), 1);
    sem->release();
    assertWaiterAcquired(std::move(waiter));
}

TEST_P(TicketSemaphoreTest, AcquireRespectsInterrupt) {
    auto sem = makeSemaphore(0);

    auto client = getServiceContext()->getService()->makeClient("interruptee");
    auto interruptOpCtx = client->makeOperationContext();

    auto* rawOpCtx = interruptOpCtx.get();

    Future<void> acquireFuture = spawn([rawSem = sem.get(), rawOpCtx]() {
        MockAdmissionContext admCtx;
        rawSem->acquire(rawOpCtx, &admCtx, Date_t::max(), /*interruptible=*/true);
    });

    waitUntilBlocked(sem.get(), 1);

    // Kill the operation -- the waiter should throw.
    interruptOpCtx->markKilled(ErrorCodes::Interrupted);

    ASSERT_THROWS_CODE(([&] {
                           _opCtx->runWithDeadline(
                               getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
                                   std::move(acquireFuture).get(_opCtx.get());
                               });
                       })(),
                       DBException,
                       ErrorCodes::Interrupted);
}

TEST_P(TicketSemaphoreTest, MultipleWaitersAllAcquire) {
    auto sem = makeSemaphore(0);

    constexpr int numWaiters = 5;
    auto futures = spawnWaiters(sem.get(), numWaiters);

    for (int i = 0; i < numWaiters; ++i) {
        sem->release();
    }

    assertAllWaitersAcquired(futures);
    ASSERT_EQ(sem->available(), 0);
}

TEST_P(TicketSemaphoreTest, ResizeWakesWaiters) {
    auto sem = makeSemaphore(0);

    // Spawn a waiter that blocks because there are 0 permits.
    auto waiter = spawnWaiter(sem.get(), "waiter");
    waitUntilBlocked(sem.get(), 1);

    // Resizing from 0 to 1 permit should wake the waiter.
    sem->resize(1);

    assertWaiterAcquired(std::move(waiter));
}

TEST_P(TicketSemaphoreTest, ResizeNegativeWhileWaitersAreWaiting) {
    // Start with 2 permits, acquire both so available = 0, then spawn waiters.
    auto sem = makeSemaphore(2);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);

    constexpr int numWaiters = 2;
    auto futures = spawnWaiters(sem.get(), numWaiters);

    // Resize permits into negative territory while waiters are blocked.
    sem->resize(-3);
    ASSERT_EQ(sem->available(), -3);

    // Releasing the 2 held permits only brings available to -1; waiters stay blocked.
    sem->release();
    sem->release();
    ASSERT_FALSE(futures[0].isReady());
    ASSERT_FALSE(futures[1].isReady());

    // Additional releases bring available positive so waiters can acquire.
    sem->release();
    sem->release();
    sem->release();

    assertAllWaitersAcquired(futures);
}

TEST_P(TicketSemaphoreTest, MaxWaitersZeroRejectsWaiters) {
    auto sem = makeSemaphore(2);
    sem->setMaxWaiters(0);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 1);

    // Acquire succeeds without queuing when a permit is available.
    MockAdmissionContext admCtx;
    ASSERT_TRUE(sem->acquire(opCtx(), &admCtx, getDeadline(), /*interruptible=*/true));
    ASSERT_EQ(sem->available(), 0);

    // But once no permits are available, it must queue — which is rejected at maxWaiters 0.
    MockAdmissionContext admCtx2;
    assertAcquireIsShed(sem.get(), &admCtx2);
}

TEST_P(TicketSemaphoreTest, MaxWaitersAllowsUpToLimit) {
    auto sem = makeSemaphore(0);
    sem->setMaxWaiters(2);

    auto futures = spawnWaiters(sem.get(), 2);

    // A 3rd waiter exceeds the limit.
    MockAdmissionContext admCtx;
    assertAcquireIsShed(sem.get(), &admCtx);

    sem->release();
    sem->release();

    assertAllWaitersAcquired(futures);
}

TEST_P(TicketSemaphoreTest, LoadShedExemptBypassesMaxWaitersLimit) {
    class ExemptAdmissionContext : public MockAdmissionContext {
    public:
        bool isLoadShedExempt() const override {
            return true;
        }
    };

    auto sem = makeSemaphore(0);
    sem->setMaxWaiters(1);

    // Fill the single waiter slot.
    MockAdmissionContext waiterCtx;
    auto firstWaiter = spawnWaiter(sem.get(), "waiter", &waiterCtx);
    waitUntilBlocked(sem.get(), 1);

    // A non-exempt operation should be load shed.
    MockAdmissionContext shedCtx;
    assertAcquireIsShed(sem.get(), &shedCtx);

    // An exempt operation should queue past the limit.
    ExemptAdmissionContext exemptCtx;
    auto exemptWaiter = spawnWaiter(sem.get(), "exemptWaiter", &exemptCtx);
    waitUntilBlocked(sem.get(), 2);

    sem->resize(2);
    assertWaiterAcquired(std::move(firstWaiter));
    assertWaiterAcquired(std::move(exemptWaiter));
}

TEST_P(TicketSemaphoreTest, LowAdmissionsBypassesMaxWaitersLimit) {
    auto sem = makeSemaphore(0);
    sem->setMaxWaiters(1);

    // Fill the single waiter slot.
    MockAdmissionContext waiterCtx;
    auto firstWaiter = spawnWaiter(sem.get(), "waiter", &waiterCtx);
    waitUntilBlocked(sem.get(), 1);

    // An operation that has never been admitted at low priority should be load shed.
    MockAdmissionContext shedCtx;
    assertAcquireIsShed(sem.get(), &shedCtx);

    // An operation that was already admitted at low priority should queue past the limit.
    MockAdmissionContext lowAdmissionCtx;
    lowAdmissionCtx.recordLowAdmission();
    auto lowAdmissionWaiter = spawnWaiter(sem.get(), "lowAdmissionWaiter", &lowAdmissionCtx);
    waitUntilBlocked(sem.get(), 2);

    sem->resize(2);
    assertWaiterAcquired(std::move(firstWaiter));
    assertWaiterAcquired(std::move(lowAdmissionWaiter));
}

TEST_P(TicketSemaphoreTest, ShedOperationDoesNotOccupyWaiterSlot) {
    auto sem = makeSemaphore(0);
    sem->setMaxWaiters(1);

    MockAdmissionContext waiterCtx;
    auto waiter = spawnWaiter(sem.get(), "waiter", &waiterCtx);
    waitUntilBlocked(sem.get(), 1);

    // The shed operation must decrement the waiter count it incremented, leaving the slot free
    // for the next operation, which is in turn shed rather than admitted past the limit.
    MockAdmissionContext shedCtx;
    assertAcquireIsShed(sem.get(), &shedCtx);
    MockAdmissionContext nextCtx;
    assertAcquireIsShed(sem.get(), &nextCtx);

    sem->release();
    assertWaiterAcquired(std::move(waiter));
    ASSERT_EQ(sem->waiters(), 0);
}

TEST_P(TicketSemaphoreTest, SetMaxWaitersDoesNotAffectExistingWaiters) {
    auto sem = makeSemaphore(0);

    auto waiter = spawnWaiter(sem.get(), "waiter");
    waitUntilBlocked(sem.get(), 1);

    // Shrink maxWaiters to 0 while 1 waiter is already queued.
    sem->setMaxWaiters(0);

    // The existing waiter should still succeed when a permit becomes available.
    sem->release();

    assertWaiterAcquired(std::move(waiter));
}

TEST_P(TicketSemaphoreTest, TryAcquireBypassesMaxWaiters) {
    auto sem = makeSemaphore(2);
    sem->setMaxWaiters(0);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);
    ASSERT_FALSE(sem->tryAcquire());
}

TEST_P(TicketSemaphoreTest, WaitersCountReflectsBlockedThreads) {
    auto sem = makeSemaphore(0);
    ASSERT_EQ(sem->waiters(), 0);

    constexpr int numWaiters = 3;
    auto futures = spawnWaiters(sem.get(), numWaiters);

    for (int i = 0; i < numWaiters; ++i) {
        sem->release();
    }

    assertAllWaitersAcquired(futures);
    ASSERT_EQ(sem->waiters(), 0);
}

TEST_P(TicketSemaphoreTest, NonInterruptibleAcquireReturnsFalseOnKillAndTimeout) {
    auto sem = makeSemaphore(0);

    auto client = getServiceContext()->getService()->makeClient("survivor");
    auto survivorOpCtx = client->makeOperationContext();
    auto* rawOpCtx = survivorOpCtx.get();

    auto deadline = Date_t::now() + Milliseconds{500};
    Future<bool> acquireFuture = spawn([rawSem = sem.get(), rawOpCtx, deadline]() {
        MockAdmissionContext admCtx;
        return rawSem->acquire(rawOpCtx, &admCtx, deadline, /*interruptible=*/false);
    });

    waitUntilBlocked(sem.get(), 1);
    survivorOpCtx->markKilled(ErrorCodes::Interrupted);

    ASSERT_FALSE(waitForWaiter(std::move(acquireFuture)));
}

TEST_P(TicketSemaphoreTest, NonInterruptibleAcquireWaitsForPermitAfterKill) {
    auto sem = makeSemaphore(0);

    auto client = getServiceContext()->getService()->makeClient("survivor");
    auto survivorOpCtx = client->makeOperationContext();
    auto* rawOpCtx = survivorOpCtx.get();

    Future<bool> acquireFuture = spawn([rawSem = sem.get(), rawOpCtx]() {
        MockAdmissionContext admCtx;
        return rawSem->acquire(rawOpCtx, &admCtx, Date_t::max(), /*interruptible=*/false);
    });

    waitUntilBlocked(sem.get(), 1);
    survivorOpCtx->markKilled(ErrorCodes::Interrupted);

    sleepFor(Milliseconds{500});
    ASSERT_FALSE(acquireFuture.isReady());

    sem->resize(1);

    assertWaiterAcquired(std::move(acquireFuture));
}

TEST_P(TicketSemaphoreTest, WaiterCountDecreasesAfterTimeout) {
    auto sem = makeSemaphore(0);
    sem->setMaxWaiters(1);

    MockAdmissionContext admCtx;
    auto shortDeadline = Date_t::now() + Milliseconds{50};
    ASSERT_FALSE(sem->acquire(opCtx(), &admCtx, shortDeadline, /*interruptible=*/false));
    ASSERT_EQ(sem->waiters(), 0);

    // The waiter slot was reclaimed, so a new waiter can queue without overflow.
    auto waiter = spawnWaiter(sem.get(), "waiter");
    waitUntilBlocked(sem.get(), 1);
    sem->release();

    assertWaiterAcquired(std::move(waiter));
}

TEST_P(TicketSemaphoreTest, ConcurrentAcquireDoesNotOverbookOrLeak) {
    constexpr int numPermits = 3;
    constexpr int numThreads = 10;
    constexpr int opsPerThread = 50;

    auto sem = makeSemaphore(numPermits);

    // Track maximum concurrent permit holders to verify no overbooking.
    Atomic<int> concurrentHolders{0};
    Atomic<int> maxConcurrentHolders{0};

    std::vector<stdx::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&, i, rawSem = sem.get()]() {
            auto client =
                getServiceContext()->getService()->makeClient("worker" + std::to_string(i));
            auto workerOpCtx = client->makeOperationContext();
            MockAdmissionContext admCtx;
            PseudoRandom rng(SecureRandom().nextInt64());

            for (int op = 0; op < opsPerThread; ++op) {
                ASSERT_TRUE(rawSem->acquire(workerOpCtx.get(), &admCtx, getDeadline(), true));

                auto current = concurrentHolders.fetchAndAdd(1) + 1;

                // Update peak.
                auto peak = maxConcurrentHolders.load();
                while (current > peak && !maxConcurrentHolders.compareAndSwap(&peak, current)) {
                }

                // Simulate short work.
                sleepFor(Milliseconds{rng.nextInt32(4)});
                concurrentHolders.fetchAndSubtract(1);
                rawSem->release();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // No overbooking: peak concurrent holders must not exceed the number of permits.
    ASSERT_LTE(maxConcurrentHolders.load(), numPermits);

    // No leak: all permits are returned.
    ASSERT_EQ(sem->available(), numPermits);
    ASSERT_EQ(concurrentHolders.load(), 0);
}

/**
 * Verifies that resize(+N) with M > N waiters unblocks exactly N waiters and leaves the remaining
 * M-N blocked.
 */
TEST_P(TicketSemaphoreTest, ResizeWakesExactlyNOfMWaiters) {
    auto sem = makeSemaphore(0);

    constexpr int numWaiters = 5;
    auto futures = spawnWaiters(sem.get(), numWaiters);

    sem->resize(3);

    _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        while (sem->waiters() > 2)
            std::this_thread::yield();
    });
    ASSERT_EQ(sem->waiters(), 2);
    ASSERT_EQ(sem->available(), 0);

    sem->resize(2);

    assertAllWaitersAcquired(futures);
    ASSERT_EQ(sem->available(), 0);
}

/**
 * Verifies that resize(+N) with N > number of waiters wakes all waiters and leaves N - waiters
 * permits available afterwards. Detects if the wakeup path grant more permits than waiters,
 * preventing permits from returning to the pool.
 */
TEST_P(TicketSemaphoreTest, ResizeExcessPermitsRemainAvailable) {
    auto sem = makeSemaphore(0);

    constexpr int numWaiters = 2;
    auto futures = spawnWaiters(sem.get(), numWaiters);

    // Add 5 permits: both waiters unblock and consume 2 permits, leaving 3 available.
    sem->resize(5);

    assertAllWaitersAcquired(futures);
    ASSERT_EQ(sem->available(), 3);
    ASSERT_EQ(sem->waiters(), 0);
}

/**
 * Verifies that a positive resize that does not push available above zero does not inadvertently
 * wake blocked waiters.
 */
TEST_P(TicketSemaphoreTest, ResizePositiveButRemainsNegativeKeepsWaitersBlocked) {
    auto sem = makeSemaphore(0);

    constexpr int numWaiters = 2;
    auto futures = spawnWaiters(sem.get(), numWaiters);

    sem->resize(-5);
    ASSERT_EQ(sem->available(), -5);

    // A positive resize that stays negative must not wake any waiter.
    sem->resize(3);
    ASSERT_EQ(sem->available(), -2);
    ASSERT_EQ(sem->waiters(), numWaiters);
    ASSERT_FALSE(futures[0].isReady());
    ASSERT_FALSE(futures[1].isReady());

    // Cross zero — both waiters should finally unblock.
    sem->resize(4);

    assertAllWaitersAcquired(futures);
    ASSERT_EQ(sem->available(), 0);
}

TEST_P(TicketSemaphoreTest, AcquireHonorsDeadlineWhenPermitsRemainNegative) {
    constexpr auto kBasePollInterval = Milliseconds{50};
    constexpr auto kAcquireTimeout = Milliseconds{200};
    auto sem = makeSemaphore(0, kBasePollInterval);

    Future<bool> acquireFuture = spawn([&, rawSem = sem.get()]() {
        auto client = getServiceContext()->getService()->makeClient("negativePermitsWaiter");
        auto waiterOpCtx = client->makeOperationContext();
        waiterOpCtx->setDeadlineAfterNowBy(Milliseconds{500}, ErrorCodes::ExceededTimeLimit);
        MockAdmissionContext admCtx;
        return rawSem->acquire(waiterOpCtx.get(), &admCtx, Date_t::now() + kAcquireTimeout, true);
    });

    waitUntilBlocked(sem.get(), 1);

    // Immediate resizing can make the permit count negative while existing tickets are still held.
    // The waiter must continue sleeping and honor its explicit deadline rather than busy-looping.
    sem->resize(-1);

    ASSERT_FALSE(waitForWaiter(std::move(acquireFuture)));
}

TEST_P(TicketSemaphoreTest, AcquireWithNegativePermitsAndShortDeadlineReturnsFalse) {
    constexpr auto kBasePollInterval = Milliseconds{100};
    constexpr auto kAcquireTimeout = Milliseconds{50};
    auto sem = makeSemaphore(-1, kBasePollInterval);

    auto client = getServiceContext()->getService()->makeClient("shortDeadlineWaiter");
    auto waiterOpCtx = client->makeOperationContext();
    waiterOpCtx->setDeadlineAfterNowBy(Milliseconds{500}, ErrorCodes::ExceededTimeLimit);
    MockAdmissionContext admCtx;

    ASSERT_FALSE(sem->acquire(
        waiterOpCtx.get(), &admCtx, Date_t::now() + kAcquireTimeout, true /* interruptible */));
}

TEST(UnorderedTicketSemaphorePollDeadlineTest, BetweenPollWindows) {
    // A deadline between poll intervals should be selected on the second wait.
    constexpr auto kMinPoll = Milliseconds{400};
    constexpr auto kMaxPoll = Milliseconds{600};
    constexpr auto kBasePoll = Milliseconds{500};

    const auto now = Date_t::fromMillisSinceEpoch(1'000'000);
    const auto until = now + Milliseconds{650};

    for (auto firstPoll : {kMinPoll, kBasePoll, kMaxPoll}) {
        const auto firstDeadline = pollDeadline(until, now, firstPoll);
        ASSERT_EQ(firstDeadline, now + firstPoll);
        ASSERT_LT(firstDeadline, until);

        for (auto secondPoll : {kMinPoll, kBasePoll, kMaxPoll}) {
            ASSERT_EQ(pollDeadline(until, firstDeadline, secondPoll), until);
        }
    }
}

TEST(UnorderedTicketSemaphorePollDeadlineTest, ShortDeadline) {
    // A deadline shorter than the poll interval should be selected on the first wait.
    const auto now = Date_t::fromMillisSinceEpoch(1'000'000);
    const auto until = now + Milliseconds{50};

    for (auto poll : {Milliseconds{400}, Milliseconds{500}, Milliseconds{600}}) {
        ASSERT_EQ(pollDeadline(until, now, poll), until);
    }
}

INSTANTIATE_TEST_SUITE_P(UnorderedTicketSemaphore,
                         TicketSemaphoreTest,
                         testing::Values([](int numPermits, Milliseconds basePollInterval)
                                             -> std::unique_ptr<TicketSemaphore> {
                             return std::make_unique<UnorderedTicketSemaphore>(
                                 numPermits, static_cast<int>(DEFAULT_MAX_CONN), basePollInterval);
                         }));

INSTANTIATE_TEST_SUITE_P(OrderedTicketSemaphore,
                         TicketSemaphoreTest,
                         testing::Values([](int numPermits,
                                            Milliseconds) -> std::unique_ptr<TicketSemaphore> {
                             return std::make_unique<OrderedTicketSemaphore>(
                                 numPermits, static_cast<int>(DEFAULT_MAX_CONN));
                         }));

}  // namespace mongo

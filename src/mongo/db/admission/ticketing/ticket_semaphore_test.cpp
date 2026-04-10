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

#include <functional>
#include <memory>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using SemaphoreFactory = std::function<std::unique_ptr<TicketSemaphore>(int)>;

// Generous timeout so tests fail with a useful diagnostic instead of hanging.
constexpr auto kWaitTimeout = Minutes{1};

Date_t getDeadline() {
    return Date_t::now() + kWaitTimeout;
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

        ThreadPool::Options opts;
        _pool = std::make_unique<ThreadPool>(opts);
        _pool->startup();
    }

    void tearDown() override {
        _pool->shutdown();
        ServiceContextTest::tearDown();
    }

    std::unique_ptr<TicketSemaphore> makeSemaphore(int numPermits) {
        return GetParam()(numPermits);
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
                stdx::this_thread::yield();
            }
        });
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

    ASSERT_TRUE(sem->acquire(opCtx(), &admCtx, Date_t::max(), true /* interruptible */));
    ASSERT_EQ(sem->available(), 2);
}

TEST_P(TicketSemaphoreTest, AcquireTimesOut) {
    auto sem = makeSemaphore(0);
    MockAdmissionContext admCtx;

    auto deadline = Date_t::now() + Milliseconds{50};
    ASSERT_FALSE(sem->acquire(opCtx(), &admCtx, deadline, true /* interruptible */));

    MockAdmissionContext admCtx2;
    auto deadline2 = Date_t::now() + Milliseconds{50};
    ASSERT_FALSE(sem->acquire(opCtx(), &admCtx2, deadline2, false /* interruptible */));
}

TEST_P(TicketSemaphoreTest, AcquireOpCtxDeadlineThrows) {
    auto sem = makeSemaphore(0);

    auto client = getServiceContext()->getService()->makeClient("deadlineTest");
    auto deadlineOpCtx = client->makeOperationContext();
    deadlineOpCtx->setDeadlineAfterNowBy(Milliseconds{50}, ErrorCodes::MaxTimeMSExpired);

    MockAdmissionContext admCtx;
    ASSERT_THROWS_CODE(
        sem->acquire(deadlineOpCtx.get(), &admCtx, Date_t::max(), true /* interruptible */),
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
    ASSERT_FALSE(
        sem->acquire(deadlineOpCtx.get(), &admCtx, shortDeadline, true /* interruptible */));
}

TEST_P(TicketSemaphoreTest, AcquireBlocksAndSucceedsWhenReleased) {
    auto sem = makeSemaphore(1);

    // Exhaust the single permit.
    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);

    // Spawn a thread that waits to acquire.
    auto* rawSem = sem.get();
    Future<bool> acquireFuture = spawn([&, rawSem]() {
        auto client = getServiceContext()->getService()->makeClient("waiter");
        auto waiterOpCtx = client->makeOperationContext();
        MockAdmissionContext waiterAdmCtx;
        return rawSem->acquire(waiterOpCtx.get(), &waiterAdmCtx, getDeadline(), true);
    });

    waitUntilBlocked(rawSem, 1);

    // Release the permit -- the waiter should wake up and succeed.
    rawSem->release();

    bool result = false;
    _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        result = std::move(acquireFuture).get(_opCtx.get());
    });

    ASSERT_TRUE(result);
}

TEST_P(TicketSemaphoreTest, AcquireRespectsInterrupt) {
    auto sem = makeSemaphore(0);

    auto client = getServiceContext()->getService()->makeClient("interruptee");
    auto interruptOpCtx = client->makeOperationContext();

    auto* rawSem = sem.get();
    auto* rawOpCtx = interruptOpCtx.get();

    Future<void> acquireFuture = spawn([&, rawSem, rawOpCtx]() {
        MockAdmissionContext admCtx;
        rawSem->acquire(rawOpCtx, &admCtx, Date_t::max(), true /* interruptible */);
    });

    waitUntilBlocked(rawSem, 1);

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
    auto* rawSem = sem.get();

    constexpr int numWaiters = 5;
    std::vector<Future<bool>> futures;

    for (int i = 0; i < numWaiters; ++i) {
        futures.push_back(spawn([&, i, rawSem, svcCtx = getServiceContext()]() {
            auto client = svcCtx->getService()->makeClient("waiter" + std::to_string(i));
            auto waiterOpCtx = client->makeOperationContext();
            MockAdmissionContext admCtx;
            return rawSem->acquire(waiterOpCtx.get(), &admCtx, getDeadline(), true);
        }));
    }

    waitUntilBlocked(rawSem, numWaiters);

    for (int i = 0; i < numWaiters; ++i) {
        rawSem->release();
    }

    for (auto& f : futures) {
        bool result = false;
        _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
            result = std::move(f).get(_opCtx.get());
        });
        ASSERT_TRUE(result);
    }
    ASSERT_EQ(rawSem->available(), 0);
}

TEST_P(TicketSemaphoreTest, ResizeWakesWaiters) {
    auto sem = makeSemaphore(0);
    auto* rawSem = sem.get();

    // Spawn a waiter that blocks because there are 0 permits.
    Future<bool> waiter = spawn([&, rawSem, svcCtx = getServiceContext()]() {
        auto client = svcCtx->getService()->makeClient("waiter");
        auto waiterOpCtx = client->makeOperationContext();
        MockAdmissionContext admCtx;
        return rawSem->acquire(waiterOpCtx.get(), &admCtx, getDeadline(), true);
    });

    waitUntilBlocked(rawSem, 1);

    // Resizing from 0 to 1 permit should wake the waiter.
    rawSem->resize(1);

    bool result = false;
    _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        result = std::move(waiter).get(_opCtx.get());
    });

    ASSERT_TRUE(result);
}

TEST_P(TicketSemaphoreTest, ResizeNegativeWhileWaitersAreWaiting) {
    // Start with 2 permits, acquire both so available = 0, then spawn waiters.
    auto sem = makeSemaphore(2);
    auto* rawSem = sem.get();

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 0);

    constexpr int numWaiters = 2;
    std::vector<Future<bool>> futures;

    for (int i = 0; i < numWaiters; ++i) {
        futures.push_back(spawn([&, i, rawSem, svcCtx = getServiceContext()]() {
            auto client = svcCtx->getService()->makeClient("waiter" + std::to_string(i));
            auto waiterOpCtx = client->makeOperationContext();
            MockAdmissionContext admCtx;
            return rawSem->acquire(waiterOpCtx.get(), &admCtx, getDeadline(), true);
        }));
    }

    waitUntilBlocked(rawSem, numWaiters);

    // Resize permits into negative territory while waiters are blocked.
    rawSem->resize(-3);
    ASSERT_EQ(rawSem->available(), -3);

    // Releasing the 2 held permits only brings available to -1; waiters stay blocked.
    rawSem->release();
    rawSem->release();
    ASSERT_FALSE(futures[0].isReady());
    ASSERT_FALSE(futures[1].isReady());

    // Additional releases bring available positive so waiters can acquire.
    rawSem->release();
    rawSem->release();
    rawSem->release();

    for (auto& f : futures) {
        bool result = false;
        _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
            result = std::move(f).get(_opCtx.get());
        });
        ASSERT_TRUE(result);
    }
}

TEST_P(TicketSemaphoreTest, MaxWaitersZeroRejectsWaiters) {
    auto sem = makeSemaphore(2);
    sem->setMaxWaiters(0);

    ASSERT_TRUE(sem->tryAcquire());
    ASSERT_EQ(sem->available(), 1);

    // Acquire succeeds without queuing when a permit is available.
    MockAdmissionContext admCtx;
    ASSERT_TRUE(sem->acquire(opCtx(), &admCtx, getDeadline(), true /* interruptible */));
    ASSERT_EQ(sem->available(), 0);

    // But once no permits are available, it must queue — which is rejected at maxWaiters 0.
    MockAdmissionContext admCtx2;
    ASSERT_THROWS_CODE(sem->acquire(opCtx(), &admCtx2, getDeadline(), true /* interruptible */),
                       DBException,
                       ErrorCodes::AdmissionQueueOverflow);
}

TEST_P(TicketSemaphoreTest, MaxWaitersAllowsUpToLimit) {
    auto sem = makeSemaphore(0);
    sem->setMaxWaiters(2);
    auto* rawSem = sem.get();

    std::vector<Future<bool>> futures;

    for (int i = 0; i < 2; ++i) {
        futures.push_back(spawn([&, i, rawSem, svcCtx = getServiceContext()]() {
            auto client = svcCtx->getService()->makeClient("waiter" + std::to_string(i));
            auto waiterOpCtx = client->makeOperationContext();
            MockAdmissionContext admCtx;
            return rawSem->acquire(waiterOpCtx.get(), &admCtx, getDeadline(), true);
        }));
    }

    waitUntilBlocked(rawSem, 2);

    // A 3rd waiter exceeds the limit.
    MockAdmissionContext admCtx;
    ASSERT_THROWS_CODE(sem->acquire(opCtx(), &admCtx, getDeadline(), true /* interruptible */),
                       DBException,
                       ErrorCodes::AdmissionQueueOverflow);

    rawSem->release();
    rawSem->release();

    for (auto& f : futures) {
        bool result = false;
        _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
            result = std::move(f).get(_opCtx.get());
        });
        ASSERT_TRUE(result);
    }
}

TEST_P(TicketSemaphoreTest, SetMaxWaitersDoesNotAffectExistingWaiters) {
    auto sem = makeSemaphore(0);
    auto* rawSem = sem.get();

    Future<bool> waiter = spawn([&, rawSem, svcCtx = getServiceContext()]() {
        auto client = svcCtx->getService()->makeClient("waiter");
        auto waiterOpCtx = client->makeOperationContext();
        MockAdmissionContext admCtx;
        return rawSem->acquire(waiterOpCtx.get(), &admCtx, getDeadline(), true);
    });

    waitUntilBlocked(rawSem, 1);

    // Shrink maxWaiters to 0 while 1 waiter is already queued.
    rawSem->setMaxWaiters(0);

    // The existing waiter should still succeed when a permit becomes available.
    rawSem->release();

    bool result = false;
    _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        result = std::move(waiter).get(_opCtx.get());
    });
    ASSERT_TRUE(result);
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
    auto* rawSem = sem.get();
    ASSERT_EQ(rawSem->waiters(), 0);

    constexpr int numWaiters = 3;
    std::vector<Future<bool>> futures;

    for (int i = 0; i < numWaiters; ++i) {
        futures.push_back(spawn([&, i, rawSem, svcCtx = getServiceContext()]() {
            auto client = svcCtx->getService()->makeClient("waiter" + std::to_string(i));
            auto waiterOpCtx = client->makeOperationContext();
            MockAdmissionContext admCtx;
            return rawSem->acquire(waiterOpCtx.get(), &admCtx, getDeadline(), true);
        }));
    }

    waitUntilBlocked(rawSem, numWaiters);

    for (int i = 0; i < numWaiters; ++i) {
        rawSem->release();
    }

    for (auto& f : futures) {
        _opCtx->runWithDeadline(
            getDeadline(), ErrorCodes::ExceededTimeLimit, [&] { std::move(f).get(_opCtx.get()); });
    }
    ASSERT_EQ(rawSem->waiters(), 0);
}

TEST_P(TicketSemaphoreTest, NonInterruptibleAcquireReturnsFalseOnKillAndTimeout) {
    auto sem = makeSemaphore(0);
    auto* rawSem = sem.get();

    auto client = getServiceContext()->getService()->makeClient("survivor");
    auto survivorOpCtx = client->makeOperationContext();
    auto* rawOpCtx = survivorOpCtx.get();

    auto deadline = Date_t::now() + Milliseconds{500};
    Future<bool> acquireFuture = spawn([&, rawSem, rawOpCtx, deadline]() {
        MockAdmissionContext admCtx;
        return rawSem->acquire(rawOpCtx, &admCtx, deadline, false /* interruptible */);
    });

    waitUntilBlocked(rawSem, 1);
    survivorOpCtx->markKilled(ErrorCodes::Interrupted);

    bool result = true;
    _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        result = std::move(acquireFuture).get(_opCtx.get());
    });
    ASSERT_FALSE(result);
}

TEST_P(TicketSemaphoreTest, NonInterruptibleAcquireWaitsForPermitAfterKill) {
    auto sem = makeSemaphore(0);
    auto* rawSem = sem.get();

    auto client = getServiceContext()->getService()->makeClient("survivor");
    auto survivorOpCtx = client->makeOperationContext();
    auto* rawOpCtx = survivorOpCtx.get();

    Future<bool> acquireFuture = spawn([&, rawSem, rawOpCtx]() {
        MockAdmissionContext admCtx;
        return rawSem->acquire(rawOpCtx, &admCtx, Date_t::max(), false /* interruptible */);
    });

    waitUntilBlocked(rawSem, 1);
    survivorOpCtx->markKilled(ErrorCodes::Interrupted);

    sleepFor(Milliseconds{500});
    ASSERT_FALSE(acquireFuture.isReady());

    rawSem->resize(1);

    bool result = false;
    _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        result = std::move(acquireFuture).get(_opCtx.get());
    });
    ASSERT_TRUE(result);
}

TEST_P(TicketSemaphoreTest, WaiterCountDecreasesAfterTimeout) {
    auto sem = makeSemaphore(0);
    sem->setMaxWaiters(1);
    auto* rawSem = sem.get();

    MockAdmissionContext admCtx;
    auto shortDeadline = Date_t::now() + Milliseconds{50};
    ASSERT_FALSE(sem->acquire(opCtx(), &admCtx, shortDeadline, false /* interruptible */));
    ASSERT_EQ(rawSem->waiters(), 0);

    // The waiter slot was reclaimed, so a new waiter can queue without overflow.
    Future<bool> waiter = spawn([&, rawSem, svcCtx = getServiceContext()]() {
        auto client = svcCtx->getService()->makeClient("waiter");
        auto waiterOpCtx = client->makeOperationContext();
        MockAdmissionContext admCtx2;
        return rawSem->acquire(
            waiterOpCtx.get(), &admCtx2, getDeadline(), true /* interruptible */);
    });

    waitUntilBlocked(rawSem, 1);
    rawSem->release();

    bool result = false;
    _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        result = std::move(waiter).get(_opCtx.get());
    });
    ASSERT_TRUE(result);
}

TEST_P(TicketSemaphoreTest, ConcurrentAcquireDoesNotOverbookOrLeak) {
    constexpr int numPermits = 3;
    constexpr int numThreads = 10;
    constexpr int opsPerThread = 50;

    auto sem = makeSemaphore(numPermits);
    auto* rawSem = sem.get();

    // Track maximum concurrent permit holders to verify no overbooking.
    AtomicWord<int> concurrentHolders{0};
    AtomicWord<int> maxConcurrentHolders{0};

    std::vector<stdx::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&, i]() {
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
    ASSERT_EQ(rawSem->available(), numPermits);
    ASSERT_EQ(concurrentHolders.load(), 0);
}

/**
 * Verifies that resize(+N) with M > N waiters unblocks exactly N waiters and leaves the remaining
 * M-N blocked.
 */
TEST_P(TicketSemaphoreTest, ResizeWakesExactlyNOfMWaiters) {
    auto sem = makeSemaphore(0);
    auto* rawSem = sem.get();

    constexpr int numWaiters = 5;
    std::vector<Future<bool>> futures;
    for (int i = 0; i < numWaiters; ++i) {
        futures.push_back(spawn([&, i, rawSem, svcCtx = getServiceContext()]() {
            auto client = svcCtx->getService()->makeClient("waiter" + std::to_string(i));
            auto waiterOpCtx = client->makeOperationContext();
            MockAdmissionContext admCtx;
            return rawSem->acquire(waiterOpCtx.get(), &admCtx, getDeadline(), true);
        }));
    }

    waitUntilBlocked(rawSem, numWaiters);

    rawSem->resize(3);

    _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
        while (rawSem->waiters() > 2)
            stdx::this_thread::yield();
    });
    ASSERT_EQ(rawSem->waiters(), 2);
    ASSERT_EQ(rawSem->available(), 0);

    rawSem->resize(2);

    for (auto& f : futures) {
        bool result = false;
        _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
            result = std::move(f).get(_opCtx.get());
        });
        ASSERT_TRUE(result);
    }
    ASSERT_EQ(rawSem->available(), 0);
}

/**
 * Verifies that resize(+N) with N > number of waiters wakes all waiters and leaves N - waiters
 * permits available afterwards. Detects if the wakeup path grant more permits than waiters,
 * preventing permits from returning to the pool.
 */
TEST_P(TicketSemaphoreTest, ResizeExcessPermitsRemainAvailable) {
    auto sem = makeSemaphore(0);
    auto* rawSem = sem.get();

    constexpr int numWaiters = 2;
    std::vector<Future<bool>> futures;
    for (int i = 0; i < numWaiters; ++i) {
        futures.push_back(spawn([&, i, rawSem, svcCtx = getServiceContext()]() {
            auto client = svcCtx->getService()->makeClient("waiter" + std::to_string(i));
            auto waiterOpCtx = client->makeOperationContext();
            MockAdmissionContext admCtx;
            return rawSem->acquire(waiterOpCtx.get(), &admCtx, getDeadline(), true);
        }));
    }

    waitUntilBlocked(rawSem, numWaiters);

    // Add 5 permits: both waiters unblock and consume 2 permits, leaving 3 available.
    rawSem->resize(5);

    for (auto& f : futures) {
        bool result = false;
        _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
            result = std::move(f).get(_opCtx.get());
        });
        ASSERT_TRUE(result);
    }
    ASSERT_EQ(rawSem->available(), 3);
    ASSERT_EQ(rawSem->waiters(), 0);
}

/**
 * Verifies that a positive resize that does not push available above zero does not inadvertently
 * wake blocked waiters.
 */
TEST_P(TicketSemaphoreTest, ResizePositiveButRemainsNegativeKeepsWaitersBlocked) {
    auto sem = makeSemaphore(0);
    auto* rawSem = sem.get();

    constexpr int numWaiters = 2;
    std::vector<Future<bool>> futures;
    for (int i = 0; i < numWaiters; ++i) {
        futures.push_back(spawn([&, i, rawSem, svcCtx = getServiceContext()]() {
            auto client = svcCtx->getService()->makeClient("waiter" + std::to_string(i));
            auto waiterOpCtx = client->makeOperationContext();
            MockAdmissionContext admCtx;
            return rawSem->acquire(waiterOpCtx.get(), &admCtx, getDeadline(), true);
        }));
    }

    waitUntilBlocked(rawSem, numWaiters);

    rawSem->resize(-5);
    ASSERT_EQ(rawSem->available(), -5);

    // A positive resize that stays negative must not wake any waiter.
    rawSem->resize(3);
    ASSERT_EQ(rawSem->available(), -2);
    ASSERT_EQ(rawSem->waiters(), numWaiters);
    ASSERT_FALSE(futures[0].isReady());
    ASSERT_FALSE(futures[1].isReady());

    // Cross zero — both waiters should finally unblock.
    rawSem->resize(4);

    for (auto& f : futures) {
        bool result = false;
        _opCtx->runWithDeadline(getDeadline(), ErrorCodes::ExceededTimeLimit, [&] {
            result = std::move(f).get(_opCtx.get());
        });
        ASSERT_TRUE(result);
    }
    ASSERT_EQ(rawSem->available(), 0);
}

INSTANTIATE_TEST_SUITE_P(UnorderedTicketSemaphore,
                         TicketSemaphoreTest,
                         testing::Values([](int numPermits) -> std::unique_ptr<TicketSemaphore> {
                             return std::make_unique<UnorderedTicketSemaphore>(
                                 numPermits, static_cast<int>(DEFAULT_MAX_CONN));
                         }));

INSTANTIATE_TEST_SUITE_P(OrderedTicketSemaphore,
                         TicketSemaphoreTest,
                         testing::Values([](int numPermits) -> std::unique_ptr<TicketSemaphore> {
                             return std::make_unique<OrderedTicketSemaphore>(
                                 numPermits, static_cast<int>(DEFAULT_MAX_CONN));
                         }));

}  // namespace mongo

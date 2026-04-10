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

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/ticketing/ordered_ticket_semaphore.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace mongo {
namespace {

/**
 * Ordered ticket semaphore fuzz tests, checks two properties:
 * 1. Priority ordering
 *    Blocks N threads with arbitrary admission counts, releases permits one-by-one, and
 *    verifies wake-up order is non-decreasing by admission count.
 *
 * 2. tryAcquire fairness
 *    Exhausts all permits, blocks waiters, releases one permit at a time, and asserts
 *    tryAcquire() returns false while any waiters remain queued.
 *
 * acquire() is called with interruptible=false so no OperationContext (and therefore no
 * ServiceContext) is required, avoiding MONGO_INITIALIZER dependencies in the fuzzer process.
 */

class OrderedTicketSemaphoreFuzz {
public:
    /**
     * Blocks N threads with the given admission counts on a zero-permit semaphore, then
     * releases permits one-by-one and asserts wake-up order is non-decreasing by admission
     * count (lowest count = highest priority).
     */
    void FuzzPriorityOrdering(std::vector<int32_t> admissionCounts) {
        for (auto& c : admissionCounts)
            c = std::abs(c) % 1000;

        const int n = static_cast<int>(admissionCounts.size());

        OrderedTicketSemaphore sem(0, /*maxWaiters=*/n + 1);

        std::vector<int32_t> wokenAdmissions(n, -1);
        Atomic<int> wakeOrder{0};

        std::vector<MockAdmissionContext> admCtxs(n);
        for (int i = 0; i < n; ++i)
            admCtxs[i].setAdmission_forTest(admissionCounts[i]);

        std::vector<stdx::thread> threads;
        threads.reserve(n);
        for (int i = 0; i < n; ++i) {
            threads.emplace_back(
                [i, &sem, &admCtxs, &admissionCounts, &wokenAdmissions, &wakeOrder]() {
                    ASSERT_TRUE(sem.acquire(nullptr, &admCtxs[i], Date_t::max(), false));
                    wokenAdmissions[wakeOrder.fetchAndAdd(1)] = admissionCounts[i];
                });
        }

        while (sem.waiters() < n)
            stdx::this_thread::yield();

        for (int i = 0; i < n; ++i) {
            sem.resize(1);
            while (wakeOrder.load() < i + 1)
                stdx::this_thread::yield();
        }

        for (auto& t : threads)
            t.join();

        auto sorted = admissionCounts;
        std::sort(sorted.begin(), sorted.end());
        ASSERT_EQ(wokenAdmissions, sorted);
    }

    /**
     * Tests tryAcquire fairness under concurrent pressure.
     */
    void FuzzTryAcquireFairness(int numWaiters, int numTryAcquireClients) {
        OrderedTicketSemaphore sem(0, /*maxWaiters=*/numWaiters + 1);

        std::vector<MockAdmissionContext> admCtxs(numWaiters);
        Atomic<int> waitersAcquired{0};
        std::vector<stdx::thread> waiterThreads;
        waiterThreads.reserve(numWaiters);
        for (int i = 0; i < numWaiters; ++i) {
            waiterThreads.emplace_back([i, &sem, &admCtxs, &waitersAcquired]() {
                ASSERT_TRUE(sem.acquire(nullptr, &admCtxs[i], Date_t::max(), false));
                waitersAcquired.fetchAndAdd(1);
            });
        }

        while (sem.waiters() < numWaiters)
            stdx::this_thread::yield();

        Atomic<int> tryAcquireSuccesses{0};
        Atomic<bool> stopTrying{false};

        std::vector<stdx::thread> tryerThreads;
        tryerThreads.reserve(numTryAcquireClients);
        for (int i = 0; i < numTryAcquireClients; ++i) {
            tryerThreads.emplace_back([&sem, &tryAcquireSuccesses, &stopTrying]() {
                while (!stopTrying.load()) {
                    if (sem.tryAcquire()) {
                        tryAcquireSuccesses.fetchAndAdd(1);
                        sem.release();  // Return the permit so it can go to a waiter
                    }
                    stdx::this_thread::yield();
                }
            });
        }

        for (int i = 0; i < numWaiters; ++i) {
            sem.resize(1);
            while (waitersAcquired.load() < i + 1)
                stdx::this_thread::yield();
        }

        stopTrying.store(true);

        for (auto& t : waiterThreads)
            t.join();
        for (auto& t : tryerThreads)
            t.join();

        ASSERT_EQ(tryAcquireSuccesses.load(), 0);
    }
};

FUZZ_TEST_F(OrderedTicketSemaphoreFuzz, FuzzPriorityOrdering)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<int32_t>()).WithMinSize(1).WithMaxSize(16));

FUZZ_TEST_F(OrderedTicketSemaphoreFuzz, FuzzTryAcquireFairness)
    .WithDomains(fuzztest::InRange(1, 16),  // numWaiters
                 fuzztest::InRange(1, 8));  // numTryAcquireClients
}  // namespace
}  // namespace mongo

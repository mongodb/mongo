/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/platform/waitable_atomic.h"

#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using unittest::JoinThread;

// Any waits with a timeout or deadline that expect to be woken by another thread should wait at
// least this long. This doesn't apply if the timeout is expected to expire, or if it is never
// expected to wait at all.
constexpr auto kTimeoutForRacyWait = Seconds(1);

TEST(AtomicWaitableTests, WaitUntilValueChangedForAtomicInt) {
    BasicWaitableAtomic<int> sharedData(10);
    JoinThread modifierThread([&]() {
        sleepmillis(20);
        sharedData.store(42);
        sharedData.notifyOne();
    });

    sharedData.wait(10);
    ASSERT_EQUALS(sharedData.load(), 42);
}

TEST(AtomicWaitableTests, WaitWithDeadlineForAtomicInt) {
    BasicWaitableAtomic<int> sharedData(10);
    JoinThread modifierThread([&]() {
        sleepmillis(10);
        sharedData.store(42);
        sharedData.notifyOne();
    });

    ASSERT_EQUALS(sharedData.waitUntil(10, Date_t::now() + kTimeoutForRacyWait), 42);
    ASSERT_EQUALS(sharedData.waitUntil(42, Date_t::now()), boost::none);
    ASSERT_EQUALS(sharedData.waitUntil(10, Date_t::now() + Milliseconds(10)), 42);
}

TEST(AtomicWaitableTests, WaitWithTimeoutForAtomicInt) {
    BasicWaitableAtomic<int> sharedData(10);
    JoinThread modifierThread([&]() {
        sleepmillis(10);
        sharedData.store(42);
        sharedData.notifyOne();
    });

    ASSERT_EQUALS(sharedData.waitFor(10, kTimeoutForRacyWait), 42);
    ASSERT_EQUALS(sharedData.waitFor(42, Nanoseconds(1)), boost::none);
    ASSERT_EQUALS(sharedData.waitFor(10, Milliseconds(10)), 42);
}

TEST(AtomicWaitableTests, WaitWithDifferentValueForAtomicInt) {
    BasicWaitableAtomic<int> sharedData(10);
    ASSERT_EQUALS(sharedData.wait(42), 10);
    ASSERT_EQUALS(sharedData.waitUntil(42, Date_t::now() + Milliseconds(1)), 10);
    ASSERT_EQUALS(sharedData.waitFor(42, Milliseconds(1)), 10);
}

TEST(AtomicWaitableTests, WaitWithTimeInPastForInt) {
    BasicWaitableAtomic<int> val(0);
    ASSERT_EQUALS(val.waitUntil(0, Date_t::now() - Milliseconds(10)), boost::none);
    ASSERT_EQUALS(val.waitFor(0, -Milliseconds(10)), boost::none);

    // Now is sort of in the past. Testing specially to make sure edge case of 0 is handled well.
    ASSERT_EQUALS(val.waitUntil(0, Date_t::now()), boost::none);
    ASSERT_EQUALS(val.waitFor(0, Milliseconds(0)), boost::none);
}

TEST(AtomicWaitableTests, WaitUntilValueChangedForAtomicBool) {
    WaitableAtomic<bool> flag(false);
    JoinThread t1([&]() {
        sleepmillis(20);
        flag.store(true);
        flag.notifyAll();
    });

    flag.wait(false);
    ASSERT_TRUE(flag.load());
}

TEST(AtomicWaitableTests, WaitWithDeadlineForAtomicBool) {
    WaitableAtomic<bool> flag(false);
    flag.store(false);
    JoinThread t1([&]() {
        sleepmillis(10);
        flag.store(true);
        flag.notifyAll();
    });

    ASSERT_TRUE(flag.waitUntil(false, Date_t::now() + kTimeoutForRacyWait));
}
TEST(AtomicWaitableTests, WaitWithTimeoutForAtomicBool) {
    WaitableAtomic<bool> flag(false);
    ASSERT_EQUALS(flag.waitFor(false, Milliseconds(10)), boost::none);
}

TEST(AtomicWaitableTests, WaitWithDifferentValueForAtomicBool) {
    WaitableAtomic<bool> flag(true);
    ASSERT(flag.wait(false));
    ASSERT(flag.waitUntil(false, Date_t::now() + Milliseconds(1)));
    ASSERT(flag.waitFor(false, Milliseconds(1)));
}

TEST(AtomicWaitableTests, WaitWithTimeInPastForBool) {
    WaitableAtomic<bool> flag(false);
    ASSERT_EQUALS(flag.waitUntil(false, Date_t::now() - Milliseconds(10)), boost::none);
    ASSERT_EQUALS(flag.waitFor(false, -Milliseconds(10)), boost::none);

    // Now is sort of in the past. Testing specially to make sure edge case of 0 is handled well.
    ASSERT_EQUALS(flag.waitUntil(0, Date_t::now()), boost::none);
    ASSERT_EQUALS(flag.waitFor(0, Milliseconds(0)), boost::none);
}

TEST(AtomicWaitableTests, WaitUntilChangedFor64Bit) {
    WaitableAtomic<uint64_t> uval(0);
    JoinThread t2([&]() {
        sleepmillis(10);
        uval.store(1);
        uval.notifyAll();
    });
    JoinThread t1([&]() { uval.wait(0); });
    t2.join();
    ASSERT_EQUALS(uval.load(), 1);
}

TEST(AtomicWaitableTests, FloatEdgeCases) {
    WaitableAtomic<float> atomic(0.0);
    ASSERT(!atomic.waitFor(-0.0, Nanoseconds(1)));  // 0.0 == -0.0

    atomic.wait(std::numeric_limits<float>::quiet_NaN());  // qNaN != 0.0 is false

    atomic.store(std::numeric_limits<float>::quiet_NaN());
    atomic.wait(std::numeric_limits<float>::quiet_NaN());      // qNaN == qNaN is false
    atomic.wait(std::numeric_limits<float>::signaling_NaN());  // qNaN == sNaN is false
}

TEST(AtomicWaitableTests, TestActiveWaiters) {
    auto atomic = WaitableAtomic<bool>(false);
    ASSERT(!hasWaiters_forTest(atomic));

    atomic.notifyAll();
    ASSERT(!hasWaiters_forTest(atomic));

    // Can't clear waiters flag on timeout because we don't know if we were the only waiter.
    atomic.waitFor(false, Nanoseconds(1));
    ASSERT(hasWaiters_forTest(atomic));

    atomic.notifyAll();
    ASSERT(!hasWaiters_forTest(atomic));
}

template <typename TypeToTest>
void tryToInduceRaces() {
    auto keepGoing = AtomicWord<bool>(true);
    auto atomic = TypeToTest(false);
    auto waitingForNot = WaitableAtomic<int>();

    auto t2 = JoinThread([&] {
        auto prng = PseudoRandom(0);
        volatile auto spinner = uint32_t(0);  // NOLINT: don't want compiler to optimize out
        for (auto next = true; keepGoing.load(); next = !next) {
            const auto mainThreadWaitingForNot = waitingForNot.load();
            atomic.store(next);
            atomic.notifyAll();

            // Now wait a random small amount to induce races.
            for (int spins = prng.nextInt32(100); spins--;) {
                spinner = spinner + prng.nextUInt32();
            }

            // 1% of the time, if the other thread is waiting for our notification,
            // don't do anything else until they have been notified. This is an attempt
            // to detect bugs which could cause missed notifications.
            if (mainThreadWaitingForNot == !next && prng.nextInt32(100) == 0) {
                LOGV2_DEBUG(8179701, 1, "Spinning thread waiting");
                while (!waitingForNot.waitFor(!next, Seconds(1))) {
                    LOGV2(8179700, "Spinning thread waited 1 second. Probable bug?");
                    if (!keepGoing.load())
                        return;
                }
            }
        }
    });

    ON_BLOCK_EXIT([&] {
        keepGoing.store(false);
        waitingForNot.store(2);  // 2 will never be passed to waitingForNot.wait()
        waitingForNot.notifyAll();
    });

    auto runs = 100'000;  // About 100ms in opt builds.
    auto last = false;
    while (runs--) {
        auto next = atomic.waitFor(last, Seconds(10));
        ASSERT(!!next) << "Main thread waiting more than 10 seconds is probably a bug";
        ASSERT_NE(!!last, !!*next);  // MSVC doesn't like bool != int, so boolify both.
        last = *next;
        waitingForNot.store(last);
        waitingForNot.notifyAll();
    }
}

TEST(AtomicWaitableTests, TryToInduceRaces_BasicWaitableAtomic) {
    tryToInduceRaces<BasicWaitableAtomic<int>>();
}
TEST(AtomicWaitableTests, TryToInduceRaces_WaitableAtomic) {
    tryToInduceRaces<WaitableAtomic<bool>>();
}

TEST(AtomicWaitableTests, NotifyOneOnlyWakesOneThread) {
    // We don't guarantee this in part because it is impossible to distinguish from spurious wakeup
    // and races. But it should generally be the case that notifyOne only wakes one thread. This
    // test is mostly to ensure that we are passing the right flags to the OS. It uses sleeps
    // because it isn't possible to reliably wait for threads to reach the blocking point or know
    // when all accidentally woken threads would have woken. Since it has some risk of failing,
    // retry a bit. It if consistently passes, even after retrying, then we are very likely passing
    // the right flags to the OS.
    int tries = 10;
    while (true) {
        try {
            constexpr int nThreads = 10;  // takes at least 100ms + 10ms per thread.
            auto keepWaiting = BasicWaitableAtomic<int>(true);
            auto numWoken = AtomicWord<int>(0);
            JoinThread threads[nThreads];
            ON_BLOCK_EXIT([&] {
                keepWaiting.store(false);  // should be false already, but to be safe.
                keepWaiting.notifyAll();
            });

            for (int i = 0; i < nThreads; i++) {
                threads[i] = JoinThread([&] {
                    keepWaiting.wait(true);
                    numWoken.fetchAndAdd(1);
                });
            }

            sleepmillis(100);  // Hopefully wait for all threads to be blocked in kernel.

            keepWaiting.store(false);

            // Not waking anyone yet. Lets check if anyone wasn't blocked in the kernel.
            sleepmillis(10);
            ASSERT_EQUALS(numWoken.load(), 0);

            // Now wake one thread at a time.
            for (int i = 0; i < nThreads; i++) {
                keepWaiting.notifyOne();

                // Wait up to 100ms for a thread to wake.
                for (int j = 0; numWoken.load() == i && j < 100; j++) {
                    sleepmillis(1);
                }
                ASSERT_EQUALS(numWoken.load(), i + 1);

                // Wait 10ms to see if any other threads wake.
                sleepmillis(10);
                ASSERT_EQUALS(numWoken.load(), i + 1);
            }

            return;  // success!
        } catch (const unittest::TestAssertionFailureException& ex) {
            if (--tries == 0)
                throw;  // give up
            LOGV2_ERROR(8179702,
                        "Test can fail spuriously, retrying",
                        "attemptsLeft"_attr = tries,
                        "error"_attr = ex);
        }
    }
}

}  // namespace
}  // namespace mongo

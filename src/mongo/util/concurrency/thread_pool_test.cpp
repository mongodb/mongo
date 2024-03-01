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


#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <mutex>
#include <ostream>
#include <utility>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/thread_pool_test_common.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {
using namespace mongo;
using namespace fmt::literals;

MONGO_INITIALIZER(ThreadPoolCommonTests)(InitializerContext*) {
    addTestsForThreadPool("ThreadPoolCommon",
                          []() { return std::make_unique<ThreadPool>(ThreadPool::Options()); });
}

class ThreadPoolTest : public unittest::Test {
protected:
    ThreadPool& makePool(ThreadPool::Options options) {
        ASSERT(!_pool);
        _pool.emplace(std::move(options));
        return *_pool;
    }

    ThreadPool& pool() {
        ASSERT(_pool);
        return *_pool;
    }

    void blockingWork() {
        stdx::unique_lock<Latch> lk(mutex);
        ++count1;
        cv1.notify_all();
        while (!flag2) {
            cv2.wait(lk);
        }
    }

    Mutex mutex = MONGO_MAKE_LATCH("ThreadPoolTest::mutex");
    stdx::condition_variable cv1;
    stdx::condition_variable cv2;
    size_t count1 = 0U;
    bool flag2 = false;

private:
    void tearDown() override {
        stdx::unique_lock<Latch> lk(mutex);
        flag2 = true;
        cv2.notify_all();
        lk.unlock();
    }

    boost::optional<ThreadPool> _pool;
};

TEST_F(ThreadPoolTest, MinPoolSize0) {
    ThreadPool::Options options;
    options.minThreads = 0;
    options.maxThreads = 1;
    options.maxIdleThreadAge = Milliseconds(100);
    auto& pool = makePool(options);
    pool.startup();
    ASSERT_EQ(0U, pool.getStats().numThreads);
    stdx::unique_lock<Latch> lk(mutex);
    pool.schedule([this](auto status) {
        ASSERT_OK(status);
        blockingWork();
    });
    while (count1 != 1U) {
        cv1.wait(lk);
    }
    auto stats = pool.getStats();
    ASSERT_EQUALS(1U, stats.numThreads);
    ASSERT_EQUALS(0U, stats.numPendingTasks);
    pool.schedule([](auto status) { ASSERT_OK(status); });
    stats = pool.getStats();
    ASSERT_EQUALS(1U, stats.numThreads);
    ASSERT_EQUALS(0U, stats.numIdleThreads);
    ASSERT_EQUALS(1U, stats.numPendingTasks);
    flag2 = true;
    cv2.notify_all();
    lk.unlock();
    Timer reapTimer;
    for (size_t i = 0; i < 100 && (stats = pool.getStats()).numThreads > options.minThreads; ++i) {
        sleepmillis(100);
    }
    const Microseconds reapTime(reapTimer.micros());
    ASSERT_EQ(options.minThreads, stats.numThreads)
        << "Failed to reap all threads after " << durationCount<Milliseconds>(reapTime) << "ms";
    lk.lock();
    flag2 = false;
    count1 = 0;
    pool.schedule([this](auto status) {
        ASSERT_OK(status);
        blockingWork();
    });
    while (count1 == 0) {
        cv1.wait(lk);
    }
    stats = pool.getStats();
    ASSERT_EQUALS(1U, stats.numThreads);
    ASSERT_EQUALS(0U, stats.numIdleThreads);
    ASSERT_EQUALS(0U, stats.numPendingTasks);
    flag2 = true;
    cv2.notify_all();
    lk.unlock();
}

TEST_F(ThreadPoolTest, MaxPoolSize20MinPoolSize15) {
    ThreadPool::Options options;
    options.minThreads = 15;
    options.maxThreads = 20;
    options.maxIdleThreadAge = Milliseconds(100);
    auto& pool = makePool(options);
    pool.startup();
    stdx::unique_lock<Latch> lk(mutex);
    for (size_t i = 0U; i < 30U; ++i) {
        pool.schedule([this, i](auto status) {
            ASSERT_OK(status) << i;
            blockingWork();
        });
    }
    while (count1 < 20U) {
        cv1.wait(lk);
    }
    ASSERT_EQ(20U, count1);
    auto stats = pool.getStats();
    ASSERT_EQ(20U, stats.numThreads);
    ASSERT_EQ(0U, stats.numIdleThreads);
    ASSERT_EQ(10U, stats.numPendingTasks);
    flag2 = true;
    cv2.notify_all();
    while (count1 < 30U) {
        cv1.wait(lk);
    }
    lk.unlock();
    stats = pool.getStats();
    ASSERT_EQ(0U, stats.numPendingTasks);
    Timer reapTimer;
    for (size_t i = 0; i < 100 && (stats = pool.getStats()).numThreads > options.minThreads; ++i) {
        sleepmillis(50);
    }
    const Microseconds reapTime(reapTimer.micros());
    ASSERT_EQ(options.minThreads, stats.numThreads)
        << "Failed to reap excess threads after " << durationCount<Milliseconds>(reapTime) << "ms";
}

DEATH_TEST_REGEX(ThreadPoolTest,
                 MaxThreadsTooFewDies,
                 "Cannot create pool.*with maximum number of threads.*less than 1") {
    ThreadPool::Options options;
    options.maxThreads = 0;
    ThreadPool pool(options);
}

DEATH_TEST_REGEX(
    ThreadPoolTest,
    MinThreadsTooManyDies,
    R"#(.*Cannot create pool.*with minimum number of threads.*larger than the configured maximum.*minThreads":6,"maxThreads":5)#") {
    ThreadPool::Options options;
    options.maxThreads = 5;
    options.minThreads = 6;
    ThreadPool pool(options);
}

TEST(ThreadPoolTest, LivePoolCleanedByDestructor) {
    ThreadPool pool((ThreadPool::Options()));
    pool.startup();
    while (pool.getStats().numThreads == 0) {
        sleepmillis(50);
    }
    // Destructor should reap leftover threads.
}

DEATH_TEST_REGEX(ThreadPoolTest,
                 DestructionDuringJoinDies,
                 "Attempted to join pool.*more than once.*DoubleJoinPool") {
    // This test is a little complicated. We need to ensure that the ThreadPool destructor runs
    // while some thread is blocked running ThreadPool::join, to see that double-join is fatal in
    // the pool destructor. To do this, we first wait for minThreads threads to have started. Then,
    // we create and lock a mutex in the test thread, schedule a work item in the pool to lock that
    // mutex, schedule an independent thread to call join, and wait for numIdleThreads to hit 0
    // inside the test thread. When that happens, we know that the thread in the pool executing our
    // mutex-lock is blocked waiting for the mutex, so the independent thread must be blocked inside
    // of join(), until the pool thread finishes. At this point, if we destroy the pool, its
    // destructor should trigger a fatal error due to double-join.
    auto mutex = MONGO_MAKE_LATCH();
    ThreadPool::Options options;
    options.minThreads = 1;
    options.poolName = "DoubleJoinPool";
    boost::optional<ThreadPool> pool;
    pool.emplace(options);
    pool->startup();
    while (pool->getStats().numThreads < 1U) {
        sleepmillis(50);
    }
    stdx::unique_lock<Latch> lk(mutex);
    pool->schedule([&mutex](auto status) {
        ASSERT_OK(status);
        stdx::lock_guard<Latch> lk(mutex);
    });
    stdx::thread t([&pool] {
        pool->shutdown();
        pool->join();
    });
    ThreadPool::Stats stats;
    while ((stats = pool->getStats()).numIdleThreads != 0U) {
        sleepmillis(50);
    }
    ASSERT_EQ(0U, stats.numPendingTasks);
    pool.reset();
    lk.unlock();
    t.join();
}

TEST_F(ThreadPoolTest, ThreadPoolRunsOnCreateThreadFunctionBeforeConsumingTasks) {
    unittest::Barrier barrier(2U);
    std::string journal;
    ThreadPool::Options options;
    options.threadNamePrefix = "mythread";
    options.maxThreads = 1U;
    options.onCreateThread = [&](const std::string& threadName) {
        journal.append("[onCreate({})]"_format(threadName));
    };

    ThreadPool pool(options);
    pool.startup();
    pool.schedule([&](auto status) {
        journal.append("[Call({})]"_format(status.toString()));
        barrier.countDownAndWait();
    });
    barrier.countDownAndWait();
    ASSERT_EQUALS(journal, "[onCreate(mythread0)][Call(OK)]");
}

TEST(ThreadPoolTest, JoinAllRetiredThreads) {
    AtomicWord<unsigned long> retiredThreads(0);
    ThreadPool::Options options;
    options.minThreads = 4;
    options.maxThreads = 8;
    options.maxIdleThreadAge = Milliseconds(100);
    options.onJoinRetiredThread = [&](const stdx::thread& t) {
        retiredThreads.addAndFetch(1);
    };
    unittest::Barrier barrier(options.maxThreads + 1);

    ThreadPool pool(options);
    for (auto i = options.maxThreads; i > 0; i--) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier.countDownAndWait();
        });
    }
    ASSERT_EQ(pool.getStats().numThreads, 0);
    pool.startup();
    barrier.countDownAndWait();

    while (pool.getStats().numThreads > options.minThreads) {
        sleepmillis(100);
    }

    pool.shutdown();
    pool.join();

    const auto expectedRetiredThreads = options.maxThreads - options.minThreads;
    ASSERT_EQ(retiredThreads.load(), expectedRetiredThreads);
    ASSERT_EQ(pool.getStats().numIdleThreads, 0);
}

TEST(ThreadPoolTest, SafeToCallWaitForIdleBeforeShutdown) {
    ThreadPool::Options options;
    options.minThreads = 1;
    options.maxThreads = 1;
    ThreadPool pool(options);
    unittest::Barrier barrier(2);
    pool.schedule([&](Status) {
        barrier.countDownAndWait();
        // We can't guarantee that ThreadPool::waitForIdle() is always called before
        // ThreadPool::shutdown(). Introducing the following sleep increases the chances of such an
        // ordering. However, this is a best-effort, and ThreadPool::shutdown() may still precede
        // ThreadPool::waitForIdle on slow machines.
        sleepmillis(10);
    });
    pool.schedule([&](Status) { pool.shutdown(); });
    pool.startup();
    barrier.countDownAndWait();
    pool.waitForIdle();
}

}  // namespace

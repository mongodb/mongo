/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/base/init.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/thread_pool_test_common.h"
#include "mongo/util/concurrency/thread_pool_test_fixture.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace {
using namespace mongo;

MONGO_INITIALIZER(ThreadPoolCommonTests)(InitializerContext*) {
    addTestsForThreadPool("ThreadPoolCommon",
                          []() { return stdx::make_unique<ThreadPool>(ThreadPool::Options()); });
    return Status::OK();
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
        stdx::unique_lock<stdx::mutex> lk(mutex);
        ++count1;
        cv1.notify_all();
        while (!flag2) {
            cv2.wait(lk);
        }
    }

    stdx::mutex mutex;
    stdx::condition_variable cv1;
    stdx::condition_variable cv2;
    size_t count1 = 0U;
    bool flag2 = false;

private:
    void tearDown() override {
        stdx::unique_lock<stdx::mutex> lk(mutex);
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
    stdx::unique_lock<stdx::mutex> lk(mutex);
    ASSERT_OK(pool.schedule([this] { blockingWork(); }));
    while (count1 != 1U) {
        cv1.wait(lk);
    }
    auto stats = pool.getStats();
    ASSERT_EQUALS(1U, stats.numThreads);
    ASSERT_EQUALS(0U, stats.numPendingTasks);
    ASSERT_OK(pool.schedule([] {}));
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
    ASSERT_OK(pool.schedule([this] { blockingWork(); }));
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
    stdx::unique_lock<stdx::mutex> lk(mutex);
    for (size_t i = 0U; i < 30U; ++i) {
        ASSERT_OK(pool.schedule([this] { blockingWork(); })) << i;
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

DEATH_TEST(ThreadPoolTest, MaxThreadsTooFewDies, "but the maximum must be at least 1") {
    ThreadPool::Options options;
    options.maxThreads = 0;
    ThreadPool pool(options);
}

DEATH_TEST(ThreadPoolTest,
           MinThreadsTooManyDies,
           "6 which is more than the configured maximum of 5") {
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

DEATH_TEST(ThreadPoolTest,
           DestructionDuringJoinDies,
           "Attempted to join pool DoubleJoinPool more than once") {
    // This test is a little complicated. We need to ensure that the ThreadPool destructor runs
    // while some thread is blocked running ThreadPool::join, to see that double-join is fatal in
    // the pool destructor. To do this, we first wait for minThreads threads to have started. Then,
    // we create and lock a mutex in the test thread, schedule a work item in the pool to lock that
    // mutex, schedule an independent thread to call join, and wait for numIdleThreads to hit 0
    // inside the test thread. When that happens, we know that the thread in the pool executing our
    // mutex-lock is blocked waiting for the mutex, so the independent thread must be blocked inside
    // of join(), until the pool thread finishes. At this point, if we destroy the pool, its
    // destructor should trigger a fatal error due to double-join.
    stdx::mutex mutex;
    ThreadPool::Options options;
    options.minThreads = 2;
    options.poolName = "DoubleJoinPool";
    boost::optional<ThreadPool> pool;
    pool.emplace(options);
    pool->startup();
    while (pool->getStats().numThreads < 2U) {
        sleepmillis(50);
    }
    stdx::unique_lock<stdx::mutex> lk(mutex);
    ASSERT_OK(pool->schedule([&mutex] { stdx::lock_guard<stdx::mutex> lk(mutex); }));
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
    bool onCreateThreadCalled = false;
    std::string taskThreadName;
    ThreadPool::Options options;
    options.threadNamePrefix = "mythread";
    options.maxThreads = 1U;
    options.onCreateThread = [&onCreateThreadCalled,
                              &taskThreadName](const std::string& threadName) {
        onCreateThreadCalled = true;
        taskThreadName = threadName;
    };

    auto& pool = makePool(options);
    pool.startup();
    unittest::Barrier barrier(2U);
    ASSERT_OK(pool.schedule([&barrier] { barrier.countDownAndWait(); }));
    barrier.countDownAndWait();

    ASSERT_TRUE(onCreateThreadCalled);
    ASSERT_EQUALS(options.threadNamePrefix + "0", taskThreadName);
}

}  // namespace

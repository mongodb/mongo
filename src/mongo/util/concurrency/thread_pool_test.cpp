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


#include "mongo/util/concurrency/thread_pool.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
class ThreadPoolTest : public unittest::Test {
protected:
    /**
     * Manages blocking work launched by the test.
     * Starts off in the blocked state.
     */
    class BlockingWorkControl {
    public:
        ~BlockingWorkControl() {
            unblock();
        }

        void block() {
            stdx::unique_lock lk(_mutex);
            _workBlocked = true;
            _cv.notify_all();
        }

        void unblock() {
            stdx::unique_lock lk(_mutex);
            _workBlocked = false;
            _cv.notify_all();
        }

        void onWorkStart() {
            stdx::unique_lock lk(_mutex);
            ++_workStarted;
            _cv.notify_all();
        }

        void pauseWhileBlocked() {
            stdx::unique_lock lk(_mutex);
            _cv.wait(lk, [&] { return !_workBlocked; });
        }

        int pauseUntilWorkStartedCountAtLeast(int count) {
            stdx::unique_lock lk(_mutex);
            _cv.wait(lk, [&] { return _workStarted >= count; });
            return _workStarted;
        }

    private:
        mutable stdx::mutex _mutex;
        stdx::condition_variable _cv;
        int _workStarted = 0;
        int _workBlocked = true;
    };

    ThreadPool& makePool(ThreadPool::Options options) {
        ASSERT(!_pool);
        _pool.emplace(std::move(options));
        return *_pool;
    }

    void destroyPool() {
        _pool.reset();
    }

    ThreadPool& getPool() {
        ASSERT(_pool);
        return *_pool;
    }

    void waitForReapTo(ThreadPool& pool, size_t upperBound) {
        Timer reapTimer;
        while (pool.getStats().numThreads > upperBound) {
            Microseconds elapsed{reapTimer.micros()};
            ASSERT_LT(elapsed, Seconds{5})
                << fmt::format("Failed to reap excess threads after {}", elapsed);
            sleepFor(Milliseconds{50});
        }
    }

    auto blockingWorkCallback(boost::optional<int> taskNumber = {}) {
        return [&, taskNumber](auto status) {
            ASSERT_OK(status) << (taskNumber ? std::to_string(*taskNumber) : std::string{});
            blockingWorkControl().onWorkStart();
            blockingWorkControl().pauseWhileBlocked();
        };
    }

    BlockingWorkControl& blockingWorkControl() {
        return _blockingWorkControl;
    }

private:
    boost::optional<ThreadPool> _pool;
    BlockingWorkControl _blockingWorkControl;
};
using ThreadPoolDeathTest = ThreadPoolTest;

TEST_F(ThreadPoolTest, MinPoolSize0) {
    ThreadPool::Options options;
    options.minThreads = 0;
    options.maxThreads = 1;
    options.maxIdleThreadAge = Milliseconds(100);
    auto& pool = makePool(options);
    pool.startup();
    ASSERT_EQ(pool.getStats().numThreads, 0);
    for (int i = 0; i < 3; ++i) {
        pool.schedule(blockingWorkCallback());
        // First task is in progress, the others are pending.
        blockingWorkControl().pauseUntilWorkStartedCountAtLeast(1);
        {
            auto stats = pool.getStats();
            ASSERT_EQ(stats.numThreads, 1);
            ASSERT_EQ(stats.numIdleThreads, 0);
            ASSERT_EQ(stats.numPendingTasks, i);
        }
    }

    blockingWorkControl().unblock();
    waitForReapTo(pool, 0);

    blockingWorkControl().block();
    // This one additional task will start on the one available thread.
    pool.schedule(blockingWorkCallback());
    blockingWorkControl().pauseUntilWorkStartedCountAtLeast(4);
    {
        auto stats = pool.getStats();
        ASSERT_EQ(stats.numThreads, 1);
        ASSERT_EQ(stats.numIdleThreads, 0);
        ASSERT_EQ(stats.numPendingTasks, 0);
    }

    blockingWorkControl().unblock();
    waitForReapTo(pool, 0);
    {
        auto stats = pool.getStats();
        ASSERT_EQ(stats.numThreads, 0);
        ASSERT_EQ(stats.numIdleThreads, 0);
        ASSERT_EQ(stats.numPendingTasks, 0);
    }
}

TEST_F(ThreadPoolTest, MaxPoolSize20MinPoolSize15) {
    static const size_t extra = 10;
    ThreadPool::Options options;
    options.minThreads = 15;
    options.maxThreads = 20;
    options.maxIdleThreadAge = Milliseconds(100);
    auto& pool = makePool(options);
    pool.startup();
    for (size_t i = 0; i < options.maxThreads + extra; ++i)
        pool.schedule(blockingWorkCallback(i));
    ASSERT_EQ(blockingWorkControl().pauseUntilWorkStartedCountAtLeast(options.maxThreads),
              options.maxThreads);
    sleepFor(Milliseconds(100));
    {
        auto stats = pool.getStats();
        ASSERT_EQ(stats.numThreads, options.maxThreads);
        ASSERT_EQ(stats.numIdleThreads, 0);
        ASSERT_EQ(stats.numPendingTasks, extra);
    }

    blockingWorkControl().unblock();
    blockingWorkControl().pauseUntilWorkStartedCountAtLeast(options.maxThreads + extra);
    ASSERT_EQ(pool.getStats().numPendingTasks, 0);
    waitForReapTo(pool, options.minThreads);
}

DEATH_TEST_REGEX_F(ThreadPoolDeathTest,
                   MaxThreadsTooFewDies,
                   "Cannot configure pool.*with maximum number of threads.*less than 1") {
    ThreadPool::Options options;
    options.maxThreads = 0;
    makePool(options);
}

DEATH_TEST_REGEX_F(
    ThreadPoolDeathTest,
    MinThreadsTooManyDies,
    R"re(.*Cannot configure pool.*with minimum number of threads.*larger than the maximum.*minThreads":6,"maxThreads":5)re") {
    ThreadPool::Options options;
    options.maxThreads = 5;
    options.minThreads = 6;
    makePool(options);
}

TEST_F(ThreadPoolTest, LivePoolCleanedByDestructor) {
    auto& pool = makePool({});
    pool.startup();
    while (pool.getStats().numThreads == 0) {
        sleepFor(Milliseconds{50});
    }
    // Destructor should reap leftover threads.
}

DEATH_TEST_REGEX_F(ThreadPoolDeathTest,
                   DestructionDuringJoinDies,
                   "Attempted to join pool.*more than once.*DoubleJoinPool") {
    // This test ensures that the ThreadPool destructor runs while some thread is blocked
    // running ThreadPool::join, to see that double-join is fatal in the pool destructor.
    stdx::mutex mutex;
    ThreadPool::Options options;
    options.minThreads = 1;
    options.maxThreads = 1;
    options.poolName = "DoubleJoinPool";
    auto& pool = makePool(options);
    pool.startup();
    ASSERT_EQ(pool.getStats().numThreads, 1);

    stdx::unique_lock lk(mutex);
    // Schedule 2 tasks to ensure that independent thread join() is blocked draining the tasks and
    // causing the ThreadPool destructor join to fail due to double-join.
    pool.schedule([&mutex](auto status) {
        ASSERT_OK(status);
        stdx::lock_guard lk(mutex);
    });
    pool.schedule([&mutex](auto status) {
        ASSERT_OK(status);
        stdx::lock_guard lk(mutex);
    });

    stdx::thread t{[&] {
        pool.shutdown();
        pool.join();
    }};
    ScopeGuard onExitGuard = [&] {
        lk.unlock();
        if (t.joinable()) {
            t.join();
        }
    };
    ThreadPool::Stats stats;
    while ((stats = pool.getStats()).numPendingTasks != 0) {
        sleepFor(Milliseconds{10});
    }
    // Accounts for cleanup and regular worker thread.
    ASSERT_EQ(stats.numThreads, 2);
    ASSERT_EQ(stats.numIdleThreads, 0);
    destroyPool();
    MONGO_UNREACHABLE;
}

TEST_F(ThreadPoolTest, ThreadPoolRunsOnCreateThreadFunctionBeforeConsumingTasks) {
    unittest::Barrier barrier(2);
    std::string journal;
    ThreadPool::Options options;
    options.threadNamePrefix = "mythread";
    options.maxThreads = 1;
    options.onCreateThread = [&](const std::string& threadName) {
        journal.append(fmt::format("[onCreate({})]", threadName));
    };

    ThreadPool pool(options);
    pool.startup();
    pool.schedule([&](auto status) {
        journal.append(fmt::format("[Call({})]", status.toString()));
        barrier.countDownAndWait();
    });
    barrier.countDownAndWait();
    ASSERT_EQ(journal, "[onCreate(mythread0)][Call(OK)]");
}

TEST_F(ThreadPoolTest, JoinAllRetiredThreads) {
    Atomic<int> retiredThreads(0);
    ThreadPool::Options options;
    options.minThreads = 4;
    options.maxThreads = 8;
    options.maxIdleThreadAge = Milliseconds(100);
    options.onJoinRetiredThread = [&](const stdx::thread& t) {
        retiredThreads.addAndFetch(1);
    };
    unittest::Barrier barrier(options.maxThreads + 1);

    auto& pool = makePool(options);
    for (auto i = size_t{0}; i < options.maxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier.countDownAndWait();
        });
    }
    ASSERT_EQ(pool.getStats().numThreads, 0);
    pool.startup();
    barrier.countDownAndWait();

    while (pool.getStats().numThreads > options.minThreads) {
        sleepFor(Microseconds{1});
    }

    pool.shutdown();
    pool.join();

    ASSERT_EQ(retiredThreads.load(), options.maxThreads - options.minThreads);
    ASSERT_EQ(pool.getStats().numIdleThreads, 0);
}

DEATH_TEST_REGEX_F(
    ThreadPoolDeathTest,
    ModifyMinThreadsGreaterThanMax,
    R"re(.*Cannot configure pool.*with minimum number of threads.*larger than the maximum.*minThreads":7,"maxThreads":5)re") {
    ThreadPool::Options options;
    options.maxThreads = 5;
    options.minThreads = 3;
    auto& pool = makePool(options);

    const size_t newMinThreads = 7;
    pool.setMinThreads(newMinThreads);
}

DEATH_TEST_REGEX_F(
    ThreadPoolDeathTest,
    ModifyMaxLessThanMin,
    R"re(.*Cannot configure pool.*with minimum number of threads.*larger than the maximum.*minThreads":3,"maxThreads":2)re") {
    ThreadPool::Options options;
    options.maxThreads = 5;
    options.minThreads = 3;
    auto& pool = makePool(options);

    const size_t newMaxThreads = 2;
    pool.setMaxThreads(newMaxThreads);
}

DEATH_TEST_REGEX_F(ThreadPoolDeathTest,
                   ModifyMaxToZero,
                   "Cannot configure pool.*with maximum number of threads.*less than 1") {
    ThreadPool::Options options;
    options.maxThreads = 5;
    options.minThreads = 0;
    auto& pool = makePool(options);

    const size_t newMaxThreads = 0;
    pool.setMaxThreads(newMaxThreads);
}

TEST_F(ThreadPoolTest, ModifyMinThreads) {
    Atomic<int> retiredThreads(0);
    ThreadPool::Options options;
    options.minThreads = 4;
    options.maxThreads = 8;
    options.maxIdleThreadAge = Milliseconds(100);
    options.onJoinRetiredThread = [&](const stdx::thread& t) {
        retiredThreads.addAndFetch(1);
    };
    unittest::Barrier barrier(options.maxThreads + 1);

    auto& pool = makePool(options);
    for (auto i = size_t{0}; i < options.maxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier.countDownAndWait();
        });
    }
    ASSERT_EQ(pool.getStats().numThreads, 0);
    pool.startup();
    barrier.countDownAndWait();

    while (pool.getStats().numThreads > options.minThreads) {
        sleepFor(Microseconds{1});
    }

    ASSERT_EQ(pool.getStats().numIdleThreads, options.minThreads);
    sleepFor(Milliseconds{100});
    ASSERT_EQ(retiredThreads.load(), options.maxThreads - options.minThreads);

    // Modify to lower value
    // reset # of retired threads
    retiredThreads.store(0);
    // barrier was reset when counter reached 0
    const size_t newMinThreads = 2;
    pool.setMinThreads(newMinThreads);
    for (auto i = size_t{0}; i < options.maxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier.countDownAndWait();
        });
    }
    barrier.countDownAndWait();

    while (pool.getStats().numThreads > newMinThreads) {
        sleepFor(Microseconds{1});
    }

    ASSERT_EQ(pool.getStats().numIdleThreads, newMinThreads);
    sleepFor(Milliseconds{100});
    ASSERT_EQ(retiredThreads.load(), options.maxThreads - newMinThreads);

    // modify to higher value
    // reset # of retired threads
    retiredThreads.store(0);
    // barrier was reset when counter reached 0
    const size_t higherMinThreads = 6;
    pool.setMinThreads(higherMinThreads);
    for (auto i = size_t{0}; i < options.maxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier.countDownAndWait();
        });
    }
    barrier.countDownAndWait();

    while (pool.getStats().numThreads > higherMinThreads) {
        sleepFor(Microseconds{1});
    }

    ASSERT_EQ(pool.getStats().numIdleThreads, higherMinThreads);
    sleepFor(Milliseconds{100});
    ASSERT_EQ(retiredThreads.load(), options.maxThreads - higherMinThreads);
    pool.shutdown();
    pool.join();
}

TEST_F(ThreadPoolTest, DecreaseMaxThreadsAndDoLessWork) {
    Atomic<int> retiredThreads(0);
    ThreadPool::Options options;
    const size_t originalMaxThreads = 8;
    options.minThreads = 4;
    options.maxThreads = originalMaxThreads;
    options.maxIdleThreadAge = Milliseconds(1000);
    options.onJoinRetiredThread = [&](const stdx::thread& t) {
        retiredThreads.addAndFetch(1);
    };

    auto& pool = makePool(options);
    ASSERT_EQ(pool.getStats().numThreads, 0);
    pool.startup();

    unittest::Barrier barrier(options.maxThreads + 1);
    for (auto i = size_t{0}; i < options.maxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier.countDownAndWait();
        });
    }
    barrier.countDownAndWait();

    // No threads should have retired yet.
    ASSERT_EQ(retiredThreads.load(), 0);

    // Modify maxThreads to a lower value;
    const size_t lowerMaxThreads = 4;
    pool.setMaxThreads(lowerMaxThreads);
    unittest::Barrier barrier2(2);
    // Schedule only one task
    pool.schedule([&](auto status) {
        ASSERT_OK(status);
        barrier2.countDownAndWait();
    });

    barrier2.countDownAndWait();
    // Cleaning up the retired threads happens after task execution, so wait briefly for this to
    // complete.
    sleepFor(Milliseconds{100});
    // Four threads should have retired due to lowering max from 8 to 4.
    ASSERT_EQ(retiredThreads.load(), originalMaxThreads - lowerMaxThreads);
    pool.shutdown();
    pool.join();
}

TEST_F(ThreadPoolTest, ModifyMaxThreads) {
    Atomic<int> retiredThreads(0);
    ThreadPool::Options options;
    options.minThreads = 4;
    options.maxThreads = 8;
    options.maxIdleThreadAge = Milliseconds(100);
    options.onJoinRetiredThread = [&](const stdx::thread& t) {
        retiredThreads.addAndFetch(1);
    };
    unittest::Barrier barrier(options.maxThreads + 1);

    auto& pool = makePool(options);
    for (auto i = size_t{0}; i < options.maxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier.countDownAndWait();
        });
    }
    ASSERT_EQ(pool.getStats().numThreads, 0);
    pool.startup();
    barrier.countDownAndWait();

    while (pool.getStats().numThreads > options.minThreads) {
        sleepFor(Microseconds{1});
    }

    ASSERT_EQ(pool.getStats().numIdleThreads, options.minThreads);
    sleepFor(Milliseconds{100});
    ASSERT_EQ(retiredThreads.load(), options.maxThreads - options.minThreads);

    // Modify to higher value
    // reset # of retired threads
    retiredThreads.store(0);
    const size_t newMaxThreads = 12;
    pool.setMaxThreads(newMaxThreads);
    // create new barrier to reflect new number of threads
    unittest::Barrier barrier2(newMaxThreads + 1);
    for (auto i = size_t{0}; i < newMaxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier2.countDownAndWait();
        });
    }
    barrier2.countDownAndWait();

    while (pool.getStats().numThreads > options.minThreads) {
        sleepFor(Microseconds{1});
    }

    ASSERT_EQ(pool.getStats().numIdleThreads, options.minThreads);
    sleepFor(Milliseconds{100});
    ASSERT_EQ(retiredThreads.load(), newMaxThreads - options.minThreads);

    // modify to lower value
    // reset # of retired threads
    retiredThreads.store(0);
    const size_t lowerMaxThreads = 6;
    pool.setMaxThreads(lowerMaxThreads);
    // create new barrier to reflect new number of threads
    unittest::Barrier barrier3(lowerMaxThreads + 1);
    for (auto i = size_t{0}; i < lowerMaxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier3.countDownAndWait();
        });
    }
    barrier3.countDownAndWait();

    while (pool.getStats().numThreads > options.minThreads) {
        sleepFor(Microseconds{1});
    }

    ASSERT_EQ(pool.getStats().numIdleThreads, options.minThreads);
    sleepFor(Milliseconds{100});
    ASSERT_EQ(retiredThreads.load(), lowerMaxThreads - options.minThreads);
    pool.shutdown();
    pool.join();
}

TEST_F(ThreadPoolTest, ModifyMaxAndMinThreads) {
    Atomic<int> retiredThreads(0);
    ThreadPool::Options options;
    options.minThreads = 4;
    options.maxThreads = 8;
    options.maxIdleThreadAge = Milliseconds(100);
    options.onJoinRetiredThread = [&](const stdx::thread& t) {
        retiredThreads.addAndFetch(1);
    };
    unittest::Barrier barrier(options.maxThreads + 1);

    auto& pool = makePool(options);
    for (auto i = size_t{0}; i < options.maxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier.countDownAndWait();
        });
    }
    ASSERT_EQ(pool.getStats().numThreads, 0);
    pool.startup();
    barrier.countDownAndWait();

    while (pool.getStats().numThreads > options.minThreads) {
        sleepFor(Microseconds{1});
    }

    ASSERT_EQ(pool.getStats().numIdleThreads, options.minThreads);
    sleepFor(Milliseconds{100});
    ASSERT_EQ(retiredThreads.load(), options.maxThreads - options.minThreads);

    // reset # of retired threads
    retiredThreads.store(0);
    const size_t newMaxThreads = 12;
    const size_t newMinThreads = 2;
    pool.setMaxThreads(newMaxThreads);
    pool.setMinThreads(newMinThreads);
    // create new barrier to reflect new number of threads
    unittest::Barrier barrier2(newMaxThreads + 1);
    for (auto i = size_t{0}; i < newMaxThreads; ++i) {
        pool.schedule([&](auto status) {
            ASSERT_OK(status);
            barrier2.countDownAndWait();
        });
    }
    barrier2.countDownAndWait();

    while (pool.getStats().numThreads > newMinThreads) {
        sleepFor(Microseconds{1});
    }

    ASSERT_EQ(pool.getStats().numIdleThreads, newMinThreads);
    sleepFor(Milliseconds{100});
    ASSERT_EQ(retiredThreads.load(), newMaxThreads - newMinThreads);
    pool.shutdown();
    pool.join();
}

TEST_F(ThreadPoolTest, SafeToCallWaitForIdleBeforeShutdown) {
    ThreadPool::Options options;
    options.minThreads = 1;
    options.maxThreads = 1;
    auto& pool = makePool(options);
    unittest::Barrier barrier(2);
    pool.schedule([&](Status) {
        barrier.countDownAndWait();
        // We can't guarantee that ThreadPool::waitForIdle() is always called before
        // ThreadPool::shutdown(). Introducing the following sleep increases the chances of such an
        // ordering. However, this is a best-effort, and ThreadPool::shutdown() may still precede
        // ThreadPool::waitForIdle on slow machines.
        sleepFor(Milliseconds{10});
    });
    pool.schedule([&](Status) { pool.shutdown(); });
    pool.startup();
    barrier.countDownAndWait();
    pool.waitForIdle();
}

TEST_F(ThreadPoolTest, UnusedPool) {
    makePool({});
}

TEST_F(ThreadPoolTest, CannotScheduleAfterShutdown) {
    auto& pool = makePool({});
    pool.shutdown();
    pool.schedule([](auto status) { ASSERT_EQ(status, ErrorCodes::ShutdownInProgress); });
}

TEST_F(ThreadPoolTest, PoolDestructorExecutesRemainingTasks) {
    auto& pool = makePool({});
    bool executed = false;
    pool.schedule([&](auto status) {
        ASSERT_OK(status);
        executed = true;
    });
    destroyPool();
    ASSERT_TRUE(executed);
}

TEST_F(ThreadPoolTest, PoolJoinExecutesRemainingTasks) {
    auto& pool = makePool({});
    bool executed = false;
    pool.schedule([&](auto status) {
        ASSERT_OK(status);
        executed = true;
    });
    pool.shutdown();
    pool.join();
    ASSERT_TRUE(executed);
}

TEST_F(ThreadPoolTest, RepeatedScheduleDoesntSmashStack) {
    auto& pool = makePool({});
    auto finished = makePromiseFuture<void>();
    auto func = [&](auto&& self, int depth) -> void {
        if (depth) {
            pool.schedule([&, depth](auto status) {
                ASSERT_OK(status);
                self(self, depth - 1);
            });
        } else {
            pool.shutdown();
            finished.promise.emplaceValue();
        }
    };
    func(func, 10'000);
    pool.startup();
    pool.join();
    finished.future.get();
}

DEATH_TEST_F(ThreadPoolDeathTest, DieOnDoubleStartUp, "already started") {
    auto& pool = makePool({});
    pool.startup();
    pool.startup();
}

DEATH_TEST_F(ThreadPoolDeathTest, DieWhenExceptionBubblesUp, "task oops") {
    auto& pool = makePool({});
    pool.startup();
    pool.schedule([](auto&&) { uassertStatusOK(Status({ErrorCodes::BadValue, "task oops"})); });
    pool.shutdown();
    pool.join();
}

DEATH_TEST_F(ThreadPoolDeathTest, DieOnDoubleJoin, "Attempted to join pool") {
    auto& pool = makePool({});
    pool.shutdown();
    pool.join();
    pool.join();
}

}  // namespace
}  // namespace mongo

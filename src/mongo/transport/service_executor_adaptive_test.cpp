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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault;

#include "mongo/platform/basic.h"

#include "boost/optional.hpp"

#include "mongo/db/service_context.h"
#include "mongo/transport/service_executor_adaptive.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

#include <asio.hpp>

namespace mongo {
namespace {
using namespace transport;

struct TestOptions : public ServiceExecutorAdaptive::Options {
    int reservedThreads() const final {
        return 1;
    }

    Milliseconds workerThreadRunTime() const final {
        return Milliseconds{1000};
    }

    int runTimeJitter() const final {
        return 0;
    }

    Milliseconds stuckThreadTimeout() const final {
        return Milliseconds{100};
    }

    Microseconds maxQueueLatency() const final {
        return duration_cast<Microseconds>(Milliseconds{10});
    }

    int idlePctThreshold() const final {
        return 0;
    }

    int recursionLimit() const final {
        return 0;
    }
};

struct RecursionOptions : public ServiceExecutorAdaptive::Options {
    int reservedThreads() const final {
        return 1;
    }

    Milliseconds workerThreadRunTime() const final {
        return Milliseconds{1000};
    }

    int runTimeJitter() const final {
        return 0;
    }

    Milliseconds stuckThreadTimeout() const final {
        return Milliseconds{100};
    }

    Microseconds maxQueueLatency() const final {
        return duration_cast<Microseconds>(Milliseconds{5});
    }

    int idlePctThreshold() const final {
        return 0;
    }

    int recursionLimit() const final {
        return 10;
    }
};

class ServiceExecutorAdaptiveFixture : public unittest::Test {
protected:
    void setUp() override {
        setGlobalServiceContext(ServiceContext::make());
        asioIoCtx = std::make_shared<asio::io_context>();
    }

    std::shared_ptr<asio::io_context> asioIoCtx;

    stdx::mutex mutex;
    AtomicWord<int> waitFor{-1};
    stdx::condition_variable cond;
    stdx::function<void()> notifyCallback = [this] {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        invariant(waitFor.load() != -1);
        waitFor.fetchAndSubtract(1);
        cond.notify_one();
        log() << "Ran callback";
    };

    void waitForCallback(int expected, boost::optional<Milliseconds> timeout = boost::none) {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        invariant(waitFor.load() != -1);
        if (timeout) {
            ASSERT_TRUE(cond.wait_for(
                lk, timeout->toSystemDuration(), [&] { return waitFor.load() == expected; }));
        } else {
            cond.wait(lk, [&] { return waitFor.load() == expected; });
        }
    }

    ServiceExecutorAdaptive::Options* config;

    template <class Options>
    std::unique_ptr<ServiceExecutorAdaptive> makeAndStartExecutor() {
        auto configOwned = stdx::make_unique<Options>();
        config = configOwned.get();
        auto exec = stdx::make_unique<ServiceExecutorAdaptive>(
            getGlobalServiceContext(), asioIoCtx, std::move(configOwned));

        ASSERT_OK(exec->start());
        log() << "wait for executor to finish starting";
        waitFor.store(1);
        ASSERT_OK(exec->schedule(notifyCallback,
                                 ServiceExecutor::kEmptyFlags,
                                 ServiceExecutorTaskName::kSSMProcessMessage));
        waitForCallback(0);
        ASSERT_EQ(exec->threadsRunning(), config->reservedThreads());

        return exec;
    }
};

/*
 * This tests that the executor will launch a new thread if the current threads are blocked, and
 * that those threads retire when they become idle.
 */
TEST_F(ServiceExecutorAdaptiveFixture, TestStuckTask) {
    stdx::mutex blockedMutex;
    stdx::unique_lock<stdx::mutex> blockedLock(blockedMutex);

    auto exec = makeAndStartExecutor<TestOptions>();
    auto guard = makeGuard([&] {
        if (blockedLock)
            blockedLock.unlock();
        ASSERT_OK(exec->shutdown(config->workerThreadRunTime() * 2));
    });

    log() << "Scheduling blocked task";
    waitFor.store(3);
    ASSERT_OK(exec->schedule(
        [this, &blockedMutex] {
            notifyCallback();
            stdx::unique_lock<stdx::mutex> lk(blockedMutex);
            notifyCallback();
        },
        ServiceExecutor::kEmptyFlags,
        ServiceExecutorTaskName::kSSMProcessMessage));

    log() << "Scheduling task stuck on blocked task";
    ASSERT_OK(exec->schedule(
        notifyCallback, ServiceExecutor::kEmptyFlags, ServiceExecutorTaskName::kSSMProcessMessage));

    log() << "Waiting for second thread to start";
    waitForCallback(1);
    ASSERT_EQ(exec->threadsRunning(), 2);

    log() << "Waiting for unstuck task to run";
    blockedLock.unlock();
    waitForCallback(0);
    ASSERT_EQ(exec->threadsRunning(), 2);

    log() << "Waiting for second thread to idle out";
    stdx::this_thread::sleep_for(config->workerThreadRunTime().toSystemDuration() * 1.5);
    ASSERT_EQ(exec->threadsRunning(), config->reservedThreads());
}

/*
 * This tests that the executor will start a new batch of reserved threads if it detects that
 * all
 * threads are running a task for longer than the stuckThreadTimeout.
 */
TEST_F(ServiceExecutorAdaptiveFixture, TestStuckThreads) {
    stdx::mutex blockedMutex;
    stdx::unique_lock<stdx::mutex> blockedLock(blockedMutex);

    auto exec = makeAndStartExecutor<TestOptions>();
    auto guard = makeGuard([&] {
        if (blockedLock)
            blockedLock.unlock();
        ASSERT_OK(exec->shutdown(config->workerThreadRunTime() * 2));
    });

    auto blockedTask = [this, &blockedMutex] {
        log() << "waiting on blocked mutex";
        notifyCallback();
        stdx::unique_lock<stdx::mutex> lk(blockedMutex);
        notifyCallback();
    };

    waitFor.store(6);
    auto tasks = waitFor.load() / 2;
    log() << "Scheduling " << tasks << " blocked tasks";
    for (auto i = 0; i < tasks; i++) {
        ASSERT_OK(exec->schedule(blockedTask,
                                 ServiceExecutor::kEmptyFlags,
                                 ServiceExecutorTaskName::kSSMProcessMessage));
    }

    log() << "Waiting for executor to start new threads";
    waitForCallback(3);

    log() << "All threads blocked, wait for executor to detect block and start a new thread.";

    // The controller thread in the adaptive executor runs on a stuckThreadTimeout in normal
    // operation where no starvation is detected (shouldn't be in this test as all threads should be
    // blocked). By waiting here for stuckThreadTimeout*3 it means that we have stuckThreadTimeout*2
    // for other waits in the controller and boot up a new thread which should be enough.
    stdx::this_thread::sleep_for(config->stuckThreadTimeout().toSystemDuration() * 3);

    ASSERT_EQ(exec->threadsRunning(), waitFor.load() + config->reservedThreads());

    log() << "Waiting for unstuck task to run";
    blockedLock.unlock();
    waitForCallback(0);
}

/*
 * This tests that the executor will launch more threads when starvation is detected. We launch
 * another task from itself so there will always be a queue of a waiting task if there's just one
 * thread.
 */
TEST_F(ServiceExecutorAdaptiveFixture, TestStarvation) {
    auto exec = makeAndStartExecutor<TestOptions>();

    // Mutex so we don't attempt to call schedule and shutdown concurrently
    stdx::mutex scheduleMutex;

    auto guard = makeGuard([&] { ASSERT_OK(exec->shutdown(config->workerThreadRunTime() * 2)); });

    bool scheduleNew{true};

    stdx::function<void()> task;
    task = [this, &task, &exec, &scheduleMutex, &scheduleNew] {
        // This sleep needs to be larger than the sleep below to be able to limit the amount of
        // starvation.
        stdx::this_thread::sleep_for(config->maxQueueLatency().toSystemDuration() * 5);

        {
            stdx::unique_lock<stdx::mutex> lock(scheduleMutex);

            if (scheduleNew) {
                ASSERT_OK(exec->schedule(task,
                                         ServiceExecutor::kEmptyFlags,
                                         ServiceExecutorTaskName::kSSMProcessMessage));
            }
        }

        // This sleep needs to be larger than maxQueueLatency, when we schedule above the controller
        // thread will wake up because starvation is detected. By the time the controller thread
        // have slept for maxQueueLatency both worker threads should be executing work and there's
        // no further starvation. It needs to be significantly larger to avoid a race with asio
        // post. In the case of the first time when there's only one worker thread, starvation
        // should be detected and the second worker will be started.
        stdx::this_thread::sleep_for(config->maxQueueLatency().toSystemDuration() * 2);
    };

    ASSERT_OK(exec->schedule(
        task, ServiceExecutor::kEmptyFlags, ServiceExecutorTaskName::kSSMProcessMessage));

    stdx::this_thread::sleep_for(config->workerThreadRunTime().toSystemDuration() * 2);
    ASSERT_EQ(exec->threadsRunning(), 2);

    stdx::unique_lock<stdx::mutex> lock(scheduleMutex);
    scheduleNew = false;
}

/*
 * This tests that the executor can execute tasks recursively. If it can't starvation will be
 * detected and new threads started.
 */
TEST_F(ServiceExecutorAdaptiveFixture, TestRecursion) {
    auto exec = makeAndStartExecutor<RecursionOptions>();

    AtomicWord<int> remainingTasks{config->recursionLimit() - 1};
    stdx::mutex mutex;
    stdx::condition_variable cv;
    stdx::function<void()> task;

    auto guard = makeGuard([&] { ASSERT_OK(exec->shutdown(config->workerThreadRunTime() * 2)); });

    task = [this, &task, &exec, &mutex, &cv, &remainingTasks] {
        if (remainingTasks.subtractAndFetch(1) == 0) {
            log() << "Signaling job done";
            cv.notify_one();
            return;
        }

        log() << "Starting task recursively";

        ASSERT_OK(exec->schedule(
            task, ServiceExecutor::kMayRecurse, ServiceExecutorTaskName::kSSMProcessMessage));

        // Make sure we don't block too long because then the block detection logic would kick in.
        stdx::this_thread::sleep_for(config->stuckThreadTimeout().toSystemDuration() /
                                     (config->recursionLimit() * 2));
        log() << "Completing task recursively";
    };

    stdx::unique_lock<stdx::mutex> lock(mutex);

    ASSERT_OK(exec->schedule(
        task, ServiceExecutor::kEmptyFlags, ServiceExecutorTaskName::kSSMProcessMessage));

    cv.wait_for(lock, config->workerThreadRunTime().toSystemDuration(), [&remainingTasks]() {
        return remainingTasks.load() == 0;
    });

    ASSERT_EQ(exec->threadsRunning(), config->reservedThreads());
}

/*
 * This tests that deferred tasks don't cause a new thread to be created, and they don't
 * interfere
 * with new normal tasks
 */
TEST_F(ServiceExecutorAdaptiveFixture, TestDeferredTasks) {
    stdx::mutex blockedMutex;
    stdx::unique_lock<stdx::mutex> blockedLock(blockedMutex);

    auto exec = makeAndStartExecutor<TestOptions>();
    auto guard = makeGuard([&] {
        if (blockedLock)
            blockedLock.unlock();
        ASSERT_OK(exec->shutdown(config->workerThreadRunTime() * 2));
    });

    waitFor.store(3);
    log() << "Scheduling a blocking task";
    ASSERT_OK(exec->schedule(
        [this, &blockedMutex] {
            stdx::unique_lock<stdx::mutex> lk(blockedMutex);
            notifyCallback();
        },
        ServiceExecutor::kEmptyFlags,
        ServiceExecutorTaskName::kSSMProcessMessage));

    log() << "Scheduling deferred task";
    ASSERT_OK(exec->schedule(notifyCallback,
                             ServiceExecutor::kDeferredTask,
                             ServiceExecutorTaskName::kSSMProcessMessage));

    ASSERT_THROWS(waitForCallback(1, config->stuckThreadTimeout()),
                  unittest::TestAssertionFailureException);

    log() << "Scheduling non-deferred task";
    ASSERT_OK(exec->schedule(
        notifyCallback, ServiceExecutor::kEmptyFlags, ServiceExecutorTaskName::kSSMProcessMessage));
    waitForCallback(1, config->stuckThreadTimeout());
    ASSERT_GT(exec->threadsRunning(), config->reservedThreads());

    blockedLock.unlock();
    waitForCallback(0);
}

}  // namespace
}  // namespace mongo

/**
  *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault;

#include "mongo/platform/basic.h"

#include "boost/optional.hpp"

#include "mongo/db/service_context_noop.h"
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
        return duration_cast<Microseconds>(Milliseconds{5});
    }

    int idlePctThreshold() const final {
        return 0;
    }

    int maxRecursion() const final {
        return 0;
    }
};

class ServiceExecutorAdaptiveFixture : public unittest::Test {
protected:
    void setUp() override {
        auto scOwned = stdx::make_unique<ServiceContextNoop>();
        setGlobalServiceContext(std::move(scOwned));
        asioIoCtx = std::make_shared<asio::io_context>();
    }

    std::shared_ptr<asio::io_context> asioIoCtx;

    stdx::mutex mutex;
    int waitFor = -1;
    stdx::condition_variable cond;
    stdx::function<void()> notifyCallback = [this] {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        invariant(waitFor != -1);
        waitFor--;
        cond.notify_one();
        log() << "Ran callback";
    };

    void waitForCallback(int expected, boost::optional<Milliseconds> timeout = boost::none) {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        invariant(waitFor != -1);
        if (timeout) {
            ASSERT_TRUE(cond.wait_for(
                lk, timeout->toSystemDuration(), [&] { return waitFor == expected; }));
        } else {
            cond.wait(lk, [&] { return waitFor == expected; });
        }
    }

    ServiceExecutorAdaptive::Options* config;
    std::unique_ptr<ServiceExecutorAdaptive> makeAndStartExecutor() {
        auto configOwned = stdx::make_unique<TestOptions>();
        config = configOwned.get();
        auto exec = stdx::make_unique<ServiceExecutorAdaptive>(
            getGlobalServiceContext(), asioIoCtx, std::move(configOwned));

        ASSERT_OK(exec->start());
        log() << "wait for executor to finish starting";
        waitFor = 1;
        ASSERT_OK(exec->schedule(notifyCallback, ServiceExecutor::EmptyFlags));
        waitForCallback(0);
        ASSERT_EQ(exec->threadsRunning(), 1);

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

    auto exec = makeAndStartExecutor();
    auto guard = MakeGuard([&] {
        if (blockedLock)
            blockedLock.unlock();
        ASSERT_OK(exec->shutdown());
    });

    log() << "Scheduling blocked task";
    waitFor = 3;
    ASSERT_OK(exec->schedule(
        [this, &blockedMutex] {
            notifyCallback();
            stdx::unique_lock<stdx::mutex> lk(blockedMutex);
            notifyCallback();
        },
        ServiceExecutor::EmptyFlags));

    log() << "Scheduling task stuck on blocked task";
    ASSERT_OK(exec->schedule(notifyCallback, ServiceExecutor::EmptyFlags));

    log() << "Waiting for second thread to start";
    waitForCallback(1);
    ASSERT_EQ(exec->threadsRunning(), 2);

    log() << "Waiting for unstuck task to run";
    blockedLock.unlock();
    waitForCallback(0);
    ASSERT_EQ(exec->threadsRunning(), 2);

    log() << "Waiting for second thread to idle out";
    stdx::this_thread::sleep_for(config->workerThreadRunTime().toSystemDuration() * 1.5);
    ASSERT_EQ(exec->threadsRunning(), 1);
}

/*
 * This tests that the executor will start a new batch of reserved threads if it detects that
 * all
 * threads are running a task for longer than the stuckThreadTimeout.
 */
TEST_F(ServiceExecutorAdaptiveFixture, TestStuckThreads) {
    stdx::mutex blockedMutex;
    stdx::unique_lock<stdx::mutex> blockedLock(blockedMutex);

    auto exec = makeAndStartExecutor();
    auto guard = MakeGuard([&] {
        if (blockedLock)
            blockedLock.unlock();
        ASSERT_OK(exec->shutdown());
    });

    auto blockedTask = [this, &blockedMutex] {
        log() << "waiting on blocked mutex";
        notifyCallback();
        stdx::unique_lock<stdx::mutex> lk(blockedMutex);
        notifyCallback();
    };

    waitFor = 6;
    log() << "Scheduling " << waitFor << " blocked tasks";
    for (auto i = 0; i < waitFor / 2; i++) {
        ASSERT_OK(exec->schedule(blockedTask, ServiceExecutor::EmptyFlags));
    }

    log() << "Waiting for executor to start new threads";
    waitForCallback(3);
    stdx::this_thread::sleep_for(config->stuckThreadTimeout().toSystemDuration() * 2);

// TODO The timing of this test is broken on windows, re-enable this test in SERVER-30475
#ifndef _WIN32
    ASSERT_EQ(exec->threadsRunning(), waitFor + 1);
#endif

    log() << "Waiting for unstuck task to run";
    blockedLock.unlock();
    waitForCallback(0);
}

/*
 * This tests that deferred tasks don't cause a new thread to be created, and they don't
 * interfere
 * with new normal tasks
 */
TEST_F(ServiceExecutorAdaptiveFixture, TestDeferredTasks) {
    stdx::mutex blockedMutex;
    stdx::unique_lock<stdx::mutex> blockedLock(blockedMutex);

    auto exec = makeAndStartExecutor();
    auto guard = MakeGuard([&] {
        if (blockedLock)
            blockedLock.unlock();
        ASSERT_OK(exec->shutdown());
    });

    waitFor = 3;
    log() << "Scheduling a blocking task";
    ASSERT_OK(exec->schedule(
        [this, &blockedMutex] {
            stdx::unique_lock<stdx::mutex> lk(blockedMutex);
            notifyCallback();
        },
        ServiceExecutor::EmptyFlags));

    log() << "Scheduling deferred task";
    ASSERT_OK(exec->schedule(notifyCallback, ServiceExecutor::DeferredTask));

    ASSERT_THROWS(waitForCallback(1, config->stuckThreadTimeout()),
                  unittest::TestAssertionFailureException);

    log() << "Scheduling non-deferred task";
    ASSERT_OK(exec->schedule(notifyCallback, ServiceExecutor::EmptyFlags));
    waitForCallback(1, config->stuckThreadTimeout());
    ASSERT_GT(exec->threadsRunning(), 1);

    blockedLock.unlock();
    waitForCallback(0);
}

}  // namespace
}  // namespace mongo

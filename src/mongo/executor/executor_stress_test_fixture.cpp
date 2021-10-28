/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/executor/executor_stress_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace executor {

ThreadPoolExecutorStressTestEngine::ThreadPoolExecutorStressTestEngine(
    std::shared_ptr<TaskExecutor> executor,
    boost::optional<NetworkInterfaceMock*> netMock,
    Milliseconds waitBeforeTermination)
    : _executor(std::move(executor)),
      _netMock(netMock),
      _waitBeforeTermination(waitBeforeTermination) {
    _timer.reset();
    _terminate.store(false);
    _threadAssertionMonitor.emplace();
    if (_netMock) {
        // If we use mocked Network, start a thread that answers pending requests.
        _addMockNetworkResponseThread();
    }
}

void ThreadPoolExecutorStressTestEngine::addSimpleSchedulingThreads(int count) {
    auto schedulerTask = [this] {
        while (!_terminate.load()) {
            CopyableCallback work = [this](const TaskExecutor::CallbackArgs&) {
                _completedWorks.increment();
            };
            const auto swCb = _executor->scheduleWork(work);
            if (!swCb.isOK()) {
                ASSERT_TRUE(_terminate.load())
                    << "Scheduling failed before termination flag was set";
                ASSERT_TRUE(_executor->isShuttingDown())
                    << "Scheduling failed before executor was shut down";
            } else {
                auto lk = stdx::lock_guard(_mutex);
                _callbacks.push_back(swCb.getValue());
            }
            while (!_terminate.load()) {
                sleepFor(kDurationBetweenSimpleSchedulings);
                auto lk = stdx::lock_guard(_mutex);
                if (_callbacks.size() < kMaxCallbacks) {
                    break;
                }
            }
        }
    };

    auto lk = stdx::lock_guard(_mutex);
    for (int i = 0; i < count; ++i) {
        // _monitor is an instance of `ThreadAssertionMonitor`
        _threads.emplace_back(_threadAssertionMonitor->spawn([schedulerTask] { schedulerTask(); }));
    }
}

void ThreadPoolExecutorStressTestEngine::addRandomCancelationThreads(int count) {
    auto cancelationTask = [this] {
        while (true) {
            TaskExecutor::CallbackHandle cb;
            {
                auto lk = stdx::lock_guard(_mutex);
                while ((_callbacks.size() > 100 || _terminate.load()) && !cb) {
                    cb = std::move(_callbacks.front());
                    _callbacks.pop_front();
                }
            }

            if (auto shouldCancel = nextRandomInt32(2) == 0; shouldCancel && cb) {
                _executor->cancel(cb);
            } else if (cb) {
                _executor->wait(cb);
            }
            if (_terminate.load()) {
                stdx::lock_guard<Latch> lk(_mutex);
                if (_callbacks.empty()) {
                    break;
                }
            }
        }
    };

    auto lk = stdx::lock_guard(_mutex);
    for (int i = 0; i < count; ++i) {
        _threads.emplace_back(cancelationTask);
    }
}

void ThreadPoolExecutorStressTestEngine::addScheduleRemoteCommandThreads(int count) {
    auto remoteSchedulerTask = [this] {
        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", BSONObj(), nullptr);
        while (!_terminate.load()) {
            const auto swCb = _executor->scheduleRemoteCommand(
                rcr, [this](const TaskExecutor::RemoteCommandCallbackArgs& ca) {
                    if (ca.response.status.isOK()) {
                        _commandsSucceeded.increment();
                    } else {
                        _commandsFailed.increment();
                    }
                });
            if (!swCb.isOK()) {
                // This race can happen only at termination.
                ASSERT_TRUE(_terminate.load())
                    << "Scheduling failed before termination flag was set";
                ASSERT_TRUE(_executor->isShuttingDown())
                    << "Scheduling failed before executor was shut down";
            }

            {
                stdx::lock_guard<Latch> lk(_mutex);
                _callbacks.push_back(swCb.getValue());
            }
            sleepFor(kDurationBetweenRemoteCommands);
        }
    };

    auto lk = stdx::lock_guard(_mutex);
    for (auto i = 0; i < count; ++i) {
        _threads.emplace_back(
            _threadAssertionMonitor->spawn([remoteSchedulerTask] { remoteSchedulerTask(); }));
    }
}

void ThreadPoolExecutorStressTestEngine::_addMockNetworkResponseThread() {
    if (!_netMock) {
        return;  // Nothing to do if not mock.
    }
    stdx::lock_guard<Latch> lk(_mutex);
    _threads.emplace_back([this] {
        while (true) {
            {
                NetworkInterfaceMock::InNetworkGuard ing(*_netMock);
                while ((*_netMock)->hasReadyRequests()) {
                    (*_netMock)->scheduleSuccessfulResponse(BSONObj());
                    (*_netMock)->runReadyNetworkOperations();
                }
            }
            sleepFor(kDurationBetweenMockedResponses);
            // Network response thread must wait until all callbacks are cleared.
            if (_terminate.load()) {
                stdx::lock_guard<Latch> lk(_mutex);
                if (_callbacks.empty()) {
                    break;
                }
            }
        }
    });
}

void ThreadPoolExecutorStressTestEngine::waitAndCleanup() {
    while (true) {
        sleepFor(Milliseconds(500));
        int64_t threadsSize;
        int64_t pendingRequestsSize;
        {
            stdx::lock_guard<Latch> lk(_mutex);
            threadsSize = _threads.size();
            pendingRequestsSize = _callbacks.size();
        }
        LOGV2(5822101,
              "Waiting for test termination",
              "completed"_attr = _completedWorks.get(),
              "commandsSucceeded"_attr = _commandsSucceeded.get(),
              "commandsFailed"_attr = _commandsFailed.get(),
              "threadsSize"_attr = threadsSize,
              "pendingRequestsSize"_attr = pendingRequestsSize);
        if (_timer.elapsed() > _waitBeforeTermination) {
            break;
        }
    }

    _terminate.store(true);

    std::list<stdx::thread> threads;
    {
        auto lk = stdx::lock_guard(_mutex);
        std::swap(threads, _threads);
    }

    while (!threads.empty()) {
        threads.front().join();
        threads.pop_front();
    }

    _executor->shutdown();
    _executor->join();

    _threadAssertionMonitor->notifyDone();
    _threadAssertionMonitor.reset();
}

int32_t ThreadPoolExecutorStressTestEngine::nextRandomInt32(int32_t max) {
    static thread_local PseudoRandom random(SecureRandom().nextInt64());
    return random.nextInt32(max);
}


}  // namespace executor
}  // namespace mongo

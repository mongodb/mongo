/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

#include "mongo/transport/service_executor_fixed.h"

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_executor_gen.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeSchedulingServiceExecutorFixedTask);

namespace transport {
namespace {
constexpr auto kThreadsRunning = "threadsRunning"_sd;
constexpr auto kExecutorLabel = "executor"_sd;
constexpr auto kExecutorName = "fixed"_sd;
}  // namespace

ServiceExecutorFixed::ServiceExecutorFixed(ThreadPool::Options options)
    : _options(std::move(options)) {
    _options.onCreateThread =
        [this, onCreate = std::move(_options.onCreateThread)](const std::string& name) mutable {
            _executorContext = std::make_unique<ExecutorThreadContext>(this->weak_from_this());
            if (onCreate) {
                onCreate(name);
            }
        };
    _threadPool = std::make_unique<ThreadPool>(_options);
}

ServiceExecutorFixed::~ServiceExecutorFixed() {
    invariant(!_canScheduleWork.load());
    if (_state == State::kNotStarted)
        return;

    // Ensures we always call "shutdown" after staring the service executor
    invariant(_state == State::kStopped);
    _threadPool->shutdown();
    _threadPool->join();
    invariant(_numRunningExecutorThreads.load() == 0);
}

Status ServiceExecutorFixed::start() {
    stdx::lock_guard<Latch> lk(_mutex);
    auto oldState = std::exchange(_state, State::kRunning);
    invariant(oldState == State::kNotStarted);
    _threadPool->startup();
    _canScheduleWork.store(true);
    LOGV2_DEBUG(
        4910501, 3, "Started fixed thread-pool service executor", "name"_attr = _options.poolName);
    return Status::OK();
}

Status ServiceExecutorFixed::shutdown(Milliseconds timeout) {
    auto waitForShutdown = [&]() mutable -> Status {
        stdx::unique_lock<Latch> lk(_mutex);
        bool success = _shutdownCondition.wait_for(lk, timeout.toSystemDuration(), [this] {
            return _numRunningExecutorThreads.load() == 0;
        });
        return success ? Status::OK()
                       : Status(ErrorCodes::ExceededTimeLimit,
                                "Failed to shutdown all executor threads within the time limit");
    };

    LOGV2_DEBUG(4910502,
                3,
                "Shutting down fixed thread-pool service executor",
                "name"_attr = _options.poolName);

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _canScheduleWork.store(false);

        auto oldState = std::exchange(_state, State::kStopped);
        if (oldState != State::kStopped) {
            _threadPool->shutdown();
        }
    }

    return waitForShutdown();
}

Status ServiceExecutorFixed::scheduleTask(Task task, ScheduleFlags flags) {
    if (!_canScheduleWork.load()) {
        return Status(ErrorCodes::ShutdownInProgress, "Executor is not running");
    }

    auto mayExecuteTaskInline = [&] {
        if (!(flags & ScheduleFlags::kMayRecurse))
            return false;
        if (!_executorContext)
            return false;
        return true;
    };


    if (mayExecuteTaskInline()) {
        invariant(_executorContext);
        if (_executorContext->getRecursionDepth() <
            fixedServiceExecutorRecursionLimit.loadRelaxed()) {
            // Recursively executing the task on the executor thread.
            _executorContext->run(std::move(task));
            return Status::OK();
        }
    }

    hangBeforeSchedulingServiceExecutorFixedTask.pauseWhileSet();

    // May throw if an attempt is made to schedule after the thread pool is shutdown.
    try {
        _threadPool->schedule([task = std::move(task)](Status status) mutable {
            internalAssert(status);
            invariant(_executorContext);
            _executorContext->run(std::move(task));
        });
    } catch (DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

void ServiceExecutorFixed::runOnDataAvailable(Session* session,
                                              OutOfLineExecutor::Task onCompletionCallback) {
    invariant(session);
    session->waitForData().thenRunOn(shared_from_this()).getAsync(std::move(onCompletionCallback));
}

void ServiceExecutorFixed::appendStats(BSONObjBuilder* bob) const {
    *bob << kExecutorLabel << kExecutorName << kThreadsRunning
         << static_cast<int>(_numRunningExecutorThreads.load());
}

int ServiceExecutorFixed::getRecursionDepthForExecutorThread() const {
    invariant(_executorContext);
    return _executorContext->getRecursionDepth();
}

}  // namespace transport
}  // namespace mongo

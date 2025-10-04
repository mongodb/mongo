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

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/client/remote_command_retry_scheduler.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(shutdownBeforeSendingRetryCommand);
}

RemoteCommandRetryScheduler::RemoteCommandRetryScheduler(
    executor::TaskExecutor* executor,
    const executor::RemoteCommandRequest& request,
    const executor::TaskExecutor::RemoteCommandCallbackFn& callback,
    std::unique_ptr<mongo::RetryStrategy> retryStrategy)
    : _executor(executor),
      _request(request),
      _callback(callback),
      _retryStrategy(std::move(retryStrategy)) {
    uassert(ErrorCodes::BadValue, "task executor cannot be null", executor);
    uassert(ErrorCodes::BadValue,
            "source in remote command request cannot be empty",
            !request.target.empty());
    uassert(ErrorCodes::BadValue,
            "database name in remote command request cannot be empty",
            !request.dbname.isEmpty());
    uassert(ErrorCodes::BadValue,
            "command object in remote command request cannot be empty",
            !request.cmdObj.isEmpty());
    uassert(ErrorCodes::BadValue, "remote command callback function cannot be null", callback);
    uassert(ErrorCodes::BadValue, "retry strategy cannot be null", _retryStrategy);
}

RemoteCommandRetryScheduler::~RemoteCommandRetryScheduler() {
    try {
        shutdown();
        join();
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

bool RemoteCommandRetryScheduler::isActive() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _isActive(lock);
}

bool RemoteCommandRetryScheduler::_isActive(WithLock lk) const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

Status RemoteCommandRetryScheduler::startup() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    switch (_state) {
        case State::kPreStart:
            _state = State::kRunning;
            break;
        case State::kRunning:
            return Status(ErrorCodes::IllegalOperation, "scheduler already started");
        case State::kShuttingDown:
            return Status(ErrorCodes::ShutdownInProgress, "scheduler shutting down");
        case State::kComplete:
            return Status(ErrorCodes::ShutdownInProgress, "scheduler completed");
    }

    auto scheduleStatus = _schedule_inlock();
    if (!scheduleStatus.isOK()) {
        _state = State::kComplete;
        return scheduleStatus;
    }

    return Status::OK();
}

void RemoteCommandRetryScheduler::shutdown() {
    executor::TaskExecutor::CallbackHandle remoteCommandCallbackHandle;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        switch (_state) {
            case State::kPreStart:
                // Transition directly from PreStart to Complete if not started yet.
                _state = State::kComplete;
                return;
            case State::kRunning:
                _state = State::kShuttingDown;
                break;
            case State::kShuttingDown:
            case State::kComplete:
                // Nothing to do if we are already in ShuttingDown or Complete state.
                return;
        }

        remoteCommandCallbackHandle = _remoteCommandCallbackHandle;
    }

    // Cancel the sleep operation
    _cancellationSource.cancel();

    invariant(remoteCommandCallbackHandle.isValid());
    _executor->cancel(remoteCommandCallbackHandle);
}

void RemoteCommandRetryScheduler::join() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condition.wait(lock, [&]() { return !_isActive(lock); });
}

std::string RemoteCommandRetryScheduler::toString() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    str::stream output;
    output << "RemoteCommandRetryScheduler";
    output << " request: " << _request.toString();
    output << " active: " << _isActive(lock);
    if (_remoteCommandCallbackHandle.isValid()) {
        output << " callbackHandle.valid: " << _remoteCommandCallbackHandle.isValid();
        output << " callbackHandle.cancelled: " << _remoteCommandCallbackHandle.isCanceled();
    }
    output << " attempt: " << _currentAttempt;
    return output;
}

Status RemoteCommandRetryScheduler::_schedule_inlock() {
    ++_currentAttempt;
    auto scheduleResult = _executor->scheduleRemoteCommand(
        _request, [this](const auto& x) { return this->_remoteCommandCallback(x); });

    if (!scheduleResult.isOK()) {
        return scheduleResult.getStatus();
    }

    _remoteCommandCallbackHandle = scheduleResult.getValue();
    return Status::OK();
}

void RemoteCommandRetryScheduler::_remoteCommandCallback(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba) {
    const auto& status = rcba.response.status;

    if (status.isOK()) {
        {
            auto _ = stdx::lock_guard<stdx::mutex>{_mutex};
            _retryStrategy->recordSuccess(rcba.request.target);
        }
        _onComplete(rcba);
        return;
    }

    auto retryDelay = [&]() -> boost::optional<Milliseconds> {
        auto _ = stdx::lock_guard<stdx::mutex>{_mutex};
        auto shouldRetry =
            _retryStrategy->recordFailureAndEvaluateShouldRetry(status, rcba.request.target, {});
        return shouldRetry ? boost::make_optional(_retryStrategy->getNextRetryDelay())
                           : boost::none;
    }();
    bool shouldRetry = retryDelay.has_value();

    if (MONGO_unlikely(shutdownBeforeSendingRetryCommand.shouldFail())) {
        shutdown();
    }

    if (!shouldRetry || status == ErrorCodes::CallbackCanceled) {
        _onComplete(rcba);
        return;
    }

    // TODO(benety): Check cumulative elapsed time of failed responses received
    // against retry policy. Requires SERVER-24067.
    auto scheduleCommand = [this]() -> Status {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (State::kShuttingDown == _state) {
            return Status(ErrorCodes::CallbackCanceled,
                          "scheduler was shut down before retrying command");
        }
        return _schedule_inlock();
    };

    auto handleScheduleError = [this, rcba](const Status& scheduleStatus) {
        if (!scheduleStatus.isOK()) {
            _onComplete({rcba.executor,
                         rcba.myHandle,
                         rcba.request,
                         executor::RemoteCommandResponse(rcba.request.target, scheduleStatus)});
        }
    };

    _executor->sleepFor(*retryDelay, _cancellationSource.token())
        .then([scheduleCommand]() { return scheduleCommand(); })
        .getAsync(handleScheduleError);
}

void RemoteCommandRetryScheduler::_onComplete(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba) {

    invariant(_callback);
    _callback(rcba);

    // This will release the resources held by the '_callback' function object. To avoid any issues
    // with destruction logic in the function object's resources accessing this
    // RemoteCommandRetryScheduler, we release this function object outside the lock.
    _callback = {};

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_isActive(lock));
    _state = State::kComplete;
    _condition.notify_all();
}

bool isMongosRetriableError(const ErrorCodes::Error& code) {
    return ErrorCodes::isRetriableError(code) || code == ErrorCodes::BalancerInterrupted;
}

}  // namespace mongo

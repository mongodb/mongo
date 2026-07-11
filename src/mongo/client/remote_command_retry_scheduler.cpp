// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    std::lock_guard<std::mutex> lock(_mutex);
    return _isActive(lock);
}

bool RemoteCommandRetryScheduler::_isActive(WithLock lk) const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

Status RemoteCommandRetryScheduler::startup() {
    std::lock_guard<std::mutex> lock(_mutex);

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
        std::lock_guard<std::mutex> lock(_mutex);
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
    std::unique_lock<std::mutex> lock(_mutex);
    _condition.wait(lock, [&]() { return !_isActive(lock); });
}

std::string RemoteCommandRetryScheduler::toString() const {
    std::lock_guard<std::mutex> lock(_mutex);
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
            auto _ = std::lock_guard{_mutex};
            _retryStrategy->recordSuccess(rcba.request.target);
        }
        _onComplete(rcba);
        return;
    }

    auto retryDelay = [&]() -> boost::optional<Milliseconds> {
        auto _ = std::lock_guard{_mutex};
        auto shouldRetry = _retryStrategy->recordFailureAndEvaluateShouldRetry(
            status, rcba.request.target, {}, boost::none);
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
        std::lock_guard<std::mutex> lock(_mutex);
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
        .then([scheduleCommand, retryDelay = *retryDelay, this] {
            {
                auto _ = std::lock_guard{_mutex};
                _retryStrategy->recordBackoff(retryDelay);
            }
            return scheduleCommand();
        })
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

    std::lock_guard<std::mutex> lock(_mutex);
    invariant(_isActive(lock));
    _state = State::kComplete;
    _condition.notify_all();
}

bool isMongosRetriableError(const ErrorCodes::Error& code) {
    return ErrorCodes::isRetriableError(code) || code == ErrorCodes::BalancerInterrupted;
}

}  // namespace mongo

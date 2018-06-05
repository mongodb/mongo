/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include <algorithm>
#include <vector>

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

class RetryPolicyImpl : public RemoteCommandRetryScheduler::RetryPolicy {
public:
    RetryPolicyImpl(std::size_t maximumAttempts,
                    Milliseconds maximumResponseElapsedTotal,
                    const std::initializer_list<ErrorCodes::Error>& retryableErrors);
    std::size_t getMaximumAttempts() const override;
    Milliseconds getMaximumResponseElapsedTotal() const override;
    bool shouldRetryOnError(ErrorCodes::Error error) const override;
    std::string toString() const override;

private:
    std::size_t _maximumAttempts;
    Milliseconds _maximumResponseElapsedTotal;
    std::vector<ErrorCodes::Error> _retryableErrors;
};

RetryPolicyImpl::RetryPolicyImpl(std::size_t maximumAttempts,
                                 Milliseconds maximumResponseElapsedTotal,
                                 const std::initializer_list<ErrorCodes::Error>& retryableErrors)
    : _maximumAttempts(maximumAttempts),
      _maximumResponseElapsedTotal(maximumResponseElapsedTotal),
      _retryableErrors(retryableErrors) {
    std::sort(_retryableErrors.begin(), _retryableErrors.end());
}

std::string RetryPolicyImpl::toString() const {
    str::stream output;
    output << "RetryPolicyImpl";
    output << " maxAttempts: " << _maximumAttempts;
    output << " maxTimeMillis: " << _maximumResponseElapsedTotal;

    if (_retryableErrors.size() > 0) {
        output << "Retryable Errors: ";
        for (auto error : _retryableErrors) {
            output << error;
        }
    }
    return output;
}

std::size_t RetryPolicyImpl::getMaximumAttempts() const {
    return _maximumAttempts;
}

Milliseconds RetryPolicyImpl::getMaximumResponseElapsedTotal() const {
    return _maximumResponseElapsedTotal;
}

bool RetryPolicyImpl::shouldRetryOnError(ErrorCodes::Error error) const {
    return std::binary_search(_retryableErrors.cbegin(), _retryableErrors.cend(), error);
}

}  // namespace

const std::initializer_list<ErrorCodes::Error> RemoteCommandRetryScheduler::kNotMasterErrors{
    ErrorCodes::NotMaster, ErrorCodes::NotMasterNoSlaveOk, ErrorCodes::NotMasterOrSecondary};

const std::initializer_list<ErrorCodes::Error> RemoteCommandRetryScheduler::kAllRetriableErrors{
    ErrorCodes::NotMaster,
    ErrorCodes::NotMasterNoSlaveOk,
    ErrorCodes::NotMasterOrSecondary,
    // If write concern failed to be satisfied on the remote server, this most probably means that
    // some of the secondary nodes were unreachable or otherwise unresponsive, so the call is safe
    // to be retried if idempotency can be guaranteed.
    ErrorCodes::WriteConcernFailed,
    ErrorCodes::HostUnreachable,
    ErrorCodes::HostNotFound,
    ErrorCodes::NetworkTimeout,
    ErrorCodes::PrimarySteppedDown,
    ErrorCodes::InterruptedDueToStepDown,
    ErrorCodes::BalancerInterrupted};

std::unique_ptr<RemoteCommandRetryScheduler::RetryPolicy>
RemoteCommandRetryScheduler::makeNoRetryPolicy() {
    return makeRetryPolicy(1U, executor::RemoteCommandRequest::kNoTimeout, {});
}

std::unique_ptr<RemoteCommandRetryScheduler::RetryPolicy>
RemoteCommandRetryScheduler::makeRetryPolicy(
    std::size_t maxAttempts,
    Milliseconds maxResponseElapsedTotal,
    const std::initializer_list<ErrorCodes::Error>& retryableErrors) {
    std::unique_ptr<RetryPolicy> policy =
        stdx::make_unique<RetryPolicyImpl>(maxAttempts, maxResponseElapsedTotal, retryableErrors);
    return policy;
}

RemoteCommandRetryScheduler::RemoteCommandRetryScheduler(
    executor::TaskExecutor* executor,
    const executor::RemoteCommandRequest& request,
    const executor::TaskExecutor::RemoteCommandCallbackFn& callback,
    std::unique_ptr<RetryPolicy> retryPolicy)
    : _executor(executor),
      _request(request),
      _callback(callback),
      _retryPolicy(std::move(retryPolicy)) {
    uassert(ErrorCodes::BadValue, "task executor cannot be null", executor);
    uassert(ErrorCodes::BadValue,
            "source in remote command request cannot be empty",
            !request.target.empty());
    uassert(ErrorCodes::BadValue,
            "database name in remote command request cannot be empty",
            !request.dbname.empty());
    uassert(ErrorCodes::BadValue,
            "command object in remote command request cannot be empty",
            !request.cmdObj.isEmpty());
    uassert(ErrorCodes::BadValue, "remote command callback function cannot be null", callback);
    uassert(ErrorCodes::BadValue, "retry policy cannot be null", _retryPolicy);
    uassert(ErrorCodes::BadValue,
            "policy max attempts cannot be zero",
            _retryPolicy->getMaximumAttempts() != 0);
    uassert(ErrorCodes::BadValue,
            "policy max response elapsed total cannot be negative",
            !(_retryPolicy->getMaximumResponseElapsedTotal() !=
                  executor::RemoteCommandRequest::kNoTimeout &&
              _retryPolicy->getMaximumResponseElapsedTotal() < Milliseconds(0)));
}

RemoteCommandRetryScheduler::~RemoteCommandRetryScheduler() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

bool RemoteCommandRetryScheduler::isActive() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _isActive_inlock();
}

bool RemoteCommandRetryScheduler::_isActive_inlock() const {
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

    invariant(remoteCommandCallbackHandle.isValid());
    _executor->cancel(remoteCommandCallbackHandle);
}

void RemoteCommandRetryScheduler::join() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condition.wait(lock, [this]() { return !_isActive_inlock(); });
}

std::string RemoteCommandRetryScheduler::toString() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    str::stream output;
    output << "RemoteCommandRetryScheduler";
    output << " request: " << _request.toString();
    output << " active: " << _isActive_inlock();
    if (_remoteCommandCallbackHandle.isValid()) {
        output << " callbackHandle.valid: " << _remoteCommandCallbackHandle.isValid();
        output << " callbackHandle.cancelled: " << _remoteCommandCallbackHandle.isCanceled();
    }
    output << " attempt: " << _currentAttempt;
    output << " retryPolicy: " << _retryPolicy->toString();
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

    // Use a lambda to avoid unnecessary lock acquisition when checking conditions for termination.
    auto getCurrentAttempt = [this]() {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _currentAttempt;
    };

    if (status.isOK() || status == ErrorCodes::CallbackCanceled ||
        !_retryPolicy->shouldRetryOnError(status.code()) ||
        getCurrentAttempt() == _retryPolicy->getMaximumAttempts()) {
        _onComplete(rcba);
        return;
    }

    // TODO(benety): Check cumulative elapsed time of failed responses received against retry
    // policy. Requires SERVER-24067.
    auto scheduleStatus = [this]() {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (State::kShuttingDown == _state) {
            return Status(ErrorCodes::CallbackCanceled,
                          "scheduler was shut down before retrying command");
        }
        return _schedule_inlock();
    }();

    if (!scheduleStatus.isOK()) {
        _onComplete({rcba.executor, rcba.myHandle, rcba.request, scheduleStatus});
        return;
    }
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
    invariant(_isActive_inlock());
    _state = State::kComplete;
    _condition.notify_all();
}

}  // namespace mongo

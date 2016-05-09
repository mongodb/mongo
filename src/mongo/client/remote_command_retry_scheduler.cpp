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
    ErrorCodes::InterruptedDueToReplStateChange};

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
    return _active;
}

Status RemoteCommandRetryScheduler::startup() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (_active) {
        return Status(ErrorCodes::IllegalOperation, "fetcher already scheduled");
    }

    auto scheduleStatus = _schedule_inlock(0);
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    _active = true;
    return Status::OK();
}

void RemoteCommandRetryScheduler::shutdown() {
    executor::TaskExecutor::CallbackHandle remoteCommandCallbackHandle;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        if (!_active) {
            return;
        }

        remoteCommandCallbackHandle = _remoteCommandCallbackHandle;
    }

    invariant(remoteCommandCallbackHandle.isValid());
    _executor->cancel(remoteCommandCallbackHandle);
}

void RemoteCommandRetryScheduler::join() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condition.wait(lock, [this]() { return !_active; });
}

Status RemoteCommandRetryScheduler::_schedule_inlock(std::size_t requestCount) {
    auto scheduleResult = _executor->scheduleRemoteCommand(
        _request,
        stdx::bind(&RemoteCommandRetryScheduler::_remoteCommandCallback,
                   this,
                   stdx::placeholders::_1,
                   requestCount + 1));

    if (!scheduleResult.isOK()) {
        return scheduleResult.getStatus();
    }

    _remoteCommandCallbackHandle = scheduleResult.getValue();
    return Status::OK();
}

void RemoteCommandRetryScheduler::_remoteCommandCallback(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba, std::size_t requestCount) {
    auto status = rcba.response.getStatus();

    if (status.isOK() || status == ErrorCodes::CallbackCanceled ||
        requestCount == _retryPolicy->getMaximumAttempts() ||
        !_retryPolicy->shouldRetryOnError(status.code())) {
        _onComplete(rcba);
        return;
    }

    // TODO(benety): Check cumulative elapsed time of failed responses received against retry
    // policy. Requires SERVER-24067.

    auto scheduleStatus = _schedule_inlock(requestCount);
    if (!scheduleStatus.isOK()) {
        _onComplete({rcba.executor, rcba.myHandle, rcba.request, scheduleStatus});
        return;
    }
}

void RemoteCommandRetryScheduler::_onComplete(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba) {
    _callback(rcba);

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_active);
    _active = false;
    _condition.notify_all();
}

}  // namespace mongo

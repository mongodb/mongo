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

#pragma once

#include <cstdlib>
#include <initializer_list>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/error_codes.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Schedules a remote command request. On receiving a response from task executor (or remote
 * server), decides if the response should be forwarded to the "_callback" provided in the
 * constructor based on the retry policy.
 *
 * If the command is successful or has been canceled (either by calling cancel() or canceled by
 * the task executor on shutdown), the response is forwarded immediately to "_callback" and the
 * scheduler becomes inactive.
 *
 * Otherwise, the retry policy (specified at construction) is used to decide if we should
 * resubmit the remote command request. The retry policy is defined by:
 *     - maximum number of times to run the remote command;
 *     - maximum elapsed time of all failed remote command responses (requires SERVER-24067);
 *     - list of error codes, if present in the response, should stop the scheduler.
 */
class RemoteCommandRetryScheduler {
    MONGO_DISALLOW_COPYING(RemoteCommandRetryScheduler);

public:
    class RetryPolicy;

    /**
     * List of not master error codes.
     */
    static const std::initializer_list<ErrorCodes::Error> kNotMasterErrors;

    /**
     * List of retriable error codes.
     */
    static const std::initializer_list<ErrorCodes::Error> kAllRetriableErrors;

    /**
     * Generates a retry policy that will send the remote command request to the source at most
     * once.
     */
    static std::unique_ptr<RetryPolicy> makeNoRetryPolicy();

    /**
     * Creates a retry policy that will send the remote command request at most "maxAttempts".
     * This policy will also direct the scheduler to stop retrying if it encounters any of the
     * errors in "nonRetryableErrors".
     * (Requires SERVER-24067) The scheduler will also stop retrying if the total elapsed time
     * of all failed requests exceeds "maxResponseElapsedTotal".
     */
    static std::unique_ptr<RetryPolicy> makeRetryPolicy(
        std::size_t maxAttempts,
        Milliseconds maxResponseElapsedTotal,
        const std::initializer_list<ErrorCodes::Error>& retryableErrors);

    /**
     * Creates scheduler but does not schedule any remote command request.
     */
    RemoteCommandRetryScheduler(executor::TaskExecutor* executor,
                                const executor::RemoteCommandRequest& request,
                                const executor::TaskExecutor::RemoteCommandCallbackFn& callback,
                                std::unique_ptr<RetryPolicy> retryPolicy);

    virtual ~RemoteCommandRetryScheduler();

    /**
     * Returns true if we have scheduled a remote command and are waiting for the response.
     */
    bool isActive() const;

    /**
     * Schedules remote command request.
     */
    Status startup();

    /**
     * Cancels scheduled remote command requests.
     * Returns immediately if the scheduler is not active.
     * It is fine to call this multiple times.
     */
    void shutdown();

    /**
     * Waits until the scheduler is inactive.
     * It is fine to call this multiple times.
     */
    void join();

private:
    /**
     * Schedules remote command to be run by the executor.
     * "requestCount" is number of requests scheduled before calling this function.
     * When this function is called for the first time by startup(), "requestCount" will be 0.
     * The executor will invoke _remoteCommandCallback() with the remote command response and
     * ("requestCount" + 1).
     * By passing "requestCount" between tasks, we avoid having to synchronize access to this count
     * if it were a field.
     */
    Status _schedule_inlock(std::size_t requestCount);

    /**
     * Callback for remote command.
     * "requestCount" is number of requests scheduled prior to this response.
     */
    void _remoteCommandCallback(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba,
                                std::size_t requestCount);

    /**
     * Notifies caller that the scheduler has completed processing responses.
     */
    void _onComplete(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba);

    // Not owned by us.
    executor::TaskExecutor* _executor;

    const executor::RemoteCommandRequest _request;
    const executor::TaskExecutor::RemoteCommandCallbackFn _callback;
    std::unique_ptr<RetryPolicy> _retryPolicy;

    // Protects member data of this scheduler declared after mutex.
    mutable stdx::mutex _mutex;

    mutable stdx::condition_variable _condition;

    // _active is true when remote command is scheduled to be run by the executor.
    bool _active = false;

    // Callback handle to the scheduled remote command.
    executor::TaskExecutor::CallbackHandle _remoteCommandCallbackHandle;
};

/**
 * Policy used by RemoteCommandRetryScheduler to determine if it is necessary to schedule another
 * remote command request.
 */
class RemoteCommandRetryScheduler::RetryPolicy {
public:
    virtual ~RetryPolicy() = default;

    /**
     * Retry scheduler should not send remote command request more than this limit.
     */
    virtual std::size_t getMaximumAttempts() const = 0;

    /**
     * Retry scheduler should not re-send remote command request if total response elapsed times of
     * prior responses exceed this limit.
     * Assumes that re-sending the command will not exceed the limit returned by
     * "getMaximumAttempts()".
     * Returns executor::RemoteCommandRequest::kNoTimeout if this limit should be ignored.
     */
    virtual Milliseconds getMaximumResponseElapsedTotal() const = 0;

    /**
     * Checks the error code in the most recent remote command response and returns true if
     * scheduler should retry the remote command request.
     * Assumes that re-sending the command will not exceed the limit returned by
     * "getMaximumAttempts()" and total response elapsed time has not been exceeded (see
     * "getMaximumResponseElapsedTotal()").
     */
    virtual bool shouldRetryOnError(ErrorCodes::Error error) const = 0;
};

}  // namespace mongo

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

#pragma once

#include <cstdlib>
#include <initializer_list>
#include <memory>

#include <fmt/format.h>

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
    RemoteCommandRetryScheduler(const RemoteCommandRetryScheduler&) = delete;
    RemoteCommandRetryScheduler& operator=(const RemoteCommandRetryScheduler&) = delete;

public:
    class RetryPolicy;

    /**
     * Generates a retry policy that will send the remote command request to the source at most
     * once.
     */
    static std::unique_ptr<RetryPolicy> makeNoRetryPolicy();

    /**
     * Creates a retry policy that will send the remote command request at most "maxAttempts".
     * (Requires SERVER-24067) The scheduler will also stop retrying if the total elapsed time
     * of all failed requests exceeds "maxResponseElapsedTotal".
     */
    template <ErrorCategory kCategory>
    static std::unique_ptr<RetryPolicy> makeRetryPolicy(std::size_t maxAttempts,
                                                        Milliseconds maxResponseElapsedTotal);

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
    bool _isActive_inlock() const;

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

    std::string toString() const;

private:
    class NoRetryPolicy;
    template <ErrorCategory kCategory>
    class RetryPolicyForCategory;

    /**
     * Schedules remote command to be run by the executor.
     * "requestCount" is number of requests scheduled before calling this function.
     * When this function is called for the first time by startup(), "requestCount" will be 0.
     * The executor will invoke _remoteCommandCallback() with the remote command response and
     * ("requestCount" + 1).
     */
    Status _schedule_inlock();

    /**
     * Callback for remote command.
     */
    void _remoteCommandCallback(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba);

    /**
     * Notifies caller that the scheduler has completed processing responses.
     */
    void _onComplete(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba);

    // Not owned by us.
    executor::TaskExecutor* _executor;

    const executor::RemoteCommandRequest _request;
    executor::TaskExecutor::RemoteCommandCallbackFn _callback;
    std::unique_ptr<RetryPolicy> _retryPolicy;
    std::size_t _currentAttempt{0};
    Milliseconds _currentUsedMillis{0};

    // Protects member data of this scheduler declared after mutex.
    mutable stdx::mutex _mutex;

    mutable stdx::condition_variable _condition;

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example,
    // Calling shutdown() when the scheduler has not started will transition from PreStart directly
    // to Complete.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };
    State _state = State::kPreStart;  // (M)

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

    virtual std::string toString() const = 0;
};

class RemoteCommandRetryScheduler::NoRetryPolicy final
    : public RemoteCommandRetryScheduler::RetryPolicy {
public:
    std::size_t getMaximumAttempts() const override {
        return 1U;
    }

    Milliseconds getMaximumResponseElapsedTotal() const override {
        return executor::RemoteCommandRequest::kNoTimeout;
    }

    bool shouldRetryOnError(ErrorCodes::Error error) const override {
        return false;
    }

    std::string toString() const override {
        return R"!({type: "NoRetryPolicy"})!";
    }
};

inline auto RemoteCommandRetryScheduler::makeNoRetryPolicy() -> std::unique_ptr<RetryPolicy> {
    return std::make_unique<NoRetryPolicy>();
}

template <ErrorCategory kCategory>
class RemoteCommandRetryScheduler::RetryPolicyForCategory final
    : public RemoteCommandRetryScheduler::RetryPolicy {
public:
    RetryPolicyForCategory(std::size_t maximumAttempts, Milliseconds maximumResponseElapsedTotal)
        : _maximumAttempts(maximumAttempts),
          _maximumResponseElapsedTotal(maximumResponseElapsedTotal){};

    std::size_t getMaximumAttempts() const override {
        return _maximumAttempts;
    }

    Milliseconds getMaximumResponseElapsedTotal() const override {
        return _maximumResponseElapsedTotal;
    }

    bool shouldRetryOnError(ErrorCodes::Error error) const override {
        return ErrorCodes::isA<kCategory>(error);
    }

    std::string toString() const override {
        using namespace fmt::literals;
        return R"!({{type: "RetryPolicyForCategory",categoryIndex: {}, maxAttempts: {}, maxTimeMS: {}}})!"_format(
            static_cast<std::underlying_type_t<ErrorCategory>>(kCategory),
            _maximumAttempts,
            _maximumResponseElapsedTotal.count());
    }

private:
    std::size_t _maximumAttempts;
    Milliseconds _maximumResponseElapsedTotal;
};

template <ErrorCategory kCategory>
auto RemoteCommandRetryScheduler::makeRetryPolicy(std::size_t maxAttempts,
                                                  Milliseconds maxResponseElapsedTotal)
    -> std::unique_ptr<RetryPolicy> {
    return std::make_unique<RetryPolicyForCategory<kCategory>>(maxAttempts,
                                                               maxResponseElapsedTotal);
}

}  // namespace mongo

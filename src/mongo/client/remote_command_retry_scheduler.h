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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

namespace mongo {

/**
 * Schedules a remote command request. On receiving a response from task executor (or remote
 * server), decides if the response should be forwarded to the "_callback" provided in the
 * constructor based on the retry strategy.
 *
 * If the command is successful or has been canceled (either by calling cancel() or canceled by
 * the task executor on shutdown), the response is forwarded immediately to "_callback" and the
 * scheduler becomes inactive.
 *
 * Otherwise, the retry strategy (specified at construction) is used to decide if we should
 * resubmit the remote command request. The retry strategy is defined by:
 *     - maximum number of times to run the remote command;
 *     - list of error codes, if present in the response, should stop the scheduler.
 */
class RemoteCommandRetryScheduler {
    RemoteCommandRetryScheduler(const RemoteCommandRetryScheduler&) = delete;
    RemoteCommandRetryScheduler& operator=(const RemoteCommandRetryScheduler&) = delete;

public:
    class RetryStrategy;

    /**
     * Creates scheduler but does not schedule any remote command request.
     */
    RemoteCommandRetryScheduler(executor::TaskExecutor* executor,
                                const executor::RemoteCommandRequest& request,
                                const executor::TaskExecutor::RemoteCommandCallbackFn& callback,
                                std::unique_ptr<mongo::RetryStrategy> retryStrategy);

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

    std::string toString() const;

private:
    /**
     * Schedules remote command to be run by the executor.
     * "requestCount" is number of requests scheduled before calling this function.
     * When this function is called for the first time by startup(), "requestCount" will be 0.
     * The executor will invoke _remoteCommandCallback() with the remote command response and
     * ("requestCount" + 1).
     */
    Status _schedule_inlock();

    bool _isActive(WithLock lk) const;

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
    std::unique_ptr<mongo::RetryStrategy> _retryStrategy;

    // Current attempt number (used for debugging/logging only).
    std::size_t _currentAttempt{0};

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

    // Cancellation source for retry calls.
    CancellationSource _cancellationSource;
};

MONGO_MOD_PUBLIC bool isMongosRetriableError(const ErrorCodes::Error& code);

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/baton.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <functional>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {

/**
 * Mock executor::TaskExecutorProxy.
 * Supports injecting errors into scheduleWork(), scheduleWorkAt() and scheduleRemoteCommand().
 */
class TaskExecutorMock : public unittest::TaskExecutorProxy {
public:
    using ShouldFailScheduleWorkRequestFn = std::function<bool()>;
    using ShouldFailScheduleRemoteCommandRequestFn =
        std::function<bool(const executor::RemoteCommandRequest&)>;

    explicit TaskExecutorMock(executor::TaskExecutor* executor);

    StatusWith<CallbackHandle> scheduleWork(CallbackFn&& work) override;
    StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, CallbackFn&& work) override;
    StatusWith<CallbackHandle> scheduleRemoteCommand(const executor::RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb,
                                                     const BatonHandle& baton = nullptr) override;
    StatusWith<CallbackHandle> scheduleExhaustRemoteCommand(
        const executor::RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const BatonHandle& baton = nullptr) override;
    bool hasTasks() override;

    // Override to make scheduleWork() fail during testing.
    ShouldFailScheduleWorkRequestFn shouldFailScheduleWorkRequest = []() {
        return false;
    };

    // Override to make scheduleWorkAt() fail during testing.
    ShouldFailScheduleWorkRequestFn shouldFailScheduleWorkAtRequest = []() {
        return false;
    };

    // If the predicate returns true, scheduleWork() will schedule the task 1 second later instead
    // of running immediately. This allows us to test cancellation handling in callbacks scheduled
    // using scheduleWork().
    ShouldFailScheduleWorkRequestFn shouldDeferScheduleWorkRequestByOneSecond = []() {
        return false;
    };

    // Override to make scheduleRemoteCommand fail during testing.
    ShouldFailScheduleRemoteCommandRequestFn shouldFailScheduleRemoteCommandRequest =
        [](const executor::RemoteCommandRequest&) {
            return false;
        };

    // Override to specify the ErrorCode calls to schedule should fail with when configured to do
    // so.
    ErrorCodes::Error failureCode = ErrorCodes::OperationFailed;
};

}  // namespace repl
}  // namespace mongo

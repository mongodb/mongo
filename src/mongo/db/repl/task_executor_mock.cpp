// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/task_executor_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <utility>

namespace mongo {
namespace repl {

TaskExecutorMock::TaskExecutorMock(executor::TaskExecutor* executor)
    : unittest::TaskExecutorProxy(executor) {}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorMock::scheduleWork(
    CallbackFn&& work) {
    if (shouldFailScheduleWorkRequest()) {
        return Status(failureCode, "failed to schedule work");
    }
    if (shouldDeferScheduleWorkRequestByOneSecond()) {
        auto when = now() + Seconds(1);
        return getExecutor()->scheduleWorkAt(when, std::move(work));
    }
    return getExecutor()->scheduleWork(std::move(work));
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorMock::scheduleWorkAt(
    Date_t when, CallbackFn&& work) {
    if (shouldFailScheduleWorkAtRequest()) {
        return Status(failureCode,
                      str::stream() << "failed to schedule work at " << when.toString());
    }
    return getExecutor()->scheduleWorkAt(when, std::move(work));
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorMock::scheduleRemoteCommand(
    const executor::RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {
    if (shouldFailScheduleRemoteCommandRequest(request)) {
        return Status(failureCode, "failed to schedule remote command");
    }
    return getExecutor()->scheduleRemoteCommand(request, cb, baton);
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorMock::scheduleExhaustRemoteCommand(
    const executor::RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {
    if (shouldFailScheduleRemoteCommandRequest(request)) {
        return Status(failureCode, "failed to schedule remote command");
    }
    return getExecutor()->scheduleExhaustRemoteCommand(request, cb, baton);
}

bool TaskExecutorMock::hasTasks() {
    return getExecutor()->hasTasks();
}

}  // namespace repl
}  // namespace mongo

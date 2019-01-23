
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/task_executor_mock.h"

namespace mongo {
namespace repl {

TaskExecutorMock::TaskExecutorMock(executor::TaskExecutor* executor)
    : unittest::TaskExecutorProxy(executor) {}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorMock::scheduleWork(CallbackFn work) {
    if (shouldFailScheduleWorkRequest()) {
        return Status(ErrorCodes::OperationFailed, "failed to schedule work");
    }
    if (shouldDeferScheduleWorkRequestByOneSecond()) {
        auto when = now() + Seconds(1);
        return getExecutor()->scheduleWorkAt(when, std::move(work));
    }
    return getExecutor()->scheduleWork(std::move(work));
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorMock::scheduleWorkAt(
    Date_t when, CallbackFn work) {
    if (shouldFailScheduleWorkAtRequest()) {
        return Status(ErrorCodes::OperationFailed,
                      str::stream() << "failed to schedule work at " << when.toString());
    }
    return getExecutor()->scheduleWorkAt(when, std::move(work));
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorMock::scheduleRemoteCommand(
    const executor::RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {
    if (shouldFailScheduleRemoteCommandRequest(request)) {
        return Status(ErrorCodes::OperationFailed, "failed to schedule remote command");
    }
    return getExecutor()->scheduleRemoteCommand(request, cb, baton);
}

}  // namespace repl
}  // namespace mongo

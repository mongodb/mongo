/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/unittest/task_executor_proxy.h"

namespace mongo {
namespace repl {

/**
 * Mock executor::TaskExecutorProxy.
 * Supports injecting errors into scheduleWork(), scheduleWorkAt() and scheduleRemoteCommand().
 */
class TaskExecutorMock : public unittest::TaskExecutorProxy {
public:
    using ShouldFailRequestFn = stdx::function<bool(const executor::RemoteCommandRequest&)>;

    TaskExecutorMock(executor::TaskExecutor* executor, ShouldFailRequestFn shouldFailRequest);

    StatusWith<CallbackHandle> scheduleWork(const CallbackFn& work) override;
    StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, const CallbackFn& work) override;
    StatusWith<CallbackHandle> scheduleRemoteCommand(const executor::RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb) override;

    // Override to make scheduleWork() fail during testing.
    bool shouldFailScheduleWork = false;

    // Override to make scheduleWorkAt() fail during testing.
    bool shouldFailScheduleWorkAt = false;

    // Set to true to make scheduleWork() schedule task 1 second later instead of running
    // immediately. This allows us to test cancellation handling in callbacks scheduled
    // using scheduleWork().
    bool shouldDeferScheduleWorkByOneSecond = false;

private:
    // This predicate is set at construction and is used to determine if scheduleRemoteCommand()
    // should return an error during testing.
    ShouldFailRequestFn _shouldFailRequest;
};

}  // namespace repl
}  // namespace mongo

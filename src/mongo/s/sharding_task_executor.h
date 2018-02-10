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

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace executor {

struct ConnectionPoolStats;
class ThreadPoolTaskExecutor;

/**
 * Implementation of a TaskExecutor that uses ThreadPoolTaskExecutor to submit tasks and allows to
 * override methods if needed.
 */
class ShardingTaskExecutor final : public TaskExecutor {
    MONGO_DISALLOW_COPYING(ShardingTaskExecutor);

public:
    ShardingTaskExecutor(std::unique_ptr<ThreadPoolTaskExecutor> executor);

    void startup() override;
    void shutdown() override;
    void join() override;
    void appendDiagnosticBSON(BSONObjBuilder* builder) const override;
    Date_t now() override;
    StatusWith<EventHandle> makeEvent() override;
    void signalEvent(const EventHandle& event) override;
    StatusWith<CallbackHandle> onEvent(const EventHandle& event, const CallbackFn& work) override;
    void waitForEvent(const EventHandle& event) override;
    StatusWith<stdx::cv_status> waitForEvent(OperationContext* opCtx,
                                             const EventHandle& event,
                                             Date_t deadline) override;
    StatusWith<CallbackHandle> scheduleWork(const CallbackFn& work) override;
    StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, const CallbackFn& work) override;
    StatusWith<CallbackHandle> scheduleRemoteCommand(const RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb) override;
    void cancel(const CallbackHandle& cbHandle) override;
    void wait(const CallbackHandle& cbHandle) override;

    void appendConnectionStats(ConnectionPoolStats* stats) const override;

private:
    std::unique_ptr<ThreadPoolTaskExecutor> _executor;
};

}  // namespace executor
}  // namespace mongo

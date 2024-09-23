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

#include <list>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {

struct ConnectionPoolStats;
class ThreadPoolTaskExecutor;

/**
 * Implementation of a TaskExecutor that uses ThreadPoolTaskExecutor to submit tasks and allows to
 * override methods if needed.
 */
class ShardingTaskExecutor final : public TaskExecutor {
    struct Passkey {
        explicit Passkey() = default;
    };

public:
    ShardingTaskExecutor(Passkey, std::shared_ptr<ThreadPoolTaskExecutor> executor);

    static std::shared_ptr<ShardingTaskExecutor> create(
        std::shared_ptr<ThreadPoolTaskExecutor> executor) {
        return std::make_shared<ShardingTaskExecutor>(Passkey{}, std::move(executor));
    }

    ShardingTaskExecutor(const ShardingTaskExecutor&) = delete;
    ShardingTaskExecutor& operator=(const ShardingTaskExecutor&) = delete;


    void startup() override;
    void shutdown() override;
    void join() override;
    SharedSemiFuture<void> joinAsync() override;
    bool isShuttingDown() const override;
    void appendDiagnosticBSON(BSONObjBuilder* builder) const override;
    Date_t now() override;
    StatusWith<EventHandle> makeEvent() override;
    void signalEvent(const EventHandle& event) override;
    StatusWith<CallbackHandle> onEvent(const EventHandle& event, CallbackFn&& work) override;
    void waitForEvent(const EventHandle& event) override;
    StatusWith<stdx::cv_status> waitForEvent(OperationContext* opCtx,
                                             const EventHandle& event,
                                             Date_t deadline) override;
    StatusWith<CallbackHandle> scheduleWork(CallbackFn&& work) override;
    StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, CallbackFn&& work) override;
    StatusWith<CallbackHandle> scheduleRemoteCommand(const RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb,
                                                     const BatonHandle& baton = nullptr) override;
    StatusWith<CallbackHandle> scheduleExhaustRemoteCommand(
        const RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const BatonHandle& baton = nullptr) override;
    bool hasTasks() override;
    void cancel(const CallbackHandle& cbHandle) override;
    void wait(const CallbackHandle& cbHandle,
              Interruptible* interruptible = Interruptible::notInterruptible()) override;

    void appendConnectionStats(ConnectionPoolStats* stats) const override;
    void appendNetworkInterfaceStats(BSONObjBuilder&) const override;

    void dropConnections(const HostAndPort& hostAndPort) override;

private:
    std::shared_ptr<ThreadPoolTaskExecutor> _executor;
};

}  // namespace executor
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <list>
#include <memory>

namespace mongo {
namespace executor {

struct ConnectionPoolStats;
class ThreadPoolTaskExecutor;

/**
 * Implementation of a TaskExecutor that uses ThreadPoolTaskExecutor to submit tasks and allows to
 * override methods if needed.
 */
class [[MONGO_MOD_PUBLIC]] ShardingTaskExecutor final : public TaskExecutor {
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
    void appendNetworkInterfaceStats(BSONObjBuilder&, bool forServerStatus = false) const override;

    void dropConnections(const HostAndPort& target, const Status& status) override;

private:
    std::shared_ptr<ThreadPoolTaskExecutor> _executor;
};

}  // namespace executor
}  // namespace mongo

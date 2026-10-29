// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace unittest {

/**
 * Proxy for the executor::TaskExecutor interface used for testing.
 *
 * Note that the following calls will affect other proxies that share the underlying executor:
 * - startup()
 * - shutdown()
 * - apperndDiagnosticBSON()
 * - appendConnectionStats()
 * - dropConnections()
 */
class [[MONGO_MOD_OPEN]] TaskExecutorProxy : public executor::TaskExecutor {
public:
    /**
     * Does not own target executor.
     */
    explicit TaskExecutorProxy(executor::TaskExecutor* executor);
    ~TaskExecutorProxy() override;

    TaskExecutorProxy(const TaskExecutorProxy&) = delete;
    TaskExecutorProxy& operator=(const TaskExecutorProxy&) = delete;

    executor::TaskExecutor* getExecutor() const;
    void setExecutor(executor::TaskExecutor* executor);

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
    StatusWith<CallbackHandle> scheduleRemoteCommand(const executor::RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb,
                                                     const BatonHandle& baton = nullptr) override;
    StatusWith<CallbackHandle> scheduleExhaustRemoteCommand(
        const executor::RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const BatonHandle& baton = nullptr) override;
    bool hasTasks() override;
    void cancel(const CallbackHandle& cbHandle) override;
    void wait(const CallbackHandle& cbHandle,
              Interruptible* interruptible = Interruptible::notInterruptible()) override;
    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;
    void dropConnections(const HostAndPort& target, const Status& status) override;
    void appendNetworkInterfaceStats(BSONObjBuilder&, bool forServerStatus) const override;

private:
    // Not owned by us.
    Atomic<executor::TaskExecutor*> _executor;
};

}  // namespace unittest
}  // namespace mongo

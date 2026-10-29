// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/task_executor_proxy.h"

#include <utility>

namespace mongo {
namespace unittest {

TaskExecutorProxy::TaskExecutorProxy(executor::TaskExecutor* executor) : _executor(executor) {}

TaskExecutorProxy::~TaskExecutorProxy() = default;

executor::TaskExecutor* TaskExecutorProxy::getExecutor() const {
    return _executor.load();
}

void TaskExecutorProxy::setExecutor(executor::TaskExecutor* executor) {
    _executor.store(executor);
}

void TaskExecutorProxy::startup() {
    _executor.load()->startup();
}

void TaskExecutorProxy::shutdown() {
    _executor.load()->shutdown();
}

void TaskExecutorProxy::join() {
    _executor.load()->join();
}

SharedSemiFuture<void> TaskExecutorProxy::joinAsync() {
    return _executor.load()->joinAsync();
}

bool TaskExecutorProxy::isShuttingDown() const {
    return _executor.load()->isShuttingDown();
}

void TaskExecutorProxy::appendDiagnosticBSON(mongo::BSONObjBuilder* builder) const {
    _executor.load()->appendDiagnosticBSON(builder);
}

Date_t TaskExecutorProxy::now() {
    return _executor.load()->now();
}

StatusWith<executor::TaskExecutor::EventHandle> TaskExecutorProxy::makeEvent() {
    return _executor.load()->makeEvent();
}

void TaskExecutorProxy::signalEvent(const EventHandle& event) {
    _executor.load()->signalEvent(event);
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::onEvent(
    const EventHandle& event, CallbackFn&& work) {
    return _executor.load()->onEvent(event, std::move(work));
}

void TaskExecutorProxy::waitForEvent(const EventHandle& event) {
    _executor.load()->waitForEvent(event);
}

StatusWith<stdx::cv_status> TaskExecutorProxy::waitForEvent(OperationContext* opCtx,
                                                            const EventHandle& event,
                                                            Date_t deadline) {
    return _executor.load()->waitForEvent(opCtx, event, deadline);
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::scheduleWork(
    CallbackFn&& work) {
    return _executor.load()->scheduleWork(std::move(work));
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::scheduleWorkAt(
    Date_t when, CallbackFn&& work) {
    return _executor.load()->scheduleWorkAt(when, std::move(work));
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::scheduleRemoteCommand(
    const executor::RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {
    return _executor.load()->scheduleRemoteCommand(request, cb, baton);
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::scheduleExhaustRemoteCommand(
    const executor::RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {
    return _executor.load()->scheduleExhaustRemoteCommand(request, cb, baton);
}

bool TaskExecutorProxy::hasTasks() {
    return _executor.load()->hasTasks();
}

void TaskExecutorProxy::cancel(const CallbackHandle& cbHandle) {
    _executor.load()->cancel(cbHandle);
}

void TaskExecutorProxy::wait(const CallbackHandle& cbHandle, Interruptible* interruptible) {
    _executor.load()->wait(cbHandle, interruptible);
}

void TaskExecutorProxy::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
    _executor.load()->appendConnectionStats(stats);
}

void TaskExecutorProxy::dropConnections(const HostAndPort& target, const Status& status) {
    _executor.load()->dropConnections(target, status);
}

void TaskExecutorProxy::appendNetworkInterfaceStats(BSONObjBuilder& bob,
                                                    bool forServerStatus) const {
    _executor.load()->appendNetworkInterfaceStats(bob, forServerStatus);
}

}  // namespace unittest
}  // namespace mongo

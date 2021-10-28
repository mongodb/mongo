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

#include "mongo/unittest/task_executor_proxy.h"

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

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::scheduleRemoteCommandOnAny(
    const executor::RemoteCommandRequestOnAny& request,
    const RemoteCommandOnAnyCallbackFn& cb,
    const BatonHandle& baton) {
    return _executor.load()->scheduleRemoteCommandOnAny(request, cb, baton);
}

StatusWith<executor::TaskExecutor::CallbackHandle>
TaskExecutorProxy::scheduleExhaustRemoteCommandOnAny(
    const executor::RemoteCommandRequestOnAny& request,
    const RemoteCommandOnAnyCallbackFn& cb,
    const BatonHandle& baton) {
    return _executor.load()->scheduleExhaustRemoteCommandOnAny(request, cb, baton);
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

void TaskExecutorProxy::appendNetworkInterfaceStats(BSONObjBuilder& bob) const {
    _executor.load()->appendNetworkInterfaceStats(bob);
}

}  // namespace unittest
}  // namespace mongo

/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/unittest/task_executor_proxy.h"

namespace mongo {
namespace unittest {

TaskExecutorProxy::TaskExecutorProxy(executor::TaskExecutor* executor) : _executor(executor) {}

TaskExecutorProxy::~TaskExecutorProxy() = default;

executor::TaskExecutor* TaskExecutorProxy::getExecutor() const {
    return _executor;
}

void TaskExecutorProxy::setExecutor(executor::TaskExecutor* executor) {
    _executor = executor;
}

void TaskExecutorProxy::startup() {
    _executor->startup();
}

void TaskExecutorProxy::shutdown() {
    _executor->shutdown();
}

void TaskExecutorProxy::join() {
    _executor->join();
}

std::string TaskExecutorProxy::getDiagnosticString() {
    return _executor->getDiagnosticString();
}

Date_t TaskExecutorProxy::now() {
    return _executor->now();
}

StatusWith<executor::TaskExecutor::EventHandle> TaskExecutorProxy::makeEvent() {
    return _executor->makeEvent();
}

void TaskExecutorProxy::signalEvent(const EventHandle& event) {
    _executor->signalEvent(event);
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::onEvent(
    const EventHandle& event, const CallbackFn& work) {
    return _executor->onEvent(event, work);
}

void TaskExecutorProxy::waitForEvent(const EventHandle& event) {
    _executor->waitForEvent(event);
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::scheduleWork(
    const CallbackFn& work) {
    return _executor->scheduleWork(work);
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::scheduleWorkAt(
    Date_t when, const CallbackFn& work) {
    return _executor->scheduleWorkAt(when, work);
}

StatusWith<executor::TaskExecutor::CallbackHandle> TaskExecutorProxy::scheduleRemoteCommand(
    const executor::RemoteCommandRequest& request, const RemoteCommandCallbackFn& cb) {
    return _executor->scheduleRemoteCommand(request, cb);
}

void TaskExecutorProxy::cancel(const CallbackHandle& cbHandle) {
    _executor->cancel(cbHandle);
}

void TaskExecutorProxy::wait(const CallbackHandle& cbHandle) {
    _executor->wait(cbHandle);
}

void TaskExecutorProxy::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
    _executor->appendConnectionStats(stats);
}

}  // namespace unittest
}  // namespace mongo

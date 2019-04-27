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

#include "mongo/executor/task_executor.h"

namespace mongo {
namespace executor {

TaskExecutor::TaskExecutor() = default;
TaskExecutor::~TaskExecutor() = default;

void TaskExecutor::schedule(OutOfLineExecutor::Task func) {
    auto cb = CallbackFn([func = std::move(func)](const CallbackArgs& args) { func(args.status); });
    auto statusWithCallback = scheduleWork(std::move(cb));
    if (!statusWithCallback.isOK()) {
        // The callback was not scheduled or moved from, it is still valid. Run it inline to inform
        // it of the error. Construct a CallbackArgs for it, only CallbackArgs::status matters here.
        CallbackArgs args(this, {}, statusWithCallback.getStatus(), nullptr);
        cb(args);
    }
}

TaskExecutor::CallbackState::CallbackState() = default;
TaskExecutor::CallbackState::~CallbackState() = default;

TaskExecutor::CallbackHandle::CallbackHandle() = default;
TaskExecutor::CallbackHandle::CallbackHandle(std::shared_ptr<CallbackState> callback)
    : _callback(std::move(callback)) {}

TaskExecutor::EventState::EventState() = default;
TaskExecutor::EventState::~EventState() = default;

TaskExecutor::EventHandle::EventHandle() = default;
TaskExecutor::EventHandle::EventHandle(std::shared_ptr<EventState> event)
    : _event(std::move(event)) {}

TaskExecutor::CallbackArgs::CallbackArgs(TaskExecutor* theExecutor,
                                         CallbackHandle theHandle,
                                         Status theStatus,
                                         OperationContext* theTxn)
    : executor(theExecutor),
      myHandle(std::move(theHandle)),
      status(std::move(theStatus)),
      opCtx(theTxn) {}


TaskExecutor::RemoteCommandCallbackArgs::RemoteCommandCallbackArgs(
    TaskExecutor* theExecutor,
    const CallbackHandle& theHandle,
    const RemoteCommandRequest& theRequest,
    const ResponseStatus& theResponse)
    : executor(theExecutor), myHandle(theHandle), request(theRequest), response(theResponse) {}

TaskExecutor::CallbackState* TaskExecutor::getCallbackFromHandle(const CallbackHandle& cbHandle) {
    return cbHandle.getCallback();
}

TaskExecutor::EventState* TaskExecutor::getEventFromHandle(const EventHandle& eventHandle) {
    return eventHandle.getEvent();
}

void TaskExecutor::setEventForHandle(EventHandle* eventHandle, std::shared_ptr<EventState> event) {
    eventHandle->setEvent(std::move(event));
}

void TaskExecutor::setCallbackForHandle(CallbackHandle* cbHandle,
                                        std::shared_ptr<CallbackState> callback) {
    cbHandle->setCallback(std::move(callback));
}

}  // namespace executor
}  // namespace mongo

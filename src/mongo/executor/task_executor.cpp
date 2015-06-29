/**
 *    Copyright (C) 2014-2015 MongoDB Inc.
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

#include "mongo/executor/task_executor.h"

namespace mongo {
namespace executor {

TaskExecutor::TaskExecutor() = default;
TaskExecutor::~TaskExecutor() = default;

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
      txn(theTxn) {}


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

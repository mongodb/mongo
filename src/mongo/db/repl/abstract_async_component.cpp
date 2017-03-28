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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/abstract_async_component.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

AbstractAsyncComponent::AbstractAsyncComponent(executor::TaskExecutor* executor,
                                               const std::string& componentName)
    : _executor(executor), _componentName(componentName) {
    uassert(ErrorCodes::BadValue, "task executor cannot be null", executor);
}

executor::TaskExecutor* AbstractAsyncComponent::_getExecutor() {
    return _executor;
}

bool AbstractAsyncComponent::isActive() noexcept {
    stdx::lock_guard<stdx::mutex> lock(*_getMutex());
    return _isActive_inlock();
}

bool AbstractAsyncComponent::_isActive_inlock() noexcept {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

bool AbstractAsyncComponent::_isShuttingDown() noexcept {
    stdx::lock_guard<stdx::mutex> lock(*_getMutex());
    return _isShuttingDown_inlock();
}

bool AbstractAsyncComponent::_isShuttingDown_inlock() noexcept {
    return State::kShuttingDown == _state;
}

Status AbstractAsyncComponent::startup() noexcept {
    stdx::lock_guard<stdx::mutex> lock(*_getMutex());
    switch (_state) {
        case State::kPreStart:
            _state = State::kRunning;
            break;
        case State::kRunning:
            return Status(ErrorCodes::IllegalOperation,
                          str::stream() << _componentName << " already started");
        case State::kShuttingDown:
            return Status(ErrorCodes::ShutdownInProgress,
                          str::stream() << _componentName << " shutting down");
        case State::kComplete:
            return Status(ErrorCodes::ShutdownInProgress,
                          str::stream() << _componentName << " completed");
    }

    auto status = _doStartup_inlock();

    if (!status.isOK()) {
        _state = State::kComplete;
        return status;
    }

    return Status::OK();
}

void AbstractAsyncComponent::shutdown() noexcept {
    stdx::lock_guard<stdx::mutex> lock(*_getMutex());
    switch (_state) {
        case State::kPreStart:
            // Transition directly from PreStart to Complete if not started yet.
            _state = State::kComplete;
            return;
        case State::kRunning:
            _state = State::kShuttingDown;
            break;
        case State::kShuttingDown:
        case State::kComplete:
            // Nothing to do if we are already in ShuttingDown or Complete state.
            return;
    }

    _doShutdown_inlock();
}

void AbstractAsyncComponent::join() noexcept {
    stdx::unique_lock<stdx::mutex> lk(*_getMutex());
    _stateCondition.wait(lk, [this]() { return !_isActive_inlock(); });
}

AbstractAsyncComponent::State AbstractAsyncComponent::getState_forTest() noexcept {
    stdx::lock_guard<stdx::mutex> lock(*_getMutex());
    return _state;
}

void AbstractAsyncComponent::_transitionToComplete() noexcept {
    stdx::lock_guard<stdx::mutex> lock(*_getMutex());
    _transitionToComplete_inlock();
}

void AbstractAsyncComponent::_transitionToComplete_inlock() noexcept {
    invariant(State::kComplete != _state);
    _state = State::kComplete;
    _stateCondition.notify_all();
}

Status AbstractAsyncComponent::_checkForShutdownAndConvertStatus_inlock(
    const executor::TaskExecutor::CallbackArgs& callbackArgs, const std::string& message) {
    return _checkForShutdownAndConvertStatus_inlock(callbackArgs.status, message);
}

Status AbstractAsyncComponent::_checkForShutdownAndConvertStatus_inlock(
    const Status& status, const std::string& message) {

    if (_isShuttingDown_inlock()) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << message << ": " << _componentName << " is shutting down");
    }

    if (!status.isOK()) {
        return Status(status.code(), message + ": " + status.reason());
    }

    return Status::OK();
}

Status AbstractAsyncComponent::_scheduleWorkAndSaveHandle_inlock(
    const executor::TaskExecutor::CallbackFn& work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    invariant(handle);
    if (_isShuttingDown_inlock()) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << "failed to schedule work " << name << ": " << _componentName
                                    << " is shutting down");
    }
    auto result = _executor->scheduleWork(work);
    if (!result.isOK()) {
        return Status(result.getStatus().code(),
                      str::stream() << "failed to schedule work " << name << ": "
                                    << result.getStatus().reason());
    }
    *handle = result.getValue();
    return Status::OK();
}

Status AbstractAsyncComponent::_scheduleWorkAtAndSaveHandle_inlock(
    Date_t when,
    const executor::TaskExecutor::CallbackFn& work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    invariant(handle);
    if (_isShuttingDown_inlock()) {
        return Status(
            ErrorCodes::CallbackCanceled,
            str::stream() << "failed to schedule work " << name << " at " << when.toString() << ": "
                          << _componentName
                          << " is shutting down");
    }
    auto result = _executor->scheduleWorkAt(when, work);
    if (!result.isOK()) {
        return Status(
            result.getStatus().code(),
            str::stream() << "failed to schedule work " << name << " at " << when.toString() << ": "
                          << result.getStatus().reason());
    }
    *handle = result.getValue();
    return Status::OK();
}

void AbstractAsyncComponent::_cancelHandle_inlock(executor::TaskExecutor::CallbackHandle handle) {
    if (!handle) {
        return;
    }
    _executor->cancel(handle);
}

std::ostream& operator<<(std::ostream& os, const AbstractAsyncComponent::State& state) {
    switch (state) {
        case AbstractAsyncComponent::State::kPreStart:
            return os << "PreStart";
        case AbstractAsyncComponent::State::kRunning:
            return os << "Running";
        case AbstractAsyncComponent::State::kShuttingDown:
            return os << "ShuttingDown";
        case AbstractAsyncComponent::State::kComplete:
            return os << "Complete";
    }
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo

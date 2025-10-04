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

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/db/repl/abstract_async_component.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <ostream>
#include <utility>

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
    return _isActive(lock);
}

bool AbstractAsyncComponent::_isActive(WithLock lk) noexcept {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

bool AbstractAsyncComponent::_isShuttingDown() noexcept {
    stdx::lock_guard<stdx::mutex> lock(*_getMutex());
    return _isShuttingDown(lock);
}

bool AbstractAsyncComponent::_isShuttingDown(WithLock lk) noexcept {
    return State::kShuttingDown == _state;
}

Status AbstractAsyncComponent::startup() {
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

    try {
        _doStartup(lock);
    } catch (const DBException& ex) {
        _state = State::kComplete;
        return ex.toStatus();
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

    _doShutdown(lock);
}

void AbstractAsyncComponent::join() noexcept {
    _preJoin();
    stdx::unique_lock<stdx::mutex> lk(*_getMutex());
    _stateCondition.wait(lk, [&]() { return !_isActive(lk); });
}

AbstractAsyncComponent::State AbstractAsyncComponent::getState_forTest() noexcept {
    stdx::lock_guard<stdx::mutex> lock(*_getMutex());
    return _state;
}

void AbstractAsyncComponent::_transitionToComplete() noexcept {
    stdx::lock_guard<stdx::mutex> lock(*_getMutex());
    _transitionToComplete(lock);
}

void AbstractAsyncComponent::_transitionToComplete(WithLock lk) noexcept {
    invariant(State::kComplete != _state);
    _state = State::kComplete;
    _stateCondition.notify_all();
}

Status AbstractAsyncComponent::_checkForShutdownAndConvertStatus(
    const executor::TaskExecutor::CallbackArgs& callbackArgs, const std::string& message) {
    stdx::unique_lock<stdx::mutex> lk(*_getMutex());
    return _checkForShutdownAndConvertStatus(lk, callbackArgs, message);
}

Status AbstractAsyncComponent::_checkForShutdownAndConvertStatus(const Status& status,
                                                                 const std::string& message) {
    stdx::unique_lock<stdx::mutex> lk(*_getMutex());
    return _checkForShutdownAndConvertStatus(lk, status, message);
}

Status AbstractAsyncComponent::_checkForShutdownAndConvertStatus(
    WithLock lk,
    const executor::TaskExecutor::CallbackArgs& callbackArgs,
    const std::string& message) {
    return _checkForShutdownAndConvertStatus(lk, callbackArgs.status, message);
}

Status AbstractAsyncComponent::_checkForShutdownAndConvertStatus(WithLock lk,
                                                                 const Status& status,
                                                                 const std::string& message) {

    if (_isShuttingDown(lk)) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << message << ": " << _componentName << " is shutting down");
    }

    return status.withContext(message);
}

Status AbstractAsyncComponent::_scheduleWorkAndSaveHandle(
    WithLock lk,
    executor::TaskExecutor::CallbackFn work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    invariant(handle);
    if (_isShuttingDown(lk)) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << "failed to schedule work " << name << ": " << _componentName
                                    << " is shutting down");
    }
    auto result = _executor->scheduleWork(std::move(work));
    if (!result.isOK()) {
        return result.getStatus().withContext(str::stream() << "failed to schedule work " << name);
    }
    *handle = result.getValue();
    return Status::OK();
}

Status AbstractAsyncComponent::_scheduleWorkAtAndSaveHandle(
    WithLock lk,
    Date_t when,
    executor::TaskExecutor::CallbackFn work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    invariant(handle);
    if (_isShuttingDown(lk)) {
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream()
                          << "failed to schedule work " << name << " at " << when.toString() << ": "
                          << _componentName << " is shutting down");
    }
    auto result = _executor->scheduleWorkAt(when, std::move(work));
    if (!result.isOK()) {
        return result.getStatus().withContext(str::stream() << "failed to schedule work " << name
                                                            << " at " << when.toString());
    }
    *handle = result.getValue();
    return Status::OK();
}

void AbstractAsyncComponent::_cancelHandle(WithLock lk,
                                           executor::TaskExecutor::CallbackHandle handle) {
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

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

#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <iosfwd>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>

namespace mongo {
namespace repl {

/**
 * This class represents an abstract base class for replication components that run asynchronously
 * using the executor::TaskExecutor framework. It defines the startup/shutdown semantics with the
 * added guarantee that components can be run at most once.
 *
 * The _state variable in this class is protected by the concrete class's mutex (returned by
 * _getMutex()).
 */
template <typename Mutex = stdx::mutex>
class MONGO_MOD_OPEN AbstractAsyncComponent {
    AbstractAsyncComponent(const AbstractAsyncComponent&) = delete;
    AbstractAsyncComponent& operator=(const AbstractAsyncComponent&) = delete;

public:
    AbstractAsyncComponent(executor::TaskExecutor* executor, const std::string& componentName)
        : _executor(executor), _componentName(componentName) {
        uassert(ErrorCodes::BadValue, "task executor cannot be null", executor);
    }

    virtual ~AbstractAsyncComponent() = default;

    /**
     * Returns true if this component is currently running or in the process of shutting down.
     */
    bool isActive() noexcept {
        stdx::lock_guard<Mutex> lock(*_getMutex());
        return _isActive(lock);
    }

    /**
     * Starts the component. If the transition from PreStart to Running is allowed, this invokes
     * _doStartup() defined in the concrete class. If _doStartup() fails, this
     * component will transition to Complete and any restarts after this will be disallowed.
     */
    Status startup() {
        stdx::lock_guard<Mutex> lock(*_getMutex());
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

    /**
     * Signals this component to begin shutting down. If the transition from Running to ShuttingDown
     * is allowed, this invokes _doShutdown() defined in the concrete class.
     * Transition directly from PreStart to Complete if not started yet.
     */
    void shutdown() noexcept {
        stdx::lock_guard<Mutex> lock(*_getMutex());
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

    /**
     * Blocks until inactive.
     */
    void join() noexcept {
        _preJoin();
        stdx::unique_lock<Mutex> lk(*_getMutex());
        _stateCondition.wait(lk, [&]() { return !_isActive(lk); });
    }

    /**
     * State transitions:
     * PreStart --> Running --> ShuttingDown --> Complete
     * It is possible to skip intermediate states. For example, calling shutdown() when the
     * component has not started will transition from PreStart directly to Complete.
     */
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };

    /**
     * Returns current component state.
     * For testing only.
     */
    State getState_forTest() noexcept {
        stdx::lock_guard<Mutex> lock(*_getMutex());
        return _state;
    }

protected:
    /**
     * Returns task executor.
     */
    executor::TaskExecutor* _getExecutor() {
        return _executor;
    }

    /**
     * Returns true if this component is currently running or in the process of shutting down.
     */
    bool _isActive(WithLock lk) noexcept {
        return State::kRunning == _state || State::kShuttingDown == _state;
    }

    /**
     * Returns true if this component has received a shutdown request ('_state' is ShuttingDown).
     */
    bool _isShuttingDown() noexcept {
        stdx::lock_guard<Mutex> lock(*_getMutex());
        return _isShuttingDown(lock);
    }

    bool _isShuttingDown(WithLock lock) noexcept {
        return State::kShuttingDown == _state;
    }

    /**
     * Transitions this component to complete and notifies any waiters on '_stateCondition'.
     * May be called at most once.
     */
    void _transitionToComplete() noexcept {
        stdx::lock_guard<Mutex> lock(*_getMutex());
        _transitionToComplete(lock);
    }

    void _transitionToComplete(WithLock lk) noexcept {
        invariant(State::kComplete != _state);
        _state = State::kComplete;
        _stateCondition.notify_all();
    }

    /**
     * Checks the given status (or embedded status inside the callback args) and current component
     * shutdown state. If the given status is not OK or if we are shutting down, returns a new error
     * status that should be passed to _finishCallback. The reason in the new error status will
     * include 'message'.
     * Otherwise, returns Status::OK().
     */
    Status _checkForShutdownAndConvertStatus(
        WithLock lk,
        const executor::TaskExecutor::CallbackArgs& callbackArgs,
        const std::string& message) {
        return _checkForShutdownAndConvertStatus(lk, callbackArgs.status, message);
    }

    Status _checkForShutdownAndConvertStatus(WithLock lk,
                                             const Status& status,
                                             const std::string& message) {

        if (_isShuttingDown(lk)) {
            return Status(ErrorCodes::CallbackCanceled,
                          str::stream()
                              << message << ": " << _componentName << " is shutting down");
        }

        return status.withContext(message);
    }

    Status _checkForShutdownAndConvertStatus(
        const executor::TaskExecutor::CallbackArgs& callbackArgs, const std::string& message) {
        stdx::unique_lock<Mutex> lk(*_getMutex());
        return _checkForShutdownAndConvertStatus(lk, callbackArgs, message);
    }

    Status _checkForShutdownAndConvertStatus(const Status& status, const std::string& message) {
        stdx::unique_lock<Mutex> lk(*_getMutex());
        return _checkForShutdownAndConvertStatus(lk, status, message);
    }

    /**
     * Schedules work to be run by the task executor.
     * Saves handle if work was successfully scheduled.
     * Returns scheduleWork status (without the handle).
     */
    Status _scheduleWorkAndSaveHandle(WithLock lk,
                                      executor::TaskExecutor::CallbackFn work,
                                      executor::TaskExecutor::CallbackHandle* handle,
                                      const std::string& name) {
        invariant(handle);
        if (_isShuttingDown(lk)) {
            return Status(ErrorCodes::CallbackCanceled,
                          str::stream() << "failed to schedule work " << name << ": "
                                        << _componentName << " is shutting down");
        }
        auto result = _executor->scheduleWork(std::move(work));
        if (!result.isOK()) {
            return result.getStatus().withContext(str::stream()
                                                  << "failed to schedule work " << name);
        }
        *handle = result.getValue();
        return Status::OK();
    }

    Status _scheduleWorkAtAndSaveHandle(WithLock lk,
                                        Date_t when,
                                        executor::TaskExecutor::CallbackFn work,
                                        executor::TaskExecutor::CallbackHandle* handle,
                                        const std::string& name) {
        invariant(handle);
        if (_isShuttingDown(lk)) {
            return Status(ErrorCodes::CallbackCanceled,
                          str::stream()
                              << "failed to schedule work " << name << " at " << when.toString()
                              << ": " << _componentName << " is shutting down");
        }
        auto result = _executor->scheduleWorkAt(when, std::move(work));
        if (!result.isOK()) {
            return result.getStatus().withContext(
                str::stream() << "failed to schedule work " << name << " at " << when.toString());
        }
        *handle = result.getValue();
        return Status::OK();
    }

    /**
     * Cancels task executor callback handle if not null.
     */
    void _cancelHandle(WithLock lk, executor::TaskExecutor::CallbackHandle handle) {
        if (!handle) {
            return;
        }
        _executor->cancel(handle);
    }

    /**
     * Starts up a component, owned by us, and checks our shutdown state at the same time. If the
     * component's startup() fails, resets the unique_ptr holding 'component' and return the error
     * from startup().
     */
    template <typename T>
    Status _startupComponent(WithLock lk, std::unique_ptr<T>& component);
    template <typename T>
    Status _startupComponent(std::unique_ptr<T>& component);

    /**
     * Shuts down a component, owned by us, if not null.
     */
    template <typename T>
    void _shutdownComponent(WithLock lk, const std::unique_ptr<T>& component);
    template <typename T>
    void _shutdownComponent(const std::unique_ptr<T>& component);

private:
    /**
     * Invoked by startup() to run startup procedure after a successful transition from PreStart to
     * Running.
     * Invoked at most once by AbstractAsyncComponent.
     *
     * If _doStartup() fails, startup() will transition this component from Running to
     * Complete. Subsequent startup() attempts will return an IllegalOperation error.
     *
     * If _doStartup() succeeds, the component stays in Running (or ShuttingDown if
     * shutdown() is called) until the component has finished its processing (transtion to
     * Complete).
     *
     * It is the responsibility of the implementation to transition the component state to Complete
     * by calling _transitionToComplete() once the component has finished its processing.
     */
    virtual void _doStartup(WithLock) = 0;

    /**
     * Runs shutdown procedure after a successful transition from Running to ShuttingDown.
     * Invoked at most once by AbstractAsyncComponent.
     * May not throw exceptions.
     */
    virtual void _doShutdown(WithLock) noexcept = 0;

    /**
     * Function invoked before join() without holding the component's mutex.
     */
    virtual void _preJoin() noexcept = 0;

    /**
     * Returns mutex to guard this component's state variable.
     */
    virtual Mutex* _getMutex() noexcept = 0;

private:
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (M)  Reads and writes guarded by mutex returned by _getMutex().

    // Task executor used to schedule tasks and remote commands.
    executor::TaskExecutor* const _executor;  // (R)

    // Component name used in error messages generated by startup().
    const std::string _componentName;  // (R)

    // Current component state. See comments for State enum class for details.
    // Protected by mutex in concrete class returned in _getMutex().
    State _state = State::kPreStart;  // (M)

    // Used by _transitionToComplete() to signal changes in '_state'.
    mutable stdx::condition_variable _stateCondition;  // (S)
};

/**
 * Insertion operator for AbstractAsyncComponent::State. Formats state for output stream.
 * For testing only.
 */
template <typename Mutex>
std::ostream& operator<<(std::ostream& os,
                         const typename AbstractAsyncComponent<Mutex>::State& state) {
    using State = typename AbstractAsyncComponent<Mutex>::State;
    switch (state) {
        case State::kPreStart:
            return os << "PreStart";
        case State::kRunning:
            return os << "Running";
        case State::kShuttingDown:
            return os << "ShuttingDown";
        case State::kComplete:
            return os << "Complete";
    }
    MONGO_UNREACHABLE;
}

template <typename Mutex>
template <typename T>
Status AbstractAsyncComponent<Mutex>::_startupComponent(WithLock lk,
                                                        std::unique_ptr<T>& component) {
    MONGO_STATIC_ASSERT(std::is_base_of<AbstractAsyncComponent<Mutex>, T>::value ||
                        std::is_base_of<AbstractAsyncComponent<stdx::mutex>, T>::value);

    if (_isShuttingDown(lk)) {
        // Save name of 'component' before resetting unique_ptr.
        auto componentToStartUp = component->_componentName;
        component.reset();
        return Status(ErrorCodes::CallbackCanceled,
                      str::stream() << "failed to start up " << componentToStartUp << ": "
                                    << _componentName << " is shutting down");
    }

    auto status = component->startup();
    if (!status.isOK()) {
        component.reset();
    }
    return status;
}

template <typename Mutex>
template <typename T>
Status AbstractAsyncComponent<Mutex>::_startupComponent(std::unique_ptr<T>& component) {
    stdx::lock_guard<Mutex> lock(*_getMutex());
    return _startupComponent(lock, component);
}

template <typename Mutex>
template <typename T>
void AbstractAsyncComponent<Mutex>::_shutdownComponent(WithLock lk,
                                                       const std::unique_ptr<T>& component) {
    MONGO_STATIC_ASSERT(std::is_base_of<AbstractAsyncComponent<Mutex>, T>::value ||
                        std::is_base_of<AbstractAsyncComponent<stdx::mutex>, T>::value);

    if (!component) {
        return;
    }
    component->shutdown();
}

template <typename Mutex>
template <typename T>
void AbstractAsyncComponent<Mutex>::_shutdownComponent(const std::unique_ptr<T>& component) {
    stdx::lock_guard<Mutex> lock(*_getMutex());
    _shutdownComponent(lock, component);
}

}  // namespace repl
}  // namespace mongo

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
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/baton.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {

class BSONObjBuilder;
class OperationContext;

namespace executor {

struct ConnectionPoolStats;

/**
 * Executor with notions of events and callbacks.
 *
 * Callbacks represent work to be performed by the executor.
 * They may be scheduled by client threads or by other callbacks.  Methods that
 * schedule callbacks return a CallbackHandle if they are able to enqueue the callback in the
 * appropriate work queue.  Every CallbackHandle represents an invocation of a function that
 * will happen before the executor goes out of scope.  Calling cancel(CallbackHandle) schedules
 * the specified callback to run with a flag indicating that it is "canceled," but it will run.
 * Client threads may block waiting for a callback to execute by calling wait(CallbackHandle).
 *
 * Events are level-triggered and may only be signaled one time.  Client threads and callbacks
 * may schedule callbacks to be run by the executor after the event is signaled, and client
 * threads may ask the executor to block them until after the event is signaled.
 *
 * If an event is unsignaled when shutdown is called, the executor will ensure that any threads
 * blocked in waitForEvent() eventually return.
 */
class TaskExecutor : public OutOfLineExecutor, public std::enable_shared_from_this<TaskExecutor> {
    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

public:
    /**
     * Error status that should be used by implementations of TaskExecutor when
     * a callback is canceled.
     */
    static const inline Status kCallbackCanceledErrorStatus{ErrorCodes::CallbackCanceled,
                                                            "Callback canceled"};

    struct CallbackArgs;
    struct RemoteCommandCallbackArgs;
    class CallbackState;
    class CallbackHandle;
    class EventState;
    class EventHandle;

    using ResponseStatus = RemoteCommandResponse;

    /**
     * Type of a regular callback function.
     *
     * The status argument passed at invocation will have code ErrorCodes::CallbackCanceled if
     * the callback was canceled for any reason (including shutdown).  Otherwise, it should have
     * Status::OK().
     */
    using CallbackFn = unique_function<void(const CallbackArgs&)>;

    /**
     * Type of a callback from a request to run a command on a remote MongoDB node.
     *
     * The StatusWith<const BSONObj> will have ErrorCodes::CallbackCanceled if the callback was
     * canceled.  Otherwise, its status will represent any failure to execute the command.
     * If the command executed and a response came back, then the status object will contain
     * the BSONObj returned by the command, with the "ok" field indicating the success of the
     * command in the usual way.
     */
    using RemoteCommandCallbackFn = std::function<void(const RemoteCommandCallbackArgs&)>;

    /**
     * Destroys the task executor. Implicitly performs the equivalent of shutdown() and join()
     * before returning, if necessary.
     */
    ~TaskExecutor() override;

    /**
     * Causes the executor to initialize its internal state (start threads if appropriate, create
     * network sockets, etc). This method may be called at most once for the lifetime of an
     * executor.
     */
    virtual void startup() = 0;

    /**
     * Signals to the executor that it should shut down. This method may be called from within a
     * callback.  As such, this method must not block. After shutdown returns, attempts to schedule
     * more tasks on the executor will return errors.
     *
     * It is legal to call this method multiple times. If the task executor goes out of scope
     * before this method is called, the destructor performs this activity.
     */
    virtual void shutdown() = 0;

    /**
     * Waits for the shutdown sequence initiated by a call to shutdown() to complete. Must not be
     * called from within a callback.
     *
     * Unlike stdx::thread::join, this method may be called from any thread that wishes to wait for
     * shutdown to complete.
     */
    virtual void join() = 0;

    /**
     * Returns a future that becomes ready when shutdown() has been called and all outstanding
     * callbacks have finished running.
     */
    virtual SharedSemiFuture<void> joinAsync() = 0;

    /**
     * Returns true if the executor is no longer active (i.e, no longer new tasks can be scheduled).
     */
    virtual bool isShuttingDown() const = 0;

    /**
     * Writes diagnostic information into "b".
     */
    virtual void appendDiagnosticBSON(BSONObjBuilder* b) const = 0;

    /**
     * Gets the current time.  Callbacks should use this method to read the system clock.
     */
    virtual Date_t now() = 0;

    /**
     * Creates a new event.  Returns a handle to the event, or ErrorCodes::ShutdownInProgress if
     * makeEvent() fails because the executor is shutting down.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    virtual StatusWith<EventHandle> makeEvent() = 0;

    /**
     * Signals the event, making waiting client threads and callbacks runnable.
     *
     * May be called up to one time per event.
     *
     * Any unsignaled event will be signaled during shutdown, and subsequent attempts to signal the
     * event will be ignored.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    virtual void signalEvent(const EventHandle& event) = 0;

    /**
     * Schedules a callback, "work", to run after "event" is signaled.  If "event"
     * has already been signaled, marks "work" as immediately runnable.
     *
     * On success, returns a handle for waiting on or canceling the callback. The provided "work"
     * argument is moved from and invalid for use in the caller. On error, returns
     * ErrorCodes::ShutdownInProgress, and "work" is still valid. If you intend to call "work" after
     * error, make sure it is an actual CallbackFn, not a lambda or other value that implicitly
     * converts to CallbackFn, since such a value would be moved from and invalidated during
     * conversion with no way to recover it.
     *
     * If "event" has yet to be signaled when "shutdown()" is called, "work" will
     * be scheduled with a status of ErrorCodes::CallbackCanceled.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    virtual StatusWith<CallbackHandle> onEvent(const EventHandle& event, CallbackFn&& work) = 0;

    /**
     * Blocks the calling thread until "event" is signaled. Also returns if the event is never
     * signaled but shutdown() is called on the executor.
     *
     * TODO(schwerin): Return ErrorCodes::ShutdownInProgress when shutdown() has been called so that
     * the caller can know which of the two reasons led to this method returning.
     *
     * NOTE: Do not call from a callback running in the executor.
     */
    virtual void waitForEvent(const EventHandle& event) = 0;

    /**
     * Same as waitForEvent without an OperationContext, but if the OperationContext gets
     * interrupted, will return the kill code, or, if the deadline passes, will return
     * Status::OK with cv_status::timeout.
     */
    virtual StatusWith<stdx::cv_status> waitForEvent(OperationContext* opCtx,
                                                     const EventHandle& event,
                                                     Date_t deadline = Date_t::max()) = 0;


    /**
     * Schedules the given Task to run in this executor.
     * Note that 'func' is implicitly noexcept and should not ever leak exceptions.
     */
    void schedule(OutOfLineExecutor::Task func) final;

    /**
     * Schedules "work" to be run by the executor ASAP.
     *
     * On success, returns a handle for waiting on or canceling the callback. The provided "work"
     * argument is moved from and invalid for use in the caller. On error, returns
     * ErrorCodes::ShutdownInProgress, and "work" is still valid. If you intend to call "work" after
     * error, make sure it is an actual CallbackFn, not a lambda or other value that implicitly
     * converts to CallbackFn, since such a value would be moved from and invalidated during
     * conversion with no way to recover it.
     *
     * "work" should be considered implicitly 'noexcept' and thus should not throw any exceptions.
     *
     * May be called by client threads or callbacks running in the executor.
     *
     * Contract: Implementations should guarantee that callback should be called *after* doing any
     * processing related to the callback.
     */
    virtual StatusWith<CallbackHandle> scheduleWork(CallbackFn&& work) = 0;

    /**
     * Schedules "work" to be run by the executor no sooner than "when".
     *
     * If "when" is <= now(), then it schedules the "work" to be run ASAP.
     *
     * On success, returns a handle for waiting on or canceling the callback. The provided "work"
     * argument is moved from and invalid for use in the caller. On error, returns
     * ErrorCodes::ShutdownInProgress, and "work" is still valid. If you intend to call "work" after
     * error, make sure it is an actual CallbackFn, not a lambda or other value that implicitly
     * converts to CallbackFn, since such a value would be moved from and invalidated during
     * conversion with no way to recover it.
     *
     * "work" should be considered implicitly 'noexcept' and thus should not throw any exceptions.
     *
     * May be called by client threads or callbacks running in the executor.
     *
     * Contract: Implementations should guarantee that callback should be called *after* doing any
     * processing related to the callback.
     */
    virtual StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, CallbackFn&& work) = 0;

    /**
     * Returns an ExecutorFuture that will be resolved with success when the given date is reached.
     *
     * If the executor is already shut down when this is called, the resulting future will be set
     * with ErrorCodes::ShutdownInProgress.
     *
     * Otherwise, if the executor shuts down or the token is canceled prior to the deadline being
     * reached, the resulting ExecutorFuture will be set with ErrorCodes::CallbackCanceled.
     */
    ExecutorFuture<void> sleepUntil(Date_t when, const CancellationToken& token);

    /**
     * Returns an ExecutorFuture that will be resolved with success after the given duration has
     * passed.
     *
     * If the executor is already shut down when this is called, the resulting future will be set
     * with ErrorCodes::ShutdownInProgress.
     *
     * Otherwise, if the executor shuts down or the token is canceled prior to the deadline being
     * reached, the resulting ExecutorFuture will be set with ErrorCodes::CallbackCanceled.
     */
    ExecutorFuture<void> sleepFor(Milliseconds duration, const CancellationToken& token) {
        return sleepUntil(now() + duration, token);
    }

    /**
     * Schedules "cb" to be run by the executor with the result of executing the remote command
     * described by "request".
     *
     * Returns a handle for waiting on or canceling the callback, or
     * ErrorCodes::ShutdownInProgress.
     *
     * May be called by client threads or callbacks running in the executor.
     *
     * Contract: Implementations should guarantee that callback should be called *after* doing any
     * processing related to the callback.
     */
    virtual StatusWith<CallbackHandle> scheduleRemoteCommand(
        const RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const BatonHandle& baton = nullptr) = 0;

    /**
     * Schedules the given request to be sent and returns a future containing the response. The
     * resulting future will be set with an error only if there is a failure to send the request.
     * Errors from the remote node will be contained in the ResponseStatus object.
     *
     * The input CancellationToken may be used to cancel sending the request. There is no guarantee
     * that this will succeed in canceling the request and the resulting ExecutorFuture may contain
     * either success or error. If cancellation is successful, the resulting ExecutorFuture will be
     * set with an error.
     */
    ExecutorFuture<TaskExecutor::ResponseStatus> scheduleRemoteCommand(
        const RemoteCommandRequest& request,
        const CancellationToken& token,
        const BatonHandle& baton = nullptr);

    /**
     * Schedules "cb" to be run by the executor on each reply received from executing the exhaust
     * remote command described by "request".
     *
     * Returns a handle for waiting on or canceling the callback, or
     * ErrorCodes::ShutdownInProgress.
     *
     * May be called by client threads or callbacks running in the executor.
     *
     * Contract: Implementations should guarantee that callback should be called *after* doing any
     * processing related to the callback.
     */
    virtual StatusWith<CallbackHandle> scheduleExhaustRemoteCommand(
        const RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const BatonHandle& baton = nullptr) = 0;

    /**
     * Schedules "cb" to be run by the executor on each reply received from executing the exhaust
     * remote command described by "request", as above, but returns a future containing the
     * last response.
     *
     * May be called by client threads or callbacks running in the executor.
     *
     * The input CancellationToken may be used to cancel sending the request. There is no guarantee
     * that this will succeed in canceling the request and the resulting ExecutorFuture may contain
     * either success or error. If cancellation is successful, the resulting ExecutorFuture will be
     * set with a CallbackCanceled error.
     *
     * Cancelling the future will also result in cancelling any outstanding invocations of the
     * callback.
     */
    ExecutorFuture<TaskExecutor::ResponseStatus> scheduleExhaustRemoteCommand(
        const RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const CancellationToken& token,
        const BatonHandle& baton = nullptr);

    /**
     * Returns true if there are any tasks scheduled on the executor.
     */
    virtual bool hasTasks() = 0;

    /**
     * If the callback referenced by "cbHandle" hasn't already executed, marks it as
     * canceled and runnable.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    virtual void cancel(const CallbackHandle& cbHandle) = 0;

    /**
     * Blocks until the executor finishes running the callback referenced by "cbHandle".
     *
     * Because callbacks all run during shutdown if they weren't run beforehand, there is no need
     * to indicate the reason for returning from wait(CallbackHandle).  It is always that the
     * callback ran.
     *
     * NOTE: Do not call from a callback running in the executor.
     *
     * Prefer passing an OperationContext* or other interruptible as the second argument to leaving
     * as not interruptible.
     */
    virtual void wait(const CallbackHandle& cbHandle,
                      Interruptible* interruptible = Interruptible::notInterruptible()) = 0;

    /**
     * Appends information about the underlying network interface's connections to the given
     * builder.
     */
    virtual void appendConnectionStats(ConnectionPoolStats* stats) const = 0;

    /**
     * Drops all connections to the given host on the network interface and relays a status message
     * describing why the connection was dropped.
     */
    virtual void dropConnections(const HostAndPort& target, const Status& status) = 0;

    /**
     * Appends statistics for the underlying network interface.
     */
    virtual void appendNetworkInterfaceStats(BSONObjBuilder&) const = 0;

protected:
    // Retrieves the Callback from a given CallbackHandle
    static CallbackState* getCallbackFromHandle(const CallbackHandle& cbHandle);

    // Retrieves the Event from a given EventHandle
    static EventState* getEventFromHandle(const EventHandle& eventHandle);

    // Sets the given CallbackHandle to point to the given callback.
    static void setCallbackForHandle(CallbackHandle* cbHandle,
                                     std::shared_ptr<CallbackState> callback);

    // Sets the given EventHandle to point to the given event.
    static void setEventForHandle(EventHandle* eventHandle, std::shared_ptr<EventState> event);

    /**
     * `TaskExecutor` is an `enable_shared_from_this` class, and parts of its
     * implementation assume that it is managed with a `std::shared_ptr`.
     * Derived classes are responsible for enforcing this.
     */
    TaskExecutor();
};

/**
 * Class representing a scheduled callback and providing methods for interacting with it.
 */
class TaskExecutor::CallbackState {
    CallbackState(const CallbackState&) = delete;
    CallbackState& operator=(const CallbackState&) = delete;

public:
    virtual ~CallbackState();

    virtual void cancel() = 0;
    virtual void waitForCompletion() = 0;
    virtual bool isCanceled() const = 0;

protected:
    CallbackState();
};

/**
 * Handle to a CallbackState.
 */
class TaskExecutor::CallbackHandle {
    friend class TaskExecutor;

public:
    CallbackHandle();

    // Exposed solely for testing.
    explicit CallbackHandle(std::shared_ptr<CallbackState> cbData);

    bool operator==(const CallbackHandle& other) const {
        return _callback == other._callback;
    }

    bool operator!=(const CallbackHandle& other) const {
        return !(*this == other);
    }

    bool isValid() const {
        return _callback.get();
    }

    /**
     * True if this handle is valid.
     */
    explicit operator bool() const {
        return isValid();
    }

    bool isCanceled() const {
        return getCallback()->isCanceled();
    }

    template <typename H>
    friend H AbslHashValue(H h, const CallbackHandle& handle) {
        return H::combine(std::move(h), handle._callback);
    }

private:
    void setCallback(std::shared_ptr<CallbackState> callback) {
        _callback = callback;
    }

    CallbackState* getCallback() const {
        return _callback.get();
    }

    std::shared_ptr<CallbackState> _callback;
};

/**
 * Class representing a scheduled event and providing methods for interacting with it.
 */
class TaskExecutor::EventState {
    EventState(const EventState&) = delete;
    EventState& operator=(const EventState&) = delete;

public:
    virtual ~EventState();

    virtual void signal() = 0;
    virtual void waitUntilSignaled() = 0;
    virtual bool isSignaled() = 0;

protected:
    EventState();
};

/**
 * Handle to an EventState.
 */
class TaskExecutor::EventHandle {
    friend class TaskExecutor;

public:
    EventHandle();
    explicit EventHandle(std::shared_ptr<EventState> event);

    bool operator==(const EventHandle& other) const {
        return _event == other._event;
    }

    bool operator!=(const EventHandle& other) const {
        return !(*this == other);
    }

    bool isValid() const {
        return _event.get();
    }

    /**
     * True if this event handle is valid.
     */
    explicit operator bool() const {
        return isValid();
    }

private:
    void setEvent(std::shared_ptr<EventState> event) {
        _event = event;
    }

    EventState* getEvent() const {
        return _event.get();
    }

    std::shared_ptr<EventState> _event;
};

/**
 * Argument passed to all callbacks scheduled via a TaskExecutor.
 */
struct TaskExecutor::CallbackArgs {
    CallbackArgs(TaskExecutor* theExecutor,
                 CallbackHandle theHandle,
                 Status theStatus,
                 OperationContext* opCtx = nullptr);

    TaskExecutor* executor;
    CallbackHandle myHandle;
    Status status;
    OperationContext* opCtx;
};

/**
 * Argument passed to all remote command callbacks scheduled via a TaskExecutor.
 */
struct TaskExecutor::RemoteCommandCallbackArgs {
    RemoteCommandCallbackArgs(TaskExecutor* theExecutor,
                              const CallbackHandle& theHandle,
                              const RemoteCommandRequest& theRequest,
                              const ResponseStatus& theResponse);

    RemoteCommandCallbackArgs(const RemoteCommandCallbackArgs& other);

    TaskExecutor* executor;
    CallbackHandle myHandle;
    RemoteCommandRequest request;
    ResponseStatus response;
};

}  // namespace executor
}  // namespace mongo

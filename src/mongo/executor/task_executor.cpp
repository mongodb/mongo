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
#include "mongo/platform/mutex.h"

#include "mongo/executor/task_executor.h"

namespace mongo {
namespace executor {

namespace {

MONGO_FAIL_POINT_DEFINE(pauseScheduleCallWithCancelTokenUntilCanceled);

/**
 * Provides exclusive access to an underlying Promise at set-time, guaranteeing that the Promise
 * will be set at most one time globally. This prevents races between completion and cancellation,
 * which normally would result in Promise throwing an invariant.
 */
template <typename T>
class ExclusivePromiseAccess {
public:
    explicit ExclusivePromiseAccess(Promise<T>&& promise) : _promise(std::move(promise)) {}

    /**
     * Sets an error on the Promise if no result has been set; otherwise, does nothing.
     */
    template <typename Error>
    void setError(Error&& e) {
        if (!_completed.swap(true)) {
            _promise.setError(std::forward<Error>(e));
        }
    }

    /**
     * Sets a valid result on the Promise if no result has been set; otherwise, does nothing.
     */
    template <typename... Args>
    void emplaceValue(Args&&... args) {
        if (!_completed.swap(true)) {
            _promise.emplaceValue(std::forward<Args>(args)...);
        }
    }

private:
    Promise<T> _promise;
    AtomicWord<bool> _completed;
};

template <typename T>
Status wrapCallbackHandleWithCancelToken(
    const std::shared_ptr<TaskExecutor> executor,
    const StatusWith<TaskExecutor::CallbackHandle> swCallbackHandle,
    std::shared_ptr<ExclusivePromiseAccess<T>> promise,
    const CancellationToken& token) {
    if (!swCallbackHandle.isOK()) {
        return swCallbackHandle.getStatus();
    }

    token.onCancel()
        .unsafeToInlineFuture()
        .then(
            [executor, promise, callbackHandle = std::move(swCallbackHandle.getValue())]() mutable {
                executor->cancel(callbackHandle);
                promise->setError(TaskExecutor::kCallbackCanceledErrorStatus);
            })
        .getAsync([](auto) {});
    return Status::OK();
}

/**
 * Takes a schedule(Exhaust)RemoteCommand(OnAny)-style function and wraps it to return a future and
 * be cancelable with CancellationTokens.
 */
template <typename Request, typename Response, typename ScheduleFn, typename CallbackFn>
ExecutorFuture<Response> wrapScheduleCallWithCancelTokenAndFuture(
    const std::shared_ptr<TaskExecutor>& executor,
    ScheduleFn&& schedule,
    const Request& request,
    const CancellationToken& token,
    const BatonHandle& baton,
    const CallbackFn& cb) {
    if (token.isCanceled()) {
        return ExecutorFuture<Response>(executor, TaskExecutor::kCallbackCanceledErrorStatus);
    }

    auto [promise, future] = makePromiseFuture<Response>();

    // This has to be made shared because otherwise we'd have to move the access into this
    // callback, and would be unable to use it in the case where scheduling the request fails below.
    auto exclusivePromiseAccess =
        std::make_shared<ExclusivePromiseAccess<Response>>(std::move(promise));
    auto signalPromiseOnCompletion = [exclusivePromiseAccess,
                                      cb = std::move(cb)](const auto& args) mutable {
        // Upon completion, unconditionally run our callback.
        cb(args);
        auto status = args.response.status;

        // Only mark the future as complete when the moreToCome flag is false, even if an error has
        // occured.
        if (!args.response.moreToCome) {
            if (status.isOK()) {
                exclusivePromiseAccess->emplaceValue(std::move(args.response));
            } else {
                // Only set an error on failures to send the request (including if the request was
                // canceled). Errors from the remote host will be contained in the response.
                exclusivePromiseAccess->setError(status);
            }
        }
    };

    // Fail point to make this method to wait until the token is canceled.
    if (!token.isCanceled()) {
        try {
            pauseScheduleCallWithCancelTokenUntilCanceled.pauseWhileSetAndNotCanceled(
                Interruptible::notInterruptible(), token);
        } catch (ExceptionFor<ErrorCodes::Interrupted>&) {
            // Swallow the interrupted exception that arrives from canceling a failpoint.
        }
    }

    auto scheduleStatus = wrapCallbackHandleWithCancelToken(
        executor,
        std::forward<ScheduleFn>(schedule)(request, std::move(signalPromiseOnCompletion), baton),
        exclusivePromiseAccess,
        token);

    if (!scheduleStatus.isOK()) {
        // If scheduleStatus is not okay, then the callback signalPromiseOnCompletion should never
        // run, meaning that it will be okay to set the promise here.
        exclusivePromiseAccess->setError(scheduleStatus);
    }

    return std::move(future).thenRunOn(executor);
}
}  // namespace

TaskExecutor::TaskExecutor() = default;
TaskExecutor::~TaskExecutor() = default;

void TaskExecutor::schedule(OutOfLineExecutor::Task func) {
    auto cb = CallbackFn([func = std::move(func)](const CallbackArgs& args) { func(args.status); });
    auto statusWithCallback = scheduleWork(std::move(cb));
    if (!statusWithCallback.isOK()) {
        // The callback was not scheduled or moved from, it is still valid. Run it inline to inform
        // it of the error. Construct a CallbackArgs for it, only CallbackArgs::status matters here.
        CallbackArgs args(this, {}, statusWithCallback.getStatus(), nullptr);
        invariant(cb);  // NOLINT(bugprone-use-after-move)
        cb(args);       // NOLINT(bugprone-use-after-move)
    }
}

ExecutorFuture<void> TaskExecutor::sleepUntil(Date_t when, const CancellationToken& token) {
    if (token.isCanceled()) {
        return ExecutorFuture<void>(shared_from_this(), TaskExecutor::kCallbackCanceledErrorStatus);
    }

    if (when <= now()) {
        return ExecutorFuture<void>(shared_from_this());
    }

    /**
     * Encapsulates the promise associated with the result future.
     */
    struct AlarmState {
        void signal(const Status& status) {
            if (status.isOK()) {
                promise->emplaceValue();
            } else {
                promise->setError(status);
            }
        }

        std::shared_ptr<ExclusivePromiseAccess<void>> promise;
    };

    auto [promise, future] = makePromiseFuture<void>();
    // This has to be shared because Promises (and therefore AlarmState) are move-only and we need
    // to maintain two copies: One to capture in the scheduleWorkAt callback, and one locally in
    // case scheduling the request fails.
    auto exclusivePromiseAccess =
        std::make_shared<ExclusivePromiseAccess<void>>(std::move(promise));
    auto alarmState = std::make_shared<AlarmState>(AlarmState{exclusivePromiseAccess});

    // Schedule a task to signal the alarm when the deadline is reached.
    auto cbHandle = scheduleWorkAt(
        when, [alarmState](const auto& args) mutable { alarmState->signal(args.status); });

    // Handle cancellation via the input CancellationToken.
    auto scheduleStatus = wrapCallbackHandleWithCancelToken(
        shared_from_this(), std::move(cbHandle), exclusivePromiseAccess, token);

    if (!scheduleStatus.isOK()) {
        // If scheduleStatus is not okay, then the callback passed to scheduleWorkAt should never
        // run, meaning that it will be okay to set the promise here.
        alarmState->signal(scheduleStatus);
    }

    return std::move(future).thenRunOn(shared_from_this());
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

TaskExecutor::RemoteCommandCallbackArgs::RemoteCommandCallbackArgs(
    const RemoteCommandOnAnyCallbackArgs& other, size_t idx)
    : executor(other.executor),
      myHandle(other.myHandle),
      request(other.request, idx),
      response(other.response) {}

TaskExecutor::RemoteCommandOnAnyCallbackArgs::RemoteCommandOnAnyCallbackArgs(
    TaskExecutor* theExecutor,
    const CallbackHandle& theHandle,
    const RemoteCommandRequestOnAny& theRequest,
    const ResponseOnAnyStatus& theResponse)
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


StatusWith<TaskExecutor::CallbackHandle> TaskExecutor::scheduleRemoteCommand(
    const RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {
    return scheduleRemoteCommandOnAny(request,
                                      [cb](const RemoteCommandOnAnyCallbackArgs& args) {
                                          cb({args, 0});
                                      },
                                      baton);
}

ExecutorFuture<TaskExecutor::ResponseStatus> TaskExecutor::scheduleRemoteCommand(
    const RemoteCommandRequest& request, const CancellationToken& token, const BatonHandle& baton) {
    return wrapScheduleCallWithCancelTokenAndFuture<decltype(request),
                                                    TaskExecutor::ResponseStatus>(
        shared_from_this(),
        [this](const auto&... args) { return scheduleRemoteCommand(args...); },
        request,
        token,
        baton,
        [](const auto& args) {});
}

ExecutorFuture<TaskExecutor::ResponseOnAnyStatus> TaskExecutor::scheduleRemoteCommandOnAny(
    const RemoteCommandRequestOnAny& request,
    const CancellationToken& token,
    const BatonHandle& baton) {
    return wrapScheduleCallWithCancelTokenAndFuture<decltype(request),
                                                    TaskExecutor::ResponseOnAnyStatus>(
        shared_from_this(),
        [this](const auto&... args) { return scheduleRemoteCommandOnAny(args...); },
        request,
        token,
        baton,
        [](const auto& args) {});
}

StatusWith<TaskExecutor::CallbackHandle> TaskExecutor::scheduleExhaustRemoteCommand(
    const RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {
    return scheduleExhaustRemoteCommandOnAny(request,
                                             [cb](const RemoteCommandOnAnyCallbackArgs& args) {
                                                 cb({args, 0});
                                             },
                                             baton);
}

ExecutorFuture<TaskExecutor::ResponseStatus> TaskExecutor::scheduleExhaustRemoteCommand(
    const RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const CancellationToken& token,
    const BatonHandle& baton) {
    return wrapScheduleCallWithCancelTokenAndFuture<decltype(request),
                                                    TaskExecutor::ResponseStatus>(
        shared_from_this(),
        [this](const auto&... args) { return scheduleExhaustRemoteCommand(args...); },
        request,
        token,
        baton,
        cb);
}

ExecutorFuture<TaskExecutor::ResponseOnAnyStatus> TaskExecutor::scheduleExhaustRemoteCommandOnAny(
    const RemoteCommandRequestOnAny& request,
    const RemoteCommandOnAnyCallbackFn& cb,
    const CancellationToken& token,
    const BatonHandle& baton) {
    return wrapScheduleCallWithCancelTokenAndFuture<decltype(request),
                                                    TaskExecutor::ResponseOnAnyStatus>(
        shared_from_this(),
        [this](const auto&... args) { return scheduleExhaustRemoteCommandOnAny(args...); },
        request,
        token,
        baton,
        cb);
}
}  // namespace executor
}  // namespace mongo

/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/executor/scoped_task_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>

namespace mongo {
namespace executor {

MONGO_FAIL_POINT_DEFINE(ScopedTaskExecutorHangBeforeSchedule);
MONGO_FAIL_POINT_DEFINE(ScopedTaskExecutorHangExitBeforeSchedule);
MONGO_FAIL_POINT_DEFINE(ScopedTaskExecutorHangAfterSchedule);

namespace {
static const inline auto kDefaultShutdownStatus =
    Status(ErrorCodes::ShutdownInProgress, "Shutting down ScopedTaskExecutor");
}

/**
 * Implements the wrapping indirection needed to satisfy the ScopedTaskExecutor contract.  Note
 * that at least shutdown() must be called on this type before destruction.
 */
class ScopedTaskExecutor::Impl : public TaskExecutor {
public:
    Impl(std::shared_ptr<TaskExecutor> executor, Status shutdownStatus)
        : _executor(std::move(executor)), _shutdownStatus(std::move(shutdownStatus)) {}

    ~Impl() override {
        // The ScopedTaskExecutor dtor calls shutdown, so this is guaranteed.
        invariant(_inShutdown);
    }

    void startup() override {
        MONGO_UNREACHABLE;
    }

    void shutdown() override {
        auto handles = [&] {
            stdx::lock_guard lk(_mutex);
            if (!_inShutdown && _cbHandles.empty()) {
                // We are guaranteed that no more callbacks can be added to _cbHandles after
                // _inShutdown is set to true. If there aren't any callbacks outstanding, then it is
                // shutdown()'s responsibility to make the futures returned by joinAll() ready.
                _promise.emplaceValue();
            }
            _inShutdown = true;

            return _cbHandles;
        }();

        for (auto& [id, handle] : handles) {
            // If we don't have a handle yet, it means there's a scheduling thread that's
            // dropped the lock but hasn't yet stashed it (or failed to schedule it on the
            // underlying executor).
            //
            // See _wrapCallback for how the scheduling thread handles those cases.
            if (handle) {
                _executor->cancel(handle);
            }
        }
    }

    void join() override {
        joinAsync().wait();
    }

    SharedSemiFuture<void> joinAsync() override {
        return _promise.getFuture();
    }

    bool isShuttingDown() const override {
        stdx::lock_guard lk(_mutex);
        return _inShutdown;
    }

    void appendDiagnosticBSON(BSONObjBuilder* b) const override {
        MONGO_UNREACHABLE;
    }

    Date_t now() override {
        return _executor->now();
    }

    StatusWith<EventHandle> makeEvent() override {
        if (stdx::lock_guard lk(_mutex); _inShutdown) {
            return _shutdownStatus;
        }

        return _executor->makeEvent();
    }

    void signalEvent(const EventHandle& event) override {
        return _executor->signalEvent(event);
    }

    StatusWith<CallbackHandle> onEvent(const EventHandle& event, CallbackFn&& work) override {
        return _wrapCallback([&](auto&& x) { return _executor->onEvent(event, std::move(x)); },
                             std::move(work));
    }

    void waitForEvent(const EventHandle& event) override {
        return _executor->waitForEvent(event);
    }

    StatusWith<stdx::cv_status> waitForEvent(OperationContext* opCtx,
                                             const EventHandle& event,
                                             Date_t deadline = Date_t::max()) override {
        return _executor->waitForEvent(opCtx, event, deadline);
    }

    StatusWith<CallbackHandle> scheduleWork(CallbackFn&& work) override {
        return _wrapCallback([&](auto&& x) { return _executor->scheduleWork(std::move(x)); },
                             std::move(work));
    }

    StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, CallbackFn&& work) override {
        return _wrapCallback(
            [&](auto&& x) { return _executor->scheduleWorkAt(when, std::move(x)); },
            std::move(work));
    }

    StatusWith<CallbackHandle> scheduleRemoteCommand(const RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb,
                                                     const BatonHandle& baton = nullptr) override {
        return _wrapCallback(
            [&](auto&& x) {
                return _executor->scheduleRemoteCommand(request, std::move(x), baton);
            },
            cb);
    }

    StatusWith<CallbackHandle> scheduleExhaustRemoteCommand(
        const RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const BatonHandle& baton = nullptr) override {
        return _wrapCallback(
            [&](auto&& x) {
                return _executor->scheduleExhaustRemoteCommand(request, std::move(x), baton);
            },
            cb);
    }

    bool hasTasks() override {
        return _executor->hasTasks();
    }

    void cancel(const CallbackHandle& cbHandle) override {
        return _executor->cancel(cbHandle);
    }

    void wait(const CallbackHandle& cbHandle,
              Interruptible* interruptible = Interruptible::notInterruptible()) override {
        return _executor->wait(cbHandle, interruptible);
    }

    void appendConnectionStats(ConnectionPoolStats* stats) const override {
        MONGO_UNREACHABLE;
    }

    void dropConnections(const HostAndPort& target, const Status& status) override {
        MONGO_UNREACHABLE;
    }

    void appendNetworkInterfaceStats(BSONObjBuilder&) const override {
        MONGO_UNREACHABLE;
    }

private:
    /**
     * Helper function to get a shared_ptr<ScopedTaskExecutor::Impl> to this object, akin to
     * shared_from_this(). TaskExecutor (the parent class of ScopedTaskExecutor::Impl) inherits from
     * std::enable_shared_from_this, so shared_from_this() returns a std::shared_ptr<TaskExecutor>,
     * which means we need to cast it to use it as a pointer to the subclass
     * ScopedTaskExecutor::Impl.
     */
    std::shared_ptr<ScopedTaskExecutor::Impl> shared_self() {
        return std::static_pointer_cast<ScopedTaskExecutor::Impl>(shared_from_this());
    }

    /**
     * Wraps a scheduling call, along with its callback, so that:
     *
     * 1. If the callback is run, it is invoked with a not-okay argument if this task executor or
     *    the underlying one has been shutdown.
     * 2. The callback handle that is returned from the call to schedule is collected and
     *    canceled, if this object is shutdown before the callback is invoked.
     *
     * Theory of operation for shutdown/join
     *
     * All callbacks that are wrapped by this method are in 1 of 5 states:
     *
     * 1. Haven't yet acquired the first lock, no recorded state.
     *
     * 2. Have stashed an entry in the _cbHandles table, but with an unset callback handle.
     * 2.a. We successfully schedule and record the callback handle after the fact.
     * 2.b. We fail to schedule, requiring us to erase recorded state directly from
     *      _wrapCallback.
     *
     * 3. Acquired the lock after calling schedule before the callback ran.  Callback handle is
     *    in the _cbHandles table and the task is cancellable.
     *
     * 4. Ran the callback before stashing the callback handle.  No entry in the table and we
     *    won't stash the handle on exit.
     *
     * What happens in shutdown (I.e. when _inShutdown is set):
     *
     * 1. Nothing.  We never record any values and return immediately with a not-ok status
     *    without running the task
     *
     * 2. We have an entry in the table, but no callback handle.
     * 2.a. The scheduling thread will notice _inShutdown after calling schedule and will cancel
     *      it on the way out.  The execution of the task will remove the entry and notify.
     * 2.b. The scheduling thread will see that the underlying executor failed and
     *      remove/notify.
     *
     * 3. We'll call cancel in shutdown.  The task will remote/notify.
     *
     * 4. The task has already completed and removed itself from the table.
     */
    template <typename ScheduleCall, typename Work>
    StatusWith<CallbackHandle> _wrapCallback(ScheduleCall&& schedule, Work&& work) {
        size_t id;

        // State 1 - No Data
        stdx::unique_lock lk(_mutex);

        // No clean up needed because we never ran or recorded anything
        if (_inShutdown) {
            return _shutdownStatus;
        }

        id = _id++;

        _cbHandles.emplace(
            std::piecewise_construct, std::forward_as_tuple(id), std::forward_as_tuple());

        if (MONGO_unlikely(ScopedTaskExecutorHangBeforeSchedule.shouldFail())) {
            ScopedTaskExecutorHangBeforeSchedule.setMode(FailPoint::off);
            lk.unlock();
            ScopedTaskExecutorHangExitBeforeSchedule.pauseWhileSet();
            lk.lock();
        }

        static_assert(std::is_same_v<Work, CallbackFn> ||
                      std::is_same_v<Work, const RemoteCommandCallbackFn&>);

        // Allocate memory to hold the moved work, but don't move it yet,
        // so TaskExecutor::schedule() works correctly if swCbHandle
        // returns an error.

        struct MoveState {
            MoveState(std::unique_ptr<CallbackFn> mem,
                      CallbackFn&& from,
                      stdx::unique_lock<stdx::mutex>& _lk)
                : work(std::move(mem)), moveFrom(std::move(from)), lk(_lk) {}

            // Dummy copy constructor so the variant has a copy constructor
            // for the RemoteCommandCallbackFn case. Should never be called.
            MoveState(const MoveState& ms) : moveFrom(std::move(ms.moveFrom)), lk(ms.lk) {
                MONGO_UNREACHABLE;
            }
            MoveState(MoveState&& ms)
                : work(std::move(ms.work)),
                  moveFrom(std::move(ms.moveFrom)),
                  lk(ms.lk),
                  tid(ms.tid) {}

            MoveState& operator=(const MoveState&) = delete;
            MoveState& operator=(MoveState&&) = delete;

            std::unique_ptr<CallbackFn> work;
            CallbackFn&& moveFrom;
            stdx::unique_lock<stdx::mutex>& lk;
            std::thread::id tid{stdx::this_thread::get_id()};
        };
        std::variant<RemoteCommandCallbackFn, MoveState> state;
        CallbackFn* moveTo = nullptr;

        if constexpr (std::is_same_v<Work, CallbackFn>) {
            auto mem = std::make_unique<CallbackFn>();
            moveTo = mem.get();
            state.template emplace<MoveState>(  // NOLINT(bugprone-use-after-move)
                std::move(mem),                 // NOLINT(bugprone-use-after-move)
                std::move(work),                // NOLINT(bugprone-use-after-move)
                lk);
        } else {
            state.template emplace<RemoteCommandCallbackFn>(work);
            lk.unlock();
        }

        // State 2 - Indeterminate state.  We don't know yet if the task will get scheduled.
        auto swCbHandle = std::forward<ScheduleCall>(schedule)(
            [id, state = std::move(state), self = shared_self()](const auto& cargs) {
                using ArgsT = std::decay_t<decltype(cargs)>;

                stdx::unique_lock<stdx::mutex> lk(self->_mutex, stdx::defer_lock);
                std::unique_lock<stdx::mutex>* useLock = nullptr;

                if constexpr (std::is_same_v<Work, CallbackFn>) {
                    // check for inline execution, tid check must be first
                    const MoveState& mstate = std::get<MoveState>(state);
                    CallbackFn& moveTo = *(mstate.work.get());
                    if (mstate.tid == stdx::this_thread::get_id() && !moveTo) {
                        // we're inline, do the move and borrow the lock
                        moveTo = std::move(mstate.moveFrom);
                        useLock = &mstate.lk;
                    }
                }

                if (!useLock) {
                    // not inline under the lock, do our own locking
                    lk.lock();
                    useLock = &lk;
                }

                auto doWorkAndNotify = [&](const ArgsT& x) noexcept {
                    useLock->unlock();
                    if constexpr (std::is_same_v<Work, CallbackFn>) {
                        (*std::get<MoveState>(state).work.get())(x);
                    } else {
                        (std::get<RemoteCommandCallbackFn>(state))(x);
                    }
                    useLock->lock();

                    // After we've run the task, we erase and notify.  Sometimes that happens
                    // before we stash the cbHandle.
                    self->_eraseAndNotifyIfNeeded(*useLock, id);
                };

                if (!self->_inShutdown) {
                    doWorkAndNotify(cargs);
                    return;
                }

                // Have to copy args because we get the arguments by const& and need to
                // modify the status field.
                auto args = cargs;

                if constexpr (std::is_same_v<ArgsT, CallbackArgs>) {
                    args.status = self->_shutdownStatus;
                } else {
                    static_assert(std::is_same_v<ArgsT, RemoteCommandCallbackArgs>,
                                  "_wrapCallback only supports CallbackArgs and "
                                  "RemoteCommandCallbackArgs");
                    args.response.status = self->_shutdownStatus;
                }

                doWorkAndNotify(args);
            });

        // We must do the move before dropping the lock, so the
        // callback blocks until the work is moved.
        if constexpr (std::is_same_v<Work, CallbackFn>) {
            if (swCbHandle.isOK() && work)            // NOLINT(bugprone-use-after-move)
                *moveTo = std::move(work);            // NOLINT(bugprone-use-after-move)
            invariant(!!swCbHandle.isOK() == !work);  // NOLINT(bugprone-use-after-move)
            lk.unlock();
        }

        ScopedTaskExecutorHangAfterSchedule.pauseWhileSet();
        lk.lock();

        if (!swCbHandle.isOK()) {
            // State 2.b - Failed to schedule
            _eraseAndNotifyIfNeeded(lk, id);
            return swCbHandle;
        }

        // State 2.a - Scheduled, but haven't stashed the cbHandle

        if (_inShutdown) {
            // If we're in shutdown, the caller of shutdown has cancelled all the handles it had
            // available (which doesn't include this one).  So we're responsible for calling
            // cancel().
            //
            // Note that the task will handle remove/notify
            lk.unlock();
            _executor->cancel(swCbHandle.getValue());

            return swCbHandle;
        }

        if (auto iter = _cbHandles.find(id); iter != _cbHandles.end()) {
            // State 3 - Handle stashed
            iter->second = swCbHandle.getValue();
        } else {
            // State 4 - Callback ran before we got here.
        }

        return swCbHandle;
    }

    void _eraseAndNotifyIfNeeded(WithLock, size_t id) {
        invariant(_cbHandles.erase(id) == 1);

        if (_inShutdown && _cbHandles.empty()) {
            // We are guaranteed that no more callbacks can be added to _cbHandles after _inShutdown
            // is set to true. If there are no more callbacks outstanding, then it is the last
            // callback's responsibility to make the futures returned by joinAll() ready.
            _promise.emplaceValue();
        }
    }

    mutable stdx::mutex _mutex;
    bool _inShutdown = false;
    std::shared_ptr<TaskExecutor> _executor;
    Status _shutdownStatus;
    size_t _id = 0;
    stdx::unordered_map<size_t, CallbackHandle> _cbHandles;

    // Promise that is set when the executor has been shut down and there aren't any outstanding
    // callbacks. Callers of joinAsync() extract futures from this promise.
    SharedPromise<void> _promise;
};

ScopedTaskExecutor::ScopedTaskExecutor(std::shared_ptr<TaskExecutor> executor)
    : _executor(std::make_shared<Impl>(std::move(executor), kDefaultShutdownStatus)) {}

ScopedTaskExecutor::ScopedTaskExecutor(std::shared_ptr<TaskExecutor> executor,
                                       Status shutdownStatus)
    : _executor(std::make_shared<Impl>(std::move(executor), shutdownStatus)) {
    invariant(ErrorCodes::isA<ErrorCategory::CancellationError>(shutdownStatus));
}

ScopedTaskExecutor::~ScopedTaskExecutor() {
    _executor->shutdown();
}

}  // namespace executor
}  // namespace mongo

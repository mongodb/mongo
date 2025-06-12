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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor


namespace mongo {
namespace executor {

namespace {
MONGO_FAIL_POINT_DEFINE(scheduleIntoPoolSpinsUntilThreadPoolTaskExecutorShutsDown);
MONGO_FAIL_POINT_DEFINE(tpteHangsBeforeDrainingCallbacks);

const Status kTaskExecutorShutdownInProgress = {ErrorCodes::ShutdownInProgress,
                                                "TaskExecutor shutdown in progress"};
}  // namespace

class ThreadPoolTaskExecutor::CallbackState : public TaskExecutor::CallbackState {
    CallbackState(const CallbackState&) = delete;
    CallbackState& operator=(const CallbackState&) = delete;

public:
    CallbackState() = default;

    ~CallbackState() override = default;

    bool isCanceled() const override {
        return cancelSource.token().isCanceled();
    }

    void cancel() override {
        cancelSource.cancel();
    }

    void waitForCompletion() override {
        MONGO_UNREACHABLE;
    }

    virtual void resetCallback() = 0;

    // Both are guarded by the owning task executor's _mutex.
    boost::optional<stdx::condition_variable> finishedCondition;
    boost::optional<std::list<std::shared_ptr<CallbackState>>::iterator> iter;

    AtomicWord<bool> isFinished{false};
    CancellationSource cancelSource;
};

struct ThreadPoolTaskExecutor::LocalCallbackState : public ThreadPoolTaskExecutor::CallbackState {
    LocalCallbackState() = default;

    void resetCallback() override {
        callback = {};
    }

    CallbackFn callback;
};

struct ThreadPoolTaskExecutor::RemoteCallbackState : public ThreadPoolTaskExecutor::CallbackState {
    RemoteCallbackState(RemoteCommandRequest request,
                        RemoteCommandCallbackFn cb,
                        const std::shared_ptr<Baton>& baton)
        : request(std::move(request)), callback(std::move(cb)), baton(baton) {}

    void resetCallback() override {
        callback = {};
    }

    void cancel() override {
        LOGV2_DEBUG(9311408, 3, "Canceling remote request", "requestId"_attr = request.id);
        ThreadPoolTaskExecutor::CallbackState::cancel();
    }

    RemoteCommandRequest request;
    std::function<void(RemoteCommandCallbackArgs)> callback;
    BatonHandle baton;
};

class ThreadPoolTaskExecutor::EventState : public TaskExecutor::EventState {
    EventState(const EventState&) = delete;
    EventState& operator=(const EventState&) = delete;

public:
    static std::shared_ptr<EventState> make() {
        return std::make_shared<EventState>();
    }

    EventState() = default;

    void signal() override {
        MONGO_UNREACHABLE;
    }
    void waitUntilSignaled() override {
        MONGO_UNREACHABLE;
    }
    bool isSignaled() override {
        MONGO_UNREACHABLE;
    }

    // All fields except promise are guarded by the owning task executor's _mutex.

    bool isSignaledFlag = false;
    stdx::condition_variable isSignaledCondition;
    EventList::iterator iter;
    SharedPromise<void> promise;
};

std::shared_ptr<ThreadPoolTaskExecutor> ThreadPoolTaskExecutor::create(
    std::unique_ptr<ThreadPoolInterface> pool, std::shared_ptr<NetworkInterface> net) {
    return std::make_shared<ThreadPoolTaskExecutor>(Passkey{}, std::move(pool), std::move(net));
}

ThreadPoolTaskExecutor::ThreadPoolTaskExecutor(Passkey,
                                               std::unique_ptr<ThreadPoolInterface> pool,
                                               std::shared_ptr<NetworkInterface> net)
    : _net(std::move(net)), _pool(std::move(pool)) {}

ThreadPoolTaskExecutor::~ThreadPoolTaskExecutor() {
    shutdown();
    join();
    invariant(_state == shutdownComplete);
}

void ThreadPoolTaskExecutor::startup() {
    _net->startup();
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_state == preStart);
    _setState_inlock(running);
    _pool->startup();
}

void ThreadPoolTaskExecutor::shutdown() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_inShutdown_inlock()) {
        return;
    }
    _setState_inlock(joinRequired);

    auto toCancel = _inProgress;
    lk.unlock();

    // Cancel all outstanding work.
    for (auto& cbState : toCancel) {
        cbState->cancel();
    }
}

SharedSemiFuture<void> ThreadPoolTaskExecutor::joinAsync() {
    MONGO_UNREACHABLE;
}

void ThreadPoolTaskExecutor::join() {
    tpteHangsBeforeDrainingCallbacks.pauseWhileSet();

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    _stateChange.wait(lk, [this] {
        if (!_inProgress.empty()) {
            return false;
        }

        switch (_state) {
            case preStart:
                return false;
            case running:
                return false;
            case joinRequired:
                return true;
            case joining:
                return false;
            case shutdownComplete:
                return true;
        }
        MONGO_UNREACHABLE;
    });

    if (_state == shutdownComplete) {
        return;
    }
    invariant(_state == joinRequired);
    _setState_inlock(joining);
    lk.unlock();
    _pool->shutdown();
    _pool->join();
    lk.lock();
    while (!_unsignaledEvents.empty()) {
        auto eventState = _unsignaledEvents.front();
        EventHandle event;
        setEventForHandle(&event, std::move(eventState));
        signalEvent_inlock(event, std::move(lk));
        lk = stdx::unique_lock<stdx::mutex>(_mutex);
    }
    lk.unlock();
    _net->shutdown();
    lk.lock();
    invariant(_inProgress.empty());
    _setState_inlock(shutdownComplete);
    return;
}

bool ThreadPoolTaskExecutor::isShuttingDown() const {
    stdx::lock_guard lk(_mutex);
    return _inShutdown_inlock();
}

void ThreadPoolTaskExecutor::appendDiagnosticBSON(BSONObjBuilder* b) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // ThreadPool details
    // TODO: fill in
    BSONObjBuilder poolCounters(b->subobjStart("pool"));
    poolCounters.appendNumber("inProgressCount", static_cast<long long>(_inProgress.size()));
    poolCounters.done();

    // Queues
    BSONObjBuilder queues(b->subobjStart("queues"));
    queues.appendNumber("networkInProgress", static_cast<long long>(_networkInProgress.load()));
    queues.appendNumber("sleepers", static_cast<long long>(_sleepers.load()));
    queues.done();

    b->appendNumber("unsignaledEvents", static_cast<long long>(_unsignaledEvents.size()));
    b->append("shuttingDown", _inShutdown_inlock());
    b->append("networkInterface", _net->getDiagnosticString());
}

Date_t ThreadPoolTaskExecutor::now() {
    return _net->now();
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::_registerCallbackState(
    std::shared_ptr<CallbackState> cbState) {
    stdx::lock_guard lk(_mutex);
    if (_inShutdown_inlock()) {
        return kTaskExecutorShutdownInProgress;
    }

    _inProgress.push_front(cbState);
    cbState->iter = _inProgress.begin();

    CallbackHandle cbHandle;
    setCallbackForHandle(&cbHandle, std::move(cbState));
    return cbHandle;
}

void ThreadPoolTaskExecutor::_unregisterCallbackState(
    const std::shared_ptr<CallbackState>& cbState) {
    cbState->isFinished.store(true);
    cbState->resetCallback();

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(cbState->iter);
    _inProgress.erase(*cbState->iter);
    cbState->iter.reset();
    if (cbState->finishedCondition) {
        cbState->finishedCondition->notify_all();
    }
    if (_inShutdown_inlock() && _inProgress.empty()) {
        _stateChange.notify_all();
    }
}

StatusWith<TaskExecutor::EventHandle> ThreadPoolTaskExecutor::makeEvent() {
    auto el = makeSingletonEventList();
    EventHandle event;
    setEventForHandle(&event, el.front());
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown_inlock()) {
        return kTaskExecutorShutdownInProgress;
    }
    _unsignaledEvents.splice(_unsignaledEvents.end(), el);
    return event;
}

void ThreadPoolTaskExecutor::signalEvent(const EventHandle& event) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    signalEvent_inlock(event, std::move(lk));
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::onEvent(const EventHandle& event,
                                                                         CallbackFn&& work) {
    if (!event.isValid()) {
        return {ErrorCodes::BadValue, "Passed invalid event handle to onEvent"};
    }
    auto cbState = std::make_shared<LocalCallbackState>();
    auto cbHandle = _registerCallbackState(cbState);
    if (!cbHandle.isOK()) {
        return cbHandle;
    }
    cbState->callback = std::move(work);

    auto eventState = checked_cast<EventState*>(getEventFromHandle(event));
    future_util::withCancellation(eventState->promise.getFuture(), cbState->cancelSource.token())
        .thenRunOn(makeGuaranteedExecutor(_pool))
        .getAsync(
            [this, cbState = std::move(cbState)](Status s) { runCallback(std::move(cbState), s); });

    return cbHandle;
}

StatusWith<stdx::cv_status> ThreadPoolTaskExecutor::waitForEvent(OperationContext* opCtx,
                                                                 const EventHandle& event,
                                                                 Date_t deadline) {
    invariant(opCtx);
    invariant(event.isValid());
    auto eventState = checked_cast<EventState*>(getEventFromHandle(event));
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    try {
        if (opCtx->waitForConditionOrInterruptUntil(
                eventState->isSignaledCondition, lk, deadline, [&] {
                    return eventState->isSignaledFlag;
                })) {
            return stdx::cv_status::no_timeout;
        }

        return stdx::cv_status::timeout;
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

void ThreadPoolTaskExecutor::waitForEvent(const EventHandle& event) {
    invariant(event.isValid());
    auto eventState = checked_cast<EventState*>(getEventFromHandle(event));
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    while (!eventState->isSignaledFlag) {
        eventState->isSignaledCondition.wait(lk);
    }
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::scheduleWork(CallbackFn&& work) {
    auto cbState = std::make_shared<LocalCallbackState>();
    auto swCbHandle = _registerCallbackState(cbState);
    if (!swCbHandle.isOK()) {
        return swCbHandle.getStatus();
    }
    cbState->callback = std::move(work);

    if (MONGO_unlikely(scheduleIntoPoolSpinsUntilThreadPoolTaskExecutorShutsDown.shouldFail())) {
        scheduleIntoPoolSpinsUntilThreadPoolTaskExecutorShutsDown.setMode(FailPoint::off);

        stdx::unique_lock lk(_mutex);
        _stateChange.wait(lk, [&] { return _inShutdown_inlock(); });
        // Wait until the executor has reached shutdown state and canceled all outstanding work
        // before proceeding.
        auto _ = cbState->cancelSource.token().onCancel().waitNoThrow();
    }

    ExecutorFuture(makeGuaranteedExecutor(_pool))
        .getAsync([this, cbState = std::move(cbState)](Status s) {
            invariant(s);
            runCallback(cbState, Status::OK());
        });
    return swCbHandle;
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::scheduleWorkAt(Date_t when,
                                                                                CallbackFn&& work) {
    if (when <= now()) {
        return scheduleWork(std::move(work));
    }

    auto cbState = std::make_shared<LocalCallbackState>();
    auto swCbHandle = _registerCallbackState(cbState);
    if (!swCbHandle.isOK()) {
        return swCbHandle;
    }
    cbState->callback = std::move(work);

    CancellationToken token = cbState->cancelSource.token();
    _sleepers.fetchAndAdd(1);
    _net->setAlarm(when, token)
        .thenRunOn(makeGuaranteedExecutor(_pool))
        .getAsync([this, cbState](Status status) {
            _sleepers.fetchAndSubtract(1);
            runCallback(cbState, status);
        });

    return swCbHandle;
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::scheduleRemoteCommand(
    const RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {

    RemoteCommandRequest scheduledRequest = request;
    scheduledRequest.dateScheduled = _net->now();

    auto cbState = std::make_shared<RemoteCallbackState>(scheduledRequest, cb, baton);
    auto swCbHandle = _registerCallbackState(cbState);
    if (!swCbHandle.isOK()) {
        return swCbHandle;
    }
    _networkInProgress.addAndFetch(1);

    LOGV2_DEBUG(22607,
                3,
                "Scheduling remote command request",
                "request"_attr = redact(scheduledRequest.toString()));

    try {
        _net->startCommand(
                swCbHandle.getValue(), scheduledRequest, baton, cbState->cancelSource.token())
            .thenRunOn(makeGuaranteedExecutor(cbState->baton, _pool))
            .getAsync([this, cbHandle = swCbHandle.getValue(), cbState](
                          StatusWith<RemoteCommandResponse> swr) {
                _networkInProgress.fetchAndSubtract(1);
                auto args = makeRemoteCallbackArgs(cbHandle, *cbState, std::move(swr));
                LOGV2_DEBUG(22608,
                            3,
                            "Received remote response",
                            "requestId"_attr = cbState->request.id,
                            "response"_attr =
                                redact(args.response.isOK() ? args.response.toString()
                                                            : args.response.status.toString()));
                cbState->callback(args);
                _unregisterCallbackState(cbState);
            });
    } catch (const DBException& e) {
        _unregisterCallbackState(cbState);
        _networkInProgress.subtractAndFetch(1);
        return e.toStatus();
    }

    return swCbHandle;
}

void ThreadPoolTaskExecutor::cancel(const CallbackHandle& cbHandle) {
    invariant(cbHandle.isValid());

    auto cbState = checked_cast<CallbackState*>(getCallbackFromHandle(cbHandle));
    if (cbState->isFinished.load()) {
        return;
    }
    cbState->cancel();
}

void ThreadPoolTaskExecutor::wait(const CallbackHandle& cbHandle, Interruptible* interruptible) {
    invariant(cbHandle.isValid());
    auto cbState = checked_cast<CallbackState*>(getCallbackFromHandle(cbHandle));
    if (cbState->isFinished.load()) {
        return;
    }
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (!cbState->finishedCondition) {
        cbState->finishedCondition.emplace();
    }

    interruptible->waitForConditionOrInterrupt(
        *cbState->finishedCondition, lk, [&] { return cbState->isFinished.load(); });
}

void ThreadPoolTaskExecutor::appendConnectionStats(ConnectionPoolStats* stats) const {
    _net->appendConnectionStats(stats);
}

void ThreadPoolTaskExecutor::appendNetworkInterfaceStats(BSONObjBuilder& bob) const {
    _net->appendStats(bob);
}

ThreadPoolTaskExecutor::EventList ThreadPoolTaskExecutor::makeSingletonEventList() {
    EventList result;
    result.emplace_front(EventState::make());
    result.front()->iter = result.begin();
    return result;
}

void ThreadPoolTaskExecutor::signalEvent_inlock(const EventHandle& event,
                                                stdx::unique_lock<stdx::mutex> lk) {
    invariant(event.isValid());
    auto eventState = checked_cast<EventState*>(getEventFromHandle(event));
    const auto wasSignaled = std::exchange(eventState->isSignaledFlag, true);
    if (MONGO_unlikely(wasSignaled && _inShutdown_inlock()))
        return;
    invariant(!wasSignaled);
    eventState->isSignaledCondition.notify_all();
    _unsignaledEvents.erase(eventState->iter);
    lk.unlock();
    eventState->promise.emplaceValue();
}

void ThreadPoolTaskExecutor::runCallback(std::shared_ptr<LocalCallbackState> cbStateArg, Status s) {
    auto status = [&] {
        if (!s.isOK()) {
            return s;
        }
        return cbStateArg->isCanceled() ? kCallbackCanceledErrorStatus : Status::OK();
    }();
    CallbackHandle cbHandle;
    setCallbackForHandle(&cbHandle, cbStateArg);
    CallbackArgs args(this, std::move(cbHandle), std::move(status));
    invariant(!cbStateArg->isFinished.load());
    cbStateArg->callback(std::move(args));
    _unregisterCallbackState(cbStateArg);
}

TaskExecutor::RemoteCommandCallbackArgs ThreadPoolTaskExecutor::makeRemoteCallbackArgs(
    const CallbackHandle& cbHandle,
    const RemoteCallbackState& cbState,
    StatusWith<RemoteCommandResponse> swr) {
    if (!swr.isOK()) {
        return RemoteCommandCallbackArgs(
            this, cbHandle, cbState.request, {cbState.request.target, swr.getStatus()});
    } else {
        return RemoteCommandCallbackArgs(this, cbHandle, cbState.request, swr.getValue());
    }
}

void ThreadPoolTaskExecutor::_continueExhaustCommand(
    CallbackHandle cbHandle,
    std::shared_ptr<RemoteCallbackState> cbState,
    std::shared_ptr<NetworkInterface::ExhaustResponseReader> rdr) {

    rdr->next()
        .thenRunOn(makeGuaranteedExecutor(cbState->baton, _pool))
        .getAsync([this, rdr, cbHandle, cbState](StatusWith<RemoteCommandResponse> swr) {
            auto args = makeRemoteCallbackArgs(cbHandle, *cbState, std::move(swr));

            LOGV2_DEBUG(9311402,
                        3,
                        "Received remote exhaust response",
                        "requestId"_attr = cbState->request.id,
                        "response"_attr =
                            redact(args.response.isOK() ? args.response.toString()
                                                        : args.response.status.toString()));

            cbState->callback(args);

            if (args.response.moreToCome) {
                _continueExhaustCommand(std::move(cbHandle), std::move(cbState), rdr);
            } else {
                LOGV2_DEBUG(9311403,
                            3,
                            "Exhaust command finished",
                            "requestId"_attr = cbState->request.id,
                            "request"_attr =
                                redact(args.response.isOK() ? args.response.toString()
                                                            : args.response.status.toString()));

                _unregisterCallbackState(cbState);
            }
        });
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::scheduleExhaustRemoteCommand(
    const RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {
    RemoteCommandRequest scheduledRequest = request;
    scheduledRequest.dateScheduled = _net->now();

    auto cbState = std::make_shared<RemoteCallbackState>(scheduledRequest, cb, baton);
    auto swCbHandle = _registerCallbackState(cbState);
    if (!swCbHandle.isOK()) {
        return swCbHandle;
    }
    _networkInProgress.addAndFetch(1);

    LOGV2_DEBUG(4495133,
                3,
                "Scheduling exhaust remote command request",
                "requestId"_attr = scheduledRequest.id,
                "request"_attr = redact(scheduledRequest.toString()));

    try {
        _net->startExhaustCommand(
                swCbHandle.getValue(), scheduledRequest, baton, cbState->cancelSource.token())
            .thenRunOn(makeGuaranteedExecutor(baton, _pool))
            .getAsync(
                [this, cbState, cbHandle = swCbHandle.getValue()](
                    StatusWith<std::shared_ptr<NetworkInterface::ExhaustResponseReader>> swr) {
                    if (!swr.isOK()) {
                        LOGV2_DEBUG(9311404,
                                    3,
                                    "Exhaust remote command request failed",
                                    "requestId"_attr = cbState->request.id,
                                    "error"_attr = swr.getStatus());
                        RemoteCommandCallbackArgs args(this,
                                                       cbHandle,
                                                       cbState->request,
                                                       {cbState->request.target, swr.getStatus()});
                        cbState->callback(args);
                        _unregisterCallbackState(cbState);
                        _networkInProgress.fetchAndSubtract(1);
                        return;
                    }
                    _continueExhaustCommand(cbHandle, cbState, swr.getValue());
                });
    } catch (const DBException& ex) {
        _unregisterCallbackState(cbState);
        _networkInProgress.fetchAndSubtract(1);
        return ex.toStatus();
    }

    return swCbHandle;
}

bool ThreadPoolTaskExecutor::hasTasks() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return !_inProgress.empty();
}

bool ThreadPoolTaskExecutor::_inShutdown_inlock() const {
    return _state >= joinRequired;
}

void ThreadPoolTaskExecutor::_setState_inlock(State newState) {
    if (newState == _state) {
        return;
    }
    _state = newState;
    _stateChange.notify_all();
}

void ThreadPoolTaskExecutor::dropConnections(const HostAndPort& target, const Status& status) {
    _net->dropConnections(target, status);
}

}  // namespace executor
}  // namespace mongo

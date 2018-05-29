/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/executor/thread_pool_task_executor.h"

#include <boost/optional.hpp>
#include <iterator>
#include <utility>

#include "mongo/base/checked_cast.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/baton.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {

namespace {
MONGO_FAIL_POINT_DEFINE(scheduleIntoPoolSpinsUntilThreadPoolShutsDown);
}

class ThreadPoolTaskExecutor::CallbackState : public TaskExecutor::CallbackState {
    MONGO_DISALLOW_COPYING(CallbackState);

public:
    static std::shared_ptr<CallbackState> make(CallbackFn&& cb,
                                               Date_t readyDate,
                                               const transport::BatonHandle& baton) {
        return std::make_shared<CallbackState>(std::move(cb), readyDate, baton);
    }

    /**
     * Do not call directly. Use make.
     */
    CallbackState(CallbackFn&& cb, Date_t theReadyDate, const transport::BatonHandle& baton)
        : callback(std::move(cb)), readyDate(theReadyDate), baton(baton) {}

    virtual ~CallbackState() = default;

    bool isCanceled() const override {
        return canceled.load() > 0;
    }

    void cancel() override {
        MONGO_UNREACHABLE;
    }

    void waitForCompletion() override {
        MONGO_UNREACHABLE;
    }

    // All fields except for "canceled" are guarded by the owning task executor's _mutex. The
    // "canceled" field may be observed without holding _mutex, but may only be set while holding
    // _mutex.

    CallbackFn callback;
    AtomicUInt32 canceled{0U};
    WorkQueue::iterator iter;
    Date_t readyDate;
    bool isNetworkOperation = false;
    AtomicWord<bool> isFinished{false};
    boost::optional<stdx::condition_variable> finishedCondition;
    transport::BatonHandle baton;
};

class ThreadPoolTaskExecutor::EventState : public TaskExecutor::EventState {
    MONGO_DISALLOW_COPYING(EventState);

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

    // All fields guarded by the owning task executor's _mutex.

    bool isSignaledFlag = false;
    stdx::condition_variable isSignaledCondition;
    EventList::iterator iter;
    WorkQueue waiters;
};

ThreadPoolTaskExecutor::ThreadPoolTaskExecutor(std::unique_ptr<ThreadPoolInterface> pool,
                                               std::shared_ptr<NetworkInterface> net)
    : _net(std::move(net)), _pool(std::move(pool)) {}

ThreadPoolTaskExecutor::~ThreadPoolTaskExecutor() {
    shutdown();
    auto lk = _join(stdx::unique_lock<stdx::mutex>(_mutex));
    invariant(_state == shutdownComplete);
}

void ThreadPoolTaskExecutor::startup() {
    _net->startup();
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown_inlock()) {
        return;
    }
    invariant(_state == preStart);
    _setState_inlock(running);
    _pool->startup();
}

void ThreadPoolTaskExecutor::shutdown() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_inShutdown_inlock()) {
        invariant(_networkInProgressQueue.empty());
        invariant(_sleepersQueue.empty());
        return;
    }
    _setState_inlock(joinRequired);
    WorkQueue pending;
    pending.splice(pending.end(), _networkInProgressQueue);
    pending.splice(pending.end(), _sleepersQueue);
    for (auto&& eventState : _unsignaledEvents) {
        pending.splice(pending.end(), eventState->waiters);
    }
    for (auto&& cbState : pending) {
        cbState->canceled.store(1);
    }
    for (auto&& cbState : _poolInProgressQueue) {
        cbState->canceled.store(1);
    }
    scheduleIntoPool_inlock(&pending, std::move(lk));
    _pool->shutdown();
}

void ThreadPoolTaskExecutor::join() {
    _join(stdx::unique_lock<stdx::mutex>(_mutex));
}

stdx::unique_lock<stdx::mutex> ThreadPoolTaskExecutor::_join(stdx::unique_lock<stdx::mutex> lk) {
    _stateChange.wait(lk, [this] {
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
        return lk;
    }
    invariant(_state == joinRequired);
    _setState_inlock(joining);
    lk.unlock();
    _pool->join();
    lk.lock();
    while (!_unsignaledEvents.empty()) {
        auto eventState = _unsignaledEvents.front();
        invariant(eventState->waiters.empty());
        EventHandle event;
        setEventForHandle(&event, std::move(eventState));
        signalEvent_inlock(event, std::move(lk));
        lk = stdx::unique_lock<stdx::mutex>(_mutex);
    }
    lk.unlock();
    _net->shutdown();

    lk.lock();
    // The _poolInProgressQueue may not be empty if the network interface attempted to schedule work
    // into _pool after _pool->shutdown(). Because _pool->join() has returned, we know that any
    // items left in _poolInProgressQueue will never be processed by another thread, so we process
    // them now.
    while (!_poolInProgressQueue.empty()) {
        auto cbState = _poolInProgressQueue.front();
        lk.unlock();
        runCallback(std::move(cbState));
        lk.lock();
    }
    invariant(_networkInProgressQueue.empty());
    invariant(_sleepersQueue.empty());
    invariant(_unsignaledEvents.empty());
    _setState_inlock(shutdownComplete);
    return lk;
}

void ThreadPoolTaskExecutor::appendDiagnosticBSON(BSONObjBuilder* b) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // ThreadPool details
    // TODO: fill in
    BSONObjBuilder poolCounters(b->subobjStart("pool"));
    poolCounters.appendIntOrLL("inProgressCount", _poolInProgressQueue.size());
    poolCounters.done();

    // Queues
    BSONObjBuilder queues(b->subobjStart("queues"));
    queues.appendIntOrLL("networkInProgress", _networkInProgressQueue.size());
    queues.appendIntOrLL("sleepers", _sleepersQueue.size());
    queues.done();

    b->appendIntOrLL("unsignaledEvents", _unsignaledEvents.size());
    b->append("shuttingDown", _inShutdown_inlock());
    b->append("networkInterface", _net->getDiagnosticString());
}

Date_t ThreadPoolTaskExecutor::now() {
    return _net->now();
}

StatusWith<TaskExecutor::EventHandle> ThreadPoolTaskExecutor::makeEvent() {
    auto el = makeSingletonEventList();
    EventHandle event;
    setEventForHandle(&event, el.front());
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown_inlock()) {
        return {ErrorCodes::ShutdownInProgress, "Shutdown in progress"};
    }
    _unsignaledEvents.splice(_unsignaledEvents.end(), el);
    return event;
}

void ThreadPoolTaskExecutor::signalEvent(const EventHandle& event) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    signalEvent_inlock(event, std::move(lk));
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::onEvent(const EventHandle& event,
                                                                         const CallbackFn& work) {
    if (!event.isValid()) {
        return {ErrorCodes::BadValue, "Passed invalid event handle to onEvent"};
    }
    auto wq = makeSingletonWorkQueue(work, nullptr);
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto eventState = checked_cast<EventState*>(getEventFromHandle(event));
    auto cbHandle = enqueueCallbackState_inlock(&eventState->waiters, &wq);
    if (!cbHandle.isOK()) {
        return cbHandle;
    }
    if (eventState->isSignaledFlag) {
        scheduleIntoPool_inlock(&eventState->waiters, std::move(lk));
    }
    return cbHandle;
}

StatusWith<stdx::cv_status> ThreadPoolTaskExecutor::waitForEvent(OperationContext* opCtx,
                                                                 const EventHandle& event,
                                                                 Date_t deadline) {
    invariant(opCtx);
    invariant(event.isValid());
    auto eventState = checked_cast<EventState*>(getEventFromHandle(event));
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // std::condition_variable::wait() can wake up spuriously, so we have to loop until the event
    // is signalled or we time out.
    while (!eventState->isSignaledFlag) {
        auto status = opCtx->waitForConditionOrInterruptNoAssertUntil(
            eventState->isSignaledCondition, lk, deadline);

        if (!status.isOK() || stdx::cv_status::timeout == status) {
            return status;
        }
    }

    return stdx::cv_status::no_timeout;
}

void ThreadPoolTaskExecutor::waitForEvent(const EventHandle& event) {
    invariant(event.isValid());
    auto eventState = checked_cast<EventState*>(getEventFromHandle(event));
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    while (!eventState->isSignaledFlag) {
        eventState->isSignaledCondition.wait(lk);
    }
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::scheduleWork(
    const CallbackFn& work) {
    auto wq = makeSingletonWorkQueue(work, nullptr);
    WorkQueue temp;
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto cbHandle = enqueueCallbackState_inlock(&temp, &wq);
    if (!cbHandle.isOK()) {
        return cbHandle;
    }
    scheduleIntoPool_inlock(&temp, std::move(lk));
    return cbHandle;
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::scheduleWorkAt(
    Date_t when, const CallbackFn& work) {
    if (when <= now()) {
        return scheduleWork(work);
    }
    auto wq = makeSingletonWorkQueue(work, nullptr, when);
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto cbHandle = enqueueCallbackState_inlock(&_sleepersQueue, &wq);
    if (!cbHandle.isOK()) {
        return cbHandle;
    }
    lk.unlock();
    _net->setAlarm(when,
                   [this, cbHandle] {
                       auto cbState =
                           checked_cast<CallbackState*>(getCallbackFromHandle(cbHandle.getValue()));
                       if (cbState->canceled.load()) {
                           return;
                       }
                       stdx::unique_lock<stdx::mutex> lk(_mutex);
                       if (cbState->canceled.load()) {
                           return;
                       }
                       scheduleIntoPool_inlock(&_sleepersQueue, cbState->iter, std::move(lk));
                   },
                   nullptr)
        .transitional_ignore();

    return cbHandle;
}

namespace {

using ResponseStatus = TaskExecutor::ResponseStatus;

// If the request received a connection from the pool but failed in its execution,
// convert the raw Status in cbData to a RemoteCommandResponse so that the callback,
// which expects a RemoteCommandResponse as part of RemoteCommandCallbackArgs,
// can be run despite a RemoteCommandResponse never having been created.
void remoteCommandFinished(const TaskExecutor::CallbackArgs& cbData,
                           const TaskExecutor::RemoteCommandCallbackFn& cb,
                           const RemoteCommandRequest& request,
                           const ResponseStatus& rs) {
    cb(TaskExecutor::RemoteCommandCallbackArgs(cbData.executor, cbData.myHandle, request, rs));
}

// If the request failed to receive a connection from the pool,
// convert the raw Status in cbData to a RemoteCommandResponse so that the callback,
// which expects a RemoteCommandResponse as part of RemoteCommandCallbackArgs,
// can be run despite a RemoteCommandResponse never having been created.
void remoteCommandFailedEarly(const TaskExecutor::CallbackArgs& cbData,
                              const TaskExecutor::RemoteCommandCallbackFn& cb,
                              const RemoteCommandRequest& request) {
    invariant(!cbData.status.isOK());
    cb(TaskExecutor::RemoteCommandCallbackArgs(
        cbData.executor, cbData.myHandle, request, {cbData.status}));
}
}  // namespace

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::scheduleRemoteCommand(
    const RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const transport::BatonHandle& baton) {
    RemoteCommandRequest scheduledRequest = request;
    if (request.timeout == RemoteCommandRequest::kNoTimeout) {
        scheduledRequest.expirationDate = RemoteCommandRequest::kNoExpirationDate;
    } else {
        scheduledRequest.expirationDate = _net->now() + scheduledRequest.timeout;
    }

    // In case the request fails to even get a connection from the pool,
    // we wrap the callback in a method that prepares its input parameters.
    auto wq = makeSingletonWorkQueue(
        [scheduledRequest, cb](const CallbackArgs& cbData) {
            remoteCommandFailedEarly(cbData, cb, scheduledRequest);
        },
        baton);
    wq.front()->isNetworkOperation = true;
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto cbHandle = enqueueCallbackState_inlock(&_networkInProgressQueue, &wq);
    if (!cbHandle.isOK())
        return cbHandle;
    const auto cbState = _networkInProgressQueue.back();
    LOG(3) << "Scheduling remote command request: " << redact(scheduledRequest.toString());
    lk.unlock();
    _net->startCommand(
            cbHandle.getValue(),
            scheduledRequest,
            [this, scheduledRequest, cbState, cb](const ResponseStatus& response) {
                using std::swap;
                CallbackFn newCb = [cb, scheduledRequest, response](const CallbackArgs& cbData) {
                    remoteCommandFinished(cbData, cb, scheduledRequest, response);
                };
                stdx::unique_lock<stdx::mutex> lk(_mutex);
                if (_inShutdown_inlock()) {
                    return;
                }
                LOG(3) << "Received remote response: "
                       << redact(response.isOK() ? response.toString()
                                                 : response.status.toString());
                swap(cbState->callback, newCb);
                scheduleIntoPool_inlock(&_networkInProgressQueue, cbState->iter, std::move(lk));
            },
            baton)
        .transitional_ignore();
    return cbHandle;
}

void ThreadPoolTaskExecutor::cancel(const CallbackHandle& cbHandle) {
    invariant(cbHandle.isValid());
    auto cbState = checked_cast<CallbackState*>(getCallbackFromHandle(cbHandle));
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_inShutdown_inlock()) {
        return;
    }
    cbState->canceled.store(1);
    if (cbState->isNetworkOperation) {
        lk.unlock();
        _net->cancelCommand(cbHandle, cbState->baton);
        return;
    }
    if (cbState->readyDate != Date_t{}) {
        // This callback might still be in the sleeper queue; if it is, schedule it now
        // rather than when the alarm fires.
        auto iter = std::find_if(_sleepersQueue.begin(),
                                 _sleepersQueue.end(),
                                 [cbState](const std::shared_ptr<CallbackState>& other) {
                                     return cbState == other.get();
                                 });
        if (iter != _sleepersQueue.end()) {
            invariant(iter == cbState->iter);
            scheduleIntoPool_inlock(&_sleepersQueue, cbState->iter, std::move(lk));
        }
    }
}

void ThreadPoolTaskExecutor::wait(const CallbackHandle& cbHandle) {
    invariant(cbHandle.isValid());
    auto cbState = checked_cast<CallbackState*>(getCallbackFromHandle(cbHandle));
    if (cbState->isFinished.load()) {
        return;
    }
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (!cbState->finishedCondition) {
        cbState->finishedCondition.emplace();
    }
    while (!cbState->isFinished.load()) {
        cbState->finishedCondition->wait(lk);
    }
}

void ThreadPoolTaskExecutor::appendConnectionStats(ConnectionPoolStats* stats) const {
    _net->appendConnectionStats(stats);
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::enqueueCallbackState_inlock(
    WorkQueue* queue, WorkQueue* wq) {
    if (_inShutdown_inlock()) {
        return {ErrorCodes::ShutdownInProgress, "Shutdown in progress"};
    }
    invariant(!wq->empty());
    queue->splice(queue->end(), *wq, wq->begin());
    invariant(wq->empty());
    CallbackHandle cbHandle;
    setCallbackForHandle(&cbHandle, queue->back());
    return cbHandle;
}

ThreadPoolTaskExecutor::WorkQueue ThreadPoolTaskExecutor::makeSingletonWorkQueue(
    CallbackFn work, const transport::BatonHandle& baton, Date_t when) {
    WorkQueue result;
    result.emplace_front(CallbackState::make(std::move(work), when, baton));
    result.front()->iter = result.begin();
    return result;
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
    invariant(!eventState->isSignaledFlag);
    eventState->isSignaledFlag = true;
    eventState->isSignaledCondition.notify_all();
    _unsignaledEvents.erase(eventState->iter);
    scheduleIntoPool_inlock(&eventState->waiters, std::move(lk));
}

void ThreadPoolTaskExecutor::scheduleIntoPool_inlock(WorkQueue* fromQueue,
                                                     stdx::unique_lock<stdx::mutex> lk) {
    scheduleIntoPool_inlock(fromQueue, fromQueue->begin(), fromQueue->end(), std::move(lk));
}

void ThreadPoolTaskExecutor::scheduleIntoPool_inlock(WorkQueue* fromQueue,
                                                     const WorkQueue::iterator& iter,
                                                     stdx::unique_lock<stdx::mutex> lk) {
    scheduleIntoPool_inlock(fromQueue, iter, std::next(iter), std::move(lk));
}

void ThreadPoolTaskExecutor::scheduleIntoPool_inlock(WorkQueue* fromQueue,
                                                     const WorkQueue::iterator& begin,
                                                     const WorkQueue::iterator& end,
                                                     stdx::unique_lock<stdx::mutex> lk) {
    dassert(fromQueue != &_poolInProgressQueue);
    std::vector<std::shared_ptr<CallbackState>> todo(begin, end);
    _poolInProgressQueue.splice(_poolInProgressQueue.end(), *fromQueue, begin, end);

    lk.unlock();

    if (MONGO_FAIL_POINT(scheduleIntoPoolSpinsUntilThreadPoolShutsDown)) {
        scheduleIntoPoolSpinsUntilThreadPoolShutsDown.setMode(FailPoint::off);
        while (_pool->schedule([] {}) != ErrorCodes::ShutdownInProgress) {
            sleepmillis(100);
        }
    }

    for (const auto& cbState : todo) {
        if (cbState->baton) {
            cbState->baton->schedule([this, cbState] { runCallback(std::move(cbState)); });
        } else {
            const auto status =
                _pool->schedule([this, cbState] { runCallback(std::move(cbState)); });
            if (status == ErrorCodes::ShutdownInProgress)
                break;
            fassert(28735, status);
        }
    }
    _net->signalWorkAvailable();
}

void ThreadPoolTaskExecutor::runCallback(std::shared_ptr<CallbackState> cbStateArg) {
    CallbackHandle cbHandle;
    setCallbackForHandle(&cbHandle, cbStateArg);
    CallbackArgs args(this,
                      std::move(cbHandle),
                      cbStateArg->canceled.load()
                          ? Status({ErrorCodes::CallbackCanceled, "Callback canceled"})
                          : Status::OK());
    invariant(!cbStateArg->isFinished.load());
    {
        // After running callback function, clear 'cbStateArg->callback' to release any resources
        // that might be held by this function object.
        // Swap 'cbStateArg->callback' with temporary copy before running callback for exception
        // safety.
        TaskExecutor::CallbackFn callback;
        std::swap(cbStateArg->callback, callback);
        callback(std::move(args));
    }
    cbStateArg->isFinished.store(true);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _poolInProgressQueue.erase(cbStateArg->iter);
    if (cbStateArg->finishedCondition) {
        cbStateArg->finishedCondition->notify_all();
    }
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

void ThreadPoolTaskExecutor::dropConnections(const HostAndPort& hostAndPort) {
    _net->dropConnections(hostAndPort);
}

}  // namespace executor
}  // namespace mongo

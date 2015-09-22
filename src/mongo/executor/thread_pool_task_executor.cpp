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

#include <iterator>

#include "mongo/base/checked_cast.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/executor/network_interface.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/log.h"

namespace mongo {
namespace executor {

class ThreadPoolTaskExecutor::CallbackState : public TaskExecutor::CallbackState {
    MONGO_DISALLOW_COPYING(CallbackState);

public:
    static std::shared_ptr<CallbackState> make(CallbackFn cb,
                                               EventHandle finishedEvent,
                                               Date_t readyDate) {
        return std::make_shared<CallbackState>(std::move(cb), std::move(finishedEvent), readyDate);
    }

    /**
     * Do not call directly. Use make.
     */
    CallbackState(CallbackFn cb, EventHandle theFinishedEvent, Date_t theReadyDate)
        : callback(std::move(cb)),
          finishedEvent(std::move(theFinishedEvent)),
          readyDate(theReadyDate) {}

    virtual ~CallbackState() = default;

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
    EventHandle finishedEvent;
    AtomicUInt32 canceled{0U};
    WorkQueue::iterator iter;
    Date_t readyDate;
    bool isNetworkOperation = false;
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
                                               std::unique_ptr<NetworkInterface> net)
    : _net(std::move(net)), _pool(std::move(pool)) {}

ThreadPoolTaskExecutor::~ThreadPoolTaskExecutor() {}

void ThreadPoolTaskExecutor::startup() {
    _net->startup();
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown) {
        return;
    }
    _pool->startup();
}

void ThreadPoolTaskExecutor::shutdown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _inShutdown = true;
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
    scheduleIntoPool_inlock(&pending);
    _net->signalWorkAvailable();
    _pool->shutdown();
}

void ThreadPoolTaskExecutor::join() {
    _pool->join();
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (!_unsignaledEvents.empty()) {
        auto eventState = _unsignaledEvents.front();
        invariant(eventState->waiters.empty());
        EventHandle event;
        setEventForHandle(&event, std::move(eventState));
        signalEvent_inlock(event);
    }
    lk.unlock();
    _net->shutdown();
    lk.lock();
    invariant(_poolInProgressQueue.empty());
    invariant(_networkInProgressQueue.empty());
    invariant(_sleepersQueue.empty());
    invariant(_unsignaledEvents.empty());
}

std::string ThreadPoolTaskExecutor::getDiagnosticString() {
    return {};
}

Date_t ThreadPoolTaskExecutor::now() {
    return _net->now();
}

StatusWith<TaskExecutor::EventHandle> ThreadPoolTaskExecutor::makeEvent() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return makeEvent_inlock();
}

void ThreadPoolTaskExecutor::signalEvent(const EventHandle& event) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    signalEvent_inlock(event);
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::onEvent(const EventHandle& event,
                                                                         const CallbackFn& work) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (!event.isValid()) {
        return {ErrorCodes::BadValue, "Passed invalid event handle to onEvent"};
    }
    auto eventState = checked_cast<EventState*>(getEventFromHandle(event));
    auto cbHandle = enqueueCallbackState_inlock(&eventState->waiters, work);
    if (!cbHandle.isOK()) {
        return cbHandle;
    }
    if (eventState->isSignaledFlag) {
        scheduleIntoPool_inlock(&eventState->waiters);
    }
    return cbHandle;
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
    WorkQueue temp;
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto cbHandle = enqueueCallbackState_inlock(&temp, work);
    if (!cbHandle.isOK()) {
        return cbHandle;
    }
    scheduleIntoPool_inlock(&temp);
    return cbHandle;
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::scheduleWorkAt(
    Date_t when, const CallbackFn& work) {
    if (when <= now()) {
        return scheduleWork(work);
    }
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto cbHandle = enqueueCallbackState_inlock(&_sleepersQueue, work, when);
    if (!cbHandle.isOK()) {
        return cbHandle;
    }
    _net->setAlarm(when,
                   [this, when, cbHandle] {
                       auto cbState =
                           checked_cast<CallbackState*>(getCallbackFromHandle(cbHandle.getValue()));
                       if (cbState->canceled.load()) {
                           return;
                       }
                       invariant(now() >= when);
                       stdx::lock_guard<stdx::mutex> lk(_mutex);
                       scheduleIntoPool_inlock(&_sleepersQueue, cbState->iter);
                   });

    return cbHandle;
}

namespace {
void remoteCommandFinished(const TaskExecutor::CallbackArgs& cbData,
                           const TaskExecutor::RemoteCommandCallbackFn& cb,
                           const RemoteCommandRequest& request,
                           const TaskExecutor::ResponseStatus& response) {
    using ResponseStatus = TaskExecutor::ResponseStatus;
    if (cbData.status.isOK()) {
        cb(TaskExecutor::RemoteCommandCallbackArgs(
            cbData.executor, cbData.myHandle, request, response));
    } else {
        cb(TaskExecutor::RemoteCommandCallbackArgs(
            cbData.executor, cbData.myHandle, request, ResponseStatus(cbData.status)));
    }
}

void remoteCommandFailedEarly(const TaskExecutor::CallbackArgs& cbData,
                              const TaskExecutor::RemoteCommandCallbackFn& cb,
                              const RemoteCommandRequest& request) {
    using ResponseStatus = TaskExecutor::ResponseStatus;
    invariant(!cbData.status.isOK());
    cb(TaskExecutor::RemoteCommandCallbackArgs(
        cbData.executor, cbData.myHandle, request, ResponseStatus(cbData.status)));
}
}  // namespace

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::scheduleRemoteCommand(
    const RemoteCommandRequest& request, const RemoteCommandCallbackFn& cb) {
    RemoteCommandRequest scheduledRequest = request;
    if (request.timeout == RemoteCommandRequest::kNoTimeout) {
        scheduledRequest.expirationDate = RemoteCommandRequest::kNoExpirationDate;
    } else {
        scheduledRequest.expirationDate = _net->now() + scheduledRequest.timeout;
    }
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto cbHandle =
        enqueueCallbackState_inlock(&_networkInProgressQueue,
                                    [scheduledRequest, cb](const CallbackArgs& cbData) {
                                        remoteCommandFailedEarly(cbData, cb, scheduledRequest);
                                    });
    if (!cbHandle.isOK())
        return cbHandle;
    const auto& cbState = _networkInProgressQueue.back();
    cbState->isNetworkOperation = true;
    LOG(4) << "Scheduling remote command request: " << scheduledRequest.toString();
    _net->startCommand(cbHandle.getValue(),
                       scheduledRequest,
                       [this, scheduledRequest, cbState, cb](const ResponseStatus& response) {
                           stdx::lock_guard<stdx::mutex> lk(_mutex);
                           if (_inShutdown) {
                               return;
                           }
                           LOG(3) << "Received remote response: "
                                  << (response.isOK() ? response.getValue().toString()
                                                      : response.getStatus().toString());
                           cbState->callback =
                               [cb, scheduledRequest, response](const CallbackArgs& cbData) {
                                   remoteCommandFinished(cbData, cb, scheduledRequest, response);
                               };
                           scheduleIntoPool_inlock(&_networkInProgressQueue, cbState->iter);
                       });
    return cbHandle;
}

void ThreadPoolTaskExecutor::cancel(const CallbackHandle& cbHandle) {
    invariant(cbHandle.isValid());
    auto cbState = checked_cast<CallbackState*>(getCallbackFromHandle(cbHandle));
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    cbState->canceled.store(1);
    if (cbState->isNetworkOperation) {
        lk.unlock();
        _net->cancelCommand(cbHandle);
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
            scheduleIntoPool_inlock(&_sleepersQueue, cbState->iter);
        }
    }
}

void ThreadPoolTaskExecutor::wait(const CallbackHandle& cbHandle) {
    invariant(cbHandle.isValid());
    auto cbState = checked_cast<CallbackState*>(getCallbackFromHandle(cbHandle));
    waitForEvent(cbState->finishedEvent);
}

void ThreadPoolTaskExecutor::appendConnectionStats(BSONObjBuilder* b) {
    _net->appendConnectionStats(b);
}

void ThreadPoolTaskExecutor::cancelAllCommands() {
    _net->cancelAllCommands();
}

StatusWith<TaskExecutor::CallbackHandle> ThreadPoolTaskExecutor::enqueueCallbackState_inlock(
    WorkQueue* queue, CallbackFn work, Date_t when) {
    auto event = makeEvent_inlock();
    if (!event.isOK()) {
        return event.getStatus();
    }
    queue->emplace_back(CallbackState::make(std::move(work), std::move(event.getValue()), when));
    queue->back()->iter = std::prev(queue->end());
    CallbackHandle cbHandle;
    setCallbackForHandle(&cbHandle, queue->back());
    return cbHandle;
}

StatusWith<ThreadPoolTaskExecutor::EventHandle> ThreadPoolTaskExecutor::makeEvent_inlock() {
    if (_inShutdown) {
        return {ErrorCodes::ShutdownInProgress, "Shutdown in progress"};
    }
    _unsignaledEvents.emplace_front(EventState::make());
    _unsignaledEvents.front()->iter = _unsignaledEvents.begin();
    EventHandle event;
    setEventForHandle(&event, _unsignaledEvents.front());
    return event;
}

void ThreadPoolTaskExecutor::signalEvent_inlock(const EventHandle& event) {
    invariant(event.isValid());
    auto eventState = checked_cast<EventState*>(getEventFromHandle(event));
    invariant(!eventState->isSignaledFlag);
    eventState->isSignaledFlag = true;
    eventState->isSignaledCondition.notify_all();
    scheduleIntoPool_inlock(&eventState->waiters);
    _unsignaledEvents.erase(eventState->iter);
}

void ThreadPoolTaskExecutor::scheduleIntoPool_inlock(WorkQueue* fromQueue) {
    scheduleIntoPool_inlock(fromQueue, fromQueue->begin(), fromQueue->end());
}

void ThreadPoolTaskExecutor::scheduleIntoPool_inlock(WorkQueue* fromQueue,
                                                     const WorkQueue::iterator& iter) {
    scheduleIntoPool_inlock(fromQueue, iter, std::next(iter));
}

void ThreadPoolTaskExecutor::scheduleIntoPool_inlock(WorkQueue* fromQueue,
                                                     const WorkQueue::iterator& begin,
                                                     const WorkQueue::iterator& end) {
    dassert(fromQueue != &_poolInProgressQueue);
    std::for_each(
        begin,
        end,
        [this](const std::shared_ptr<CallbackState>& cbState) {
            fassert(28735, _pool->schedule([this, cbState] { runCallback(std::move(cbState)); }));
        });
    _poolInProgressQueue.splice(_poolInProgressQueue.end(), *fromQueue, begin, end);
    _net->signalWorkAvailable();
}

void ThreadPoolTaskExecutor::runCallback(std::shared_ptr<CallbackState> cbStateArg) {
    auto cbStatePtr = cbStateArg.get();
    CallbackHandle cbHandle;
    setCallbackForHandle(&cbHandle, std::move(cbStateArg));
    CallbackArgs args(this,
                      std::move(cbHandle),
                      cbStatePtr->canceled.load()
                          ? Status({ErrorCodes::CallbackCanceled, "Callback canceled"})
                          : Status::OK());
    cbStatePtr->callback(std::move(args));
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (cbStatePtr->finishedEvent.isValid()) {
        signalEvent_inlock(cbStatePtr->finishedEvent);
    }
    _poolInProgressQueue.erase(cbStatePtr->iter);
}

}  // namespace executor
}  // namespace mongo

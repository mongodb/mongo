/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_executor.h"

#include <limits>

#include "mongo/db/client.h"
#include "mongo/db/repl/database_task.h"
#include "mongo/executor/network_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {

const char kReplicationExecutorThreadName[] = "ReplicationExecutor";

stdx::function<void()> makeNoExcept(const stdx::function<void()>& fn);

}  // namespace

using executor::NetworkInterface;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

ReplicationExecutor::ReplicationExecutor(NetworkInterface* netInterface, int64_t prngSeed)
    : _random(prngSeed),
      _networkInterface(netInterface),
      _inShutdown(false),
      _dblockWorkers(OldThreadPool::DoNotStartThreadsTag(), 3, "replExecDBWorker-"),
      _dblockTaskRunner(&_dblockWorkers),
      _dblockExclusiveLockTaskRunner(&_dblockWorkers) {}

ReplicationExecutor::~ReplicationExecutor() {
    // join must have been called
    invariant(!_executorThread.joinable());
}

BSONObj ReplicationExecutor::getDiagnosticBSON() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    BSONObjBuilder builder;

    // Counters
    BSONObjBuilder counters(builder.subobjStart("counters"));
    counters.appendIntOrLL("eventCreated", _counterCreatedEvents);
    counters.appendIntOrLL("eventWait", _counterCreatedEvents);
    counters.appendIntOrLL("cancels", _counterCancels);
    counters.appendIntOrLL("waits", _counterWaits);
    counters.appendIntOrLL("scheduledNetCmd", _counterScheduledCommands);
    counters.appendIntOrLL("scheduledDBWork", _counterScheduledDBWorks);
    counters.appendIntOrLL("scheduledXclWork", _counterScheduledExclusiveWorks);
    counters.appendIntOrLL("scheduledWorkAt", _counterScheduledWorkAts);
    counters.appendIntOrLL("scheduledWork", _counterScheduledWorks);
    counters.appendIntOrLL("schedulingFailures", _counterSchedulingFailures);
    counters.done();

    // Queues
    BSONObjBuilder queues(builder.subobjStart("queues"));
    queues.appendIntOrLL("networkInProgress", _networkInProgressQueue.size());
    queues.appendIntOrLL("dbWorkInProgress", _dbWorkInProgressQueue.size());
    queues.appendIntOrLL("exclusiveInProgress", _exclusiveLockInProgressQueue.size());
    queues.appendIntOrLL("sleepers", _sleepersQueue.size());
    queues.appendIntOrLL("ready", _readyQueue.size());
    queues.appendIntOrLL("free", _freeQueue.size());
    queues.done();

    builder.appendIntOrLL("unsignaledEvents", _unsignaledEvents.size());
    builder.appendIntOrLL("eventWaiters", _totalEventWaiters);
    builder.append("shuttingDown", _inShutdown);
    builder.append("networkInterface", _networkInterface->getDiagnosticString());
    return builder.obj();
}

std::string ReplicationExecutor::getDiagnosticString() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _getDiagnosticString_inlock();
}

std::string ReplicationExecutor::_getDiagnosticString_inlock() const {
    str::stream output;
    output << "ReplicationExecutor";
    output << " networkInProgress:" << _networkInProgressQueue.size();
    output << " dbWorkInProgress:" << _dbWorkInProgressQueue.size();
    output << " exclusiveInProgress:" << _exclusiveLockInProgressQueue.size();
    output << " sleeperQueue:" << _sleepersQueue.size();
    output << " ready:" << _readyQueue.size();
    output << " free:" << _freeQueue.size();
    output << " unsignaledEvents:" << _unsignaledEvents.size();
    output << " eventWaiters:" << _totalEventWaiters;
    output << " shuttingDown:" << _inShutdown;
    output << " networkInterface:" << _networkInterface->getDiagnosticString();
    return output;
}

Date_t ReplicationExecutor::now() {
    return _networkInterface->now();
}

void ReplicationExecutor::run() {
    setThreadName(kReplicationExecutorThreadName);
    Client::initThread(kReplicationExecutorThreadName);
    _networkInterface->startup();
    _dblockWorkers.startThreads();
    std::pair<WorkItem, CallbackHandle> work;
    while ((work = getWork()).first.callback.isValid()) {
        {
            stdx::lock_guard<stdx::mutex> lk(_terribleExLockSyncMutex);
            const Callback* callback = _getCallbackFromHandle(work.first.callback);
            const Status inStatus = callback->_isCanceled
                ? Status(ErrorCodes::CallbackCanceled, "Callback canceled")
                : Status::OK();
            makeNoExcept(
                stdx::bind(callback->_callbackFn, CallbackArgs(this, work.second, inStatus)))();
        }
        signalEvent(work.first.finishedEvent);
    }
    finishShutdown();
    _networkInterface->shutdown();
}

void ReplicationExecutor::startup() {
    // Ensure that thread has not yet been created
    invariant(!_executorThread.joinable());

    _executorThread = stdx::thread([this] { run(); });
}

void ReplicationExecutor::shutdown() {
    // Correct shutdown needs to:
    // * Disable future work queueing.
    // * drain all of the unsignaled events, sleepers, and ready queue, by running those
    //   callbacks with a "shutdown" or "canceled" status.
    // * Signal all threads blocked in waitForEvent, and wait for them to return from that method.
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown)
        return;
    _inShutdown = true;

    _readyQueue.splice(_readyQueue.end(), _dbWorkInProgressQueue);
    _readyQueue.splice(_readyQueue.end(), _exclusiveLockInProgressQueue);
    _readyQueue.splice(_readyQueue.end(), _networkInProgressQueue);
    _readyQueue.splice(_readyQueue.end(), _sleepersQueue);
    for (auto event : _unsignaledEvents) {
        _readyQueue.splice(_readyQueue.end(), _getEventFromHandle(event)->_waiters);
    }
    for (auto readyWork : _readyQueue) {
        auto callback = _getCallbackFromHandle(readyWork.callback);
        callback->_isCanceled = true;
        callback->_isSleeper = false;
    }

    _networkInterface->signalWorkAvailable();
}

void ReplicationExecutor::join() {
    invariant(_executorThread.joinable());
    _executorThread.join();
}

void ReplicationExecutor::finishShutdown() {
    _dblockExclusiveLockTaskRunner.cancel();
    _dblockTaskRunner.cancel();
    _dblockWorkers.join();
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_inShutdown);
    invariant(_dbWorkInProgressQueue.empty());
    invariant(_exclusiveLockInProgressQueue.empty());
    invariant(_readyQueue.empty());
    invariant(_sleepersQueue.empty());

    while (!_unsignaledEvents.empty()) {
        EventList::iterator eventIter = _unsignaledEvents.begin();
        invariant(_getEventFromHandle(*eventIter)->_waiters.empty());
        signalEvent_inlock(*eventIter);
    }

    while (_totalEventWaiters > 0)
        _noMoreWaitingThreads.wait(lk);

    invariant(_dbWorkInProgressQueue.empty());
    invariant(_exclusiveLockInProgressQueue.empty());
    invariant(_readyQueue.empty());
    invariant(_sleepersQueue.empty());
    invariant(_unsignaledEvents.empty());
}

void ReplicationExecutor::maybeNotifyShutdownComplete_inlock() {
    if (_totalEventWaiters == 0)
        _noMoreWaitingThreads.notify_all();
}

StatusWith<ReplicationExecutor::EventHandle> ReplicationExecutor::makeEvent() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ++_counterCreatedEvents;
    return makeEvent_inlock();
}

StatusWith<ReplicationExecutor::EventHandle> ReplicationExecutor::makeEvent_inlock() {
    if (_inShutdown)
        return StatusWith<EventHandle>(ErrorCodes::ShutdownInProgress, "Shutdown in progress");

    _unsignaledEvents.emplace_back();
    auto event = std::make_shared<Event>(this, --_unsignaledEvents.end());
    setEventForHandle(&_unsignaledEvents.back(), std::move(event));
    return _unsignaledEvents.back();
}

void ReplicationExecutor::signalEvent(const EventHandle& eventHandle) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    signalEvent_inlock(eventHandle);
}

void ReplicationExecutor::signalEvent_inlock(const EventHandle& eventHandle) {
    Event* event = _getEventFromHandle(eventHandle);
    event->_signal_inlock();
    _unsignaledEvents.erase(event->_iter);
}

void ReplicationExecutor::waitForEvent(const EventHandle& event) {
    ++_counterWaitEvents;
    _getEventFromHandle(event)->waitUntilSignaled();
}

void ReplicationExecutor::cancel(const CallbackHandle& cbHandle) {
    ++_counterCancels;
    _getCallbackFromHandle(cbHandle)->cancel();
};

void ReplicationExecutor::wait(const CallbackHandle& cbHandle) {
    ++_counterWaits;
    _getCallbackFromHandle(cbHandle)->waitForCompletion();
};

StatusWith<ReplicationExecutor::CallbackHandle> ReplicationExecutor::onEvent(
    const EventHandle& eventHandle, const CallbackFn& work) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    WorkQueue* queue = &_readyQueue;
    Event* event = _getEventFromHandle(eventHandle);
    if (!event->_isSignaled) {
        queue = &event->_waiters;
    } else {
        queue = &_readyQueue;
        _networkInterface->signalWorkAvailable();
    }
    return enqueueWork_inlock(queue, work);
}

static void remoteCommandFinished(const ReplicationExecutor::CallbackArgs& cbData,
                                  const ReplicationExecutor::RemoteCommandCallbackFn& cb,
                                  const RemoteCommandRequest& request,
                                  const ResponseStatus& response) {
    if (cbData.status.isOK()) {
        cb(ReplicationExecutor::RemoteCommandCallbackArgs(
            cbData.executor, cbData.myHandle, request, response));
    } else {
        cb(ReplicationExecutor::RemoteCommandCallbackArgs(
            cbData.executor, cbData.myHandle, request, ResponseStatus(cbData.status)));
    }
}

static void remoteCommandFailedEarly(const ReplicationExecutor::CallbackArgs& cbData,
                                     const ReplicationExecutor::RemoteCommandCallbackFn& cb,
                                     const RemoteCommandRequest& request) {
    invariant(!cbData.status.isOK());
    cb(ReplicationExecutor::RemoteCommandCallbackArgs(
        cbData.executor, cbData.myHandle, request, ResponseStatus(cbData.status)));
}

void ReplicationExecutor::_finishRemoteCommand(const RemoteCommandRequest& request,
                                               const ResponseStatus& response,
                                               const CallbackHandle& cbHandle,
                                               const uint64_t expectedHandleGeneration,
                                               const RemoteCommandCallbackFn& cb) {
    Callback* callback = _getCallbackFromHandle(cbHandle);
    const WorkQueue::iterator iter = callback->_iter;

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown) {
        return;
    }

    if (expectedHandleGeneration != iter->generation) {
        return;
    }

    LOG(4) << "Received remote response: "
           << (response.isOK() ? response.getValue().toString() : response.getStatus().toString());

    callback->_callbackFn =
        stdx::bind(remoteCommandFinished, stdx::placeholders::_1, cb, request, response);
    _readyQueue.splice(_readyQueue.end(), _networkInProgressQueue, iter);
}

StatusWith<ReplicationExecutor::CallbackHandle> ReplicationExecutor::scheduleRemoteCommand(
    const RemoteCommandRequest& request, const RemoteCommandCallbackFn& cb) {
    RemoteCommandRequest scheduledRequest = request;
    if (request.timeout == RemoteCommandRequest::kNoTimeout) {
        scheduledRequest.expirationDate = RemoteCommandRequest::kNoExpirationDate;
    } else {
        scheduledRequest.expirationDate = _networkInterface->now() + scheduledRequest.timeout;
    }
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    StatusWith<CallbackHandle> handle = enqueueWork_inlock(
        &_networkInProgressQueue,
        stdx::bind(remoteCommandFailedEarly, stdx::placeholders::_1, cb, scheduledRequest));
    if (handle.isOK()) {
        _getCallbackFromHandle(handle.getValue())->_iter->isNetworkOperation = true;

        LOG(4) << "Scheduling remote request: " << request.toString();

        _networkInterface->startCommand(
            handle.getValue(),
            scheduledRequest,
            stdx::bind(&ReplicationExecutor::_finishRemoteCommand,
                       this,
                       scheduledRequest,
                       stdx::placeholders::_1,
                       handle.getValue(),
                       _getCallbackFromHandle(handle.getValue())->_iter->generation,
                       cb));
    }
    ++_counterScheduledCommands;
    return handle;
}

StatusWith<ReplicationExecutor::CallbackHandle> ReplicationExecutor::scheduleWork(
    const CallbackFn& work) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _networkInterface->signalWorkAvailable();
    const auto status = enqueueWork_inlock(&_readyQueue, work);
    if (status.isOK()) {
        ++_counterScheduledWorks;
    }
    return status;
}

StatusWith<ReplicationExecutor::CallbackHandle> ReplicationExecutor::scheduleWorkAt(
    Date_t when, const CallbackFn& work) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    WorkQueue temp;
    StatusWith<CallbackHandle> cbHandle = enqueueWork_inlock(&temp, work);
    if (!cbHandle.isOK())
        return cbHandle;
    auto callback = _getCallbackFromHandle(cbHandle.getValue());
    callback->_iter->readyDate = when;
    callback->_isSleeper = true;
    WorkQueue::iterator insertBefore = _sleepersQueue.begin();
    while (insertBefore != _sleepersQueue.end() && insertBefore->readyDate <= when)
        ++insertBefore;
    _sleepersQueue.splice(insertBefore, temp, temp.begin());
    ++_counterScheduledWorkAts;
    _networkInterface->signalWorkAvailable();
    return cbHandle;
}

StatusWith<ReplicationExecutor::CallbackHandle> ReplicationExecutor::scheduleDBWork(
    const CallbackFn& work) {
    return scheduleDBWork(work, NamespaceString(), MODE_NONE);
}

StatusWith<ReplicationExecutor::CallbackHandle> ReplicationExecutor::scheduleDBWork(
    const CallbackFn& work, const NamespaceString& nss, LockMode mode) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    StatusWith<CallbackHandle> handle = enqueueWork_inlock(&_dbWorkInProgressQueue, work);
    if (handle.isOK()) {
        auto doOp = stdx::bind(&ReplicationExecutor::_doOperation,
                               this,
                               stdx::placeholders::_1,
                               stdx::placeholders::_2,
                               handle.getValue(),
                               &_dbWorkInProgressQueue,
                               nullptr);
        auto task = [doOp](OperationContext* txn, const Status& status) {
            makeNoExcept(stdx::bind(doOp, txn, status))();
            return TaskRunner::NextAction::kDisposeOperationContext;
        };
        if (mode == MODE_NONE && nss.ns().empty()) {
            _dblockTaskRunner.schedule(task);
        } else {
            _dblockTaskRunner.schedule(DatabaseTask::makeCollectionLockTask(task, nss, mode));
        }
    }
    ++_counterScheduledDBWorks;
    return handle;
}

void ReplicationExecutor::_doOperation(OperationContext* txn,
                                       const Status& taskRunnerStatus,
                                       const CallbackHandle& cbHandle,
                                       WorkQueue* workQueue,
                                       stdx::mutex* terribleExLockSyncMutex) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_inShutdown)
        return;
    Callback* callback = _getCallbackFromHandle(cbHandle);
    const WorkQueue::iterator iter = callback->_iter;
    iter->callback = CallbackHandle();
    _freeQueue.splice(_freeQueue.begin(), *workQueue, iter);
    lk.unlock();
    {
        std::unique_ptr<stdx::lock_guard<stdx::mutex>> terribleLock(
            terribleExLockSyncMutex ? new stdx::lock_guard<stdx::mutex>(*terribleExLockSyncMutex)
                                    : nullptr);
        // Only possible task runner error status is CallbackCanceled.
        callback->_callbackFn(
            CallbackArgs(this,
                         cbHandle,
                         (callback->_isCanceled || !taskRunnerStatus.isOK()
                              ? Status(ErrorCodes::CallbackCanceled, "Callback canceled")
                              : Status::OK()),
                         txn));
    }
    lk.lock();
    signalEvent_inlock(callback->_finishedEvent);
}

StatusWith<ReplicationExecutor::CallbackHandle>
ReplicationExecutor::scheduleWorkWithGlobalExclusiveLock(const CallbackFn& work) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    StatusWith<CallbackHandle> handle = enqueueWork_inlock(&_exclusiveLockInProgressQueue, work);
    if (handle.isOK()) {
        auto doOp = stdx::bind(&ReplicationExecutor::_doOperation,
                               this,
                               stdx::placeholders::_1,
                               stdx::placeholders::_2,
                               handle.getValue(),
                               &_exclusiveLockInProgressQueue,
                               &_terribleExLockSyncMutex);
        _dblockExclusiveLockTaskRunner.schedule(DatabaseTask::makeGlobalExclusiveLockTask(
            [doOp](OperationContext* txn, const Status& status) {
                makeNoExcept(stdx::bind(doOp, txn, status))();
                return TaskRunner::NextAction::kDisposeOperationContext;
            }));
    }
    ++_counterScheduledExclusiveWorks;
    return handle;
}

void ReplicationExecutor::appendConnectionStats(executor::ConnectionPoolStats* stats) const {
    _networkInterface->appendConnectionStats(stats);
}

std::pair<ReplicationExecutor::WorkItem, ReplicationExecutor::CallbackHandle>
ReplicationExecutor::getWork() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (true) {
        const Date_t now = _networkInterface->now();
        Date_t nextWakeupDate = scheduleReadySleepers_inlock(now);
        if (!_readyQueue.empty()) {
            break;
        } else if (_inShutdown) {
            return std::make_pair(WorkItem(), CallbackHandle());
        }
        lk.unlock();
        if (nextWakeupDate == Date_t::max()) {
            _networkInterface->waitForWork();
        } else {
            _networkInterface->waitForWorkUntil(nextWakeupDate);
        }
        lk.lock();
    }
    const WorkItem work = *_readyQueue.begin();
    const CallbackHandle cbHandle = work.callback;
    _readyQueue.begin()->callback = CallbackHandle();
    _freeQueue.splice(_freeQueue.begin(), _readyQueue, _readyQueue.begin());
    return std::make_pair(work, cbHandle);
}

int64_t ReplicationExecutor::nextRandomInt64(int64_t limit) {
    return _random.nextInt64(limit);
}

Date_t ReplicationExecutor::scheduleReadySleepers_inlock(const Date_t now) {
    WorkQueue::iterator iter = _sleepersQueue.begin();
    while ((iter != _sleepersQueue.end()) && (iter->readyDate <= now)) {
        auto callback = ReplicationExecutor::_getCallbackFromHandle(iter->callback);
        callback->_isSleeper = false;
        ++iter;
    }
    _readyQueue.splice(_readyQueue.end(), _sleepersQueue, _sleepersQueue.begin(), iter);
    if (iter == _sleepersQueue.end()) {
        // indicate no sleeper to wait for
        return Date_t::max();
    }
    return iter->readyDate;
}

StatusWith<ReplicationExecutor::CallbackHandle> ReplicationExecutor::enqueueWork_inlock(
    WorkQueue* queue, const CallbackFn& callbackFn) {
    invariant(callbackFn);
    StatusWith<EventHandle> event = makeEvent_inlock();
    if (!event.isOK())
        return StatusWith<CallbackHandle>(event.getStatus());

    if (_freeQueue.empty())
        _freeQueue.push_front(WorkItem());
    const WorkQueue::iterator iter = _freeQueue.begin();
    WorkItem& work = *iter;

    invariant(!work.callback.isValid());
    setCallbackForHandle(&work.callback,
                         std::shared_ptr<executor::TaskExecutor::CallbackState>(
                             new Callback(this, callbackFn, iter, event.getValue())));

    work.generation++;
    work.finishedEvent = event.getValue();
    work.readyDate = Date_t();
    queue->splice(queue->end(), _freeQueue, iter);
    return StatusWith<CallbackHandle>(work.callback);
}

ReplicationExecutor::WorkItem::WorkItem() : generation(0U), isNetworkOperation(false) {}

ReplicationExecutor::Event::Event(ReplicationExecutor* executor, const EventList::iterator& iter)
    : executor::TaskExecutor::EventState(), _executor(executor), _isSignaled(false), _iter(iter) {}

ReplicationExecutor::Event::~Event() {}

void ReplicationExecutor::Event::signal() {
    // Must go through executor to signal so that this can be removed from the _unsignaledEvents
    // EventList.
    _executor->signalEvent(*_iter);
}

void ReplicationExecutor::Event::_signal_inlock() {
    invariant(!_isSignaled);
    _isSignaled = true;

    if (!_waiters.empty()) {
        _executor->_readyQueue.splice(_executor->_readyQueue.end(), _waiters);
        _executor->_networkInterface->signalWorkAvailable();
    }

    _isSignaledCondition.notify_all();
}

void ReplicationExecutor::Event::waitUntilSignaled() {
    stdx::unique_lock<stdx::mutex> lk(_executor->_mutex);
    ++_executor->_totalEventWaiters;
    while (!_isSignaled) {
        _isSignaledCondition.wait(lk);
    }
    --_executor->_totalEventWaiters;
    _executor->maybeNotifyShutdownComplete_inlock();
}

bool ReplicationExecutor::Event::isSignaled() {
    stdx::lock_guard<stdx::mutex> lk(_executor->_mutex);
    return _isSignaled;
}

ReplicationExecutor::Callback::Callback(ReplicationExecutor* executor,
                                        const CallbackFn callbackFn,
                                        const WorkQueue::iterator& iter,
                                        const EventHandle& finishedEvent)
    : executor::TaskExecutor::CallbackState(),
      _executor(executor),
      _callbackFn(callbackFn),
      _isCanceled(false),
      _isSleeper(false),
      _iter(iter),
      _finishedEvent(finishedEvent) {}

ReplicationExecutor::Callback::~Callback() {}

bool ReplicationExecutor::Callback::isCanceled() const {
    stdx::unique_lock<stdx::mutex> lk(_executor->_mutex);
    return _isCanceled;
}

void ReplicationExecutor::Callback::cancel() {
    stdx::unique_lock<stdx::mutex> lk(_executor->_mutex);
    _isCanceled = true;

    if (_isSleeper) {
        _isSleeper = false;
        _executor->_readyQueue.splice(
            _executor->_readyQueue.end(), _executor->_sleepersQueue, _iter);
    }

    if (_iter->isNetworkOperation) {
        lk.unlock();
        _executor->_networkInterface->cancelCommand(_iter->callback);
    }
}

void ReplicationExecutor::Callback::waitForCompletion() {
    _executor->waitForEvent(_finishedEvent);
}

ReplicationExecutor::Event* ReplicationExecutor::_getEventFromHandle(
    const EventHandle& eventHandle) {
    return static_cast<Event*>(getEventFromHandle(eventHandle));
}

ReplicationExecutor::Callback* ReplicationExecutor::_getCallbackFromHandle(
    const CallbackHandle& callbackHandle) {
    return static_cast<Callback*>(getCallbackFromHandle(callbackHandle));
}

namespace {

void callNoExcept(const stdx::function<void()>& fn) {
    try {
        fn();
    } catch (...) {
        auto status = exceptionToStatus();
        log() << "Exception thrown in ReplicationExecutor callback: " << status;
        std::terminate();
    }
}

stdx::function<void()> makeNoExcept(const stdx::function<void()>& fn) {
    return stdx::bind(callNoExcept, fn);
}

}  // namespace

}  // namespace repl
}  // namespace mongo

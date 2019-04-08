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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"

#include <algorithm>
#include <iterator>

#include "mongo/executor/connection_pool_stats.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {

using CallbackHandle = TaskExecutor::CallbackHandle;
using ResponseStatus = TaskExecutor::ResponseStatus;

NetworkInterfaceMock::NetworkInterfaceMock()
    : _waitingToRunMask(0),
      _currentlyRunning(kNoThread),
      _now(fassert(18653, dateFromISOString("2014-08-01T00:00:00Z"))),
      _hasStarted(false),
      _inShutdown(false),
      _executorNextWakeupDate(Date_t::max()) {}

NetworkInterfaceMock::~NetworkInterfaceMock() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(!_hasStarted || inShutdown());
    invariant(_scheduled.empty());
    invariant(_blackHoled.empty());
}

void NetworkInterfaceMock::logQueues() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    const std::vector<std::pair<std::string, const NetworkOperationList*>> queues{
        {"unscheduled", &_unscheduled},
        {"scheduled", &_scheduled},
        {"processing", &_processing},
        {"blackholes", &_blackHoled}};

    for (auto&& queue : queues) {
        if (queue.second->empty()) {
            continue;
        }

        log() << "**** queue: " << queue.first << " ****";
        for (auto&& item : *queue.second) {
            log() << "\t\t " << item.getDiagnosticString();
        }
    }
}

std::string NetworkInterfaceMock::getDiagnosticString() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return str::stream() << "NetworkInterfaceMock -- waitingToRunMask:" << _waitingToRunMask
                         << ", now:" << _now_inlock().toString() << ", hasStarted:" << _hasStarted
                         << ", inShutdown: " << _inShutdown.load()
                         << ", processing: " << _processing.size()
                         << ", scheduled: " << _scheduled.size()
                         << ", blackHoled: " << _blackHoled.size()
                         << ", unscheduled: " << _unscheduled.size();
}

Date_t NetworkInterfaceMock::now() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _now_inlock();
}

std::string NetworkInterfaceMock::getHostName() {
    return "thisisourhostname";
}

Status NetworkInterfaceMock::startCommand(const CallbackHandle& cbHandle,
                                          RemoteCommandRequest& request,
                                          RemoteCommandCompletionFn&& onFinish,
                                          const BatonHandle& baton) {
    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterfaceMock shutdown in progress"};
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    const Date_t now = _now_inlock();
    auto op = NetworkOperation(cbHandle, request, now, std::move(onFinish));

    // If we don't have a hook, or we have already 'connected' to this host, enqueue the op.
    if (!_hook || _connections.count(request.target)) {
        _enqueueOperation_inlock(std::move(op));
    } else {
        _connectThenEnqueueOperation_inlock(request.target, std::move(op));
    }

    return Status::OK();
}

void NetworkInterfaceMock::setHandshakeReplyForHost(
    const mongo::HostAndPort& host, mongo::executor::RemoteCommandResponse&& reply) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto it = _handshakeReplies.find(host);
    if (it == std::end(_handshakeReplies)) {
        auto res = _handshakeReplies.emplace(host, std::move(reply));
        invariant(res.second);
    } else {
        it->second = std::move(reply);
    }
}

void NetworkInterfaceMock::cancelCommand(const CallbackHandle& cbHandle, const BatonHandle& baton) {
    invariant(!inShutdown());

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ResponseStatus rs(ErrorCodes::CallbackCanceled, "Network operation canceled", Milliseconds(0));

    // We mimic the real NetworkInterface by only delivering the CallbackCanceled status if the
    // operation has not already received a response (i.e., is not already in the _scheduled queue).
    std::vector<NetworkOperationList*> queuesToCheck{&_unscheduled, &_blackHoled, &_processing};
    _interruptWithResponse_inlock(cbHandle, queuesToCheck, rs);
}

void NetworkInterfaceMock::_interruptWithResponse_inlock(
    const CallbackHandle& cbHandle,
    const std::vector<NetworkOperationList*>& queuesToCheck,
    const ResponseStatus& response) {

    auto matchFn = [&cbHandle](const auto& ops) { return ops.isForCallback(cbHandle); };

    for (auto list : queuesToCheck) {
        auto noi = std::find_if(list->begin(), list->end(), matchFn);
        if (noi == list->end()) {
            continue;
        }
        _scheduled.splice(_scheduled.begin(), *list, noi);
        noi->setResponse(_now_inlock(), response);
        return;
    }
}

Status NetworkInterfaceMock::setAlarm(const TaskExecutor::CallbackHandle& cbHandle,
                                      const Date_t when,
                                      unique_function<void(Status)> action) {
    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterfaceMock shutdown in progress"};
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    if (when <= _now_inlock()) {
        lk.unlock();
        action(Status::OK());
        return Status::OK();
    }
    _alarms.emplace(cbHandle, when, std::move(action));

    return Status::OK();
}

void NetworkInterfaceMock::cancelAlarm(const TaskExecutor::CallbackHandle& cbHandle) {
    // Alarms live in a priority queue, so removing them isn't worth it
    // Thus we add the handle to a map and check at fire time
    _canceledAlarms.insert(cbHandle);
}

Status NetworkInterfaceMock::schedule(unique_function<void(Status)> action) {
    // Call the task immediately, we have no out-of-line executor
    action(Status::OK());

    // Say we scheduled the task fine, because we ran it inline
    return Status::OK();
}

bool NetworkInterfaceMock::onNetworkThread() {
    return _currentlyRunning == kNetworkThread;
}

void NetworkInterfaceMock::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _startup_inlock();
}

void NetworkInterfaceMock::_startup_inlock() {
    invariant(!_hasStarted);
    _hasStarted = true;
    _inShutdown.store(false);
    invariant(_currentlyRunning == kNoThread);
    _currentlyRunning = kExecutorThread;
}

void NetworkInterfaceMock::shutdown() {
    invariant(!inShutdown());

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (!_hasStarted) {
        _startup_inlock();
    }
    _inShutdown.store(true);
    NetworkOperationList todo;
    todo.splice(todo.end(), _scheduled);
    todo.splice(todo.end(), _unscheduled);
    todo.splice(todo.end(), _processing);
    todo.splice(todo.end(), _blackHoled);

    const Date_t now = _now_inlock();
    _waitingToRunMask |= kExecutorThread;  // Prevents network thread from scheduling.
    lk.unlock();
    for (NetworkOperationIterator iter = todo.begin(); iter != todo.end(); ++iter) {
        warning() << "Mock network interface shutting down with outstanding request: "
                  << iter->getRequest();
        iter->setResponse(
            now, {ErrorCodes::ShutdownInProgress, "Shutting down mock network", Milliseconds(0)});
        iter->finishResponse();
    }
    lk.lock();
    invariant(_currentlyRunning == kExecutorThread);
    _currentlyRunning = kNoThread;
    _waitingToRunMask = kNetworkThread;
    _shouldWakeNetworkCondition.notify_one();
}

bool NetworkInterfaceMock::inShutdown() const {
    return _inShutdown.load();
}

void NetworkInterfaceMock::enterNetwork() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (!_isNetworkThreadRunnable_inlock()) {
        _shouldWakeNetworkCondition.wait(lk);
    }
    _currentlyRunning = kNetworkThread;
    _waitingToRunMask &= ~kNetworkThread;
}

void NetworkInterfaceMock::exitNetwork() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_currentlyRunning != kNetworkThread) {
        return;
    }
    _currentlyRunning = kNoThread;
    if (_isExecutorThreadRunnable_inlock()) {
        _shouldWakeExecutorCondition.notify_one();
    }
    _waitingToRunMask |= kNetworkThread;
}

bool NetworkInterfaceMock::hasReadyRequests() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    return _hasReadyRequests_inlock();
}

bool NetworkInterfaceMock::_hasReadyRequests_inlock() {
    if (_unscheduled.empty())
        return false;
    if (_unscheduled.front().getNextConsiderationDate() > _now_inlock()) {
        return false;
    }
    return true;
}

NetworkInterfaceMock::NetworkOperationIterator NetworkInterfaceMock::getNextReadyRequest() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    while (!_hasReadyRequests_inlock()) {
        _waitingToRunMask |= kExecutorThread;
        _runReadyNetworkOperations_inlock(&lk);
    }
    invariant(_hasReadyRequests_inlock());
    _processing.splice(_processing.begin(), _unscheduled, _unscheduled.begin());
    return _processing.begin();
}

NetworkInterfaceMock::NetworkOperationIterator NetworkInterfaceMock::getFrontOfUnscheduledQueue() {
    return getNthUnscheduledRequest(0);
}

NetworkInterfaceMock::NetworkOperationIterator NetworkInterfaceMock::getNthUnscheduledRequest(
    size_t n) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    invariant(_hasReadyRequests_inlock());

    // Linear time, but it's just for testing so no big deal.
    invariant(_unscheduled.size() > n);
    auto it = _unscheduled.begin();
    std::advance(it, n);
    return it;
}

void NetworkInterfaceMock::scheduleResponse(NetworkOperationIterator noi,
                                            Date_t when,
                                            const ResponseStatus& response) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    NetworkOperationIterator insertBefore = _scheduled.begin();
    while ((insertBefore != _scheduled.end()) && (insertBefore->getResponseDate() <= when)) {
        ++insertBefore;
    }

    // If no RemoteCommandResponse was returned (for example, on a simulated network error), then
    // do not attempt to run the metadata hook, since there is no returned metadata.
    if (_metadataHook && response.isOK()) {
        _metadataHook
            ->readReplyMetadata(
                noi->getRequest().opCtx, noi->getRequest().target.toString(), response.data)
            .transitional_ignore();
    }

    noi->setResponse(when, response);
    _scheduled.splice(insertBefore, _processing, noi);
}

RemoteCommandRequest NetworkInterfaceMock::scheduleSuccessfulResponse(const BSONObj& response) {
    return scheduleSuccessfulResponse(RemoteCommandResponse(response, Milliseconds(0)));
}

RemoteCommandRequest NetworkInterfaceMock::scheduleSuccessfulResponse(
    const RemoteCommandResponse& response) {
    return scheduleSuccessfulResponse(getNextReadyRequest(), response);
}

RemoteCommandRequest NetworkInterfaceMock::scheduleSuccessfulResponse(
    NetworkOperationIterator noi, const RemoteCommandResponse& response) {
    return scheduleSuccessfulResponse(noi, now(), response);
}

RemoteCommandRequest NetworkInterfaceMock::scheduleSuccessfulResponse(
    NetworkOperationIterator noi, Date_t when, const RemoteCommandResponse& response) {
    scheduleResponse(noi, when, response);
    return noi->getRequest();
}

RemoteCommandRequest NetworkInterfaceMock::scheduleErrorResponse(const Status& response) {
    return scheduleErrorResponse(getNextReadyRequest(), response);
}

RemoteCommandRequest NetworkInterfaceMock::scheduleErrorResponse(const ResponseStatus response) {
    auto noi = getNextReadyRequest();
    scheduleResponse(noi, now(), response);
    return noi->getRequest();
}

RemoteCommandRequest NetworkInterfaceMock::scheduleErrorResponse(NetworkOperationIterator noi,
                                                                 const Status& response) {
    return scheduleErrorResponse(noi, now(), response);
}

RemoteCommandRequest NetworkInterfaceMock::scheduleErrorResponse(NetworkOperationIterator noi,
                                                                 Date_t when,
                                                                 const Status& response) {
    scheduleResponse(noi, when, response);
    return noi->getRequest();
}

void NetworkInterfaceMock::blackHole(NetworkOperationIterator noi) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    _blackHoled.splice(_blackHoled.end(), _processing, noi);
}

void NetworkInterfaceMock::requeueAt(NetworkOperationIterator noi, Date_t dontAskUntil) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    invariant(noi->getNextConsiderationDate() < dontAskUntil);
    invariant(_now_inlock() < dontAskUntil);
    NetworkOperationIterator insertBefore = _unscheduled.begin();
    for (; insertBefore != _unscheduled.end(); ++insertBefore) {
        if (insertBefore->getNextConsiderationDate() >= dontAskUntil) {
            break;
        }
    }
    noi->setNextConsiderationDate(dontAskUntil);
    _unscheduled.splice(insertBefore, _processing, noi);
}

Date_t NetworkInterfaceMock::runUntil(Date_t until) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    invariant(until > _now_inlock());
    while (until > _now_inlock()) {
        _runReadyNetworkOperations_inlock(&lk);
        if (_hasReadyRequests_inlock()) {
            break;
        }
        Date_t newNow = _executorNextWakeupDate;
        if (!_alarms.empty() && _alarms.top().when < newNow) {
            newNow = _alarms.top().when;
        }
        if (!_scheduled.empty() && _scheduled.front().getResponseDate() < newNow) {
            newNow = _scheduled.front().getResponseDate();
        }
        if (until < newNow) {
            newNow = until;
        }
        invariant(_now_inlock() <= newNow);
        _now = newNow;
        _waitingToRunMask |= kExecutorThread;
    }
    _runReadyNetworkOperations_inlock(&lk);
    return _now_inlock();
}

void NetworkInterfaceMock::advanceTime(Date_t newTime) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    invariant(newTime > _now_inlock());
    _now = newTime;

    _waitingToRunMask |= kExecutorThread;
    _runReadyNetworkOperations_inlock(&lk);
}

void NetworkInterfaceMock::runReadyNetworkOperations() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    _runReadyNetworkOperations_inlock(&lk);
}

void NetworkInterfaceMock::waitForWork() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kExecutorThread);
    _waitForWork_inlock(&lk);
}

void NetworkInterfaceMock::waitForWorkUntil(Date_t when) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kExecutorThread);
    _executorNextWakeupDate = when;
    if (_executorNextWakeupDate <= _now_inlock()) {
        return;
    }
    _waitForWork_inlock(&lk);
}

void NetworkInterfaceMock::_enqueueOperation_inlock(
    mongo::executor::NetworkInterfaceMock::NetworkOperation&& op) {
    auto insertBefore =
        std::upper_bound(std::begin(_unscheduled),
                         std::end(_unscheduled),
                         op,
                         [](const NetworkOperation& a, const NetworkOperation& b) {
                             return a.getNextConsiderationDate() < b.getNextConsiderationDate();
                         });

    const auto timeout = op.getRequest().timeout;
    auto cbh = op.getCallbackHandle();

    _unscheduled.emplace(insertBefore, std::move(op));

    if (timeout != RemoteCommandRequest::kNoTimeout) {
        invariant(timeout >= Milliseconds(0));
        ResponseStatus rs(
            ErrorCodes::NetworkInterfaceExceededTimeLimit, "Network timeout", Milliseconds(0));
        std::vector<NetworkOperationList*> queuesToCheck{&_unscheduled, &_blackHoled, &_scheduled};
        _alarms.emplace(cbh, _now_inlock() + timeout, [
            this,
            cbh = std::move(cbh),
            queuesToCheck = std::move(queuesToCheck),
            rs = std::move(rs)
        ](Status) { _interruptWithResponse_inlock(cbh, queuesToCheck, rs); });
    }
}

void NetworkInterfaceMock::_connectThenEnqueueOperation_inlock(const HostAndPort& target,
                                                               NetworkOperation&& op) {
    invariant(_hook);  // if there is no hook, we shouldn't even hit this codepath
    invariant(!_connections.count(target));

    auto handshakeReplyIter = _handshakeReplies.find(target);

    auto handshakeReply = (handshakeReplyIter != std::end(_handshakeReplies))
        ? handshakeReplyIter->second
        : RemoteCommandResponse(BSONObj(), Milliseconds(0));

    auto valid = _hook->validateHost(target, op.getRequest().cmdObj, handshakeReply);
    if (!valid.isOK()) {
        op.setResponse(_now_inlock(), valid);
        op.finishResponse();
        return;
    }

    auto swHookPostconnectCommand = _hook->makeRequest(target);

    if (!swHookPostconnectCommand.isOK()) {
        op.setResponse(_now_inlock(), swHookPostconnectCommand.getStatus());
        op.finishResponse();
        return;
    }

    boost::optional<RemoteCommandRequest> hookPostconnectCommand =
        std::move(swHookPostconnectCommand.getValue());

    if (!hookPostconnectCommand) {
        // If we don't have a post connect command, enqueue the actual command.
        _connections.emplace(op.getRequest().target);
        _enqueueOperation_inlock(std::move(op));
        return;
    }

    auto cbh = op.getCallbackHandle();
    // The completion handler for the postconnect command schedules the original command.
    auto postconnectCompletionHandler = [ this, op = std::move(op) ](ResponseStatus rs) mutable {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (!rs.isOK()) {
            op.setResponse(_now_inlock(), rs);
            op.finishResponse();
            return;
        }

        auto handleStatus = _hook->handleReply(op.getRequest().target, std::move(rs));

        if (!handleStatus.isOK()) {
            op.setResponse(_now_inlock(), handleStatus);
            op.finishResponse();
            return;
        }

        _connections.emplace(op.getRequest().target);
        _enqueueOperation_inlock(std::move(op));
    };

    auto postconnectOp = NetworkOperation(cbh,
                                          std::move(*hookPostconnectCommand),
                                          _now_inlock(),
                                          std::move(postconnectCompletionHandler));

    _enqueueOperation_inlock(std::move(postconnectOp));
}

void NetworkInterfaceMock::setConnectionHook(std::unique_ptr<NetworkConnectionHook> hook) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_hasStarted);
    invariant(!_hook);
    _hook = std::move(hook);
}

void NetworkInterfaceMock::setEgressMetadataHook(
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_hasStarted);
    invariant(!_metadataHook);
    _metadataHook = std::move(metadataHook);
}

void NetworkInterfaceMock::signalWorkAvailable() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _waitingToRunMask |= kExecutorThread;
    if (_currentlyRunning == kNoThread) {
        _shouldWakeExecutorCondition.notify_one();
    }
}

void NetworkInterfaceMock::_runReadyNetworkOperations_inlock(stdx::unique_lock<stdx::mutex>* lk) {
    while (!_alarms.empty() && _now_inlock() >= _alarms.top().when) {
        auto& alarm = _alarms.top();

        // If the handle isn't cancelled, then run it
        auto iter = _canceledAlarms.find(alarm.handle);
        if (iter == _canceledAlarms.end()) {
            lk->unlock();
            alarm.action(Status::OK());
            lk->lock();
        } else {
            _canceledAlarms.erase(iter);
        }

        _alarms.pop();
    }
    while (!_scheduled.empty() && _scheduled.front().getResponseDate() <= _now_inlock()) {
        invariant(_currentlyRunning == kNetworkThread);
        NetworkOperation op = std::move(_scheduled.front());
        _scheduled.pop_front();
        _waitingToRunMask |= kExecutorThread;
        lk->unlock();
        op.finishResponse();
        lk->lock();
    }
    invariant(_currentlyRunning == kNetworkThread);
    if (!(_waitingToRunMask & kExecutorThread)) {
        return;
    }
    _shouldWakeExecutorCondition.notify_one();
    _currentlyRunning = kNoThread;
    while (!_isNetworkThreadRunnable_inlock()) {
        _shouldWakeNetworkCondition.wait(*lk);
    }
    _currentlyRunning = kNetworkThread;
    _waitingToRunMask &= ~kNetworkThread;
}

void NetworkInterfaceMock::_waitForWork_inlock(stdx::unique_lock<stdx::mutex>* lk) {
    if (_waitingToRunMask & kExecutorThread) {
        _waitingToRunMask &= ~kExecutorThread;
        return;
    }
    _currentlyRunning = kNoThread;
    while (!_isExecutorThreadRunnable_inlock()) {
        _waitingToRunMask |= kNetworkThread;
        _shouldWakeNetworkCondition.notify_one();
        _shouldWakeExecutorCondition.wait(*lk);
    }
    _currentlyRunning = kExecutorThread;
    _waitingToRunMask &= ~kExecutorThread;
}

bool NetworkInterfaceMock::_isNetworkThreadRunnable_inlock() {
    if (_currentlyRunning != kNoThread) {
        return false;
    }
    if (_waitingToRunMask != kNetworkThread) {
        return false;
    }
    return true;
}

bool NetworkInterfaceMock::_isExecutorThreadRunnable_inlock() {
    if (_currentlyRunning != kNoThread) {
        return false;
    }
    return _waitingToRunMask & kExecutorThread;
}

static const ResponseStatus kUnsetResponse(ErrorCodes::InternalError,
                                           "NetworkOperation::_response never set");

NetworkInterfaceMock::NetworkOperation::NetworkOperation()
    : _requestDate(),
      _nextConsiderationDate(),
      _responseDate(),
      _request(),
      _response(kUnsetResponse),
      _onFinish() {}

NetworkInterfaceMock::NetworkOperation::NetworkOperation(const CallbackHandle& cbHandle,
                                                         const RemoteCommandRequest& theRequest,
                                                         Date_t theRequestDate,
                                                         RemoteCommandCompletionFn onFinish)
    : _requestDate(theRequestDate),
      _nextConsiderationDate(theRequestDate),
      _responseDate(),
      _cbHandle(cbHandle),
      _request(theRequest),
      _response(kUnsetResponse),
      _onFinish(std::move(onFinish)) {}

std::string NetworkInterfaceMock::NetworkOperation::getDiagnosticString() const {
    return str::stream() << "NetworkOperation -- request:'" << _request.toString()
                         << "', responseStatus: '" << _response.status.toString()
                         << "', responseBody: '" << (_response.isOK() ? _response.toString() : "")
                         << "', reqDate: " << _requestDate.toString()
                         << ", nextConsiderDate: " << _nextConsiderationDate.toString()
                         << ", respDate: " << _responseDate.toString();
}

void NetworkInterfaceMock::NetworkOperation::setNextConsiderationDate(
    Date_t nextConsiderationDate) {
    invariant(nextConsiderationDate > _nextConsiderationDate);
    _nextConsiderationDate = nextConsiderationDate;
}

void NetworkInterfaceMock::NetworkOperation::setResponse(Date_t responseDate,
                                                         const ResponseStatus& response) {
    invariant(responseDate >= _requestDate);
    _responseDate = responseDate;
    _response = response;
}

void NetworkInterfaceMock::NetworkOperation::finishResponse() {
    invariant(_onFinish);
    _onFinish(_response);
    _onFinish = RemoteCommandCompletionFn();
}

NetworkInterfaceMock::InNetworkGuard::InNetworkGuard(NetworkInterfaceMock* net) : _net(net) {
    _net->enterNetwork();
}

void NetworkInterfaceMock::InNetworkGuard::dismiss() {
    _callExitNetwork = false;
    _net->exitNetwork();
}

NetworkInterfaceMock::InNetworkGuard::~InNetworkGuard() {
    if (_callExitNetwork)
        _net->exitNetwork();
}

NetworkInterfaceMock* NetworkInterfaceMock::InNetworkGuard::operator->() const {
    return _net;
}

NetworkInterfaceMockClockSource::NetworkInterfaceMockClockSource(NetworkInterfaceMock* net)
    : _net(net) {
    _tracksSystemClock = false;
}

}  // namespace executor
}  // namespace mongo

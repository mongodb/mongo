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


#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <type_traits>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace executor {

namespace {
const Status kNetworkInterfaceMockShutdownInProgress = {
    ErrorCodes::ShutdownInProgress, "NetworkInterfaceMock shutdown in progress"};
}

MONGO_FAIL_POINT_DEFINE(networkInterfaceMockFailToSchedule);

using CallbackHandle = TaskExecutor::CallbackHandle;
using ResponseStatus = TaskExecutor::ResponseStatus;

NetworkInterfaceMock::NetworkInterfaceMock()
    : _clkSource(std::make_unique<ClockSourceMock>()),
      _waitingToRunMask(0),
      _currentlyRunning(kNoThread),
      _hasStarted(false),
      _inShutdown(false),
      _executorNextWakeupDate(Date_t::max()) {}

NetworkInterfaceMock::~NetworkInterfaceMock() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(!_hasStarted || inShutdown());
}

std::string NetworkInterfaceMock::getDiagnosticString() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return str::stream() << "NetworkInterfaceMock -- waitingToRunMask:" << _waitingToRunMask
                         << ", now:" << _now_inlock(lk).toString() << ", hasStarted:" << _hasStarted
                         << ", inShutdown: " << _inShutdown.load()
                         << ", operations: " << _operations.size()
                         << ", responses: " << _responses.size();
}

Date_t NetworkInterfaceMock::now() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _now_inlock(lk);
}

std::string NetworkInterfaceMock::getHostName() {
    return "thisisourhostname";
}

/**
 * Starts a remote command with an implementation common to both the exhaust and non-exhaust
 * variants.
 */
SemiFuture<TaskExecutor::ResponseStatus> NetworkInterfaceMock::_startOperation(
    const TaskExecutor::CallbackHandle& cbHandle,
    RemoteCommandRequest& request,
    bool awaitExhaust,
    const BatonHandle& baton,
    const CancellationToken& token) {
    if (inShutdown()) {
        return kNetworkInterfaceMockShutdownInProgress;
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    const Date_t now = _now_inlock(lk);

    LOGV2(5440600,
          "Scheduling operation",
          "when"_attr = now,
          "request"_attr = request,
          "awaitExhaust"_attr = awaitExhaust);

    auto [promise, future] = makePromiseFuture<TaskExecutor::ResponseStatus>();
    auto op = NetworkOperation(cbHandle, request, now, token, std::move(promise));

    // If we don't have a hook, we have already 'connected' to this host, or we want to receive
    // the next exhaust response, enqueue the op without connecting.
    if (awaitExhaust || !_hook || _connections.count(request.target)) {
        _enqueueOperation_inlock(lk, std::move(op));
    } else {
        _connectThenEnqueueOperation_inlock(lk, request.target, std::move(op));
    }

    return std::move(future).semi();
}

SemiFuture<TaskExecutor::ResponseStatus> NetworkInterfaceMock::startCommand(
    const CallbackHandle& cbHandle,
    RemoteCommandRequest& request,
    const BatonHandle& baton,
    const CancellationToken& token) {
    if (inShutdown() || networkInterfaceMockFailToSchedule.shouldFail()) {
        uassertStatusOK(kNetworkInterfaceMockShutdownInProgress);
    }

    return _startOperation(cbHandle, request, /* awaitExhaust */ false, baton, token);
}

SemiFuture<RemoteCommandResponse> NetworkInterfaceMock::ExhaustResponseReaderMock::next() {
    auto token = _cancelSource.token();
    if (token.isCanceled()) {
        return Status(ErrorCodes::CallbackCanceled, "Exhaust command canceled");
    } else if (_state == State::kDone) {
        return Status(ErrorCodes::ExhaustCommandFinished, "Exhaust command finished");
    }

    auto prior = std::exchange(_state, State::kExhaust);
    return _interface->_startOperation(
        _cbHandle, _initialRequest, prior == State::kExhaust, nullptr, token);
}

SemiFuture<std::shared_ptr<NetworkInterface::ExhaustResponseReader>>
NetworkInterfaceMock::startExhaustCommand(const CallbackHandle& cbHandle,
                                          RemoteCommandRequest& request,
                                          const BatonHandle& baton,
                                          const CancellationToken& token) {
    if (inShutdown() || networkInterfaceMockFailToSchedule.shouldFail()) {
        uassertStatusOK(kNetworkInterfaceMockShutdownInProgress);
    }

    std::shared_ptr<ExhaustResponseReader> reader =
        std::make_shared<ExhaustResponseReaderMock>(this, cbHandle, request, baton, token);
    return reader;
}

void NetworkInterfaceMock::setHandshakeReplyForHost(
    const mongo::HostAndPort& host, mongo::executor::RemoteCommandResponse&& reply) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
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

    stdx::unique_lock lk(_mutex);
    auto op = _getNetworkOperation_inlock(lk, cbHandle);
    if (op == _operations.end()) {
        return;
    }

    auto source = op->getCancellationSource();
    lk.unlock();
    source.cancel();
}

void NetworkInterfaceMock::_interruptWithResponse_inlock(stdx::unique_lock<stdx::mutex>& lk,
                                                         const CallbackHandle& cbHandle,
                                                         const ResponseStatus& response) {
    auto noi = _getNetworkOperation_inlock(lk, cbHandle);
    if (noi == _operations.end()) {
        return;
    }

    // We've effectively observed the NetworkOperation.
    noi->markAsProcessing();
    _scheduleResponse_inlock(lk, noi, _now_inlock(lk), response);
}

NetworkInterfaceMock::NetworkOperationIterator NetworkInterfaceMock::_getNetworkOperation_inlock(
    WithLock, const CallbackHandle& cbHandle) {
    auto matchFn = [&cbHandle](const auto& ops) {
        return ops.isForCallback(cbHandle) && !ops.isFinished();
    };
    return std::find_if(_operations.begin(), _operations.end(), matchFn);
}

void NetworkInterfaceMock::AlarmInfo::cancel() {
    LOGV2(9311405, "Canceling alarm", "id"_attr = id);
    promise.setError({ErrorCodes::CallbackCanceled, "Alarm canceled"});
}

SemiFuture<void> NetworkInterfaceMock::setAlarm(const Date_t when, const CancellationToken& token) {
    if (inShutdown()) {
        return kNetworkInterfaceMockShutdownInProgress;
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    if (when <= _now_inlock(lk)) {
        return Status::OK();
    }

    auto [promise, future] = makePromiseFuture<void>();
    auto id = _nextAlarmId++;
    auto it = _alarms.insert({when, AlarmInfo(id, when, std::move(promise))});
    _alarmsById[id] = it;

    lk.unlock();

    token.onCancel().unsafeToInlineFuture().getAsync([this, id](Status status) {
        if (!status.isOK()) {
            return;
        }

        stdx::lock_guard lk(_mutex);

        auto it = _alarmsById.find(id);
        if (it == _alarmsById.end()) {
            return;
        }

        _canceledAlarms.insert(id);
    });

    return std::move(future).semi();
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
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _startup_inlock(lk);
}

void NetworkInterfaceMock::_startup_inlock(stdx::unique_lock<stdx::mutex>& lk) {
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
        _startup_inlock(lk);
    }
    _inShutdown.store(true);
    auto todo = std::exchange(_operations, {});

    const Date_t now = _now_inlock(lk);
    _waitingToRunMask |= kExecutorThread;  // Prevents network thread from scheduling.
    for (auto& op : todo) {
        auto response =
            NetworkResponse{{},
                            now,
                            ResponseStatus::make_forTest(Status(ErrorCodes::ShutdownInProgress,
                                                                "Shutting down mock network"),
                                                         Milliseconds(0))};
        if (op.fulfillResponse_inlock(lk, std::move(response))) {
            LOGV2_WARNING(22590,
                          "Mock network interface shutting down with outstanding request",
                          "request"_attr = op.getRequest());
        }
    }
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
    while (!_isNetworkThreadRunnable_inlock(lk)) {
        _shouldWakeNetworkCondition.wait(lk);
    }
    _currentlyRunning = kNetworkThread;
    _waitingToRunMask &= ~kNetworkThread;
}

void NetworkInterfaceMock::exitNetwork() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_currentlyRunning != kNetworkThread) {
        return;
    }
    _currentlyRunning = kNoThread;
    if (_isExecutorThreadRunnable_inlock(lk)) {
        _shouldWakeExecutorCondition.notify_one();
    }
    _waitingToRunMask |= kNetworkThread;
}

bool NetworkInterfaceMock::hasReadyRequests() {
    return getNumReadyRequests() > 0;
}

size_t NetworkInterfaceMock::getNumReadyRequests() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    return _getNumReadyRequests_inlock(lk);
}

size_t NetworkInterfaceMock::_getNumReadyRequests_inlock(stdx::unique_lock<stdx::mutex>& lk) {
    return std::accumulate(_operations.begin(), _operations.end(), 0, [](auto sum, auto& op) {
        return op.hasReadyRequest() ? sum + 1 : sum;
    });
}

bool NetworkInterfaceMock::isNetworkOperationIteratorAtEnd(
    const NetworkInterfaceMock::NetworkOperationIterator& itr) {
    return itr == _operations.end();
}

NetworkInterfaceMock::NetworkOperationIterator NetworkInterfaceMock::getNextReadyRequest() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);

    auto findNextReadyRequest = [&] {
        return std::find_if(
            _operations.begin(), _operations.end(), [](auto& op) { return op.hasReadyRequest(); });
    };

    auto noi = findNextReadyRequest();
    while (noi == _operations.end()) {
        _waitingToRunMask |= kExecutorThread;
        _runReadyNetworkOperations_inlock(lk);

        noi = findNextReadyRequest();
    }
    noi->markAsProcessing();

    return noi;
}

NetworkInterfaceMock::NetworkOperationIterator NetworkInterfaceMock::getFrontOfReadyQueue() {
    return getNthReadyRequest(0);
}

NetworkInterfaceMock::NetworkOperationIterator NetworkInterfaceMock::getNthReadyRequest(size_t n) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);

    // Linear time, but it's just for testing so no big deal.
    auto noi = _operations.begin();
    for (; noi != _operations.end(); ++noi) {
        if (noi->hasReadyRequest()) {
            if (n == 0) {
                return noi;
            } else {
                --n;
            }
        }
    }

    return _operations.end();
}

void NetworkInterfaceMock::_scheduleResponse_inlock(stdx::unique_lock<stdx::mutex>& lk,
                                                    NetworkOperationIterator noi,
                                                    Date_t when,
                                                    const TaskExecutor::ResponseStatus& response) {
    auto insertBefore = std::find_if(_responses.begin(),
                                     _responses.end(),
                                     [when](const auto& response) { return response.when > when; });

    _responses.insert(insertBefore, NetworkResponse{noi, when, response});
    LOGV2(5440601,
          "Scheduling response",
          "when"_attr = when,
          "request"_attr = noi->getRequest(),
          "response"_attr = response);
}

void NetworkInterfaceMock::scheduleResponse(NetworkOperationIterator noi,
                                            Date_t when,
                                            const TaskExecutor::ResponseStatus& response) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    noi->assertNotBlackholed();
    _scheduleResponse_inlock(lk, noi, when, response);
}

RemoteCommandRequest NetworkInterfaceMock::scheduleSuccessfulResponse(const BSONObj& response) {
    return scheduleSuccessfulResponse(ResponseStatus::make_forTest(response, Milliseconds(0)));
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
    scheduleResponse(noi, when, ResponseStatus::make_forTest(response));
    return noi->getRequest();
}

void NetworkInterfaceMock::blackHole(NetworkOperationIterator noi) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    noi->markAsBlackholed();
}

Date_t NetworkInterfaceMock::runUntil(Date_t until) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    invariant(until > _now_inlock(lk));
    while (until > _now_inlock(lk)) {
        _runReadyNetworkOperations_inlock(lk);
        if (_getNumReadyRequests_inlock(lk) > 0) {
            break;
        }
        Date_t newNow = _executorNextWakeupDate;
        if (!_alarms.empty() && _alarms.begin()->second.when < newNow) {
            newNow = _alarms.begin()->second.when;
        }
        if (!_responses.empty() && _responses.front().when < newNow) {
            newNow = _responses.front().when;
        }
        if (until < newNow) {
            newNow = until;
        }

        auto duration = newNow - _now_inlock(lk);
        invariant(duration >= Milliseconds{0});
        _clkSource->advance(duration);

        _waitingToRunMask |= kExecutorThread;
    }
    _runReadyNetworkOperations_inlock(lk);
    return _now_inlock(lk);
}

void NetworkInterfaceMock::advanceTime(Date_t newTime) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);

    auto duration = newTime - _now_inlock(lk);
    invariant(duration > Milliseconds{0});
    _clkSource->advance(duration);

    _waitingToRunMask |= kExecutorThread;
    _runReadyNetworkOperations_inlock(lk);
}

void NetworkInterfaceMock::runReadyNetworkOperations() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    _runReadyNetworkOperations_inlock(lk);
}

bool NetworkInterfaceMock::_hasUnfinishedNetworkOperations() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    for (auto& op : _operations) {
        if (!op.isFinished()) {
            return true;
        }
    }

    return false;
}

void NetworkInterfaceMock::drainUnfinishedNetworkOperations() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(_currentlyRunning == kNetworkThread);
    }

    while (_hasUnfinishedNetworkOperations()) {
        runReadyNetworkOperations();
    }
}

void NetworkInterfaceMock::waitForWork() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kExecutorThread);
    _waitForWork_inlock(lk);
}

void NetworkInterfaceMock::waitForWorkUntil(Date_t when) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kExecutorThread);
    _executorNextWakeupDate = when;
    if (_executorNextWakeupDate <= _now_inlock(lk)) {
        return;
    }
    _waitForWork_inlock(lk);
}

void NetworkInterfaceMock::_enqueueOperation_inlock(stdx::unique_lock<stdx::mutex>& lk,
                                                    NetworkOperation&& op) {
    const auto timeout = op.getRequest().timeout;
    auto cbh = op.getCallbackHandle();
    auto token = op.getCancellationSource().token();
    auto diagnosticString = op.getDiagnosticString();

    _operations.emplace_back(std::forward<NetworkOperation>(op));

    if (timeout != RemoteCommandRequest::kNoTimeout) {
        invariant(timeout >= Milliseconds(0));
        auto [promise, future] = makePromiseFuture<void>();
        auto when = _now_inlock(lk) + timeout;
        _alarms.insert({when, AlarmInfo(_nextAlarmId++, when, std::move(promise))});
        std::move(future).getAsync([this, cbh](Status status) {
            if (!status.isOK()) {
                return;
            }

            stdx::unique_lock<stdx::mutex> lk(_mutex);
            auto response = ResponseStatus::make_forTest(
                Status(ErrorCodes::NetworkInterfaceExceededTimeLimit, "Network timeout"),
                Milliseconds(0));
            _interruptWithResponse_inlock(lk, cbh, std::move(response));
        });
    }

    lk.unlock();
    token.onCancel().unsafeToInlineFuture().getAsync(
        [this, cbh, requestString = std::move(diagnosticString)](Status status) {
            if (!status.isOK()) {
                return;
            }

            LOGV2(9786900, "Canceling network operation", "request"_attr = requestString);
            if (_onCancelAction) {
                _onCancelAction();
            }

            stdx::unique_lock<stdx::mutex> lk(_mutex);
            ResponseStatus rs = ResponseStatus::make_forTest(
                Status(ErrorCodes::CallbackCanceled, "Network operation canceled"),
                Milliseconds(0));

            _interruptWithResponse_inlock(lk, cbh, rs);
        });
    lk.lock();
}

void NetworkInterfaceMock::_connectThenEnqueueOperation_inlock(stdx::unique_lock<stdx::mutex>& lk,
                                                               const HostAndPort& target,
                                                               NetworkOperation&& op) {
    invariant(_hook);  // if there is no hook, we shouldn't even hit this codepath
    invariant(!_connections.count(target));

    auto handshakeReplyIter = _handshakeReplies.find(target);

    auto handshakeReply = (handshakeReplyIter != std::end(_handshakeReplies))
        ? handshakeReplyIter->second
        : ResponseStatus::make_forTest(BSONObj(), Milliseconds(0));

    auto valid = _hook->validateHost(target, op.getRequest().cmdObj, handshakeReply);
    if (!valid.isOK()) {
        auto response = NetworkResponse{{}, _now_inlock(lk), ResponseStatus::make_forTest(valid)};
        op.fulfillResponse_inlock(lk, std::move(response));
        return;
    }

    auto swHookPostconnectCommand = _hook->makeRequest(target);

    if (!swHookPostconnectCommand.isOK()) {
        auto response =
            NetworkResponse{{},
                            _now_inlock(lk),
                            ResponseStatus::make_forTest(swHookPostconnectCommand.getStatus())};
        op.fulfillResponse_inlock(lk, std::move(response));
        return;
    }

    boost::optional<RemoteCommandRequest> hookPostconnectCommand =
        std::move(swHookPostconnectCommand.getValue());

    if (!hookPostconnectCommand) {
        // If we don't have a post connect command, enqueue the actual command.
        _connections.emplace(op.getRequest().target);
        _enqueueOperation_inlock(lk, std::move(op));
        return;
    }

    auto cbh = op.getCallbackHandle();
    const auto& token = op.getCancellationSource().token();

    auto [promise, future] = makePromiseFuture<TaskExecutor::ResponseStatus>();
    std::move(future).getAsync([this, op = std::move(op)](
                                   StatusWith<TaskExecutor::ResponseStatus> swRs) mutable {
        if (!swRs.isOK()) {
            return;
        }

        auto rs = swRs.getValue();
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        if (!rs.isOK()) {
            auto response = NetworkResponse{{}, _now_inlock(lk), rs};
            op.fulfillResponse_inlock(lk, std::move(response));
            return;
        }

        auto handleStatus = _hook->handleReply(op.getRequest().target, std::move(rs));
        if (!handleStatus.isOK()) {
            auto response =
                NetworkResponse{{}, _now_inlock(lk), ResponseStatus::make_forTest(handleStatus)};
            op.fulfillResponse_inlock(lk, std::move(response));
            return;
        }

        _connections.emplace(op.getRequest().target);
        _enqueueOperation_inlock(lk, std::move(op));
    });
    auto postconnectOp = NetworkOperation(
        cbh, std::move(*hookPostconnectCommand), _now_inlock(lk), token, std::move(promise));

    _enqueueOperation_inlock(lk, std::move(postconnectOp));
}

void NetworkInterfaceMock::setConnectionHook(std::unique_ptr<NetworkConnectionHook> hook) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(!_hasStarted);
    invariant(!_hook);
    _hook = std::move(hook);
}

void NetworkInterfaceMock::setEgressMetadataHook(
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(!_hasStarted);
    invariant(!_metadataHook);
    _metadataHook = std::move(metadataHook);
}

void NetworkInterfaceMock::signalWorkAvailable() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _waitingToRunMask |= kExecutorThread;
    if (_currentlyRunning == kNoThread) {
        _shouldWakeExecutorCondition.notify_one();
    }
}

void NetworkInterfaceMock::_runReadyNetworkOperations_inlock(stdx::unique_lock<stdx::mutex>& lk) {
    while (!_alarms.empty() && _now_inlock(lk) >= _alarms.begin()->first) {
        AlarmInfo alarm = std::move(_alarms.begin()->second);
        _alarms.erase(_alarms.begin());
        _alarmsById.erase(alarm.id);
        auto wasCanceled = _canceledAlarms.erase(alarm.id);

        // If the handle isn't canceled, then run it
        if (!wasCanceled) {
            lk.unlock();
            alarm.promise.emplaceValue();
            lk.lock();
        } else {
            lk.unlock();
            alarm.cancel();
            lk.lock();
        }
    }

    while (!_canceledAlarms.empty()) {
        auto id = *_canceledAlarms.begin();
        _canceledAlarms.erase(_canceledAlarms.begin());
        auto it = _alarmsById[id];
        AlarmInfo alarm = std::move(it->second);
        _alarms.erase(it);
        _alarmsById.erase(id);

        lk.unlock();
        alarm.cancel();
        lk.lock();
    }

    while (!_responses.empty() && _now_inlock(lk) >= _responses.front().when) {
        invariant(_currentlyRunning == kNetworkThread);
        auto response = std::exchange(_responses.front(), {});
        _responses.pop_front();
        _waitingToRunMask |= kExecutorThread;

        auto noi = response.noi;

        LOGV2(5440602,
              "Processing response",
              "when"_attr = response.when,
              "request"_attr = noi->getRequest(),
              "response"_attr = response.response);

        if (_metadataHook && response.response.isOK()) {
            _metadataHook->readReplyMetadata(noi->getRequest().opCtx, response.response.data)
                .transitional_ignore();
        }

        // The NetworkInterface can recieve multiple responses for a particular request (e.g.
        // cancellation and a 'true' scheduled response). But each request can only have one logical
        // response. This choice of the one logical response is mediated by the _isFinished field of
        // the NetworkOperation; whichever response sets this first via
        // NetworkOperation::fulfillResponse wins. NetworkOperation::fulfillResponse returns `true`
        // if the given response was accepted by the NetworkOperation as its sole logical response.
        //
        // We care about this here because we only want to increment the counters for operations
        // succeeded/failed for the responses that are actually used,
        Status localResponseStatus = response.response.status;
        bool noiUsedThisResponse = noi->fulfillResponse_inlock(lk, std::move(response));
        if (noiUsedThisResponse) {
            _counters.sent++;
            if (localResponseStatus.isOK()) {
                _counters.succeeded++;
            } else if (ErrorCodes::isCancellationError(localResponseStatus)) {
                _counters.canceled++;
            } else {
                _counters.failed++;
            }
        }
    }
    invariant(_currentlyRunning == kNetworkThread);
    if (!(_waitingToRunMask & kExecutorThread)) {
        return;
    }
    _shouldWakeExecutorCondition.notify_one();
    _currentlyRunning = kNoThread;
    while (!_isNetworkThreadRunnable_inlock(lk)) {
        _shouldWakeNetworkCondition.wait(lk);
    }
    _currentlyRunning = kNetworkThread;
    _waitingToRunMask &= ~kNetworkThread;
}

bool NetworkInterfaceMock::hasReadyNetworkOperations() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_currentlyRunning == kNetworkThread);
    if (!_alarms.empty() && _now_inlock(lk) >= _alarms.begin()->second.when) {
        return true;
    }

    if (!_responses.empty() && _responses.front().when <= _now_inlock(lk)) {
        return true;
    }
    return false;
}

void NetworkInterfaceMock::_waitForWork_inlock(stdx::unique_lock<stdx::mutex>& lk) {
    if (_waitingToRunMask & kExecutorThread) {
        _waitingToRunMask &= ~kExecutorThread;
        return;
    }
    _currentlyRunning = kNoThread;
    while (!_isExecutorThreadRunnable_inlock(lk)) {
        _waitingToRunMask |= kNetworkThread;
        _shouldWakeNetworkCondition.notify_one();
        _shouldWakeExecutorCondition.wait(lk);
    }
    _currentlyRunning = kExecutorThread;
    _waitingToRunMask &= ~kExecutorThread;
}

bool NetworkInterfaceMock::_isNetworkThreadRunnable_inlock(stdx::unique_lock<stdx::mutex>& lk) {
    if (_currentlyRunning != kNoThread) {
        return false;
    }
    if (_waitingToRunMask != kNetworkThread) {
        return false;
    }
    return true;
}

bool NetworkInterfaceMock::_isExecutorThreadRunnable_inlock(stdx::unique_lock<stdx::mutex>& lk) {
    if (_currentlyRunning != kNoThread) {
        return false;
    }
    return _waitingToRunMask & kExecutorThread;
}

NetworkInterfaceMock::NetworkOperation::NetworkOperation()
    : _requestDate(), _request(), _cancelSource(), _respPromise() {}

NetworkInterfaceMock::NetworkOperation::NetworkOperation(
    const CallbackHandle& cbHandle,
    const RemoteCommandRequest& theRequest,
    Date_t theRequestDate,
    const CancellationToken& token,
    Promise<TaskExecutor::ResponseStatus> promise)
    : _requestDate(theRequestDate),
      _cbHandle(cbHandle),
      _request(theRequest),
      _cancelSource(token),
      _respPromise(std::move(promise)) {}

std::string NetworkInterfaceMock::NetworkOperation::getDiagnosticString() const {
    return str::stream() << "NetworkOperation -- request:'" << _request.toString()
                         << ", reqDate: " << _requestDate.toString();
}

bool NetworkInterfaceMock::NetworkOperation::fulfillResponse_inlock(
    stdx::unique_lock<stdx::mutex>& lk, NetworkResponse response) {
    if (_isFinished) {
        // Nothing to do.
        return false;
    }

    _isFinished = true;
    response.response.target = _request.target;

    // Release the lock since inline callbacks will attempt to grab the lock again.
    lk.unlock();
    _respPromise.emplaceValue(response.response);
    lk.lock();

    return true;
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

}  // namespace executor
}  // namespace mongo

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

#include "mongo/platform/basic.h"

#include "mongo/executor/network_interface_mock.h"

#include "mongo/db/repl/operation_context_repl_mock.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {

    NetworkInterfaceMock::NetworkInterfaceMock()
        : _waitingToRunMask(0),
          _currentlyRunning(kNoThread),
          _now(fassertStatusOK(18653, dateFromISOString("2014-08-01T00:00:00Z"))),
          _hasStarted(false),
          _inShutdown(false),
          _executorNextWakeupDate(Date_t::max()) {
    }

    NetworkInterfaceMock::~NetworkInterfaceMock() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(!_hasStarted || _inShutdown);
        invariant(_scheduled.empty());
        invariant(_blackHoled.empty());
    }

    std::string NetworkInterfaceMock::getDiagnosticString() {
        // TODO something better.
        return "NetworkInterfaceMock diagnostics here";
    }

    Date_t NetworkInterfaceMock::now() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _now_inlock();
    }

    void NetworkInterfaceMock::startCommand(
            const repl::ReplicationExecutor::CallbackHandle& cbHandle,
            const RemoteCommandRequest& request,
            const RemoteCommandCompletionFn& onFinish) {

        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(!_inShutdown);
        const Date_t now = _now_inlock();
        NetworkOperationIterator insertBefore = _unscheduled.begin();
        while ((insertBefore != _unscheduled.end()) &&
               (insertBefore->getNextConsiderationDate() <= now)) {

            ++insertBefore;
        }
        _unscheduled.insert(insertBefore, NetworkOperation(cbHandle, request, now, onFinish));
    }

    static bool findAndCancelIf(
            const stdx::function<bool (const NetworkInterfaceMock::NetworkOperation&)>& matchFn,
            NetworkInterfaceMock::NetworkOperationList* other,
            NetworkInterfaceMock::NetworkOperationList* scheduled,
            const Date_t now) {
        const NetworkInterfaceMock::NetworkOperationIterator noi =
            std::find_if(other->begin(), other->end(), matchFn);
        if (noi == other->end()) {
            return false;
        }
        scheduled->splice(scheduled->begin(), *other, noi);
        noi->setResponse(now, repl::ResponseStatus(ErrorCodes::CallbackCanceled,
                                                   "Network operation canceled"));
        return true;
    }

    void NetworkInterfaceMock::cancelCommand(
            const repl::ReplicationExecutor::CallbackHandle& cbHandle) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(!_inShutdown);
        stdx::function<bool (const NetworkOperation&)> matchesHandle = stdx::bind(
                &NetworkOperation::isForCallback,
                stdx::placeholders::_1,
                cbHandle);
        const Date_t now = _now_inlock();
        if (findAndCancelIf(matchesHandle, &_unscheduled, &_scheduled, now)) {
            return;
        }
        if (findAndCancelIf(matchesHandle, &_blackHoled, &_scheduled, now)) {
            return;
        }
        if (findAndCancelIf(matchesHandle, &_scheduled, &_scheduled, now)) {
            return;
        }
        // No not-in-progress network command matched cbHandle.  Oh, well.
    }

    void NetworkInterfaceMock::startup() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(!_hasStarted);
        _hasStarted = true;
        _inShutdown = false;
        invariant(_currentlyRunning == kNoThread);
        _currentlyRunning = kExecutorThread;
    }

    void NetworkInterfaceMock::shutdown() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(_hasStarted);
        invariant(!_inShutdown);
        _inShutdown = true;
        NetworkOperationList todo;
        todo.splice(todo.end(), _scheduled);
        todo.splice(todo.end(), _unscheduled);
        todo.splice(todo.end(), _processing);
        todo.splice(todo.end(), _blackHoled);

        const Date_t now = _now_inlock();
        _waitingToRunMask |= kExecutorThread;  // Prevents network thread from scheduling.
        lk.unlock();
        for (NetworkOperationIterator iter = todo.begin(); iter != todo.end(); ++iter) {
            iter->setResponse(now, repl::ResponseStatus(ErrorCodes::ShutdownInProgress,
                                                        "Shutting down mock network"));
            iter->finishResponse();
        }
        lk.lock();
        invariant(_currentlyRunning == kExecutorThread);
        _currentlyRunning = kNoThread;
        _waitingToRunMask = kNetworkThread;
        _shouldWakeNetworkCondition.notify_one();
    }

    void NetworkInterfaceMock::enterNetwork() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (!_isNetworkThreadRunnable_inlock()) {
            _shouldWakeNetworkCondition.wait(lk);
        }
        _currentlyRunning = kNetworkThread;
        _waitingToRunMask &= ~kNetworkThread;
    }

    void NetworkInterfaceMock::exitNetwork() {
        boost::lock_guard<boost::mutex> lk(_mutex);
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
        boost::lock_guard<boost::mutex> lk(_mutex);
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
        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(_currentlyRunning == kNetworkThread);
        while (!_hasReadyRequests_inlock()) {
            _waitingToRunMask |= kExecutorThread;
            _runReadyNetworkOperations_inlock(&lk);
        }
        invariant(_hasReadyRequests_inlock());
        _processing.splice(_processing.begin(), _unscheduled, _unscheduled.begin());
        return _processing.begin();
    }

    void NetworkInterfaceMock::scheduleResponse(
            NetworkOperationIterator noi,
            Date_t when,
            const repl::ResponseStatus& response) {

        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(_currentlyRunning == kNetworkThread);
        NetworkOperationIterator insertBefore = _scheduled.begin();
        while ((insertBefore != _scheduled.end()) && (insertBefore->getResponseDate() <= when)) {
            ++insertBefore;
        }
        noi->setResponse(when, response);
        _scheduled.splice(insertBefore, _processing, noi);
    }

    void NetworkInterfaceMock::blackHole(NetworkOperationIterator noi) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(_currentlyRunning == kNetworkThread);
        _blackHoled.splice(_blackHoled.end(), _processing, noi);
    }

    void NetworkInterfaceMock::requeueAt(NetworkOperationIterator noi, Date_t dontAskUntil) {
        boost::lock_guard<boost::mutex> lk(_mutex);
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

    void NetworkInterfaceMock::runUntil(Date_t until) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(_currentlyRunning == kNetworkThread);
        invariant(until > _now_inlock());
        while (until > _now_inlock()) {
            _runReadyNetworkOperations_inlock(&lk);
            if (_hasReadyRequests_inlock()) {
                break;
            }
            Date_t newNow = _executorNextWakeupDate;
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
    }

    void NetworkInterfaceMock::runReadyNetworkOperations() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(_currentlyRunning == kNetworkThread);
        _runReadyNetworkOperations_inlock(&lk);
    }

    void NetworkInterfaceMock::waitForWork() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(_currentlyRunning == kExecutorThread);
        _waitForWork_inlock(&lk);
    }

    void NetworkInterfaceMock::waitForWorkUntil(Date_t when) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        invariant(_currentlyRunning == kExecutorThread);
        _executorNextWakeupDate = when;
        if (_executorNextWakeupDate <= _now_inlock()) {
            return;
        }
        _waitForWork_inlock(&lk);
    }

    void NetworkInterfaceMock::signalWorkAvailable() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        _waitingToRunMask |= kExecutorThread;
        if (_currentlyRunning == kNoThread) {
            _shouldWakeExecutorCondition.notify_one();
        }
    }

    void NetworkInterfaceMock::_runReadyNetworkOperations_inlock(
            boost::unique_lock<boost::mutex>* lk) {
        while (!_scheduled.empty() && _scheduled.front().getResponseDate() <= _now_inlock()) {
            invariant(_currentlyRunning == kNetworkThread);
            NetworkOperation op = _scheduled.front();
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

    void NetworkInterfaceMock::_waitForWork_inlock(boost::unique_lock<boost::mutex>* lk) {
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

    static const StatusWith<RemoteCommandResponse> kUnsetResponse(
            ErrorCodes::InternalError,
            "NetworkOperation::_response never set");

    NetworkInterfaceMock::NetworkOperation::NetworkOperation()
        : _requestDate(),
          _nextConsiderationDate(),
          _responseDate(),
          _request(),
          _response(kUnsetResponse),
          _onFinish() {
    }

    NetworkInterfaceMock::NetworkOperation::NetworkOperation(
            const repl::ReplicationExecutor::CallbackHandle& cbHandle,
            const RemoteCommandRequest& theRequest,
            Date_t theRequestDate,
            const RemoteCommandCompletionFn& onFinish)
        : _requestDate(theRequestDate),
          _nextConsiderationDate(theRequestDate),
          _responseDate(),
          _cbHandle(cbHandle),
          _request(theRequest),
          _response(kUnsetResponse),
          _onFinish(onFinish) {
    }

    NetworkInterfaceMock::NetworkOperation::~NetworkOperation() {}

    void NetworkInterfaceMock::NetworkOperation::setNextConsiderationDate(
            Date_t nextConsiderationDate) {

        invariant(nextConsiderationDate > _nextConsiderationDate);
        _nextConsiderationDate = nextConsiderationDate;
    }

    void NetworkInterfaceMock::NetworkOperation::setResponse(
            Date_t responseDate,
            const repl::ResponseStatus& response) {

        invariant(responseDate >= _requestDate);
        _responseDate = responseDate;
        _response = response;
    }

    void NetworkInterfaceMock::NetworkOperation::finishResponse() {
        invariant(_onFinish);
        _onFinish(_response);
        _onFinish = RemoteCommandCompletionFn();
    }

}  // namespace executor
}  // namespace mongo

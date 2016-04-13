/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/db/repl/scatter_gather_runner.h"

#include <algorithm>

#include "mongo/base/status_with.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

using executor::RemoteCommandRequest;
using LockGuard = stdx::lock_guard<stdx::mutex>;
using CallbackHandle = ReplicationExecutor::CallbackHandle;
using EventHandle = ReplicationExecutor::EventHandle;
using RemoteCommandCallbackArgs = ReplicationExecutor::RemoteCommandCallbackArgs;
using RemoteCommandCallbackFn = ReplicationExecutor::RemoteCommandCallbackFn;

ScatterGatherRunner::ScatterGatherRunner(ScatterGatherAlgorithm* algorithm,
                                         ReplicationExecutor* executor)
    : _executor(executor), _impl(std::make_shared<RunnerImpl>(algorithm, executor)) {}

Status ScatterGatherRunner::run() {
    auto finishEvh = start();
    if (!finishEvh.isOK()) {
        return finishEvh.getStatus();
    }
    _executor->waitForEvent(finishEvh.getValue());
    return Status::OK();
}

StatusWith<EventHandle> ScatterGatherRunner::start() {
    // Callback has a shared pointer to the RunnerImpl, so it's always safe to
    // access the RunnerImpl.
    // Note: this creates a cycle of shared_ptr:
    //     RunnerImpl -> Callback in _callbacks -> RunnerImpl
    // We must remove callbacks after using them, to break this cycle.
    std::shared_ptr<RunnerImpl>& impl = _impl;
    auto cb = [impl](const RemoteCommandCallbackArgs& cbData) { impl->processResponse(cbData); };
    return _impl->start(cb);
}

void ScatterGatherRunner::cancel() {
    _impl->cancel();
}

/**
 * Scatter gather runner implementation.
 */
ScatterGatherRunner::RunnerImpl::RunnerImpl(ScatterGatherAlgorithm* algorithm,
                                            ReplicationExecutor* executor)
    : _executor(executor), _algorithm(algorithm) {}

StatusWith<EventHandle> ScatterGatherRunner::RunnerImpl::start(
    const RemoteCommandCallbackFn processResponseCB) {
    LockGuard lk(_mutex);

    invariant(!_started);
    _started = true;
    StatusWith<EventHandle> evh = _executor->makeEvent();
    if (!evh.isOK()) {
        return evh;
    }
    _sufficientResponsesReceived = evh.getValue();
    ScopeGuard earlyReturnGuard = MakeGuard(&RunnerImpl::_signalSufficientResponsesReceived, this);

    std::vector<RemoteCommandRequest> requests = _algorithm->getRequests();
    for (size_t i = 0; i < requests.size(); ++i) {
        const StatusWith<CallbackHandle> cbh =
            _executor->scheduleRemoteCommand(requests[i], processResponseCB);
        if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
            return StatusWith<EventHandle>(cbh.getStatus());
        }
        fassert(18743, cbh.getStatus());
        _callbacks.push_back(cbh.getValue());
    }

    if (_callbacks.empty() || _algorithm->hasReceivedSufficientResponses()) {
        invariant(_algorithm->hasReceivedSufficientResponses());
        _signalSufficientResponsesReceived();
    }

    earlyReturnGuard.Dismiss();
    return evh;
}

void ScatterGatherRunner::RunnerImpl::cancel() {
    LockGuard lk(_mutex);

    invariant(_started);
    _signalSufficientResponsesReceived();
}

void ScatterGatherRunner::RunnerImpl::processResponse(
    const ReplicationExecutor::RemoteCommandCallbackArgs& cbData) {
    LockGuard lk(_mutex);

    if (!_sufficientResponsesReceived.isValid()) {
        // We've received sufficient responses and it's not safe to access the algorithm any more.
        return;
    }

    // Remove the callback from our vector to break the cycle of shared_ptr.
    auto iter = std::find(_callbacks.begin(), _callbacks.end(), cbData.myHandle);
    invariant(iter != _callbacks.end());
    std::swap(*iter, _callbacks.back());
    _callbacks.pop_back();

    if (cbData.response.getStatus() == ErrorCodes::CallbackCanceled) {
        return;
    }

    _algorithm->processResponse(cbData.request, cbData.response);
    if (_algorithm->hasReceivedSufficientResponses()) {
        _signalSufficientResponsesReceived();
    } else {
        invariant(!_callbacks.empty());
    }
}

void ScatterGatherRunner::RunnerImpl::_signalSufficientResponsesReceived() {
    if (_sufficientResponsesReceived.isValid()) {
        std::for_each(_callbacks.begin(),
                      _callbacks.end(),
                      stdx::bind(&ReplicationExecutor::cancel, _executor, stdx::placeholders::_1));
        // Clear _callbacks to break the cycle of shared_ptr.
        _callbacks.clear();
        _executor->signalEvent(_sufficientResponsesReceived);
        _sufficientResponsesReceived = EventHandle();
    }
}

}  // namespace repl
}  // namespace mongo

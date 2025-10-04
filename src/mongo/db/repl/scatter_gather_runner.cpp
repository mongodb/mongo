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


#include <cstddef>

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

using executor::RemoteCommandRequest;
using LockGuard = stdx::lock_guard<stdx::mutex>;
using CallbackHandle = executor::TaskExecutor::CallbackHandle;
using EventHandle = executor::TaskExecutor::EventHandle;
using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using RemoteCommandCallbackFn = executor::TaskExecutor::RemoteCommandCallbackFn;

ScatterGatherRunner::ScatterGatherRunner(std::shared_ptr<ScatterGatherAlgorithm> algorithm,
                                         executor::TaskExecutor* executor,
                                         std::string logMessage)
    : _executor(executor),
      _impl(std::make_shared<RunnerImpl>(std::move(algorithm), executor, std::move(logMessage))) {}

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
    auto cb = [impl](const RemoteCommandCallbackArgs& cbData) {
        impl->processResponse(cbData);
    };
    return _impl->start(cb);
}

void ScatterGatherRunner::cancel() {
    _impl->cancel();
}

/**
 * Scatter gather runner implementation.
 */
ScatterGatherRunner::RunnerImpl::RunnerImpl(std::shared_ptr<ScatterGatherAlgorithm> algorithm,
                                            executor::TaskExecutor* executor,
                                            std::string logMessage)
    : _executor(executor), _algorithm(std::move(algorithm)), _logMessage(std::move(logMessage)) {}

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
    ScopeGuard earlyReturnGuard([this] { _signalSufficientResponsesReceived(); });

    std::vector<RemoteCommandRequest> requests = _algorithm->getRequests();
    for (size_t i = 0; i < requests.size(); ++i) {
        LOGV2(21752,
              "Scheduling remote command request",
              "context"_attr = _logMessage,
              "request"_attr = requests[i].toString());
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

    earlyReturnGuard.dismiss();
    return evh;
}

void ScatterGatherRunner::RunnerImpl::cancel() {
    LockGuard lk(_mutex);

    invariant(_started);
    _signalSufficientResponsesReceived();
}

void ScatterGatherRunner::RunnerImpl::processResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {
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

    _algorithm->processResponse(cbData.request, cbData.response);
    if (_algorithm->hasReceivedSufficientResponses()) {
        _signalSufficientResponsesReceived();
    } else {
        invariant(!_callbacks.empty());
    }
}

void ScatterGatherRunner::RunnerImpl::_signalSufficientResponsesReceived() {
    if (_sufficientResponsesReceived.isValid()) {
        for (const CallbackHandle& cbh : _callbacks) {
            _executor->cancel(cbh);
        };
        // Clear _callbacks to break the cycle of shared_ptr.
        _callbacks.clear();
        _executor->signalEvent(_sufficientResponsesReceived);
        _sufficientResponsesReceived = EventHandle();
    }
}

}  // namespace repl
}  // namespace mongo

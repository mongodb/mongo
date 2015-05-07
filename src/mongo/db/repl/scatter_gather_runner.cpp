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

    ScatterGatherRunner::ScatterGatherRunner(ScatterGatherAlgorithm* algorithm) :
        _algorithm(algorithm),
        _started(false) {
    }

    ScatterGatherRunner::~ScatterGatherRunner() {
    }

    static void startTrampoline(const ReplicationExecutor::CallbackData& cbData,
                                ScatterGatherRunner* runner,
                                StatusWith<ReplicationExecutor::EventHandle>* result) {

        *result = runner->start(cbData.executor);
    }

    Status ScatterGatherRunner::run(ReplicationExecutor* executor) {
        StatusWith<ReplicationExecutor::EventHandle> finishEvh(ErrorCodes::InternalError,
                                                               "Not set");
        StatusWith<ReplicationExecutor::CallbackHandle> startCBH = executor->scheduleWork(
                stdx::bind(startTrampoline, stdx::placeholders::_1, this, &finishEvh));
        if (!startCBH.isOK()) {
            return startCBH.getStatus();
        }
        executor->wait(startCBH.getValue());
        if (!finishEvh.isOK()) {
            return finishEvh.getStatus();
        }
        executor->waitForEvent(finishEvh.getValue());
        return Status::OK();
    }

    StatusWith<ReplicationExecutor::EventHandle> ScatterGatherRunner::start(
            ReplicationExecutor* executor,
            const stdx::function<void ()>& onCompletion) {

        invariant(!_started);
        _started = true;
        _actualResponses = 0;
        _onCompletion = onCompletion;
        StatusWith<ReplicationExecutor::EventHandle> evh = executor->makeEvent();
        if (!evh.isOK()) {
            return evh;
        }
        _sufficientResponsesReceived = evh.getValue();
        ScopeGuard earlyReturnGuard = MakeGuard(
                &ScatterGatherRunner::_signalSufficientResponsesReceived,
                this,
                executor);

        const ReplicationExecutor::RemoteCommandCallbackFn cb = stdx::bind(
                &ScatterGatherRunner::_processResponse,
                stdx::placeholders::_1,
                this);

        std::vector<RemoteCommandRequest> requests = _algorithm->getRequests();
        for (size_t i = 0; i < requests.size(); ++i) {
            const StatusWith<ReplicationExecutor::CallbackHandle> cbh =
                executor->scheduleRemoteCommand(requests[i], cb);
            if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
                return StatusWith<ReplicationExecutor::EventHandle>(cbh.getStatus());
            }
            fassert(18743, cbh.getStatus());
            _callbacks.push_back(cbh.getValue());
        }

        if (_callbacks.empty() || _algorithm->hasReceivedSufficientResponses()) {
            invariant(_algorithm->hasReceivedSufficientResponses());
            _signalSufficientResponsesReceived(executor);
        }

        earlyReturnGuard.Dismiss();
        return evh;
    }

    void ScatterGatherRunner::cancel(ReplicationExecutor* executor) {
        invariant(_started);
        _signalSufficientResponsesReceived(executor);
    }

    void ScatterGatherRunner::_processResponse(
            const ReplicationExecutor::RemoteCommandCallbackData& cbData,
            ScatterGatherRunner* runner) {

        // It is possible that the ScatterGatherRunner has already gone out of scope, if the
        // response indicates the callback was canceled.  In that case, do not access any members
        // of "runner" and return immediately.
        if (cbData.response.getStatus() == ErrorCodes::CallbackCanceled) {
            return;
        }

        ++runner->_actualResponses;
        runner->_algorithm->processResponse(cbData.request, cbData.response);
        if (runner->_algorithm->hasReceivedSufficientResponses()) {
            runner->_signalSufficientResponsesReceived(cbData.executor);
        }
        else {
            invariant(runner->_actualResponses < runner->_callbacks.size());
        }
    }

    void ScatterGatherRunner::_signalSufficientResponsesReceived(ReplicationExecutor* executor) {
        if (_sufficientResponsesReceived.isValid()) {
            std::for_each(_callbacks.begin(),
                          _callbacks.end(),
                          stdx::bind(&ReplicationExecutor::cancel,
                                     executor,
                                     stdx::placeholders::_1));
            const ReplicationExecutor::EventHandle h = _sufficientResponsesReceived;
            _sufficientResponsesReceived = ReplicationExecutor::EventHandle();
            if (_onCompletion) {
                _onCompletion();
            }
            executor->signalEvent(h);
        }
    }

}  // namespace repl
}  // namespace mongo

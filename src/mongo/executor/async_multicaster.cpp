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


#include "mongo/executor/async_multicaster.h"

#include "mongo/client/retry_strategy.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace executor {

AsyncMulticaster::AsyncMulticaster(std::shared_ptr<executor::TaskExecutor> executor,
                                   Options options)
    : _options(options), _executor(std::move(executor)) {}

std::vector<AsyncMulticaster::Reply> AsyncMulticaster::multicast(
    const std::vector<HostAndPort> servers,
    const DatabaseName& theDbName,
    const BSONObj& theCmdObj,
    OperationContext* opCtx,
    Milliseconds timeoutMillis) {

    auto state = std::make_shared<State>(servers.size());
    state->out.reserve(servers.size());
    state->strategies.reserve(servers.size());

    // Set default strategy factory if none provided
    if (!_options.strategyFactory) {
        _options.strategyFactory = []() {
            return std::make_unique<DefaultRetryStrategy>(
                DefaultRetryStrategy::RetryCriteria{
                    [](Status s, std::span<const std::string> labels) {
                        return containsRetryableLabels(labels);
                    }},
                DefaultRetryStrategy::getRetryParametersFromServerParameters());
        };
    }

    // Update the strategy creation loop in multicast method:
    for (const auto& server : servers) {
        state->strategies.emplace(server, _options.strategyFactory());
    }

    for (const auto& server : servers) {
        stdx::unique_lock<stdx::mutex> lk(state->mutex);
        // spin up no more than maxConcurrency tasks at once
        opCtx->waitForConditionOrInterrupt(
            state->cv, lk, [&] { return state->running < _options.maxConcurrency; });
        ++state->running;
        RemoteCommandRequest request{server, theDbName, theCmdObj, opCtx, timeoutMillis};
        _scheduleAttempt(state, request, opCtx->getCancellationToken(), server);
    }

    stdx::unique_lock<stdx::mutex> lk(state->mutex);
    opCtx->waitForConditionOrInterrupt(state->cv, lk, [&] { return state->leftToDo == 0; });

    return std::move(state->out);
}

void AsyncMulticaster::_scheduleAttempt(std::shared_ptr<State> state,
                                        const RemoteCommandRequest& request,
                                        const CancellationToken& cancellationToken,
                                        const HostAndPort& server) {
    uassertStatusOK(_executor->scheduleRemoteCommand(
        request,
        [this, state, server, request, cancellationToken](
            const TaskExecutor::RemoteCommandCallbackArgs& cbData) {
            stdx::lock_guard<stdx::mutex> lk(state->mutex);

            auto it = state->strategies.find(cbData.request.target);
            tassert(10944600,
                    str::stream() << "Retry strategy not found for target shard "
                                  << cbData.request.target,
                    it != state->strategies.end());
            auto* hostRetryStrategy = it->second.get();

            bool shouldRetry = false;

            if (cbData.response.isOK() && cbData.response.getErrorLabels().empty()) {
                hostRetryStrategy->recordSuccess(cbData.request.target);
            } else {
                shouldRetry = hostRetryStrategy->recordFailureAndEvaluateShouldRetry(
                    cbData.response.status,
                    cbData.request.target,
                    cbData.response.getErrorLabels());
            }
            if (shouldRetry) {
                const auto delay = hostRetryStrategy->getNextRetryDelay();
                _executor->sleepFor(delay, cancellationToken)
                    .getAsync([this,
                               state,
                               server,
                               request,
                               cancellationToken,
                               originalResponse = cbData.response,
                               delay](Status s) {
                        stdx::lock_guard<stdx::mutex> lk(state->mutex);
                        if (s.isOK()) {
                            auto it = state->strategies.find(server);
                            if (it != state->strategies.end()) {
                                it->second->recordBackoff(delay);
                            }
                            _scheduleAttempt(state, request, cancellationToken, server);
                        } else {
                            _onComplete(lk, state, server, originalResponse);
                        }
                    });
            } else {
                _onComplete(lk, state, cbData.request.target, cbData.response);
            }
        }));
}

void AsyncMulticaster::_onComplete(WithLock,
                                   std::shared_ptr<State> state,
                                   const HostAndPort& server,
                                   const RemoteCommandResponse& response) {
    state->out.emplace_back(std::forward_as_tuple(server, response));

    // If we were the last job, flush the done flag and release via notify.
    if (!--(state->leftToDo)) {
        state->cv.notify_one();
    }

    if (--(state->running) < kMaxConcurrency) {
        state->cv.notify_one();
    }
}

}  // namespace executor
}  // namespace mongo

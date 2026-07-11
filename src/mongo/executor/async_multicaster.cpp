// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/executor/async_multicaster.h"

#include "mongo/client/retry_strategy.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/stdx/condition_variable.h"
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
                DefaultRetryStrategy::unconditionallyRetryableCriteria,
                DefaultRetryStrategy::getRetryParametersFromServerParameters());
        };
    }

    // Update the strategy creation loop in multicast method:
    for (const auto& server : servers) {
        state->strategies.emplace(server, _options.strategyFactory());
    }

    for (const auto& server : servers) {
        std::unique_lock<std::mutex> lk(state->mutex);
        // spin up no more than maxConcurrency tasks at once
        opCtx->waitForConditionOrInterrupt(
            state->cv, lk, [&] { return state->running < _options.maxConcurrency; });
        ++state->running;
        RemoteCommandRequest request{server, theDbName, theCmdObj, opCtx, timeoutMillis};
        _scheduleAttempt(state, request, opCtx->getCancellationToken(), server);
    }

    std::unique_lock<std::mutex> lk(state->mutex);
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
            std::lock_guard<std::mutex> lk(state->mutex);

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
                    cbData.response.getErrorLabels(),
                    cbData.response.getBaseBackoffMS());
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
                        std::lock_guard<std::mutex> lk(state->mutex);
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

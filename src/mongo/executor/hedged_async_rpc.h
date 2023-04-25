/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/kill_operations_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_error_info.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/hedge_options_util.h"
#include "mongo/executor/hedging_metrics.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"
#include "mongo/util/net/hostandport.h"
#include <cstddef>
#include <memory>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor

namespace mongo {
namespace async_rpc {

namespace hedging_rpc_details {
/**
 * Given a vector of input Futures, whenAnyThat returns a Future which holds the value
 * of the first of those futures to resolve with a status, value, and index that
 * satisfies the conditions in the ConditionCallable Callable.
 */
template <typename SingleResponse, typename ConditionCallable>
Future<SingleResponse> whenAnyThat(std::vector<ExecutorFuture<SingleResponse>>&& futures,
                                   ConditionCallable&& shouldAccept) {
    invariant(futures.size() > 0);

    struct SharedBlock {
        SharedBlock(Promise<SingleResponse> result) : resultPromise(std::move(result)) {}
        // Tracks whether or not the resultPromise has been set.
        AtomicWord<bool> done{false};
        // The promise corresponding to the resulting SemiFuture returned by this function.
        Promise<SingleResponse> resultPromise;
    };

    Promise<SingleResponse> promise{NonNullPromiseTag{}};
    auto future = promise.getFuture();
    auto sharedBlock = std::make_shared<SharedBlock>(std::move(promise));

    for (size_t i = 0; i < futures.size(); ++i) {
        std::move(futures[i])
            .getAsync(
                [sharedBlock, myIndex = i, shouldAccept](StatusOrStatusWith<SingleResponse> value) {
                    if (shouldAccept(value, myIndex)) {
                        // If this is the first input future to complete and satisfy the
                        // shouldAccept condition, change done to true and set the value on the
                        // promise.
                        if (!sharedBlock->done.swap(true)) {
                            sharedBlock->resultPromise.setFrom(std::move(value));
                        }
                    }
                });
    }

    return future;
}

HedgingMetrics* getHedgingMetrics(ServiceContext* svcCtx) {
    auto hm = HedgingMetrics::get(svcCtx);
    invariant(hm);
    return hm;
}

/**
 * Schedules a remote `_killOperations` on `exec` (or `baton`) for all targets, aiming to kill any
 * operations identified by `opKey`.
 */
void killOperations(ServiceContext* svcCtx,
                    std::vector<HostAndPort>& targets,
                    std::shared_ptr<executor::TaskExecutor> exec,
                    UUID opKey) {
    KillOperationsRequest cmd({opKey});
    auto options = std::make_shared<AsyncRPCOptions<KillOperationsRequest>>(
        std::move(cmd),
        std::move(exec),
        CancellationToken::uncancelable(),
        std::make_shared<NeverRetryPolicy>(),
        GenericArgs());
    for (const auto& target : targets) {
        LOGV2_DEBUG(7301601,
                    2,
                    "Sending killOperations to cancel the remote command",
                    "operationKey"_attr = opKey,
                    "target"_attr = target);
        sendCommand(options, svcCtx, std::make_unique<FixedTargeter>(target))
            .ignoreValue()
            .getAsync([](Status) {});
    }
}
}  // namespace hedging_rpc_details

/**
 * sendHedgedCommand is a hedged version of the sendCommand function. It asynchronously executes a
 * hedged request by sending commands to multiple targets through sendCommand and then returns a
 * SemiFuture with the first result to become ready.
 *
 * In order to hedge, the command must be eligible for hedging, the hedgingMode server parameter
 * must be enabled, and multiple hosts must be provided by the targeter. If any of those conditions
 * is false, then the function will not hedge, and instead will just target the first host in the
 * vector provided by resolve.
 *
 * Accepts an optional UUID to be used as `clientOperationKey` for all remote requests.
 */
template <typename CommandType>
SemiFuture<AsyncRPCResponse<typename CommandType::Reply>> sendHedgedCommand(
    CommandType cmd,
    OperationContext* opCtx,
    std::unique_ptr<Targeter> targeter,
    std::shared_ptr<executor::TaskExecutor> exec,
    CancellationToken token,
    std::shared_ptr<RetryPolicy> retryPolicy = std::make_shared<NeverRetryPolicy>(),
    ReadPreferenceSetting readPref = ReadPreferenceSetting(ReadPreference::PrimaryOnly),
    GenericArgs genericArgs = GenericArgs(),
    BatonHandle baton = nullptr,
    boost::optional<UUID> clientOperationKey = boost::none) {
    using SingleResponse = AsyncRPCResponse<typename CommandType::Reply>;

    invariant(opCtx);
    auto svcCtx = opCtx->getServiceContext();

    if (MONGO_unlikely(clientOperationKey && !genericArgs.stable.getClientOperationKey())) {
        genericArgs.stable.setClientOperationKey(*clientOperationKey);
    }

    // Set up cancellation token to cancel remaining hedged operations.
    CancellationSource hedgeCancellationToken{token};
    auto targetsAttempted = std::make_shared<std::vector<HostAndPort>>();
    auto proxyExec = std::make_shared<detail::ProxyingExecutor>(baton, exec);
    auto tryBody = [=, targeter = std::move(targeter)]() mutable {
        HedgeOptions opts = getHedgeOptions(CommandType::kCommandName, readPref);
        auto operationKey = [&] {
            // Check if the caller has provided an operation key, and hedging is not enabled. If so,
            // we will attach the caller-provided key to all remote commands sent to resolved
            // targets. Note that doing so may have side-effects if the operation is retried:
            // cancelling the Nth attempt may impact the (N + 1)th attempt as they share `opKey`.
            if (auto& opKey = genericArgs.stable.getClientOperationKey();
                opKey && !opts.isHedgeEnabled) {
                return *opKey;
            }

            // The caller has not provided an operation key or hedging is enabled, so we generate a
            // new `clientOperationKey` for each attempt. The operationKey allows cancelling remote
            // operations. A new one is generated here to ensure retry attempts are isolated:
            // cancelling the Nth attempt does not impact the (N + 1)th attempt.
            auto opKey = UUID::gen();
            genericArgs.stable.setClientOperationKey(opKey);
            return opKey;
        }();

        return targeter->resolve(token)
            .thenRunOn(proxyExec)
            .onError([](Status status) -> StatusWith<std::vector<HostAndPort>> {
                // Targeting error; rewrite it to a RemoteCommandExecutionError and skip
                // command execution body. We'll retry if the policy indicates to.
                return Status{AsyncRPCErrorInfo(status), status.reason()};
            })
            .then([=](std::vector<HostAndPort> targets) {
                invariant(targets.size(),
                          "Successful targeting implies there are hosts to target.");
                *targetsAttempted = targets;

                auto hm = hedging_rpc_details::getHedgingMetrics(getGlobalServiceContext());
                if (opts.isHedgeEnabled) {
                    hm->incrementNumTotalOperations();
                }

                std::vector<ExecutorFuture<SingleResponse>> requests;

                // We'll send 1 authoritative command + however many hedges we can.
                size_t hostsToTarget = std::min(opts.hedgeCount + 1, targets.size());

                bool targetHostsInAlphabeticalOrder = MONGO_unlikely(
                    hedgedReadsSendRequestsToTargetHostsInAlphabeticalOrder.shouldFail(
                        [&](const BSONObj&) { return opts.isHedgeEnabled; }));

                if (targetHostsInAlphabeticalOrder) {
                    std::sort(targets.begin(), targets.end(), [](auto&& a, auto&& b) {
                        return compareByLowerHostThenPort(a, b);
                    });
                }

                for (size_t i = 0; i < hostsToTarget; i++) {
                    std::unique_ptr<Targeter> t = std::make_unique<FixedTargeter>(targets[i]);
                    // We explicitly pass "NeverRetryPolicy" here because the retry mechanism
                    // is implemented at the hedged command runner level and not at the
                    // 'sendCommand' level.
                    auto options = std::make_shared<AsyncRPCOptions<CommandType>>(
                        cmd,
                        exec,
                        hedgeCancellationToken.token(),
                        std::make_shared<NeverRetryPolicy>(),
                        genericArgs);
                    options->baton = baton;
                    requests.push_back(
                        sendCommand(options, opCtx, std::move(t)).thenRunOn(proxyExec));
                }

                if (opts.hedgeCount > 0) {
                    hm->incrementNumTotalHedgedOperations();
                }

                /**
                 * When whenAnyThat is used in sendHedgedCommand, the shouldAccept function
                 * always accepts the future with index 0, which we treat as the
                 * "authoritative" request. This is the codepath followed when we are not
                 * hedging or there is only 1 target provided.
                 */
                return hedging_rpc_details::whenAnyThat(
                           std::move(requests),
                           [&](StatusWith<SingleResponse> response, size_t index) {
                               Status commandStatus = response.getStatus();

                               if (index == 0) {
                                   return true;
                               }

                               if (commandStatus.code() == Status::OK()) {
                                   hedging_rpc_details::getHedgingMetrics(getGlobalServiceContext())
                                       ->incrementNumAdvantageouslyHedgedOperations();
                                   return true;
                               }

                               invariant(commandStatus.code() ==
                                             ErrorCodes::RemoteCommandExecutionError,
                                         commandStatus.toString());
                               boost::optional<Status> remoteErr;
                               auto extraInfo = commandStatus.extraInfo<AsyncRPCErrorInfo>();
                               if (extraInfo->isRemote()) {
                                   remoteErr = extraInfo->asRemote().getRemoteCommandResult();
                               }

                               if (remoteErr && isIgnorableAsHedgeResult(*remoteErr)) {
                                   return false;
                               }

                               hedging_rpc_details::getHedgingMetrics(getGlobalServiceContext())
                                   ->incrementNumAdvantageouslyHedgedOperations();
                               return true;
                           })
                    .tap([=](const SingleResponse& response) {
                        // We received a successful response, so we should try to clean state on
                        // other hosts. Note that there is no guarantee the clean-up happens after
                        // the target receives the original command.
                        std::vector<HostAndPort> targets;
                        std::copy_if(targetsAttempted->begin(),
                                     targetsAttempted->end(),
                                     std::back_inserter(targets),
                                     [&](HostAndPort& h) { return h != response.targetUsed; });
                        hedging_rpc_details::killOperations(svcCtx, targets, exec, operationKey);
                    })
                    .tapError([=](const Status&) {
                        // The hedged operation failed, so we should try to clean state on all
                        // targets. Note that this is just an attempt and does not guarantee no
                        // state is leaked, as the clean-up command may be received by a target
                        // before the original operaiton.
                        hedging_rpc_details::killOperations(
                            svcCtx, *targetsAttempted, exec, operationKey);
                    });
            });
    };
    return AsyncTry<decltype(tryBody)>(std::move(tryBody))
        .until([token, retryPolicy](StatusWith<SingleResponse> swResponse) {
            return token.isCanceled() ||
                !retryPolicy->recordAndEvaluateRetry(swResponse.getStatus());
        })
        .withBackoffBetweenIterations(detail::RetryDelayAsBackoff(retryPolicy.get()))
        .on(proxyExec, CancellationToken::uncancelable())
        // We go inline here to intercept executor-shutdown errors and re-write them
        // so that the API always returns RemoteCommandExecutionError. Additionally,
        // we need to make sure we cancel outstanding requests.
        .unsafeToInlineFuture()
        .onCompletion([hedgeCancellationToken, targetsAttempted](
                          StatusWith<SingleResponse> result) mutable -> StatusWith<SingleResponse> {
            hedgeCancellationToken.cancel();
            if (!result.isOK()) {
                auto status = result.getStatus();
                if (status.code() == ErrorCodes::RemoteCommandExecutionError) {
                    auto extraInfo = result.getStatus().template extraInfo<AsyncRPCErrorInfo>();
                    AsyncRPCErrorInfo extraInfoCopy = *extraInfo;
                    extraInfoCopy.setTargetsAttempted(*targetsAttempted);
                    result = Status{std::move(extraInfoCopy), status.reason()};
                    return result;
                }
                // The API implementation guarantees that all errors are provided as
                // RemoteCommandExecutionError, so if we've reached this code, it means that the API
                // internals were unable to run due to executor shutdown. Today, the only guarantee
                // we can make about an executor-shutdown error is that it is in the cancellation
                // category. We dassert that this is the case to make it easy to find errors in the
                // API implementation's error-handling while still ensuring that we always return
                // the correct error code in production.
                dassert(ErrorCodes::isA<ErrorCategory::CancellationError>(status.code()));
                return Status{AsyncRPCErrorInfo(status, *targetsAttempted),
                              "Remote command execution failed due to executor shutdown"};
            }
            return result;
        })
        .semi();
}

}  // namespace async_rpc
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT

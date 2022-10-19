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

#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/async_rpc_error_info.h"
#include "mongo/executor/async_rpc_retry_policy.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor

/**
 * This header provides an API of `sendCommand(...)` functions that can be used to asynchronously
 * invoke well-typed commands on a remote node. Each takes an IDL-defined or similarly specified
 * command type as argument, and invokes it on a remote node, internally handling targeting
 * the remote node, cancellation, and performing retries as needed according to rules provided by
 * arguments. Each function returns a future containing the response to the command, parsed into the
 * response-type provided. See the function comments below for details.
 */
namespace mongo::async_rpc {
using executor::TaskExecutor;

/**
 * The response type used by `sendCommand(...)`  functions, containing the typed response to the
 * command as well as the host it was run on
 */
template <typename CommandReplyType>
struct AsyncRPCResponse {
    CommandReplyType response;
    HostAndPort targetUsed;
};

/**
 * The response type used by `sendCommand(...)` functions if the return type of the command is
 * 'void'.
 */
template <>
struct AsyncRPCResponse<void> {
    HostAndPort targetUsed;
};

/**
 * Details used internally by the API. Users of the API can skip the code in this namespace
 * and proceed to the `sendCommand(...)` functions below.
 */
namespace detail {
struct AsyncRPCInternalResponse {
    BSONObj response;
    HostAndPort targetUsed;
};

/**
 * The AsyncRPCRunner class stores the implementation details used by the free function
 * async_rpc::sendCommand defined below. It takes a command and runs it on the provided
 * HostAndPort/database name asynchronously, using the given executor. It keeps the executor alive
 * for the duration of the command's execution; to cancel the command's execution, use the provided
 * cancellation token.
 *
 * This is *not* part of the public API, and is deliberately in the detail namespace. Use the
 * async_rpc::sendCommand free-function/public API below instead, which contains
 * additional functionality and type checking.
 */
class AsyncRPCRunner {
public:
    virtual ~AsyncRPCRunner() = default;
    virtual ExecutorFuture<AsyncRPCInternalResponse> _sendCommand(
        StringData dbName,
        BSONObj cmdBSON,
        Targeter* targeter,
        OperationContext* opCtx,
        std::shared_ptr<TaskExecutor> exec,
        CancellationToken token) = 0;
    static AsyncRPCRunner* get(ServiceContext* serviceContext);
    static void set(ServiceContext* serviceContext, std::unique_ptr<AsyncRPCRunner> theRunner);
};

/**
 * Returns a RemoteCommandExecutionError with ErrorExtraInfo populated to contain
 * details about any error, local or remote, contained in `r`.
 */
inline Status makeErrorIfNeeded(TaskExecutor::ResponseOnAnyStatus r) {
    if (r.status.isOK() && getStatusFromCommandResult(r.data).isOK() &&
        getWriteConcernStatusFromCommandResult(r.data).isOK() &&
        getFirstWriteErrorStatusFromCommandResult(r.data).isOK()) {
        return Status::OK();
    }
    return {AsyncRPCErrorInfo(r), "Remote command execution failed"};
}

/**
 * Adaptor that allows a RetryPolicy to be used with AsyncTry::withBackoffBetweenIterations.
 */
struct RetryDelayAsBackoff {
    RetryDelayAsBackoff(RetryPolicy* policy) : _policy{policy} {}
    Milliseconds nextSleep() {
        return _policy->getNextRetryDelay();
    }
    RetryPolicy* _policy;
};

template <typename CommandType>
ExecutorFuture<AsyncRPCResponse<typename CommandType::Reply>> sendCommandWithRunner(
    CommandType cmd,
    detail::AsyncRPCRunner* runner,
    OperationContext* opCtx,
    std::unique_ptr<Targeter> targeter,
    std::shared_ptr<executor::TaskExecutor> exec,
    CancellationToken token,
    std::shared_ptr<RetryPolicy> retryPolicy = std::make_shared<NeverRetryPolicy>()) {
    using ReplyType = AsyncRPCResponse<typename CommandType::Reply>;
    auto tryBody = [=, targeter = std::move(targeter)] {
        // Execute the command after extracting the db name and bson from the CommandType.
        // Wrapping this function allows us to separate the CommandType parsing logic from the
        // implementation details of executing the remote command asynchronously.
        return runner->_sendCommand(
            cmd.getDbName().db(), cmd.toBSON({}), targeter.get(), opCtx, exec, token);
    };
    auto resFuture = AsyncTry<decltype(tryBody)>(std::move(tryBody))
                         .until([token, retryPolicy, cmd](
                                    StatusWith<detail::AsyncRPCInternalResponse> swResponse) {
                             bool shouldStopRetry = token.isCanceled() ||
                                 !retryPolicy->recordAndEvaluateRetry(swResponse.getStatus());
                             return shouldStopRetry;
                         })
                         .withBackoffBetweenIterations(RetryDelayAsBackoff(retryPolicy.get()))
                         .on(exec, CancellationToken::uncancelable());

    return std::move(resFuture)
        .then([](detail::AsyncRPCInternalResponse r) -> ReplyType {
            // TODO SERVER-67661: Make IDL reply types have string representation for logging
            auto res = CommandType::Reply::parseSharingOwnership(IDLParserContext("AsyncRPCRunner"),
                                                                 r.response);

            return {res, r.targetUsed};
        })
        .unsafeToInlineFuture()
        .onError(
            // We go inline here to intercept executor-shutdown errors and re-write them
            // so that the API always returns RemoteCommandExecutionError.
            [](Status s) -> StatusWith<ReplyType> {
                if (s.code() == ErrorCodes::RemoteCommandExecutionError) {
                    return s;
                }
                // The API implementation guarantees that all errors are provided as
                // RemoteCommandExecutionError, so if we've reached this code, it means that the API
                // internals were unable to run due to executor shutdown. Today, the only guarantee
                // we can make about an executor-shutdown error is that it is in the cancellation
                // category. We dassert that this is the case to make it easy to find errors in the
                // API implementation's error-handling while still ensuring that we always return
                // the correct error code in production.
                dassert(ErrorCodes::isA<ErrorCategory::CancellationError>(s.code()));
                return Status{AsyncRPCErrorInfo(s),
                              "Remote command execution failed due to executor shutdown"};
            })
        .thenRunOn(exec);
}
}  // namespace detail

/**
 * Execute the command asynchronously on the given target with the provided executor.
 *
 * The command type specified must have a `toBSON(BSONObj)` member function that transforms the
 * command into BSON suitable for sending over-the-wire. It also must have a nested `Reply` type
 * with a static `Reply parseSharingOwnership(BSONObj)` member function that parses BSON recieved in
 * response to the command into the `Reply` type. Note all IDL-defined command types meet these
 * requirements. Returns an ExecutorFuture with the reply from the IDL command.
 *
 * If there is any error, local or remote, while executing the command, the future is set with
 * ErrorCodes::RemoteCommandExecutionError. This is the only error returned by the API. Additional
 * information about the error, such as its provenance, code, whether it was a command-error or
 * write{concern}error, etc, is available in the ExtraInfo object attached to the error. See
 * async_rpc_error_info.h for details.
 *
 * Cancelling the source associated with the provided token will cancel any outstanding RPC work.
 * The `targeter` and optional `retryPolicy` arguments allow you to specify how to target the
 * command and when to retry it; see the class comments for those arguments for details. The default
 * retry policy is to not do any retries.
 *
 * The `opCtx` argument is used by NetworkEgressMetadataHooks to append operation-specific metadata
 * (i.e. potential cluster-time ticking). (TODO: SERVER-70191) Additionally, if the `opCtx` has an
 * attached baton, the baton may be used to run portions of the commands targeting logic and/or
 * retry logic, as well as process the network response.
 */
template <typename CommandType>
ExecutorFuture<AsyncRPCResponse<typename CommandType::Reply>> sendCommand(
    CommandType cmd,
    OperationContext* opCtx,
    std::unique_ptr<Targeter> targeter,
    std::shared_ptr<executor::TaskExecutor> exec,
    CancellationToken token,
    std::shared_ptr<RetryPolicy> retryPolicy = std::make_shared<NeverRetryPolicy>()) {
    auto runner = detail::AsyncRPCRunner::get(opCtx->getServiceContext());
    return detail::sendCommandWithRunner(
        cmd, runner, opCtx, std::move(targeter), exec, token, retryPolicy);
}

/**
 * This function operates the same to `sendCommand` above, but without taking an operation context.
 * It therefore does not append operation/client specific metadata via NetworkEgressMetadataHooks,
 * and all work runs on the provided executor.
 */
template <typename CommandType>
ExecutorFuture<AsyncRPCResponse<typename CommandType::Reply>> sendCommand(
    CommandType cmd,
    ServiceContext* const svcCtx,
    std::unique_ptr<Targeter> targeter,
    std::shared_ptr<executor::TaskExecutor> exec,
    CancellationToken token,
    std::shared_ptr<RetryPolicy> retryPolicy = std::make_shared<NeverRetryPolicy>()) {
    // Execute the command after extracting the db name and bson from the CommandType.
    // Wrapping this function allows us to separate the CommandType parsing logic from the
    // implementation details of executing the remote command asynchronously.
    auto runner = detail::AsyncRPCRunner::get(svcCtx);
    return detail::sendCommandWithRunner(
        cmd, runner, nullptr, std::move(targeter), exec, token, retryPolicy);
}
}  // namespace mongo::async_rpc
#undef MONGO_LOGV2_DEFAULT_COMPONENT

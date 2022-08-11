/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <vector>

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo {
namespace resharding {

/**
 * Converts a vector of SharedSemiFutures into a vector of ExecutorFutures.
 */
std::vector<ExecutorFuture<void>> thenRunAllOn(const std::vector<SharedSemiFuture<void>>& futures,
                                               ExecutorPtr executor);

/**
 * Given a vector of input futures, returns a future that becomes ready when either
 *
 *  (a) all of the input futures have become ready with success, or
 *  (b) one of the input futures has become ready with an error.
 *
 * This function returns an immediately ready future when the vector of input futures is empty.
 */
ExecutorFuture<void> whenAllSucceedOn(const std::vector<SharedSemiFuture<void>>& futures,
                                      ExecutorPtr executor);

/**
 * Given a vector of input futures, returns a future that becomes ready when all of the input
 * futures have become ready with success or failure.
 *
 * If one of the input futures becomes ready with an error, then the cancellation source is canceled
 * in an attempt to speed up the other input futures becoming ready. After all of the input futures
 * have become ready, the returned future becomes ready with the first error that had occurred.
 *
 * This function returns an immediately ready future when the vector of input futures is empty.
 */
ExecutorFuture<void> cancelWhenAnyErrorThenQuiesce(
    const std::vector<SharedSemiFuture<void>>& futures,
    ExecutorPtr executor,
    CancellationSource cancelSource);

/**
 * A fluent-style API for executing asynchronous, future-returning try-until loops, specialized
 * around the retry logic for components within resharding's primary-only services.
 *
 * Example usage:
 *
 *      ExecutorFuture<void> future =
 *          resharding::WithAutomaticRetry(
 *              [this, chainCtx] { chainCtx->moreToCome = doOneBatch(); })
 *              .onTransientError([](const Status& status) {
 *                  LOGV2(123,
 *                        "Transient error while doing batch",
 *                        "error"_attr = redact(status));
 *              })
 *              .onUnrecoverableError([](const Status& status) {
 *                  LOGV2_ERROR(456,
 *                              "Operation-fatal error for resharding while doing batch",
 *                              "error"_attr = redact(status));
 *              })
 *              .until([chainCtx](const Status& status) {
 *                  return status.isOK() && !chainCtx->moreToCome;
 *              })
 *              .on(std::move(executor), std::move(cancelToken));
 */
template <typename BodyCallable>
class [[nodiscard]] WithAutomaticRetry {
public:
    explicit WithAutomaticRetry(BodyCallable && body) : _body{std::move(body)} {}

    decltype(auto) onTransientError(unique_function<void(const Status&)> onTransientError)&& {
        invariant(!_onTransientError, "Cannot call onTransientError() twice");
        _onTransientError = std::move(onTransientError);
        return std::move(*this);
    }

    decltype(auto) onUnrecoverableError(
        unique_function<void(const Status&)> onUnrecoverableError)&& {
        invariant(!_onUnrecoverableError, "Cannot call onUnrecoverableError() twice");
        _onUnrecoverableError = std::move(onUnrecoverableError);
        return std::move(*this);
    }

    template <typename StatusType>
    auto until(unique_function<bool(const StatusType&)> condition)&& {
        invariant(_onTransientError, "Must call onTransientError() first");
        invariant(_onUnrecoverableError, "Must call onUnrecoverableError() first");

        return AsyncTry<BodyCallable>(std::move(_body))
            .until([onTransientError = std::move(_onTransientError),
                    onUnrecoverableError = std::move(_onUnrecoverableError),
                    condition = std::move(condition)](const StatusType& statusOrStatusWith) {
                Status status = _getStatus(statusOrStatusWith);

                if (status.isA<ErrorCategory::RetriableError>() ||
                    status == ErrorCodes::FailedToSatisfyReadPreference ||
                    status.isA<ErrorCategory::CursorInvalidatedError>() ||
                    status == ErrorCodes::Interrupted ||
                    status.isA<ErrorCategory::CancellationError>() ||
                    status.isA<ErrorCategory::NotPrimaryError>()) {
                    // Always attempt to retry on any type of retryable error. Also retry on errors
                    // from stray killCursors and killOp commands being run. Cancellation and
                    // NotPrimary errors may indicate the primary-only service Instance will be shut
                    // down or is shutting down now. However, it is also possible that the error
                    // originated from a remote response rather than being an error generated by
                    // this shard itself. Defer whether or not to retry to the state of the
                    // cancellation token. This means the body of the AsyncTry may continue to run
                    // and error more times until the cancellation token is actually canceled if
                    // this shard was in the midst of stepping down.
                    onTransientError(status);
                } else if (!status.isOK()) {
                    // Any other kind of error is fatal for the resharding operation. Do not attempt
                    // to retry.
                    onUnrecoverableError(status);
                    return true;
                }

                return condition(statusOrStatusWith);
            });
    }

private:
    static const Status& _getStatus(const Status& status) {
        return status;
    }

    template <typename ValueType>
    static const Status& _getStatus(const StatusWith<ValueType>& statusWith) {
        return statusWith.getStatus();
    }

    BodyCallable _body;

    unique_function<void(const Status&)> _onTransientError;
    unique_function<void(const Status&)> _onUnrecoverableError;
};

/**
 * Wrapper class around CancelableOperationContextFactory which uses resharding::WithAutomaticRetry
 * to ensure all cancelable operations will be retried if able upon failure.
 */
class RetryingCancelableOperationContextFactory {
public:
    RetryingCancelableOperationContextFactory(CancellationToken cancelToken, ExecutorPtr executor)
        : _factory{std::move(cancelToken), std::move(executor)} {}

    template <typename BodyCallable>
    decltype(auto) withAutomaticRetry(BodyCallable&& body) const {
        return resharding::WithAutomaticRetry([this, body]() { return body(_factory); });
    }

private:
    const CancelableOperationContextFactory _factory;
};

}  // namespace resharding
}  // namespace mongo

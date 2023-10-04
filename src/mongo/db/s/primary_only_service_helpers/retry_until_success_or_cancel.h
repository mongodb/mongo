/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/primary_only_service_helpers/retrying_cancelable_operation_context_factory.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace primary_only_service_helpers {

/**
 * Wrapper class around RetryingCancelableOperationContextFactory/WithAutomaticRetry to avoid
 * needing to reconfigure WithAutomaticRetry for every invocation.
 */
class RetryUntilSuccessOrCancel {
public:
    RetryUntilSuccessOrCancel(StringData serviceName,
                              std::shared_ptr<executor::ScopedTaskExecutor> taskExecutor,
                              std::shared_ptr<ThreadPool> markKilledExecutor,
                              CancellationToken token,
                              BSONObj metadata = {},
                              RetryabilityPredicate isRetryable = kDefaultRetryabilityPredicate);

    /**
     * The operation function will run on the provided task executor until one of the following
     * occurs:
     * 1. The operation returns success.
     * 2. The operation fails with an unrecoverable error according to the RetryabilityPredicate.
     * 3. The provided cancel token is cancelled.
     * Returns a future containing the return value of the operation function.
     * Logs a message containing the service name and metadata on any transient or unrecoverable
     * error.
     */
    template <typename Function>
    auto untilSuccessOrCancel(const std::string& operationName, Function&& operation) {
        static_assert(
            std::is_invocable_v<Function, const CancelableOperationContextFactory&>,
            "Operation function must accept const reference to CancelableOperationContextFactory "
            "as parameter.");
        using FuturizedResultType =
            FutureContinuationResult<Function, const CancelableOperationContextFactory&>;
        using StatusifiedResultType =
            decltype(std::declval<Future<FuturizedResultType>>().getNoThrow());
        return _retryFactory.withAutomaticRetry(std::forward<Function>(operation))
            .onTransientError([this, operationName](const Status& status) {
                logError("transient", operationName, status);
            })
            .onUnrecoverableError([this, operationName](const Status& status) {
                logError("unrecoverable", operationName, status);
            })
            .template until<StatusifiedResultType>(
                [](const auto& statusLike) { return statusLike.isOK(); })
            .withBackoffBetweenIterations(kBackoff)
            .on(**_taskExecutor, _token);
    }

private:
    void logError(StringData errorKind,
                  const std::string& operationName,
                  const Status& status) const;

    const static Backoff kBackoff;

    const std::string _serviceName;
    std::shared_ptr<executor::ScopedTaskExecutor> _taskExecutor;
    CancellationToken _token;
    RetryingCancelableOperationContextFactory _retryFactory;
    const BSONObj _metadata;
};

}  // namespace primary_only_service_helpers
}  // namespace mongo

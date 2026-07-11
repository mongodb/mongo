// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/primary_only_service_helpers/retrying_cancelable_operation_context_factory.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace primary_only_service_helpers {

/**
 * Wrapper class around RetryingCancelableOperationContextFactory/WithAutomaticRetry to avoid
 * needing to reconfigure WithAutomaticRetry for every invocation.
 */
class RetryUntilSuccessOrCancel {
public:
    RetryUntilSuccessOrCancel(std::string_view serviceName,
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
            std::is_invocable_v<Function,
                                std::shared_ptr<HierarchicalCancelableOperationContextFactory>>,
            "Operation function must accept "
            "shared_ptr<HierarchicalCancelableOperationContextFactory> "
            "as parameter.");
        using FuturizedResultType = FutureContinuationResult<
            Function,
            std::shared_ptr<HierarchicalCancelableOperationContextFactory>>;
        using StatusifiedResultType =
            decltype(std::declval<Future<FuturizedResultType>>().getNoThrow());
        return _retryFactory.withAutomaticRetry(std::forward<Function>(operation))
            .onTransientError([this, operationName](const Status& status) {
                logError("transient", operationName, status);
            })
            .onUnrecoverableError([this, operationName](const Status& status) {
                logError("unrecoverable", operationName, status);
            })
            .runOn(**_taskExecutor, _token);
    }

private:
    void logError(std::string_view errorKind,
                  const std::string& operationName,
                  const Status& status) const;

    const std::string _serviceName;
    std::shared_ptr<executor::ScopedTaskExecutor> _taskExecutor;
    CancellationToken _token;
    RetryingCancelableOperationContextFactory _retryFactory;
    const BSONObj _metadata;
};

}  // namespace primary_only_service_helpers
}  // namespace mongo

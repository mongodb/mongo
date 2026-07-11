// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/primary_only_service_helpers/cancel_state.h"
#include "mongo/db/s/primary_only_service_helpers/retry_until_success_or_cancel.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace primary_only_service_helpers {

/**
 * Helper class intended for writing retry loops for PrimaryOnlyServices that follow the following
 * pattern when performing operations:
 * 1. Perform local writes.
 * 2. Update in-memory state.
 * 3. Wait for majority commit.
 */
class RetryUntilMajorityCommit {
public:
    RetryUntilMajorityCommit(std::string_view serviceName,
                             std::shared_ptr<executor::ScopedTaskExecutor> executor,
                             const CancelState* cancelState,
                             BSONObj metadata = {});

    /**
     * Runs the given function on the task executor and retries until it either succeeds or the
     * stepdown token is cancelled (i.e. the operation will be continued on a new primary). Use this
     * for operations which are effectively not allowed to fail according to the logic of the
     * PrimaryOnlyService. Returns an executor future.
     */
    template <typename Function>
    auto untilStepdownOrSuccess(const std::string& operationName, Function&& function) {
        return _retryUntilStepdown.untilSuccessOrCancel(operationName,
                                                        std::forward<Function>(function));
    }

    /**
     * Runs the given function on the task executor and retries until it either succeeds, fails with
     * an unrecoverable error (categorized by kDefaultRetryabilityPredicate in
     * with_automatic_retry.h), or the CancelState::_abortOrStepdownToken is cancelled (i.e. we are
     * stepping down or were explicitly aborted). Use this for operations that are allowed to fail
     * and use a future continuation to handle this failure. Returns an executor future.
     */
    template <typename Function>
    auto untilAbortOrSuccess(const std::string& operationName, Function&& function) {
        return _retryUntilAbort.untilSuccessOrCancel(operationName,
                                                     std::forward<Function>(function));
    }

    /**
     * Same as untilStepdownOrSuccess, except will also wait for the most recent opTime to be
     * majority committed after the operation succeeds.
     */
    template <typename Function>
    auto untilStepdownOrMajorityCommit(const std::string& operationName, Function&& function) {
        return untilMajorityCommit(
            operationName, std::forward<Function>(function), _retryUntilStepdown);
    }

    /**
     * Same as untilAbortOrSuccess, except will also wait for the most recent opTime to be
     * majority committed after the operation succeeds.
     */
    template <typename Function>
    auto untilAbortOrMajorityCommit(const std::string& operationName, Function&& function) {
        return untilMajorityCommit(
            operationName, std::forward<Function>(function), _retryUntilAbort);
    }

    enum Event { kAbort, kStepdown };

    template <typename Function>
    auto untilSuccessOr(Event event, const std::string& operationName, Function&& function) {
        switch (event) {
            case Event::kAbort:
                return untilAbortOrSuccess(operationName, std::forward<Function>(function));
            case Event::kStepdown:
                return untilStepdownOrSuccess(operationName, std::forward<Function>(function));
        }
        MONGO_UNREACHABLE;
    }

    template <typename Function>
    auto untilMajorityCommitOr(Event event, const std::string& operationName, Function&& function) {
        switch (event) {
            case Event::kAbort:
                return untilAbortOrMajorityCommit(operationName, std::forward<Function>(function));
            case Event::kStepdown:
                return untilStepdownOrMajorityCommit(operationName,
                                                     std::forward<Function>(function));
        }
        MONGO_UNREACHABLE;
    }

private:
    template <typename Function>
    auto untilMajorityCommit(const std::string& operationName,
                             Function&& function,
                             RetryUntilSuccessOrCancel& retry) {
        return retry.untilSuccessOrCancel(operationName, std::forward<Function>(function))
            .then([this, operationName]() { return _waitForMajorityOrStepdown(operationName); });
    }

    ExecutorFuture<void> _waitForMajorityOrStepdown(const std::string& operationName);

    RetryUntilSuccessOrCancel _createRetryHelper(CancellationToken token,
                                                 RetryabilityPredicate predicate);

    const std::string _serviceName;
    const CancelState* _cancelState;
    const BSONObj _metadata;

    std::shared_ptr<executor::ScopedTaskExecutor> _taskExecutor;
    std::shared_ptr<ThreadPool> _markKilledExecutor;

    RetryUntilSuccessOrCancel _retryUntilStepdown;
    RetryUntilSuccessOrCancel _retryUntilAbort;
};

}  // namespace primary_only_service_helpers
}  // namespace mongo

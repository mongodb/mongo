// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/s/primary_only_service_helpers/retrying_cancelable_operation_context_factory.h"
#include "mongo/db/s/primary_only_service_helpers/with_automatic_retry.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"

#include <string>
#include <utility>
#include <vector>

namespace mongo {
namespace resharding {

using primary_only_service_helpers::kRetryabilityPredicateIncludeWriteConcernTimeout;
using primary_only_service_helpers::RetryabilityPredicate;
using primary_only_service_helpers::RetryingCancelableOperationContextFactory;

const auto kRetryabilityPredicateIncludeLockTimeoutAndWriteConcern = [](const Status& status) {
    return kRetryabilityPredicateIncludeWriteConcernTimeout(status) ||
        status == ErrorCodes::LockTimeout;
};

// Treats errors caused by an active replica set write block as retryable. Used on its own to extend
// a base retryability so that a held resharding operation keeps retrying instead of failing while a
// replica set write block is active.
const auto kRetryabilityPredicateReplicaSetWritesBlocked = [](const Status& status) {
    return status == ErrorCodes::ReplicaSetWritesBlocked ||
        (status == ErrorCodes::IndexBuildAborted &&
         status.reason().find("Write blocking") != std::string::npos);
};

const auto kRetryabilityPredicateIncludeReplicaSetWritesBlockedAndLockTimeoutAndWriteConcern =
    [](const Status& status) {
        return kRetryabilityPredicateIncludeLockTimeoutAndWriteConcern(status) ||
            kRetryabilityPredicateReplicaSetWritesBlocked(status);
    };

// Extends the default write-concern-timeout retryability with only an active replica set write
// block, without also making LockTimeout retryable. Used by the resharding cloners so they hold and
// resume through a write block while otherwise preserving their default retryability; LockTimeout
// is left to be retried by the recipient state machine that drives them.
const auto kRetryabilityPredicateIncludeReplicaSetWritesBlockedAndWriteConcern =
    [](const Status& status) {
        return kRetryabilityPredicateIncludeWriteConcernTimeout(status) ||
            kRetryabilityPredicateReplicaSetWritesBlocked(status);
    };

/**
 * Rate-limits the "writes to this replica set are blocked" warning that the resharding cloner,
 * oplog applier, and recipient state machine each emit while held on a replica set write block.
 */
bool shouldLogWriteBlockWarning(Atomic<long long>& lastWarningAt);

template <typename BodyCallable>
primary_only_service_helpers::WithAutomaticRetry<BodyCallable> WithAutomaticRetry(
    BodyCallable&& body) {
    return primary_only_service_helpers::WithAutomaticRetry<BodyCallable>(
        std::move(body), kRetryabilityPredicateIncludeWriteConcernTimeout);
}

template <typename BodyCallable>
primary_only_service_helpers::WithAutomaticRetry<BodyCallable> WithAutomaticRetry(
    BodyCallable&& body, RetryabilityPredicate isRetryable) {
    return primary_only_service_helpers::WithAutomaticRetry<BodyCallable>(std::move(body),
                                                                          std::move(isRetryable));
}

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
 * Runs the callable until it succeeds or the stepdown token is canceled. Retries on any error with
 * exponential backoff between attempts, invoking 'onRetry' (if provided) with the failure status
 * before each retry.
 */
template <typename SleepableExecutor>
ExecutorFuture<void> runUntilSuccessOrStepdown(unique_function<void()> callable,
                                               SleepableExecutor executor,
                                               const CancellationToken& stepdownToken,
                                               unique_function<void(const Status&)> onRetry = {}) {
    static const Backoff kUntilSuccessOrStepdownBackoff(Seconds(1), Milliseconds::max());

    return AsyncTry([callable = std::move(callable)] { callable(); })
        .until([stepdownToken, onRetry = std::move(onRetry)](Status status) {
            const bool done = status.isOK() || stepdownToken.isCanceled();
            if (!done && onRetry) {
                onRetry(status);
            }
            return done;
        })
        .withBackoffBetweenIterations(kUntilSuccessOrStepdownBackoff)
        .on(std::move(executor), CancellationToken::uncancelable());
}

}  // namespace resharding
}  // namespace mongo

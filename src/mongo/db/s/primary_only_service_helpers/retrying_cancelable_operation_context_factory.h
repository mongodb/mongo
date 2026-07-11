// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/s/primary_only_service_helpers/with_automatic_retry.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace primary_only_service_helpers {

/**
 * Wrapper class around HierarchicalCancelableOperationContextFactory which creates a new
 * child factory for each withAutomaticRetry invocation. This ensures that listeners attached
 * during the retry scope are cleaned up when the scope completes, addressing memory accumulation
 * issues with long-lived cancellation tokens (SERVER-103945).
 */
class RetryingCancelableOperationContextFactory {
public:
    RetryingCancelableOperationContextFactory(
        CancellationToken cancelToken,
        ExecutorPtr executor,
        RetryabilityPredicate isRetryable = kDefaultRetryabilityPredicate)
        : _isRetryable{std::move(isRetryable)},
          _factory{std::make_unique<HierarchicalCancelableOperationContextFactory>(
              std::move(cancelToken), std::move(executor))} {}

    template <typename BodyCallable>
    decltype(auto) withAutomaticRetry(BodyCallable&& body) const {
        // Create a NEW child for each retry scope. Using shared_ptr allows the child
        // to be captured by value in multiple lambdas within the future chain.
        auto child = _factory->createSharedChild();
        return WithAutomaticRetry(
            [child, body = std::forward<BodyCallable>(body)]() { return body(child); },
            _isRetryable);
    }

    /**
     * Overload that extends the factory's default retryability predicate for a single retry scope,
     * rather than replacing it. Errors matching either the base predicate or additionalPredicate
     * are treated as retryable.
     */
    template <typename BodyCallable>
    decltype(auto) withAutomaticRetryExtending(BodyCallable&& body,
                                               RetryabilityPredicate additionalPredicate) const {
        auto child = _factory->createSharedChild();
        return WithAutomaticRetry(
            [child, body = std::forward<BodyCallable>(body)]() { return body(child); },
            [base = _isRetryable, extra = std::move(additionalPredicate)](const Status& s) {
                return base(s) || extra(s);
            });
    }

private:
    RetryabilityPredicate _isRetryable;
    std::unique_ptr<HierarchicalCancelableOperationContextFactory> _factory;
};

}  // namespace primary_only_service_helpers
}  // namespace mongo


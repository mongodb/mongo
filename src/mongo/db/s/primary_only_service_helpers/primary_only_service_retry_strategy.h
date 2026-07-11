// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/retry_strategy.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace primary_only_service_helpers {

using RetryabilityPredicate = std::function<bool(const Status&)>;

class PrimaryOnlyServiceRetryStrategy final : public RetryStrategy {
public:
    using RetryCriteria = DefaultRetryStrategy::RetryCriteria;

    PrimaryOnlyServiceRetryStrategy(RetryabilityPredicate retryabilityPredicate,
                                    unique_function<void(const Status&)> onTransientError,
                                    unique_function<void(const Status&)> onUnrecoverableError);
    [[nodiscard]]
    bool recordFailureAndEvaluateShouldRetry(Status s,
                                             const boost::optional<HostAndPort>& origin,
                                             std::span<const std::string> errorLabels,
                                             boost::optional<Milliseconds> baseBackoffMS) override;
    void recordSuccess(const boost::optional<HostAndPort>& origin) override;
    Milliseconds getNextRetryDelay() const override;
    const TargetingMetadata& getTargetingMetadata() const override;
    void recordBackoff(Milliseconds backoff) override;

private:
    BackoffWithJitter _backoffWithJitter;
    std::unique_ptr<RetryStrategy> _underlyingStrategy;
    unique_function<void(const Status&)> _onTransientError;
    unique_function<void(const Status&)> _onUnrecoverableError;
};

}  // namespace primary_only_service_helpers
}  // namespace mongo

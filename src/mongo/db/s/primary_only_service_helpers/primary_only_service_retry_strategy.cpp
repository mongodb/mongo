// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/primary_only_service_helpers/primary_only_service_retry_strategy.h"

namespace mongo {
namespace primary_only_service_helpers {

namespace {
DefaultRetryStrategy::RetryParameters getRetryParameters() {
    auto base = DefaultRetryStrategy::getRetryParametersFromServerParameters();
    base.maxRetryAttempts = std::numeric_limits<std::int32_t>::max();
    return base;
}

PrimaryOnlyServiceRetryStrategy::RetryCriteria makeCriteriaAdapter(
    RetryabilityPredicate retryabilityPredicate) {
    return [retryabilityPredicate = std::move(retryabilityPredicate)](
               Status s, std::span<const std::string> errorLabels) {
        return retryabilityPredicate(s);
    };
}
}  // namespace


PrimaryOnlyServiceRetryStrategy::PrimaryOnlyServiceRetryStrategy(
    RetryabilityPredicate retryabilityPredicate,
    unique_function<void(const Status&)> onTransientError,
    unique_function<void(const Status&)> onUnrecoverableError)
    : _backoffWithJitter(getRetryParameters().baseBackoff, getRetryParameters().maxBackoff),
      _underlyingStrategy(std::make_unique<DefaultRetryStrategy>(
          makeCriteriaAdapter(std::move(retryabilityPredicate)), getRetryParameters())),
      _onTransientError(std::move(onTransientError)),
      _onUnrecoverableError(std::move(onUnrecoverableError)) {}

bool PrimaryOnlyServiceRetryStrategy::recordFailureAndEvaluateShouldRetry(
    Status s,
    const boost::optional<HostAndPort>& origin,
    std::span<const std::string> errorLabels,
    boost::optional<Milliseconds> baseBackoffMS) {
    auto willRetry = _underlyingStrategy->recordFailureAndEvaluateShouldRetry(
        s, origin, errorLabels, baseBackoffMS);
    if (willRetry) {
        _backoffWithJitter.incrementAttemptCount();
        _onTransientError(s);
    } else {
        _onUnrecoverableError(s);
    }
    return willRetry;
}

void PrimaryOnlyServiceRetryStrategy::recordSuccess(const boost::optional<HostAndPort>& origin) {
    return _underlyingStrategy->recordSuccess(origin);
}

Milliseconds PrimaryOnlyServiceRetryStrategy::getNextRetryDelay() const {
    return std::max(_underlyingStrategy->getNextRetryDelay(), _backoffWithJitter.getBackoffDelay());
}

const TargetingMetadata& PrimaryOnlyServiceRetryStrategy::getTargetingMetadata() const {
    return _underlyingStrategy->getTargetingMetadata();
}

void PrimaryOnlyServiceRetryStrategy::recordBackoff(Milliseconds backoff) {
    return _underlyingStrategy->recordBackoff(backoff);
}

}  // namespace primary_only_service_helpers
}  // namespace mongo

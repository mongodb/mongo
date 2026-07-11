// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/retry_strategy.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/retry_strategy_server_parameters_gen.h"
#include "mongo/db/error_labels.h"

#include <algorithm>
#include <string_view>

namespace mongo {
namespace {

bool isRetryableError(std::span<const std::string> errorLabels) {
    constexpr auto isRetryableErrorLabel = [](std::string_view label) {
        return label == ErrorLabel::kRetryableWrite || label == ErrorLabel::kRetryableError;
    };

    return std::ranges::find_if(errorLabels, isRetryableErrorLabel) != errorLabels.end();
}

}  // namespace

bool containsSystemOverloadedErrorLabel(std::span<const std::string> errorLabels) {
    return std::ranges::find(errorLabels, ErrorLabel::kSystemOverloadedError) != errorLabels.end();
}

bool DefaultRetryStrategy::defaultRetryCriteria(Status s,
                                                std::span<const std::string> errorLabels) {
    return s.isA<ErrorCategory::RetriableError>() || isRetryableError(errorLabels);
}

bool DefaultRetryStrategy::unconditionallyRetryableCriteria(
    Status s, std::span<const std::string> errorLabels) {
    return std::ranges::find(errorLabels, ErrorLabel::kRetryableError) != errorLabels.end();
}

bool DefaultRetryStrategy::recordFailureAndEvaluateShouldRetry(
    Status s,
    const boost::optional<HostAndPort>& target,
    std::span<const std::string> errorLabels,
    boost::optional<Milliseconds> baseBackoffMS) {

    if (_retryAttemptCount >= _maxRetryAttempts || !_retryCriteria(s, errorLabels)) {
        return false;
    }

    if (containsSystemOverloadedErrorLabel(errorLabels)) {
        if (target &&
            std::ranges::find(_targetingMetadata.deprioritizedServers, *target) ==
                _targetingMetadata.deprioritizedServers.end()) {
            _targetingMetadata.deprioritizedServers.emplace_back(*target);
        }
        _backoffWithJitter.incrementAttemptCount();
        _lastBaseBackoffMS = baseBackoffMS;
    }

    ++_retryAttemptCount;
    return true;
}

auto DefaultRetryStrategy::getRetryParametersFromServerParameters() -> RetryParameters {
    return {
        gDefaultClientMaxRetryAttempts.loadRelaxed(),
        Milliseconds{gDefaultClientBaseBackoffMillis.loadRelaxed()},
        Milliseconds{gDefaultClientMaxBackoffMillis.loadRelaxed()},
    };
}

bool AdaptiveRetryStrategy::recordFailureAndEvaluateShouldRetry(
    Status s,
    const boost::optional<HostAndPort>& target,
    std::span<const std::string> errorLabels,
    boost::optional<Milliseconds> baseBackoffMS) {
    const bool targetOverloaded = containsSystemOverloadedErrorLabel(errorLabels);

    const auto evaluateShouldRetry = [&] {
        if (targetOverloaded) {
            _previousAttemptOverloaded = true;
            return _budget->tryAcquireToken();
        }

        _budget->recordNotOverloaded(_previousAttemptOverloaded);
        _previousAttemptOverloaded = false;
        return true;
    };

    return evaluateShouldRetry() &&
        _underlyingStrategy->recordFailureAndEvaluateShouldRetry(
            s, target, errorLabels, baseBackoffMS);
}

void AdaptiveRetryStrategy::recordSuccess(const boost::optional<HostAndPort>& target) {
    _budget->recordNotOverloaded(_previousAttemptOverloaded);
    _underlyingStrategy->recordSuccess(target);
}

bool AdaptiveRetryStrategy::RetryBudget::tryAcquireToken() {
    auto currentBalance = _balance.load();
    do {
        if (currentBalance < 1) {
            return false;
        }
    } while (MONGO_unlikely(!_balance.compareAndSwap(&currentBalance, currentBalance - 1)));

    return true;
}

void AdaptiveRetryStrategy::RetryBudget::recordNotOverloaded(bool returnExtraToken) {
    auto lk = _rwMutex.readLock();

    const auto amountReturned = _returnRate + (returnExtraToken ? 1. : 0.);
    auto currentBalance = _balance.load();
    do {
        if (currentBalance >= _capacity) {
            return;
        }
    } while (MONGO_unlikely(!_balance.compareAndSwap(
        &currentBalance, std::min(_capacity, currentBalance + amountReturned))));
}

void AdaptiveRetryStrategy::RetryBudget::updateRateParameters(double returnRate, double capacity) {
    auto lk = _rwMutex.writeLock();
    _returnRate = returnRate;

    if (capacity < _capacity) {
        auto currentBalance = _balance.load();
        do {
            if (currentBalance <= capacity) {
                break;
            }
        } while (MONGO_unlikely(!_balance.compareAndSwap(&currentBalance, capacity)));
    }

    _capacity = capacity;
}

double AdaptiveRetryStrategy::RetryBudget::getBalance_forTest() const {
    return _balance.load();
}

void AdaptiveRetryStrategy::RetryBudget::appendStats(BSONObjBuilder* bob) const {
    bob->append("retryBudgetTokenBucketBalance", _balance.loadRelaxed());
}

}  // namespace mongo

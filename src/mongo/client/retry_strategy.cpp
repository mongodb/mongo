/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/client/retry_strategy.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/retry_strategy_server_parameters_gen.h"
#include "mongo/db/error_labels.h"

#include <algorithm>

namespace mongo {

bool DefaultRetryStrategy::defaultRetryCriteria(Status s,
                                                std::span<const std::string> errorLabels) {
    constexpr auto isRetryableErrorLabel = [](StringData label) {
        return label == ErrorLabel::kRetryableWrite || label == ErrorLabel::kRetryableError;
    };

    return s.isA<ErrorCategory::RetriableError>() ||
        std::ranges::find_if(errorLabels, isRetryableErrorLabel) != errorLabels.end();
}

bool DefaultRetryStrategy::recordFailureAndEvaluateShouldRetry(
    Status s,
    const boost::optional<HostAndPort>& target,
    std::span<const std::string> errorLabels) {

    if (_retryAttemptCount >= _maxRetryAttempts || !_retryCriteria(s, errorLabels)) {
        return false;
    }

    if (std::ranges::find(errorLabels, ErrorLabel::kSystemOverloadedError) != errorLabels.end()) {
        if (target) {
            _targetingMetadata.deprioritizedServers.emplace(*target);
        }
        _backoffWithJitter.incrementAttemptCount();
    }

    ++_retryAttemptCount;
    return true;
}

auto DefaultRetryStrategy::backoffFromServerParameters() -> BackoffParameters {
    return {
        gDefaultClientMaxRetryAttempts.loadRelaxed(),
        Milliseconds{gDefaultClientBaseBackoffMillis.loadRelaxed()},
        Milliseconds{gDefaultClientMaxBackoffMillis.loadRelaxed()},
    };
}

bool AdaptiveRetryStrategy::recordFailureAndEvaluateShouldRetry(
    Status s,
    const boost::optional<HostAndPort>& target,
    std::span<const std::string> errorLabels) {
    const bool targetOverloaded =
        std::ranges::find(errorLabels, ErrorLabel::kSystemOverloadedError) != errorLabels.end();

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
        _underlyingStrategy->recordFailureAndEvaluateShouldRetry(s, target, errorLabels);
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

}  // namespace mongo

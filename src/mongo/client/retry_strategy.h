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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/backoff_with_jitter.h"
#include "mongo/db/error_labels.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>

namespace mongo {

struct TargetingMetadata {
    stdx::unordered_set<HostAndPort> deprioritizedServers;
};

/**
 * Interface for implementing retry behavior. Allows user to specify exactly how much time we
 * should wait between retries and if it should retry.
 *
 * At the end of each request, either 'recordFailureAndEvaluateShouldRetry' or 'recordSuccess'
 * must be called. When the retry strategy returns that a retry should be done, it is expected
 * that users perform a wait for the amount of time returned by 'getNextRetryDelay'.
 */
class RetryStrategy {
public:
    virtual ~RetryStrategy() = default;

    /**
     * Returns true if the request that generated the status and error labels should be retried.
     *
     * This function should be called at the end of each failed request.
     */
    [[nodiscard]]
    virtual bool recordFailureAndEvaluateShouldRetry(Status s,
                                                     const boost::optional<HostAndPort>& origin,
                                                     std::span<const std::string> errorLabels) = 0;

    /**
     * Records a successful request. Should be called at the end of successful request even if no
     * retries have been performed.
     */
    virtual void recordSuccess(const boost::optional<HostAndPort>& origin) = 0;

    /**
     * Returns a delay amount corresponding to the amount of retries with a jitter applied to it.
     */
    virtual Milliseconds getNextRetryDelay() const = 0;

    virtual const TargetingMetadata& getTargetingMetadata() const = 0;
};

/**
 * Implements the basic behavior for retryability of failed requests.
 *
 * Uses exponential backoff with jitter to compute the time to wait when target is determined to
 * be overloaded. This is to avoid sending more requests to a target that won't be able to process
 * the request.
 *
 * Uses a callback as a retry condition.
 */
class DefaultRetryStrategy final : public RetryStrategy {
public:
    using RetryCriteria = std::function<bool(Status s, std::span<const std::string> errorLabels)>;

    /**
     * The default retry criteria will return true if the error is in the 'RetriableError'
     * error category or if the error has one of the labels:
     *  - 'RetryableWriteError'
     *  - 'RetryableError'
     */
    // TODO: SERVER-108613 Implement this function as a normal static function and relocate
    // implementation in the cpp file.
    static constexpr auto defaultRetryCriteria = [](Status s,
                                                    std::span<const std::string> errorLabels) {
        constexpr auto isRetryableErrorLabel = [](StringData label) {
            return label == ErrorLabel::kRetryableWrite || label == ErrorLabel::kRetryableError;
        };

        return s.isA<ErrorCategory::RetriableError>() ||
            std::ranges::find_if(errorLabels, isRetryableErrorLabel) != errorLabels.end();
    };

    struct BackoffParameters {
        // Maximum number of retries after initial retriable error.
        std::int32_t maxRetryAttempts;
        // The base of the exponent used when calculating backoff times.
        Milliseconds baseBackoff;
        // The maximum time a single backoff can take.
        Milliseconds maxBackoff;
    };

    DefaultRetryStrategy()
        : DefaultRetryStrategy{defaultRetryCriteria, backoffFromServerParameters()} {}

    DefaultRetryStrategy(RetryCriteria retryCriteria, BackoffParameters backoffParameters)
        : _retryCriteria{std::move(retryCriteria)},
          _backoffWithJitter{backoffParameters.baseBackoff, backoffParameters.maxBackoff},
          _maxRetryAttempts{backoffParameters.maxRetryAttempts} {}

    [[nodiscard]]
    bool recordFailureAndEvaluateShouldRetry(Status s,
                                             const boost::optional<HostAndPort>& target,
                                             std::span<const std::string> errorLabels) override;

    void recordSuccess(const boost::optional<HostAndPort>& target) override {
        // Noop, as there's nothing to cleanup on success.
    }

    static BackoffParameters backoffFromServerParameters();

    Milliseconds getNextRetryDelay() const override {
        return _backoffWithJitter.getBackoffDelay();
    }

    const TargetingMetadata& getTargetingMetadata() const override {
        return _targetingMetadata;
    }

private:
    RetryCriteria _retryCriteria;
    BackoffWithJitter _backoffWithJitter;
    std::int32_t _maxRetryAttempts;
    std::int32_t _retryAttemptCount = 0;
    TargetingMetadata _targetingMetadata;
};

/**
 * A retry strategy based on a shared token bucket to prevent overloading a system. It is intended
 * to share the state for all requests targeted at a particular system. This strategy wraps another
 * strategy to add adaptive behavior on top of it. By default, wraps a 'DefaultRetryStrategy'.
 *
 * For every failed request that has the error label 'SystemOverloadedError', a token will be
 * tentatively acquired. If the request fails for other reasons, the budget is replenished by the
 * return rate as this is a sign of the system recovering.
 *
 * If a token is consumed by a particular instance of this strategy and then the request succeeds,
 * the consumed token is also returned on top of replenishing the budget by the return rate.
 */
class AdaptiveRetryStrategy final : public RetryStrategy {
public:
    class RetryBudget {
    public:
        /**
         * Constructs the retry budget for a given return rate and capacity.
         *
         * The return rate is the amount of tokens returned per successful request to
         * replenish the budget.
         *
         * The capacity is the maximum amount of tokens available.
         */
        RetryBudget(double returnRate, double capacity)
            : _capacity{capacity}, _returnRate{returnRate}, _balance{capacity} {}

        /**
         * Thread-safe. Called when server parameters are updated.
         */
        void updateRateParameters(double returnRate, double capacity);

        double getBalance_forTest() const;

    private:
        friend AdaptiveRetryStrategy;

        /**
         * Thread-safe. Attempts to consume tokens from the token bucket.
         *  - If the balance is depleted, returns false.
         *  - If the token acquisition is successful, returns true.
         *
         * Used when evaluating retries to limit prevent overloading on a system.
         */
        bool tryAcquireToken();

        /**
         * Thread-safe. Returns a 'returnRate' amount of tokens back to the bucket.
         * Used when a request is successful in order to allow more retries to occur.
         *
         * If 'returnExtraToken' is true, it will also return a full token in addition to
         * the return rate.
         */
        void recordNotOverloaded(bool returnExtraToken);

        // Mutex that protects all non-atomic variables of this class.
        WriteRarelyRWMutex _rwMutex;
        double _capacity;
        double _returnRate;

        Atomic<double> _balance;
    };

    using RetryCriteria = DefaultRetryStrategy::RetryCriteria;
    using BackoffParameters = DefaultRetryStrategy::BackoffParameters;
    static constexpr auto defaultRetryCriteria = DefaultRetryStrategy::defaultRetryCriteria;

    AdaptiveRetryStrategy(std::shared_ptr<RetryBudget> budget,
                          std::unique_ptr<RetryStrategy> underlyingStrategy)
        : _underlyingStrategy{std::move(underlyingStrategy)}, _budget{std::move(budget)} {
        invariant(_budget);
        invariant(_underlyingStrategy);
    }

    explicit AdaptiveRetryStrategy(
        std::shared_ptr<RetryBudget> budget,
        RetryCriteria retryCriteria = defaultRetryCriteria,
        BackoffParameters backoffParameters = DefaultRetryStrategy::backoffFromServerParameters())
        : _underlyingStrategy{std::make_unique<DefaultRetryStrategy>(std::move(retryCriteria),
                                                                     backoffParameters)},
          _budget{std::move(budget)} {
        invariant(_budget);
    }

    /**
     * Determines whether the operation should be retried based on the retry budget.
     *
     * If the error label 'SystemOverloadError' is present in 'errorLabels', does the following:
     * - If a token cannot be acquired, the function immediately returns false.
     * - If a token is successfully acquired, delegates to the underlying strategy.
     *
     * If 'SystemOverloadError' is not present in 'errorLabels', replenishes the retry budget and
     * delegates to the underlying strategy.
     */
    [[nodiscard]]
    bool recordFailureAndEvaluateShouldRetry(Status s,
                                             const boost::optional<HostAndPort>& target,
                                             std::span<const std::string> errorLabels) override;

    /**
     * Replenishes the retry budget to allow more retries.
     */
    void recordSuccess(const boost::optional<HostAndPort>& target) override;

    Milliseconds getNextRetryDelay() const override {
        return _underlyingStrategy->getNextRetryDelay();
    }

    const TargetingMetadata& getTargetingMetadata() const override {
        return _underlyingStrategy->getTargetingMetadata();
    }

private:
    std::unique_ptr<RetryStrategy> _underlyingStrategy;
    std::shared_ptr<RetryBudget> _budget;
    bool _previousAttemptOverloaded = false;
};

}  // namespace mongo

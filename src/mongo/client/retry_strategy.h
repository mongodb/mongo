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

#include "mongo/base/status.h"
#include "mongo/client/backoff_with_jitter.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

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
 *
 * Usage example:
 *
 *     Status status = ...;
 *
 *     while(!status.isOK() &&
 *           strategy.recordFailureAndEvaluateShouldRetry(status, target, labels)) {
 *         wait_for(strategy.getNextRetryDelay());
 *         status = ...;
 *     }
 *
 *     if (status.isOK()) {
 *         strategy.recordSuccess();
 *     }
 *
 *  See 'runWithRetryStrategy' for a reference usage of retry strategies.
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

    /**
     * A type that encapsulates a value or a result with error labels, and also contains information
     * about targeting.
     *
     * This utility type is necessary because there is no existing type that carries error labels
     * alongside a status.
     *
     * TODO: SERVER-104141: Replace this type with StatusWith.
     */
    template <typename T>
    class [[nodiscard]] Result {
    public:
        using value_type = T;

        /**
         * Constructor for the OK case.
         *
         * Not marked constexpr because HostAndPort is not constexpr compatible.
         */
        Result(T data, HostAndPort origin)
            : _status{Status::OK()},
              _valueOrError{std::in_place_type<T>, std::move(data)},
              _origin{std::move(origin)} {}

        /**
         * Constructor for the OK case.
         */
        explicit(false) constexpr Result(T data)
            : _status{Status::OK()}, _valueOrError{std::in_place_type<T>, std::move(data)} {}

        /**
         * Converting constructor for the OK case.
         */
        template <typename U>
        requires(!std::same_as<std::remove_cvref_t<T>, std::remove_cvref_t<U>> &&
                 std::constructible_from<T, U>)
        explicit(!std::convertible_to<U, T>) constexpr Result(U&& data)
            : _status{Status::OK()}, _valueOrError{std::in_place_type<T>, std::forward<U>(data)} {}

        /**
         * Converting constructor for the OK case.
         *
         * Not marked constexpr because HostAndPort is not constexpr compatible.
         */
        template <typename U>
        requires(!std::same_as<std::remove_cvref_t<T>, std::remove_cvref_t<U>> &&
                 std::constructible_from<T, U>)
        Result(U&& data, HostAndPort origin)
            : _status{Status::OK()},
              _valueOrError{std::in_place_type<T>, std::forward<U>(data)},
              _origin{std::move(origin)} {}

        /**
         * Converting constructor from another result type.
         */
        template <typename U>
        requires(!std::same_as<T, U> && std::constructible_from<T, U>)
        explicit(!std::convertible_to<U, T>) constexpr Result(const Result<U>& result)
            : _status{result._status},
              _valueOrError{convert_variant(result._valueOrError)},
              _origin{result._origin} {}

        /**
         * Converting move constructor from another result type.
         */
        template <typename U>
        requires(!std::same_as<T, U> && std::constructible_from<T, U>)
        explicit(!std::convertible_to<U, T>) constexpr Result(Result<U>&& result)
            : _status{std::exchange(result._status, Status::OK())},
              _valueOrError{convert_variant(std::exchange(result._valueOrError, {}))},
              _origin{std::exchange(result._origin, {})} {}

        /**
         * Constructor for the error case.
         */
        constexpr Result(Status s, std::vector<std::string> errorLabels)
            : _status{std::move(s)},
              _valueOrError{std::in_place_type<ErrorLabels>, std::move(errorLabels)} {
            dassert(!_status.isOK());
        }

        /**
         * Constructor for the error case.
         *
         * This constructor is not constexpr only because HostAndPort cannot be used in constexpr
         * context.
         */
        Result(Status s, std::vector<std::string> errorLabels, HostAndPort origin)
            : _status{std::move(s)},
              _valueOrError{std::in_place_type<ErrorLabels>, std::move(errorLabels)},
              _origin{std::move(origin)} {
            dassert(!_status.isOK());
        }

        constexpr T& getValue() & {
            dassert(isOK());
            return std::get<T>(_valueOrError);
        }

        constexpr T&& getValue() && {
            dassert(isOK());
            return std::get<T>(std::move(_valueOrError));
        }

        constexpr const T& getValue() const& {
            dassert(isOK());
            return std::get<T>(_valueOrError);
        }

        constexpr const T&& getValue() const&& {
            dassert(isOK());
            return std::get<T>(std::move(_valueOrError));
        }

        constexpr bool isOK() const {
            return std::holds_alternative<T>(_valueOrError);
        }

        constexpr const Status& getStatus() const {
            return _status;
        }

        constexpr std::span<const std::string> getErrorLabels() const {
            dassert(!isOK());
            return std::get<ErrorLabels>(_valueOrError);
        }

        constexpr const boost::optional<HostAndPort>& getOrigin() const {
            return _origin;
        }

        constexpr bool operator==(const T& value) const {
            return isOK() && std::get<T>(_valueOrError) == value;
        }

        constexpr bool operator==(const Status& status) const {
            return _status == status;
        }

        constexpr bool operator==(ErrorCodes::Error code) const {
            return _status == code;
        }

        // This includes conversions to StatusWith<T> where T is this class's T.
        template <typename U>
        requires(std::constructible_from<T, U>)
        explicit(!std::convertible_to<U, T>) operator StatusWith<U>() const& {
            return std::visit(
                [&]<typename V>(const V& value) {
                    if constexpr (std::same_as<T, V>) {
                        return StatusWith<U>{U(value)};
                    } else if constexpr (std::same_as<ErrorLabels, V>) {
                        return StatusWith<U>{_status};
                    } else {
                        static_assert(!std::same_as<V, V>, "condition not exhaustive");
                    }
                },
                _valueOrError);
        }

        template <typename U>
        requires(std::constructible_from<T, U>)
        explicit(!std::convertible_to<U, T>) operator StatusWith<U>() && {
            return std::visit(
                [&]<typename V>(V& value) {
                    if constexpr (std::same_as<std::remove_cvref_t<T>, std::remove_cvref_t<V>>) {
                        return StatusWith<U>{U(std::move(value))};
                    } else if constexpr (std::same_as<ErrorLabels, V>) {
                        return StatusWith<U>{std::move(_status)};
                    } else {
                        static_assert(!std::same_as<V, V>, "condition not exhaustive");
                    }
                },
                _valueOrError);
        }

        friend std::string stringify_forTest(const Result& result) {
            if (result.isOK()) {
                return unittest::stringify::invoke(result.getValue());
            } else {
                return unittest::stringify::invoke(result.getStatus());
            }
        }

    private:
        // We friend all templates of this class to allow direct access for
        // constructors from other types of 'Result<T>'.
        template <typename>
        friend class Result;

        using ErrorLabels = std::vector<std::string>;
        using ValueOrErrorLabels = std::variant<T, ErrorLabels>;

        template <typename U>
        requires(std::constructible_from<T, U>)
        auto convert_variant(const std::variant<U, ErrorLabels>& other_variant)
            -> ValueOrErrorLabels {
            if (auto* value = std::get_if<0>(&other_variant)) {
                return ValueOrErrorLabels{std::in_place_index<0>, *value};
            } else if (auto* errorLabels = std::get_if<1>(&other_variant)) {
                return ValueOrErrorLabels{std::in_place_index<1>, *errorLabels};
            }

            MONGO_UNREACHABLE;
        }

        template <typename U>
        requires(std::constructible_from<T, U>)
        auto convert_variant(std::variant<U, ErrorLabels>&& other_variant) -> ValueOrErrorLabels {
            if (auto* value = std::get_if<0>(&other_variant)) {
                return ValueOrErrorLabels{std::in_place_index<0>, std::move(*value)};
            } else if (auto* errorLabels = std::get_if<1>(&other_variant)) {
                return ValueOrErrorLabels{std::in_place_index<1>, std::move(*errorLabels)};
            }

            MONGO_UNREACHABLE;
        }


        Status _status;
        ValueOrErrorLabels _valueOrError;
        boost::optional<HostAndPort> _origin;
    };
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
    static bool defaultRetryCriteria(Status s, std::span<const std::string> errorLabels);

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

    static bool defaultRetryCriteria(Status s, std::span<const std::string> errorLabels) {
        return DefaultRetryStrategy::defaultRetryCriteria(s, errorLabels);
    }

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

/**
 * A reference implementation for running a function with a retry strategy. It calls
 * 'runOperation' in a loop until one of the following conditions is met:
 *  - The operation succeeds.
 *  - The operation is interrupted.
 *  - The retry strategy evaluates that the operation should not be retried.
 *
 * All template parameters are intended to be deduced.
 *
 * The 'runOperation' function must accept a 'const TargetingMetadata&' parameter and
 * return a 'RetryStrategy::Result<T>'. The parameter is equivalent to a
 * 'std::function<RetryStrategy::Result<T>(const TargetingMetadata&)>'.
 *
 * All exceptions thrown by 'runOperation' will be turned into Status and the retry strategy may
 * choose to retry on that Status.
 *
 * Usage example:
 *
 *     StatusWith<StringData> result = runWithRetryStrategy(
 *         opCtx,
 *         strategy,
 *         [](const TargetingMetadata& targetingMetadata) -> RetryStrategy::Result<StringData> {
 *             // on success.
 *             return "value"_sd;
 *             // on failure. Target is the host and port on which the request was performed.
 *             return {status, target, errorLabels};
 *         }
 *     );
 */
template <std::invocable<const TargetingMetadata&> F,
          typename Result = std::invoke_result_t<F, const TargetingMetadata&>,
          typename T = typename Result::value_type>
requires(std::same_as<Result, RetryStrategy::Result<T>>)
StatusWith<T> runWithRetryStrategy(Interruptible* interruptible,
                                   RetryStrategy& strategy,
                                   F runOperation) {
    invariant(interruptible);

    auto run = [&] {
        try {
            return runOperation(strategy.getTargetingMetadata());
        } catch (const DBException& e) {
            return RetryStrategy::Result<T>{e.toStatus(), {}};
        }
    };

    auto result = run();

    while (!result.isOK() &&
           strategy.recordFailureAndEvaluateShouldRetry(
               result.getStatus(), result.getOrigin(), result.getErrorLabels())) {
        const auto delay = strategy.getNextRetryDelay();

        try {
            if (delay == Milliseconds{0}) {
                interruptible->checkForInterrupt();
            } else {
                interruptible->sleepFor(delay);
            }
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        result = run();
    }

    if (result.isOK()) {
        strategy.recordSuccess(result.getOrigin());
    }

    return result;
}

}  // namespace mongo

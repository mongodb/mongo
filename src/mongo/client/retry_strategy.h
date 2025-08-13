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
        std::int32_t maxRetryAttempts;
        // Base backoff is used as a factor to the exponential growth.
        Milliseconds baseBackoff;
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

}  // namespace mongo

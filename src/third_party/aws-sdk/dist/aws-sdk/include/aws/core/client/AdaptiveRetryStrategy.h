/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/client/RetryStrategy.h>
#include <aws/core/utils/DateTime.h>

namespace Aws
{
namespace Client
{

/**
 * A helper class of the AdaptiveRetryStrategy
 * representing a (send) token bucket with a dynamically changing fill rate and capacity.
 */
class AWS_CORE_API RetryTokenBucket
{
public:
    /**
     * C-tor
     */
    RetryTokenBucket() = default;

    /**
     * Acquire tokens from the bucket. If the bucket contains enough capacity
     * to satisfy the request, this method will return immediately, otherwise
     * the method will block the calling thread until enough tokens are refilled
     * unless fast fail is provided as an argument.
     */
    bool Acquire(size_t amount = 1, bool fastFail = false);

    /**
     * Update limiter's client sending rate during the request bookkeeping process
     * based on a service response.
     */
    void UpdateClientSendingRate(bool throttlingResponse, const Aws::Utils::DateTime& now = Aws::Utils::DateTime::Now());

protected:
    /**
     * Internal C-tor for unit testing
     */
    RetryTokenBucket(double fillRate, double maxCapacity, double currentCapacity,
                     const Aws::Utils::DateTime& lastTimestamp, double measuredTxRate, double lastTxRateBucket,
                     size_t requestCount, bool enabled, double lastMaxRate, const Aws::Utils::DateTime& lastThrottleTime);

    /**
     * Internal method to update the token bucket's fill rate when we receive a response from the service.
     * The update amount will depend on whether or not a throttling response is received from a service.
     * The request rate is measured using an exponentially smoothed average,
     * with the rate being updated in half second buckets.
     */
    void UpdateMeasuredRate(const Aws::Utils::DateTime& now = Aws::Utils::DateTime::Now());

    /**
     * Internal method to enable rate limiting.
     */
    void Enable();

    /**
     * Internal method to refill and update refill rate with a new refill rate.
     */
    void UpdateRate(double newRps, const Aws::Utils::DateTime& now = Aws::Utils::DateTime::Now());

    /**
     * Internal method to refill send tokens based on a current fill rate.
     */
    void Refill(const Aws::Utils::DateTime& now = Aws::Utils::DateTime::Now());

    /**
     * Internal method to compute time window for a last max fill rate.
     */
    double CalculateTimeWindow() const;

    /**
     * Internal method with a modified CUBIC algorithm to compute new max sending rate for a successful response.
     */
    double CUBICSuccess(const Aws::Utils::DateTime& timestamp, const double timeWindow) const;

    /**
     * Internal method with a modified CUBIC algorithm to compute new max sending rate for a throttled response.
     */
    double CUBICThrottle(const double rateToUse) const;

    // The rate at which token are replenished.
    double m_fillRate = 0.0;
    // The maximum capacity allowed in the token bucket.
    double m_maxCapacity = 0.0;
    // The current capacity of the token bucket.
    double m_currentCapacity = 0.0;
    // The last time the token bucket was refilled.
    Aws::Utils::DateTime m_lastTimestamp;
    // The smoothed rate which tokens are being retrieved.
    double m_measuredTxRate = 0.0;
    // The last half second time bucket used.
    double m_lastTxRateBucket = 0.0;
    // The number of requests seen within the current time bucket.
    size_t m_requestCount = 0;
    // Boolean indicating if the token bucket is enabled.
    bool m_enabled = false;
    // The maximum rate when the client was last throttled.
    double m_lastMaxRate = 0.0;
    // The last time when the client was throttled.
    Aws::Utils::DateTime m_lastThrottleTime;

    // TokenBucket's mutex to synchronize read/write operations
    std::recursive_mutex m_mutex;
};

/**
 * A retry strategy that builds on the standard strategy and introduces congestion control through
 * client side rate limiting.
 */
class AWS_CORE_API AdaptiveRetryStrategy : public StandardRetryStrategy
{
public:
    /**
     * C-tors
     */
    AdaptiveRetryStrategy(long maxAttempts = 3);
    AdaptiveRetryStrategy(std::shared_ptr<RetryQuotaContainer> retryQuotaContainer, long maxAttempts = 3);

    /**
     * Retrieve and consume a send token.
     * Returns true if send token is available.
     *
     * If there is not sufficient capacity, HasSendToken() will either sleep a certain amount of time until the rate
     * limiter can retrieve a token from its token bucket or return false indicating there is insufficient capacity.
     */
    virtual bool HasSendToken() override;

    /**
     * Update status, like the information of retry quota when receiving a response.
     */
    virtual void RequestBookkeeping(const HttpResponseOutcome& httpResponseOutcome) override;
    virtual void RequestBookkeeping(const HttpResponseOutcome& httpResponseOutcome, const AWSError<CoreErrors>& lastError) override;

    const char* GetStrategyName() const override { return "adaptive";}

protected:
    RetryTokenBucket m_retryTokenBucket;
    bool m_fastFail = false;

private:
    /**
     * An internal helper function to check if a given service response is classified as a throttled one.
     */
    static bool IsThrottlingResponse(const HttpResponseOutcome& httpResponseOutcome);
};

} // namespace Client
} // namespace Aws

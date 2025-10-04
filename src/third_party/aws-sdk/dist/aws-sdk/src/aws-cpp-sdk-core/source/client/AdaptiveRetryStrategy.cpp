/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AdaptiveRetryStrategy.h>

#include <aws/core/client/AWSError.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/utils/memory/stl/AWSSet.h>

#include <cmath>
#include <thread>

using namespace Aws::Utils::Threading;

namespace Aws
{
    namespace Client
    {
        static const double MIN_FILL_RATE = 0.5;
        static const double MIN_CAPACITY = 1;

        static const double SMOOTH = 0.8;
        static const double BETA = 0.7;
        static const double SCALE_CONSTANT = 0.4;

        // A static list containing all service exception names classified as throttled.
        static const char* THROTTLING_EXCEPTIONS[] {
                "Throttling", "ThrottlingException", "ThrottledException", "RequestThrottledException",
                "TooManyRequestsException", "ProvisionedThroughputExceededException", "TransactionInProgressException",
                "RequestLimitExceeded", "BandwidthLimitExceeded", "LimitExceededException", "RequestThrottled",
                "SlowDown", "PriorRequestNotComplete", "EC2ThrottledException"};
        static const size_t THROTTLING_EXCEPTIONS_SZ = sizeof(THROTTLING_EXCEPTIONS) / sizeof(THROTTLING_EXCEPTIONS[0]);


        // C-tor for unit testing
        RetryTokenBucket::RetryTokenBucket(double fillRate, double maxCapacity, double currentCapacity,
                         const Aws::Utils::DateTime& lastTimestamp, double measuredTxRate, double lastTxRateBucket,
                         size_t requestCount, bool enabled, double lastMaxRate, const Aws::Utils::DateTime& lastThrottleTime)
                         :
                         m_fillRate(fillRate), m_maxCapacity(maxCapacity), m_currentCapacity(currentCapacity),
                         m_lastTimestamp(lastTimestamp), m_measuredTxRate(measuredTxRate),
                         m_lastTxRateBucket(lastTxRateBucket), m_requestCount(requestCount), m_enabled(enabled),
                         m_lastMaxRate(lastMaxRate), m_lastThrottleTime(lastThrottleTime)
        {}

        bool RetryTokenBucket::Acquire(size_t amount, bool fastFail)
        {
            std::lock_guard<std::recursive_mutex> locker(m_mutex);
            if (!m_enabled)
            {
                return true;
            }
            Refill();
            bool notEnough = amount > m_currentCapacity;
            if (notEnough && fastFail) {
                return false;
            }
            // If all the tokens couldn't be acquired immediately, wait enough
            // time to fill the remainder.
            if (notEnough) {
                std::chrono::duration<double> waitTime((amount - m_currentCapacity) / m_fillRate);
                std::this_thread::sleep_for(waitTime);
                Refill();
            }
            m_currentCapacity -= amount;
            return true;
        }

        void RetryTokenBucket::Refill(const Aws::Utils::DateTime& now)
        {
            std::lock_guard<std::recursive_mutex> locker(m_mutex);

            if (0 == m_lastTimestamp.Millis()) {
                m_lastTimestamp = now;
                return;
            }

            double fillAmount = (std::abs(now.Millis() - m_lastTimestamp.Millis()))/1000.0 * m_fillRate;
            m_currentCapacity = (std::min)(m_maxCapacity, m_currentCapacity + fillAmount);
            m_lastTimestamp = now;
        }

        void RetryTokenBucket::UpdateRate(double newRps, const Aws::Utils::DateTime& now)
        {
            std::lock_guard<std::recursive_mutex> locker(m_mutex);

            Refill(now);
            m_fillRate = (std::max)(newRps, MIN_FILL_RATE);
            m_maxCapacity = (std::max)(newRps, MIN_CAPACITY);
            m_currentCapacity = (std::min)(m_currentCapacity, m_maxCapacity);
        }

        void RetryTokenBucket::UpdateMeasuredRate(const Aws::Utils::DateTime& now)
        {
            std::lock_guard<std::recursive_mutex> locker(m_mutex);

            double t = now.Millis() / 1000.0;
            double timeBucket = floor(t * 2.0) / 2.0;
            m_requestCount += 1;
            if (timeBucket > m_lastTxRateBucket) {
                double currentRate = m_requestCount / (timeBucket - m_lastTxRateBucket);
                m_measuredTxRate = (currentRate * SMOOTH) + (m_measuredTxRate * (1 - SMOOTH));
                m_requestCount = 0;
                m_lastTxRateBucket = timeBucket;
            }
        }

        void RetryTokenBucket::UpdateClientSendingRate(bool isThrottlingResponse, const Aws::Utils::DateTime& now)
        {
            std::lock_guard<std::recursive_mutex> locker(m_mutex);

            UpdateMeasuredRate(now);

            double calculatedRate = 0.0;
            if (isThrottlingResponse)
            {
                double rateToUse = m_measuredTxRate;
                if (m_enabled)
                    rateToUse = (std::min)(rateToUse, m_fillRate);

                m_lastMaxRate = rateToUse;
                m_lastThrottleTime = now;

                calculatedRate = CUBICThrottle(rateToUse);
                Enable();
            }
            else
            {
                double timeWindow = CalculateTimeWindow();
                calculatedRate = CUBICSuccess(now, timeWindow);
            }

            double newRate = (std::min)(calculatedRate, 2.0 * m_measuredTxRate);
            UpdateRate(newRate, now);
        }

        void RetryTokenBucket::Enable()
        {
            std::lock_guard<std::recursive_mutex> locker(m_mutex);
            m_enabled = true;
        }

        double RetryTokenBucket::CalculateTimeWindow() const
        {
            return pow(((m_lastMaxRate * (1.0 - BETA)) / SCALE_CONSTANT), (1.0 / 3));
        }

        double RetryTokenBucket::CUBICSuccess(const Aws::Utils::DateTime& timestamp, const double timeWindow) const
        {
            double dt = (timestamp.Millis() - m_lastThrottleTime.Millis()) / 1000.0;
            double calculatedRate = SCALE_CONSTANT * pow(dt - timeWindow, 3.0) + m_lastMaxRate;
            return calculatedRate;
        }

        double RetryTokenBucket::CUBICThrottle(const double rateToUse) const
        {
            double calculatedRate = rateToUse * BETA;
            return calculatedRate;
        }


        AdaptiveRetryStrategy::AdaptiveRetryStrategy(long maxAttempts) :
                StandardRetryStrategy(maxAttempts)
        {}

        AdaptiveRetryStrategy::AdaptiveRetryStrategy(std::shared_ptr<RetryQuotaContainer> retryQuotaContainer, long maxAttempts) :
                StandardRetryStrategy(retryQuotaContainer, maxAttempts)
        {}

        bool AdaptiveRetryStrategy::HasSendToken()
        {
            return m_retryTokenBucket.Acquire(1, m_fastFail);
        }

        void AdaptiveRetryStrategy::RequestBookkeeping(const HttpResponseOutcome& httpResponseOutcome)
        {
            if (httpResponseOutcome.IsSuccess())
            {
                m_retryQuotaContainer->ReleaseRetryQuota(Aws::Client::NO_RETRY_INCREMENT);
                m_retryTokenBucket.UpdateClientSendingRate(false);
            }
            else
            {
                m_retryTokenBucket.UpdateClientSendingRate(IsThrottlingResponse(httpResponseOutcome));
            }
        }

        void AdaptiveRetryStrategy::RequestBookkeeping(const HttpResponseOutcome& httpResponseOutcome, const AWSError<CoreErrors>& lastError)
        {
            if (httpResponseOutcome.IsSuccess())
            {
                m_retryQuotaContainer->ReleaseRetryQuota(lastError);
                m_retryTokenBucket.UpdateClientSendingRate(false);
            }
            else
            {
                m_retryTokenBucket.UpdateClientSendingRate(IsThrottlingResponse(httpResponseOutcome));
            }
        }

        bool AdaptiveRetryStrategy::IsThrottlingResponse(const HttpResponseOutcome& httpResponseOutcome)
        {
            if(httpResponseOutcome.IsSuccess())
                return false;

            const AWSError<CoreErrors>& error = httpResponseOutcome.GetError();
            if (error.ShouldThrottle()) {
                return true;
            }
            const Aws::Client::CoreErrors enumValue = error.GetErrorType();
            switch(enumValue)
            {
                case Aws::Client::CoreErrors::THROTTLING:
                case Aws::Client::CoreErrors::SLOW_DOWN:
                    return true;
                default:
                    break;
            }

            if(std::find(THROTTLING_EXCEPTIONS,
                         THROTTLING_EXCEPTIONS + THROTTLING_EXCEPTIONS_SZ, error.GetExceptionName()) != THROTTLING_EXCEPTIONS + THROTTLING_EXCEPTIONS_SZ)
            {
                return true;
            }

            return false;
        }
    }
}

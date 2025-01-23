/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/RetryStrategy.h>

#include <aws/core/client/AWSError.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/utils/Outcome.h>

using namespace Aws::Utils::Threading;

namespace Aws
{
    namespace Client
    {
        static const int INITIAL_RETRY_TOKENS = 500;
        static const int RETRY_COST = 5;
        static const int TIMEOUT_RETRY_COST = 10;

        StandardRetryStrategy::StandardRetryStrategy(long maxAttempts) :
            m_retryQuotaContainer(Aws::MakeShared<DefaultRetryQuotaContainer>("StandardRetryStrategy")),
            m_maxAttempts(maxAttempts)
        {
          srand((unsigned int)time(NULL));
        }

        StandardRetryStrategy::StandardRetryStrategy(std::shared_ptr<RetryQuotaContainer> retryQuotaContainer, long maxAttempts) :
            m_retryQuotaContainer(retryQuotaContainer),
            m_maxAttempts(maxAttempts)
        {
          srand((unsigned int)time(NULL));
        }

        void StandardRetryStrategy::RequestBookkeeping(const HttpResponseOutcome& httpResponseOutcome)
        {
            if (httpResponseOutcome.IsSuccess())
            {
                m_retryQuotaContainer->ReleaseRetryQuota(NO_RETRY_INCREMENT);
            }
        }

        void StandardRetryStrategy::RequestBookkeeping(const HttpResponseOutcome& httpResponseOutcome, const AWSError<CoreErrors>& lastError)
        {
            if (httpResponseOutcome.IsSuccess())
            {
                m_retryQuotaContainer->ReleaseRetryQuota(lastError);
            }
        }

        bool StandardRetryStrategy::ShouldRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const
        {
            if (!error.ShouldRetry())
                return false;

            if (attemptedRetries + 1 >= m_maxAttempts)
                return false;

            return m_retryQuotaContainer->AcquireRetryQuota(error);
        }

        long StandardRetryStrategy::CalculateDelayBeforeNextRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const
        {
            AWS_UNREFERENCED_PARAM(error);
            // Maximum left shift factor is capped by ceil(log2(max_delay)), to avoid wrap-around and overflow into negative values:
            return (std::min)(rand() % 1000 * (1 << (std::min)(attemptedRetries, 15L)), 20000);
        }

        DefaultRetryQuotaContainer::DefaultRetryQuotaContainer() : m_retryQuota(INITIAL_RETRY_TOKENS)
        {}

        bool DefaultRetryQuotaContainer::AcquireRetryQuota(int capacityAmount)
        {
            WriterLockGuard guard(m_retryQuotaLock);

            if (capacityAmount > m_retryQuota)
            {
                return false;
            }
            else
            {
                m_retryQuota -= capacityAmount;
                return true;
            }
        }

        bool DefaultRetryQuotaContainer::AcquireRetryQuota(const AWSError<CoreErrors>& error)
        {
            int capacityAmount = error.GetErrorType() == CoreErrors::REQUEST_TIMEOUT ? TIMEOUT_RETRY_COST : RETRY_COST;
            return AcquireRetryQuota(capacityAmount);
        }

        void DefaultRetryQuotaContainer::ReleaseRetryQuota(int capacityAmount)
        {
            WriterLockGuard guard(m_retryQuotaLock);
            m_retryQuota = (std::min)(m_retryQuota + capacityAmount, INITIAL_RETRY_TOKENS);
        }

        void DefaultRetryQuotaContainer::ReleaseRetryQuota(const AWSError<CoreErrors>& error)
        {
            int capacityAmount = error.GetErrorType() == CoreErrors::REQUEST_TIMEOUT ? TIMEOUT_RETRY_COST : RETRY_COST;
            ReleaseRetryQuota(capacityAmount);
        }
    }
}

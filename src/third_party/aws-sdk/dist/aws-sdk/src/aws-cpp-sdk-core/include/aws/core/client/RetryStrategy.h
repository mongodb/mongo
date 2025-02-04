/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>
#include <memory>

namespace Aws
{
    namespace Http
    {
        class HttpResponse;
    }

    namespace Utils
    {
        template<typename R, typename E>
        class Outcome;
    }

    namespace Client
    {
        static const int NO_RETRY_INCREMENT = 1;

        enum class CoreErrors;
        template<typename ERROR_TYPE>
        class AWSError;

        typedef Utils::Outcome<std::shared_ptr<Aws::Http::HttpResponse>, AWSError<CoreErrors>> HttpResponseOutcome;

        /**
         * Interface for defining a Retry Strategy. Override this class to provide your own custom retry behavior.
         */
        class AWS_CORE_API RetryStrategy
        {
        public:
            virtual ~RetryStrategy() = default;
            /**
             * Returns true if the error can be retried given the error and the number of times already tried.
             */
            virtual bool ShouldRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const = 0;

            /**
             * Calculates the time in milliseconds the client should sleep before attempting another request based on the error and attemptedRetries count.
             */
            virtual long CalculateDelayBeforeNextRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const = 0;

            /**
             * Gets max number of attempts allowed for an operation.
             * Returns non positive value if not defined.
             */
            virtual long GetMaxAttempts() const { return 0; }

            /**
             * Retrieves send tokens from the bucket. Throws an exception if not available.
             */
            virtual void GetSendToken() {}

            /**
             * Retrieves send tokens from the bucket. Returns true is send token is retrieved.
             */
            virtual bool HasSendToken()
            {
                GetSendToken();  // first call old method for backward compatibility
                return true;
            }

            /**
             * Update status, like the information of retry quota when receiving a response.
             */
            virtual void RequestBookkeeping(const HttpResponseOutcome& /* httpResponseOutcome */) {}
            virtual void RequestBookkeeping(const HttpResponseOutcome& /* httpResponseOutcome */, const AWSError<CoreErrors>& /* lastError */) {}

            /**
             * Get return strategy name.
             */
            virtual const char* GetStrategyName() const
            {
                // SDK provided strategies will provide actual name
                return "custom";
            }
        };

        /**
         * The container for retry quotas.
         * A failed request will acquire retry quotas to retry.
         * And a successful request will release quotas back.
         * If running out of retry quotas, then the client is not able to retry.
         */
        class AWS_CORE_API RetryQuotaContainer
        {
        public:
            virtual ~RetryQuotaContainer() = default;
            virtual bool AcquireRetryQuota(int capacityAmount) = 0;
            virtual bool AcquireRetryQuota(const AWSError<CoreErrors>& error) = 0;
            virtual void ReleaseRetryQuota(int capacityAmount) = 0;
            virtual void ReleaseRetryQuota(const AWSError<CoreErrors>& lastError) = 0;
            virtual int GetRetryQuota() const = 0;
        };

        class AWS_CORE_API DefaultRetryQuotaContainer : public RetryQuotaContainer
        {
        public:
            DefaultRetryQuotaContainer();
            virtual ~DefaultRetryQuotaContainer() = default;
            virtual bool AcquireRetryQuota(int capacityAmount) override;
            virtual bool AcquireRetryQuota(const AWSError<CoreErrors>& error) override;
            virtual void ReleaseRetryQuota(int capacityAmount) override;
            virtual void ReleaseRetryQuota(const AWSError<CoreErrors>& lastError) override;
            virtual int GetRetryQuota() const override { return m_retryQuota; }

        protected:
            mutable Aws::Utils::Threading::ReaderWriterLock m_retryQuotaLock;
            int m_retryQuota;
        };

        class AWS_CORE_API StandardRetryStrategy : public RetryStrategy
        {
        public:
            StandardRetryStrategy(long maxAttempts = 3);
            StandardRetryStrategy(std::shared_ptr<RetryQuotaContainer> retryQuotaContainer, long maxAttempts = 3);

            virtual void RequestBookkeeping(const HttpResponseOutcome& httpResponseOutcome) override;
            virtual void RequestBookkeeping(const HttpResponseOutcome& httpResponseOutcome, const AWSError<CoreErrors>& lastError) override;

            virtual bool ShouldRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const override;

            virtual long CalculateDelayBeforeNextRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const override;

            virtual long GetMaxAttempts() const override { return m_maxAttempts; }

            const char* GetStrategyName() const override { return "standard";}

        protected:
            std::shared_ptr<RetryQuotaContainer> m_retryQuotaContainer;
            long m_maxAttempts;
        };
    } // namespace Client
} // namespace Aws

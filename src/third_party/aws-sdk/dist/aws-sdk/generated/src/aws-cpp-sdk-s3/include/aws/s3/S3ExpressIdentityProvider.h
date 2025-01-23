/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/memory/stl/AWSSet.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/ConcurrentCache.h>
#include <aws/core/auth/signer/AWSAuthSignerBase.h>
#include <aws/s3/S3ExpressIdentity.h>
#include <smithy/identity/resolver/AwsIdentityResolverBase.h>
#include <thread>
#include <condition_variable>

namespace Aws {
    namespace Http {
        struct ServiceSpecificParameters;
    }

    namespace S3 {
        class S3Client;
        class S3ExpressIdentityProvider: public smithy::IdentityResolverBase<S3ExpressIdentity> {
        public:
            explicit S3ExpressIdentityProvider(const S3Client &s3Client) : m_s3Client(s3Client) {}

            virtual S3ExpressIdentity
            GetS3ExpressIdentity(const std::shared_ptr<Aws::Http::ServiceSpecificParameters> &serviceSpecificParameters) = 0;

            ResolveIdentityFutureOutcome
            getIdentity(const IdentityProperties& identityProperties, const AdditionalParameters& additionalParameters) override;
            S3ExpressIdentity
            getIdentity(const Aws::String &bucketName);

            virtual ~S3ExpressIdentityProvider() {}

        protected:
            std::shared_ptr<std::mutex> GetMutexForBucketName(const Aws::String& bucketName);

        private:
            const S3Client &m_s3Client;
            mutable std::mutex m_bucketNameMapMutex;
            Aws::Map<Aws::String, std::shared_ptr<std::mutex>> m_bucketNameMutex;
        };

        class DefaultS3ExpressIdentityProvider : public S3ExpressIdentityProvider {
        public:
            explicit DefaultS3ExpressIdentityProvider(const S3Client &m_s3Client);

            DefaultS3ExpressIdentityProvider(const S3Client &s3Client,
                std::shared_ptr<Utils::ConcurrentCache<Aws::String, S3ExpressIdentity>> credentialsCache);

            DefaultS3ExpressIdentityProvider(const DefaultS3ExpressIdentityProvider& other) = delete;
            DefaultS3ExpressIdentityProvider(DefaultS3ExpressIdentityProvider&& other) noexcept = delete;
            DefaultS3ExpressIdentityProvider& operator=(const DefaultS3ExpressIdentityProvider& other) = delete;
            DefaultS3ExpressIdentityProvider& operator=(DefaultS3ExpressIdentityProvider&& other) noexcept = delete;

            virtual ~DefaultS3ExpressIdentityProvider() override = default;

            S3ExpressIdentity GetS3ExpressIdentity(const std::shared_ptr<Aws::Http::ServiceSpecificParameters> &serviceSpecificParameters) override;

        private:
            mutable std::shared_ptr<Aws::Utils::ConcurrentCache<Aws::String, S3ExpressIdentity>> m_credentialsCache;
        };

        class DefaultAsyncS3ExpressIdentityProvider : public S3ExpressIdentityProvider {
        public:
            explicit DefaultAsyncS3ExpressIdentityProvider(const S3Client &m_s3Client,
                std::chrono::minutes refreshPeriod = std::chrono::minutes(1));

            DefaultAsyncS3ExpressIdentityProvider(const S3Client &s3Client,
                std::shared_ptr<Utils::ConcurrentCache<Aws::String, S3ExpressIdentity>> credentialsCache,
                std::chrono::minutes refreshPeriod = std::chrono::minutes(1));

            DefaultAsyncS3ExpressIdentityProvider(const DefaultAsyncS3ExpressIdentityProvider& other) = delete;
            DefaultAsyncS3ExpressIdentityProvider(DefaultAsyncS3ExpressIdentityProvider&& other) noexcept = delete;
            DefaultAsyncS3ExpressIdentityProvider& operator=(const DefaultAsyncS3ExpressIdentityProvider& other) = delete;
            DefaultAsyncS3ExpressIdentityProvider& operator=(DefaultAsyncS3ExpressIdentityProvider&& other) noexcept = delete;

            virtual ~DefaultAsyncS3ExpressIdentityProvider() override;

            S3ExpressIdentity GetS3ExpressIdentity(const std::shared_ptr<Aws::Http::ServiceSpecificParameters> &serviceSpecificParameters) override;

        private:
            void refreshIdentities(std::chrono::minutes refreshPeriod);
            void threadSafeKeyInsert(const Aws::String& key);
            bool threadSafeKeyHas(const Aws::String& key);
            void threadSafeKeyEmpty();

            mutable std::shared_ptr<Aws::Utils::ConcurrentCache<Aws::String, S3ExpressIdentity>> m_credentialsCache;
            Aws::Set<Aws::String> m_keysUsed;
            mutable std::mutex m_keysUsedMutex;
            mutable bool m_shouldStopBackgroundRefresh;
            Aws::UniquePtr<std::thread> m_backgroundRefreshThread;
            mutable std::mutex m_shutDownMutex;
            mutable std::condition_variable m_shutdownCondition;
        };
    }
}

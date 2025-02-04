/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/auth/bearer-token-provider/AWSBearerTokenProviderBase.h>

#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>

namespace Aws
{
    namespace Auth 
    {
        /**
        * To support usage of SSO bearerToken.
        * The SSO token provider assumes that an SSO access token has already been resolved and cached to disk.
        */
        class AWS_CORE_API SSOBearerTokenProvider : public AWSBearerTokenProviderBase
        {
        public:
            SSOBearerTokenProvider();
            explicit SSOBearerTokenProvider(const Aws::String& awsProfile);
            explicit SSOBearerTokenProvider(const Aws::String& awsProfile, std::shared_ptr<const Aws::Client::ClientConfiguration> config);
            /**
            * Retrieves the bearerToken if found, otherwise returns empty credential set.
            */
            AWSBearerToken GetAWSBearerToken() override;

        protected:
            struct CachedSsoToken
            {
            public:
                Aws::String accessToken;
                Aws::Utils::DateTime expiresAt;
                Aws::String refreshToken;
                Aws::String clientId;
                Aws::String clientSecret;
                Aws::Utils::DateTime registrationExpiresAt;
                Aws::String region;
                Aws::String startUrl;
            };

            static const size_t REFRESH_ATTEMPT_INTERVAL_S;
            static const size_t REFRESH_WINDOW_BEFORE_EXPIRATION_S;
            // Profile description variables
            Aws::UniquePtr<Aws::Internal::SSOCredentialsClient> m_client;
            Aws::String m_profileToUse;
            std::shared_ptr<const Aws::Client::ClientConfiguration> m_config;

            mutable Aws::Auth::AWSBearerToken m_token;
            mutable Aws::Utils::DateTime m_lastUpdateAttempt;

            mutable Aws::Utils::Threading::ReaderWriterLock m_reloadLock;

            void Reload();
            void RefreshFromSso();
            CachedSsoToken LoadAccessTokenFile() const;
            bool WriteAccessTokenFile(const CachedSsoToken& token) const;
        };
    } // namespace Auth
} // namespace Aws

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/bearer-token-provider/SSOBearerTokenProvider.h>
#include <memory>

namespace Aws {
    namespace Auth {
        /**
         * To support usage of SSO credentials
         */
        class AWS_CORE_API SSOCredentialsProvider : public AWSCredentialsProvider
        {
        public:
            SSOCredentialsProvider();
            explicit SSOCredentialsProvider(const Aws::String& profile);
            explicit SSOCredentialsProvider(const Aws::String& profile, std::shared_ptr<const Aws::Client::ClientConfiguration> config);
            /**
             * Retrieves the credentials if found, otherwise returns empty credential set.
             */
            AWSCredentials GetAWSCredentials() override;

        private:
            Aws::UniquePtr<Aws::Internal::SSOCredentialsClient> m_client;
            Aws::Auth::AWSCredentials m_credentials;

            // Profile description variables
            Aws::String m_profileToUse;

            // The AWS account ID that temporary AWS credentials are resolved for.
            Aws::String m_ssoAccountId;
            // The AWS region where the SSO directory for the given sso_start_url is hosted.
            // This is independent of the general region configuration and MUST NOT be conflated.
            Aws::String m_ssoRegion;
            // The expiration time of the accessToken.
            Aws::Utils::DateTime m_expiresAt;
            // The SSO Token Provider
            Aws::Auth::SSOBearerTokenProvider m_bearerTokenProvider;
            // The client configuration to use
            std::shared_ptr<const Aws::Client::ClientConfiguration> m_config;

            void Reload() override;
            void RefreshIfExpired();
            Aws::String LoadAccessTokenFile(const Aws::String& ssoAccessTokenPath);
        };
    } // namespace Auth
} // namespace Aws

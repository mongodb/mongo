/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/auth/AWSCredentialsProvider.h>

namespace Aws
{
    namespace Auth
    {
        /**
        * General HTTP Credentials Provider (previously known as ECS credentials provider)
        * implementation that loads credentials from an arbitrary HTTP(S) endpoint specified by the environment
        * or loop back / Amazon ECS / Amazon EKS container host metadata services by default.
        */
        class AWS_CORE_API GeneralHTTPCredentialsProvider : public AWSCredentialsProvider
        {
        private:
            static bool ShouldCreateGeneralHTTPProvider(const Aws::String& relativeUri, const Aws::String& absoluteUri, const Aws::String authToken);
        public:
            using ShouldCreateFunc = std::function<bool(const Aws::String& relativeUri, const Aws::String& absoluteUri, const Aws::String authToken)>;

            /**
             * Initializes the provider to retrieve credentials from a general http provided endpoint every 5 minutes or before it
             * expires.
             * @param relativeUri A path appended to the metadata service endpoint. OR
             * @param absoluteUri The full URI to resolve to get credentials.
             * @param authToken An optional authorization token passed to the URI via the 'Authorization' HTTP header.
             * @param authTokenFilePath A path to a file with optional authorization token passed to the URI via the 'Authorization' HTTP header.
             * @param refreshRateMs The number of milliseconds after which the credentials will be fetched again.
             * @param ShouldCreateFunc
             */
            GeneralHTTPCredentialsProvider(const Aws::String& relativeUri,
                                           const Aws::String& absoluteUri,
                                           const Aws::String& authToken = "",
                                           const Aws::String& authTokenFilePath = "",
                                           long refreshRateMs = REFRESH_THRESHOLD,
                                           ShouldCreateFunc shouldCreateFunc = ShouldCreateGeneralHTTPProvider);

            /**
             * Check if GeneralHTTPCredentialsProvider was initialized with allowed configuration
             * @return true if provider configuration is valid
             */
            bool IsValid() const
            {
                if (!m_ecsCredentialsClient)
                    return false;
                if (!m_authTokenFilePath.empty())
                    return !LoadTokenFromFile().empty();
                return true;
            }

            /**
             * Initializes the provider to retrieve credentials from the ECS metadata service every 5 minutes,
             * or before it expires.
             * @param resourcePath A path appended to the metadata service endpoint.
             * @param refreshRateMs The number of milliseconds after which the credentials will be fetched again.
             */
            // TODO: 1.12: AWS_DEPRECATED("This c-tor is deprecated, please use one above.")
            GeneralHTTPCredentialsProvider(const char* resourcePath, long refreshRateMs = REFRESH_THRESHOLD)
              : GeneralHTTPCredentialsProvider(resourcePath, "", "", "", refreshRateMs)
            {}

            /**
             * Initializes the provider to retrieve credentials from a provided endpoint every 5 minutes or before it
             * expires.
             * @param endpoint The full URI to resolve to get credentials.
             * @param token An optional authorization token passed to the URI via the 'Authorization' HTTP header.
             * @param refreshRateMs The number of milliseconds after which the credentials will be fetched again.
             */
            // TODO: 1.12: AWS_DEPRECATED("This c-tor is deprecated, please use one above.")
            GeneralHTTPCredentialsProvider(const char* endpoint, const char* token, long refreshRateMs = REFRESH_THRESHOLD)
              : GeneralHTTPCredentialsProvider("", endpoint, token, "", refreshRateMs)
            {}

            /**
             * Initializes the provider to retrieve credentials using the provided client.
             * @param client The ECSCredentialsClient instance to use when retrieving credentials.
             * @param refreshRateMs The number of milliseconds after which the credentials will be fetched again.
             */
            GeneralHTTPCredentialsProvider(const std::shared_ptr<Aws::Internal::ECSCredentialsClient>& client,
                    long refreshRateMs = REFRESH_THRESHOLD);
            /**
            * Retrieves the credentials if found, otherwise returns empty credential set.
            */
            AWSCredentials GetAWSCredentials() override;

            static const char AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE[];
            static const char AWS_CONTAINER_CREDENTIALS_RELATIVE_URI[];
            static const char AWS_CONTAINER_CREDENTIALS_FULL_URI[];
            static const char AWS_CONTAINER_AUTHORIZATION_TOKEN[];

            static const char AWS_ECS_CONTAINER_HOST[];
            static const char AWS_EKS_CONTAINER_HOST[];
            static const char AWS_EKS_CONTAINER_HOST_IPV6[];

        protected:
            void Reload() override;

        private:
            bool ExpiresSoon() const;
            void RefreshIfExpired();

            Aws::String LoadTokenFromFile() const;

            std::shared_ptr<Aws::Internal::ECSCredentialsClient> m_ecsCredentialsClient;
            Aws::String m_authTokenFilePath;

            long m_loadFrequencyMs = REFRESH_THRESHOLD;
            Aws::Auth::AWSCredentials m_credentials;
        };

        // GeneralHTTPCredentialsProvider was previously known as TaskRoleCredentialsProvider or "ECS credentials provider"
        using TaskRoleCredentialsProvider = GeneralHTTPCredentialsProvider;
    } // namespace Auth
} // namespace Aws

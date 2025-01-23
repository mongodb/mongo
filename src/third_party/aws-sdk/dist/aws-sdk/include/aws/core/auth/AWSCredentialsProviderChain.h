/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <memory>

namespace Aws
{
    namespace Auth
    {
        /**
         * Abstract class for providing chains of credentials providers. When a credentials provider in the chain returns empty credentials,
         * We go on to the next provider until we have either exhausted the installed providers in the chain or something returns non-empty credentials.
         */
        class AWS_CORE_API AWSCredentialsProviderChain : public AWSCredentialsProvider
        {
        public:
            virtual ~AWSCredentialsProviderChain() = default;

            /**
             * When a credentials provider in the chain returns empty credentials,
             * We go on to the next provider until we have either exhausted the installed providers in the chain or something returns non-empty credentials.
             */
            virtual AWSCredentials GetAWSCredentials();

            /**
             * Gets all providers stored in this chain.
             */
            const Aws::Vector<std::shared_ptr<AWSCredentialsProvider>>& GetProviders() const { return m_providerChain; }

        protected:
            /**
             * This class is only allowed to be initialized by subclasses.
             */
            AWSCredentialsProviderChain() = default;

            /**
             * Adds a provider to the back of the chain.
             */
            void AddProvider(const std::shared_ptr<AWSCredentialsProvider>& provider) { m_providerChain.push_back(provider); }


        private:
            Aws::Vector<std::shared_ptr<AWSCredentialsProvider> > m_providerChain;
            std::shared_ptr<AWSCredentialsProvider> m_cachedProvider;
            mutable Aws::Utils::Threading::ReaderWriterLock m_cachedProviderLock;
        };

        /**
         * Creates an AWSCredentialsProviderChain which uses in order EnvironmentAWSCredentialsProvider, ProfileConfigFileAWSCredentialsProvider,
         * ProcessCredentialsProvider, STSAssumeRoleWebIdentityCredentialsProvider and SSOCredentialsProvider.
         */
        class AWS_CORE_API DefaultAWSCredentialsProviderChain : public AWSCredentialsProviderChain
        {
        public:
            /**
             * Initializes the provider chain with EnvironmentAWSCredentialsProvider, ProfileConfigFileAWSCredentialsProvider,
             * ProcessCredentialsProvider, STSAssumeRoleWebIdentityCredentialsProvider and SSOCredentialsProvider in that order.
             */
            DefaultAWSCredentialsProviderChain();

            DefaultAWSCredentialsProviderChain(const DefaultAWSCredentialsProviderChain& chain);
        };

    } // namespace Auth
} // namespace Aws

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/auth/bearer-token-provider/AWSBearerTokenProviderChainBase.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <memory>

namespace Aws
{
    namespace Auth
    {
        /**
         * Default built-in AWSBearerTokenProviderChainBase implementation that includes Aws::Auth::SSOBearerTokenProvider in the chain.
         */
        class AWS_CORE_API DefaultBearerTokenProviderChain : public AWSBearerTokenProviderChainBase
        {
        public:
            DefaultBearerTokenProviderChain();
            virtual ~DefaultBearerTokenProviderChain() = default;

            /**
             * Return bearer token, implementation of a base class interface
             */
            virtual AWSBearerToken GetAWSBearerToken() override;

            /**
             * Gets all providers stored in this chain.
             */
            const Aws::Vector<std::shared_ptr<AWSBearerTokenProviderBase>>& GetProviders() override
            {
                return m_providerChain;
            }

        protected:
            /**
             * Adds a provider to the back of the chain.
             */
            void AddProvider(const std::shared_ptr<AWSBearerTokenProviderBase>& provider) { m_providerChain.push_back(provider); }

            Aws::Vector<std::shared_ptr<AWSBearerTokenProviderBase> > m_providerChain;
        };

    } // namespace Auth
} // namespace Aws

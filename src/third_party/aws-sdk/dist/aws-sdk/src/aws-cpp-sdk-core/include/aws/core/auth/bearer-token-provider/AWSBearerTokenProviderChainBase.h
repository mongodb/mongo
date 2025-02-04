/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/auth/bearer-token-provider/AWSBearerTokenProviderBase.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <memory>

namespace Aws
{
    namespace Auth
    {
        /**
         * Abstract class for providing chains of bearer token providers
         */
        class AWS_CORE_API AWSBearerTokenProviderChainBase : public AWSBearerTokenProviderBase
        {
        public:
            virtual ~AWSBearerTokenProviderChainBase() = default;

            /**
             * Gets all providers stored in this chain.
             */
            virtual const Aws::Vector<std::shared_ptr<AWSBearerTokenProviderBase>>& GetProviders() = 0;
        };
    } // namespace Auth
} // namespace Aws

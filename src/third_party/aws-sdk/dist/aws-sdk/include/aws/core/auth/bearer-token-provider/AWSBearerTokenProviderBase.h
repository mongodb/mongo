/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/auth/AWSBearerToken.h>

namespace Aws
{
    namespace Auth
    {
        /**
          * Abstract class for retrieving Bearer Token. Create a derived class from this to allow
          * various methods of storing and retrieving auth bearer tokens.
          */
        class AWS_CORE_API AWSBearerTokenProviderBase
        {
        public:
            virtual ~AWSBearerTokenProviderBase() = default;

            /**
             * The core of the bearer token provider interface. Override this method to control how credentials are retrieved.
             */
            virtual AWSBearerToken GetAWSBearerToken() = 0;
        };
    } // namespace Auth
} // namespace Aws

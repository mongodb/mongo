/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/auth/bearer-token-provider/AWSBearerTokenProviderBase.h>

namespace Aws
{
    namespace Auth
    {
         /**
          * A simple string provider. It takes the AccessKeyId and the SecretKey as constructor args and
          * provides them through the interface. This is the default class for AWSClients that take string
          * arguments for BearerToken.
          */
        class AWS_CORE_API StaticAWSBearerTokenProvider : public AWSBearerTokenProviderBase
        {
        public:
            /**
            * Initializes object from BearerToken object. Everything is copied.
            */
            StaticAWSBearerTokenProvider(const AWSBearerToken& BearerToken)
                : m_bearerToken(BearerToken)
            { }

            /**
            * Initializes object from opaque token string and expiration. Everything is copied.
            */
            StaticAWSBearerTokenProvider(const Aws::String& token, const Aws::Utils::DateTime& expiration)
                : m_bearerToken(token, expiration)
            { }


            /**
             * Returns the BearerToken this object was initialized with as an AWSBearerToken object.
             */
            AWSBearerToken GetAWSBearerToken() override
            {
                return m_bearerToken;
            }

        protected:
            AWSBearerToken m_bearerToken;
        };
    } // namespace Auth
} // namespace Aws

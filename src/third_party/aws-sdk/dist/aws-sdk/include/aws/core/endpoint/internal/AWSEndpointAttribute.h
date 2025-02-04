/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#pragma once

#include <aws/core/client/AWSError.h>
#include <aws/crt/Optional.h>

namespace Aws
{
    namespace Internal
    {
        namespace Endpoint
        {
            class AWS_CORE_API EndpointAuthScheme
            {
            public:
                virtual ~EndpointAuthScheme(){};

                inline const Aws::String& GetName() const
                {
                    return m_name;
                }
                inline void SetName(Aws::String name)
                {
                    m_name = std::move(name);
                }

                inline const Crt::Optional<Aws::String>& GetSigningName() const
                {
                    return m_signingName;
                }
                inline void SetSigningName(Aws::String signingName)
                {
                    m_signingName = std::move(signingName);
                }

                inline const Crt::Optional<Aws::String>& GetSigningRegion() const
                {
                    return m_signingRegion;
                }
                inline void SetSigningRegion(Aws::String signingRegion)
                {
                    m_signingRegion = std::move(signingRegion);
                }

                inline const Crt::Optional<Aws::String>& GetSigningRegionSet() const
                {
                    return m_signingRegionSet;
                }
                inline void SetSigningRegionSet(Aws::String signingRegionSet)
                {
                    m_signingRegionSet = std::move(signingRegionSet);
                }

                inline const Crt::Optional<bool>& GetDisableDoubleEncoding() const
                {
                    return m_disableDoubleEncoding;
                }
                inline void SetDisableDoubleEncoding(bool disableDoubleEncoding)
                {
                    m_disableDoubleEncoding = disableDoubleEncoding;
                }

            private:
                Aws::String m_name;

                Crt::Optional<Aws::String> m_signingName;
                Crt::Optional<Aws::String> m_signingRegion;
                Crt::Optional<Aws::String> m_signingRegionSet;
                Crt::Optional<bool> m_disableDoubleEncoding;
            };

            /**
             * A grab bag property map of endpoint attributes. The values here are considered unstable.
             * C++ SDK supports only endpoint attributes "AuthScheme" and "Bucket Type".
             */
            struct AWS_CORE_API EndpointAttributes
            {
                Aws::Internal::Endpoint::EndpointAuthScheme authScheme;
                Aws::String backend;
                bool useS3ExpressAuth;

                static EndpointAttributes BuildEndpointAttributesFromJson(const Aws::String& iJsonStr);
            };
        } // namespace Endpoint
    } // namespace Internal
} // namespace Aws

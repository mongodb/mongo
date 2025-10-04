/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/http/URI.h>

#include <aws/core/endpoint/internal/AWSEndpointAttribute.h>

namespace Aws
{
    namespace Client
    {
        template<typename ERROR_TYPE>
        class AWSError;

        enum class CoreErrors;
    }
    namespace Endpoint
    {
        /**
         * A public type that encapsulates the information about an endpoint
         */
        class AWS_CORE_API AWSEndpoint
        {
        public:
            using EndpointAttributes = Internal::Endpoint::EndpointAttributes;

            virtual ~AWSEndpoint()
            {};

            Aws::String GetURL() const;
            void SetURL(Aws::String url);

            const Aws::Http::URI& GetURI() const;
            void SetURI(Aws::Http::URI uri);

            template<typename T>
            inline void AddPathSegment(T&& pathSegment)
            {
                m_uri.AddPathSegment(std::forward<T>(pathSegment));
            }

            template<typename T>
            inline void AddPathSegments(T&& pathSegments)
            {
                m_uri.AddPathSegments(std::forward<T>(pathSegments));
            }

            inline void SetRfc3986Encoded(bool rfcEncoded)
            {
                m_uri.SetRfc3986Encoded(rfcEncoded);
            }

            using OptionalError = Crt::Optional<Aws::Client::AWSError<Aws::Client::CoreErrors>>;
            OptionalError AddPrefixIfMissing(const Aws::String& prefix);

            void SetQueryString(const Aws::String& queryString);

            const Crt::Optional<EndpointAttributes>& GetAttributes() const;
            Crt::Optional<EndpointAttributes>& AccessAttributes();
            void SetAttributes(EndpointAttributes&& attributes);

            const Aws::UnorderedMap<Aws::String, Aws::String>& GetHeaders() const;
            void SetHeaders(Aws::UnorderedMap<Aws::String, Aws::String> headers);

        protected:
            // A URI containing at minimum the scheme and host. May optionally include a port and a path.
            Aws::Http::URI m_uri;

            // A grab bag property map of endpoint attributes. The values here are considered unstable.
            Crt::Optional<EndpointAttributes> m_attributes;

            // A map of additional headers to be set when calling the endpoint.
            // Note: the values in these maps are Lists to support multi-value headers.
            Aws::UnorderedMap<Aws::String, Aws::String> m_headers;
        };
    }
}

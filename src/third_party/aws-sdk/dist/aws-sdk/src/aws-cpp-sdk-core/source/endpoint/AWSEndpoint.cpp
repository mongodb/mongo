/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/endpoint/AWSEndpoint.h>
#include <aws/core/utils/DNS.h>

namespace Aws
{
namespace Endpoint
{

Aws::String AWSEndpoint::GetURL() const
{
    return m_uri.GetURIString();
}

void AWSEndpoint::SetURL(Aws::String url)
{
    m_uri = std::move(url);
}

const Aws::Http::URI& AWSEndpoint::GetURI() const
{
    return m_uri;
}

void AWSEndpoint::SetURI(Aws::Http::URI uri)
{
    m_uri = std::move(uri);
}

AWSEndpoint::OptionalError AWSEndpoint::AddPrefixIfMissing(const Aws::String& prefix)
{
    if (m_uri.GetAuthority().rfind(prefix, 0) == 0)
    {
        // uri already starts with a prefix
        return OptionalError();
    }

    if (Aws::Utils::IsValidHost(prefix + m_uri.GetAuthority()))
    {
        m_uri.SetAuthority(prefix + m_uri.GetAuthority());
        return OptionalError();
    }

    return OptionalError(
        Aws::Client::AWSError<Aws::Client::CoreErrors>(
            Aws::Client::CoreErrors::ENDPOINT_RESOLUTION_FAILURE, "",
            Aws::String("Failed to add host prefix, resulting uri is an invalid hostname: ") + prefix + m_uri.GetAuthority(),
            false/*retryable*/));
}

void AWSEndpoint::SetQueryString(const Aws::String& queryString)
{
    m_uri.SetQueryString(queryString);
}

const Crt::Optional<AWSEndpoint::EndpointAttributes>& AWSEndpoint::GetAttributes() const
{
    return m_attributes;
}

Crt::Optional<AWSEndpoint::EndpointAttributes>& AWSEndpoint::AccessAttributes()
{
    return m_attributes;
}

void AWSEndpoint::SetAttributes(AWSEndpoint::EndpointAttributes&& attributes)
{
    m_attributes = std::move(attributes);
}

const Aws::UnorderedMap<Aws::String, Aws::String>& AWSEndpoint::GetHeaders() const
{
    return m_headers;
}

void AWSEndpoint::SetHeaders(Aws::UnorderedMap<Aws::String, Aws::String> headers)
{
    m_headers = std::move(headers);
}

} // namespace Endpoint
} // namespace Aws

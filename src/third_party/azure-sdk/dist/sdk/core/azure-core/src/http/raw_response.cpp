// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/http/raw_response.hpp"

#include "azure/core/http/http.hpp"

using namespace Azure::Core::IO;
using namespace Azure::Core::Http;

HttpStatusCode RawResponse::GetStatusCode() const { return m_statusCode; }

std::string const& RawResponse::GetReasonPhrase() const { return m_reasonPhrase; }

Azure::Core::CaseInsensitiveMap const& RawResponse::GetHeaders() const { return this->m_headers; }

void RawResponse::SetHeader(std::string const& name, std::string const& value)
{
  return _detail::RawResponseHelpers::InsertHeaderWithValidation(this->m_headers, name, value);
}

void RawResponse::SetBodyStream(std::unique_ptr<BodyStream> stream)
{
  this->m_bodyStream = std::move(stream);
}

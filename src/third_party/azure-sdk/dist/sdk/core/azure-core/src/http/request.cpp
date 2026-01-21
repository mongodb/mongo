// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/http/http.hpp"
#include "azure/core/internal/io/null_body_stream.hpp"
#include "azure/core/internal/strings.hpp"

#include <map>
#include <string>
#include <vector>

using namespace Azure::Core;
using namespace Azure::Core::Http;
using namespace Azure::Core::IO::_internal;

namespace {
// returns left map plus all items in right
// when duplicates, left items are preferred
static Azure::Core::CaseInsensitiveMap MergeMaps(
    Azure::Core::CaseInsensitiveMap left,
    Azure::Core::CaseInsensitiveMap const& right)
{
  left.insert(right.begin(), right.end());
  return left;
}
} // namespace

Request::Request(HttpMethod httpMethod, Url url, bool shouldBufferResponse)
    : Request(httpMethod, std::move(url), NullBodyStream::GetNullBodyStream(), shouldBufferResponse)
{
}

Request::Request(HttpMethod httpMethod, Url url)
    : Request(httpMethod, std::move(url), NullBodyStream::GetNullBodyStream(), true)
{
}

Azure::Nullable<std::string> Request::GetHeader(std::string const& name)
{
  for (auto const& hdrs : {m_retryHeaders, m_headers})
  {
    auto const header = hdrs.find(name);
    if (header != hdrs.end())
    {
      return header->second;
    }
  }

  return {};
}

void Request::SetHeader(std::string const& name, std::string const& value)
{
  return _detail::RawResponseHelpers::InsertHeaderWithValidation(
      m_retryModeEnabled ? m_retryHeaders : m_headers,
      Azure::Core::_internal::StringExtensions::ToLower(name),
      value);
}

void Request::RemoveHeader(std::string const& name)
{
  this->m_headers.erase(name);
  this->m_retryHeaders.erase(name);
}

void Request::StartTry()
{
  this->m_retryModeEnabled = true;
  this->m_retryHeaders.clear();

  // Make sure to rewind the body stream before each attempt, including the first.
  // It's possible the request doesn't have a body, so make sure to check if a body stream exists.
  if (auto bodyStream = this->GetBodyStream())
  {
    bodyStream->Rewind();
  }
}

HttpMethod const& Request::GetMethod() const { return this->m_method; }

Azure::Core::CaseInsensitiveMap Request::GetHeaders() const
{
  // create map with retry headers which are the most important and we don't want
  // to override them with any duplicate header
  return MergeMaps(this->m_retryHeaders, this->m_headers);
}

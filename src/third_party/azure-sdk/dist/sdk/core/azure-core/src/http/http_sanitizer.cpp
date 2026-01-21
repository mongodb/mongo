// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/internal/http/http_sanitizer.hpp"

#include "azure/core/url.hpp"

#include <iterator>
#include <sstream>

namespace {
std::string const RedactedPlaceholder = "REDACTED";
}

using Azure::Core::Http::_internal::HttpSanitizer;

Azure::Core::Url HttpSanitizer::SanitizeUrl(Azure::Core::Url const& url) const
{
  std::ostringstream ss;

  // Sanitize the non-query part of the URL (remove username and password).
  if (!url.GetScheme().empty())
  {
    ss << url.GetScheme() << "://";
  }
  ss << url.GetHost();
  if (url.GetPort() != 0)
  {
    ss << ":" << url.GetPort();
  }
  if (!url.GetPath().empty())
  {
    ss << "/" << url.GetPath();
  }

  {
    auto encodedRequestQueryParams = url.GetQueryParameters();

    std::remove_const<std::remove_reference<decltype(encodedRequestQueryParams)>::type>::type
        loggedQueryParams;

    if (!encodedRequestQueryParams.empty())
    {
      auto const& unencodedAllowedQueryParams = m_allowedHttpQueryParameters;
      if (!unencodedAllowedQueryParams.empty())
      {
        std::remove_const<std::remove_reference<decltype(unencodedAllowedQueryParams)>::type>::type
            encodedAllowedQueryParams;
        std::transform(
            unencodedAllowedQueryParams.begin(),
            unencodedAllowedQueryParams.end(),
            std::inserter(encodedAllowedQueryParams, encodedAllowedQueryParams.begin()),
            [](std::string const& s) { return Url::Encode(s); });

        for (auto const& encodedRequestQueryParam : encodedRequestQueryParams)
        {
          if (encodedRequestQueryParam.second.empty()
              || (encodedAllowedQueryParams.find(encodedRequestQueryParam.first)
                  != encodedAllowedQueryParams.end()))
          {
            loggedQueryParams.insert(encodedRequestQueryParam);
          }
          else
          {
            loggedQueryParams.insert(
                std::make_pair(encodedRequestQueryParam.first, RedactedPlaceholder));
          }
        }
      }
      else
      {
        for (auto const& encodedRequestQueryParam : encodedRequestQueryParams)
        {
          loggedQueryParams.insert(
              std::make_pair(encodedRequestQueryParam.first, RedactedPlaceholder));
        }
      }

      ss << Azure::Core::_detail::FormatEncodedUrlQueryParameters(loggedQueryParams);
    }
  }
  return Azure::Core::Url(ss.str());
}

std::string HttpSanitizer::SanitizeHeader(std::string const& header, std::string const& value) const
{
  return (m_allowedHttpHeaders.find(header) != m_allowedHttpHeaders.end()) ? value
                                                                           : RedactedPlaceholder;
}

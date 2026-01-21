// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/core/url.hpp"

#include <string>

namespace Azure { namespace Core { namespace Http { namespace _internal {
  class HttpSanitizer final {
    /**
     * @brief HTTP header names that are allowed to be logged.
     */
    Azure::Core::CaseInsensitiveSet m_allowedHttpHeaders;

    /**
     * @brief HTTP query parameter names that are allowed to be logged.
     */
    std::set<std::string> m_allowedHttpQueryParameters;

  public:
    HttpSanitizer() = default;
    HttpSanitizer(
        std::set<std::string> const& allowedHttpQueryParameters,
        Azure::Core::CaseInsensitiveSet const& allowedHttpHeaders)
        : m_allowedHttpHeaders(allowedHttpHeaders),
          m_allowedHttpQueryParameters(allowedHttpQueryParameters)
    {
    }
    /**
     * @brief Sanitizes the specified URL according to the sanitization rules configured.
     *
     * @param url Url to sanitize. Specified elements will be redacted from the URL.
     * @return sanitized URL.
     */
    Azure::Core::Url SanitizeUrl(Url const& url) const;
    /**
     * @brief Sanitizes the provided HTTP header value according to the sanitization rules
     * configured.
     *
     * @param headerName Name of the header to sanitize.
     * @param headerValue Current value of the header to sanitize.
     * @return Sanitized header value.
     */
    std::string SanitizeHeader(std::string const& headerName, std::string const& headerValue) const;
  };
}}}} // namespace Azure::Core::Http::_internal

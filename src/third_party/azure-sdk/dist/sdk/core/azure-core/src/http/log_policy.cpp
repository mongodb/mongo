// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/http/policies/policy.hpp"
#include "azure/core/internal/diagnostics/log.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <sstream>
#include <type_traits>

using Azure::Core::Context;
using namespace Azure::Core;
using namespace Azure::Core::Http;
using namespace Azure::Core::Http::Policies;
using namespace Azure::Core::Http::Policies::_internal;

namespace {
std::string RedactedPlaceholder = "REDACTED";

inline void AppendHeaders(
    std::ostringstream& log,
    Azure::Core::Http::_internal::HttpSanitizer const& httpSanitizer,
    Azure::Core::CaseInsensitiveMap const& headers)
{
  for (auto const& header : headers)
  {
    log << std::endl << header.first << " : ";

    if (!header.second.empty())
    {
      log << httpSanitizer.SanitizeHeader(header.first, header.second);
    }
  }
}

inline std::string GetRequestLogMessage(
    Azure::Core::Http::_internal::HttpSanitizer const& httpSanitizer,
    Request const& request)
{
  std::ostringstream log;
  log << "HTTP Request : " << request.GetMethod().ToString() << " ";

  Azure::Core::Url urlToLog(httpSanitizer.SanitizeUrl(request.GetUrl()));
  log << urlToLog.GetAbsoluteUrl();

  AppendHeaders(log, httpSanitizer, request.GetHeaders());
  return log.str();
}

inline std::string GetResponseLogMessage(
    Azure::Core::Http::_internal::HttpSanitizer const& httpSanitizer,
    RawResponse const& response,
    std::chrono::system_clock::duration const& duration)
{
  std::ostringstream log;

  log << "HTTP/" << response.GetMajorVersion() << '.' << response.GetMinorVersion() << " Response ("
      << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()
      << "ms) : " << static_cast<int>(response.GetStatusCode()) << " "
      << response.GetReasonPhrase();

  AppendHeaders(log, httpSanitizer, response.GetHeaders());
  return log.str();
}
} // namespace

std::set<std::string> const Policies::_detail::g_defaultAllowedHttpQueryParameters = {
    "api-version",
};

CaseInsensitiveSet const Policies::_detail::g_defaultAllowedHttpHeaders
    = {"Accept",
       "Accept-Ranges",
       "Cache-Control",
       "Connection",
       "Content-Length",
       "Content-Range",
       "Content-Type",
       "Date",
       "ETag",
       "Expires",
       "If-Match",
       "If-Modified-Since",
       "If-None-Match",
       "If-Unmodified-Since",
       "Last-Modified",
       "Pragma",
       "Range",
       "Request-Id",
       "Retry-After",
       "Server",
       "traceparent",
       "tracestate",
       "Transfer-Encoding",
       "User-Agent",
       "WWW-Authenticate",
       "x-ms-client-request-id",
       "x-ms-date",
       "x-ms-error-code",
       "x-ms-range",
       "x-ms-request-id",
       "x-ms-return-client-request-id",
       "x-ms-version"};

std::unique_ptr<RawResponse> LogPolicy::Send(
    Request& request,
    NextHttpPolicy nextPolicy,
    Context const& context) const
{
  using Azure::Core::Diagnostics::Logger;
  using Azure::Core::Diagnostics::_internal::Log;

  if (Log::ShouldWrite(Logger::Level::Verbose))
  {
    Log::Write(Logger::Level::Informational, GetRequestLogMessage(m_httpSanitizer, request));
  }
  else
  {
    return nextPolicy.Send(request, context);
  }

  auto const start = std::chrono::system_clock::now();
  auto response = nextPolicy.Send(request, context);
  auto const end = std::chrono::system_clock::now();

  Log::Write(
      Logger::Level::Informational, GetResponseLogMessage(m_httpSanitizer, *response, end - start));

  return response;
}

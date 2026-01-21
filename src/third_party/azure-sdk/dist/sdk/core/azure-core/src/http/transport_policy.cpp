// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/http/policies/policy.hpp"
#include "azure/core/platform.hpp"

#if defined(BUILD_CURL_HTTP_TRANSPORT_ADAPTER)
#include "azure/core/http/curl_transport.hpp"
#endif

#if defined(BUILD_TRANSPORT_WINHTTP_ADAPTER)
#include "azure/core/http/win_http_transport.hpp"
#endif

#include <sstream>
#include <string>

using Azure::Core::Context;
using namespace Azure::Core::IO;
using namespace Azure::Core::Http;
using namespace Azure::Core::Http::Policies;
using namespace Azure::Core::Http::Policies::_internal;

namespace Azure { namespace Core { namespace Http { namespace Policies { namespace _detail {
  namespace {
    /**
     * @brief Returns "true" if any specific transport options have been specified.
     */
    bool AreAnyTransportOptionsSpecified(TransportOptions const& transportOptions)
    {
      return (transportOptions.HttpProxy.HasValue() || transportOptions.ProxyPassword.HasValue()
              || transportOptions.ProxyUserName.HasValue()
              || transportOptions.EnableCertificateRevocationListCheck
              || !transportOptions.ExpectedTlsRootCertificate.empty())
          || transportOptions.DisableTlsCertificateValidation;
    }
  } // namespace

  std::shared_ptr<HttpTransport> GetTransportAdapter(TransportOptions const& transportOptions)
  {
    // The order of these checks is important so that WinHTTP is picked over libcurl on
    // Windows, when both are defined.
#if defined(BUILD_TRANSPORT_CUSTOM_ADAPTER)
    return ::AzureSdkGetCustomHttpTransport();
    (void)transportOptions;
#elif defined(BUILD_TRANSPORT_WINHTTP_ADAPTER)
    // Since C++11: If multiple threads attempt to initialize the same static local variable
    // concurrently, the initialization occurs exactly once. We depend on this behavior to ensure
    // that the singleton defaultTransport is correctly initialized.
    static std::shared_ptr<HttpTransport> defaultTransport(std::make_shared<WinHttpTransport>());
    if (AreAnyTransportOptionsSpecified(transportOptions))
    {
      return std::make_shared<Azure::Core::Http::WinHttpTransport>(transportOptions);
    }
    else
    {
      //      std::call_once(createTransportOnce, []() {
      //      defaultTransport = std::make_shared<Azure::Core::Http::WinHttpTransport>();
      //    });
      return defaultTransport;
    }
#elif defined(BUILD_CURL_HTTP_TRANSPORT_ADAPTER)
    static std::shared_ptr<HttpTransport> defaultTransport(std::make_shared<CurlTransport>());
    if (AreAnyTransportOptionsSpecified(transportOptions))
    {
      return std::make_shared<Azure::Core::Http::CurlTransport>(transportOptions);
    }
    return defaultTransport;
#else
    return std::shared_ptr<HttpTransport>();
#endif
  }
}}}}} // namespace Azure::Core::Http::Policies::_detail

TransportPolicy::TransportPolicy(TransportOptions const& options) : m_options(options)
{
  // If there's no transport specified, then we need to create one.
  // If there is one specified, it's an error to specify other options.
  if (m_options.Transport)
  {
#if !defined(BUILD_TRANSPORT_CUSTOM_ADAPTER)
    if (_detail::AreAnyTransportOptionsSpecified(options))
    {
      AZURE_ASSERT_MSG(
          false, "Invalid parameter: Proxies cannot be specified when a transport is specified.");
    }
#endif
  }
  else
  {
    // Configure a transport adapter based on the options and compiler switches.
    m_options.Transport = _detail::GetTransportAdapter(m_options);
  }
}

std::unique_ptr<RawResponse> TransportPolicy::Send(
    Request& request,
    NextHttpPolicy,
    Context const& context) const
{
  // Before doing any work, check to make sure that the context hasn't already been cancelled.
  context.ThrowIfCancelled();

  /*
   * The transport policy is always the last policy.
   *
   * Default behavior for all requests is to download the full response to the RawResponse's
   * buffer.
   *
   ********************************** Notes ************************************************
   *
   * - If ReadToEnd() fails while downloading all the response, the retry policy will make sure to
   * re-send the request to re-start the download.
   *
   * - If the request returns error (statusCode >= 300), even if `request.ShouldBufferResponse()`,
   *the response will be download to the response's buffer.
   *
   ***********************************************************************************
   *
   */
  auto response = m_options.Transport->Send(request, context);
  auto statusCode = static_cast<typename std::underlying_type<Http::HttpStatusCode>::type>(
      response->GetStatusCode());

  // special case to return a response with BodyStream to read directly from socket
  // Return only if response did not fail.
  if (!request.ShouldBufferResponse() && statusCode < 300)
  {
    return response;
  }

  // At this point, either the request is `shouldBufferResponse` or it return with an error code.
  // The entire payload needs must be downloaded to the response's buffer.
  auto bodyStream = response->ExtractBodyStream();
  response->SetBody(bodyStream->ReadToEnd(context));

  // BodyStream is moved out of response. This makes transport implementation to clean any active
  // session with sockets or internal state.
  return response;
}

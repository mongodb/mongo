// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/http/policies/policy.hpp"
#include "azure/core/internal/diagnostics/log.hpp"
#include "azure/core/internal/http/http_sanitizer.hpp"
#include "azure/core/internal/tracing/service_tracing.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <thread>

using Azure::Core::Context;
using namespace Azure::Core::Http;
using namespace Azure::Core::Http::Policies;
using namespace Azure::Core::Http::Policies::_internal;
using namespace Azure::Core::Tracing::_internal;

std::unique_ptr<RawResponse> RequestActivityPolicy::Send(
    Request& request,
    NextHttpPolicy nextPolicy,
    Context const& context) const
{
  // Find a tracing factory from our context. Note that the factory value is owned by the
  // context chain so we can manage a raw pointer to the factory.
  auto tracingFactory = TracingContextFactory::CreateFromContext(context);

  // If our tracing factory has a tracer attached to it, register the request with the tracer.
  if (tracingFactory && tracingFactory->HasTracer())
  {
    // Create a tracing span over the HTTP request.
    std::string spanName("HTTP ");
    spanName.append(request.GetMethod().ToString());

    CreateSpanOptions createOptions;
    createOptions.Kind = SpanKind::Client;
    createOptions.Attributes = tracingFactory->CreateAttributeSet();
    // Note that the AttributeSet takes a *reference* to the values passed into the
    // AttributeSet. This means that all the values passed into the AttributeSet MUST be
    // stabilized across the lifetime of the AttributeSet.

    // Note that request.GetMethod() returns an HttpMethod object, which is always a static
    // object, and thus its lifetime is constant. That is not the case for the other values
    // stored in the attributes.
    createOptions.Attributes->AddAttribute(
        TracingAttributes::HttpMethod.ToString(), request.GetMethod().ToString());

    const std::string sanitizedUrl = m_httpSanitizer.SanitizeUrl(request.GetUrl()).GetAbsoluteUrl();
    createOptions.Attributes->AddAttribute(TracingAttributes::HttpUrl.ToString(), sanitizedUrl);

    createOptions.Attributes->AddAttribute(
        TracingAttributes::NetPeerPort.ToString(), request.GetUrl().GetPort());
    const std::string host = request.GetUrl().GetScheme() + "://" + request.GetUrl().GetHost();
    createOptions.Attributes->AddAttribute(TracingAttributes::NetPeerName.ToString(), host);

    const Azure::Nullable<std::string> requestId = request.GetHeader("x-ms-client-request-id");
    if (requestId.HasValue())
    {
      createOptions.Attributes->AddAttribute(
          TracingAttributes::RequestId.ToString(), requestId.Value());
    }

    auto userAgent{request.GetHeader("User-Agent")};
    if (userAgent.HasValue())
    {
      createOptions.Attributes->AddAttribute(
          TracingAttributes::HttpUserAgent.ToString(), userAgent.Value());
    }

    auto contextAndSpan = tracingFactory->CreateTracingContext(spanName, createOptions, context);
    auto scope = std::move(contextAndSpan.Span);

    // Propagate information from the scope to the HTTP headers.
    //
    // This will add the "traceparent" header and any other OpenTelemetry related headers.
    scope.PropagateToHttpHeaders(request);

    try
    {
      // Send the request on to the service.
      auto response = nextPolicy.Send(request, contextAndSpan.Context);

      // And register the headers we received from the service.
      scope.AddAttribute(
          TracingAttributes::HttpStatusCode.ToString(),
          std::to_string(static_cast<int>(response->GetStatusCode())));
      auto const& responseHeaders = response->GetHeaders();
      auto serviceRequestId = responseHeaders.find("x-ms-request-id");
      if (serviceRequestId != responseHeaders.end())
      {
        scope.AddAttribute(
            TracingAttributes::ServiceRequestId.ToString(), serviceRequestId->second);
      }

      return response;
    }
    catch (const TransportException& e)
    {
      scope.AddEvent(e);
      scope.SetStatus(SpanStatus::Error);

      // Rethrow the exception.
      throw;
    }
  }
  else
  {
    return nextPolicy.Send(request, context);
  }
}

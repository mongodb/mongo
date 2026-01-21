// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/http/policies/policy.hpp"

using Azure::Core::Context;
using namespace Azure::Core::Http;
using namespace Azure::Core::Http::Policies;
using namespace Azure::Core::Http::Policies::_internal;

std::unique_ptr<RawResponse> Azure::Core::Http::Policies::_internal::TelemetryPolicy::Send(
    Request& request,
    NextHttpPolicy nextPolicy,
    Context const& context) const
{
  static std::string const UserAgent{"User-Agent"};

  if (!request.GetHeader(UserAgent).HasValue())
  {
    request.SetHeader(UserAgent, m_telemetryId);
  }

  return nextPolicy.Send(request, context);
}

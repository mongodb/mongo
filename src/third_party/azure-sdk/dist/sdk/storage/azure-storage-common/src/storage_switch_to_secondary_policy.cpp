// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <azure/storage/common/internal/storage_switch_to_secondary_policy.hpp>

namespace Azure { namespace Storage { namespace _internal {

  Azure::Core::Context::Key const SecondaryHostReplicaStatusKey;

  std::unique_ptr<Azure::Core::Http::RawResponse> StorageSwitchToSecondaryPolicy::Send(
      Azure::Core::Http::Request& request,
      Azure::Core::Http::Policies::NextHttpPolicy nextPolicy,
      const Azure::Core::Context& context) const
  {
    std::shared_ptr<bool> replicaStatus;
    context.TryGetValue(SecondaryHostReplicaStatusKey, replicaStatus);

    bool considerSecondary = (request.GetMethod() == Azure::Core::Http::HttpMethod::Get
                              || request.GetMethod() == Azure::Core::Http::HttpMethod::Head)
        && !m_secondaryHost.empty() && replicaStatus && *replicaStatus;

    if (considerSecondary
        && Azure::Core::Http::Policies::_internal::RetryPolicy::GetRetryCount(context) > 0)
    {
      // switch host
      if (request.GetUrl().GetHost() == m_primaryHost)
      {
        request.GetUrl().SetHost(m_secondaryHost);
      }
      else
      {
        request.GetUrl().SetHost(m_primaryHost);
      }
    }

    auto response = nextPolicy.Send(request, context);

    if (considerSecondary
        && (response->GetStatusCode() == Azure::Core::Http::HttpStatusCode::NotFound
            || response->GetStatusCode() == Core::Http::HttpStatusCode::PreconditionFailed)
        && request.GetUrl().GetHost() == m_secondaryHost)
    {
      *replicaStatus = false;
      // switch back
      request.GetUrl().SetHost(m_primaryHost);
      response = nextPolicy.Send(request, context);
    }

    return response;
  }

}}} // namespace Azure::Storage::_internal

// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <azure/core/http/policies/policy.hpp>

#include <mutex>
#include <shared_mutex>

namespace Azure { namespace Storage { namespace _internal {

  class StorageBearerTokenAuthenticationPolicy final
      : public Core::Http::Policies::_internal::BearerTokenAuthenticationPolicy {
  public:
    /**
     * @brief Construct a Storage Bearer Token challenge authentication policy.
     *
     * @param credential An #Azure::Core::TokenCredential to use with this policy.
     * @param tokenRequestContext A context to get the token in.
     * @param enableTenantDiscovery Enables tenant discovery through the authorization challenge.
     */
    explicit StorageBearerTokenAuthenticationPolicy(
        std::shared_ptr<const Azure::Core::Credentials::TokenCredential> credential,
        Azure::Core::Credentials::TokenRequestContext tokenRequestContext,
        bool enableTenantDiscovery)
        : BearerTokenAuthenticationPolicy(std::move(credential), tokenRequestContext),
          m_scopes(tokenRequestContext.Scopes), m_safeTenantId(tokenRequestContext.TenantId),
          m_enableTenantDiscovery(enableTenantDiscovery)
    {
    }

    ~StorageBearerTokenAuthenticationPolicy() override {}

    std::unique_ptr<HttpPolicy> Clone() const override
    {
      return std::unique_ptr<HttpPolicy>(new StorageBearerTokenAuthenticationPolicy(*this));
    }

  private:
    struct SafeTenantId
    {
    public:
      explicit SafeTenantId(std::string tenantId) : m_tenantId(std::move(tenantId)) {}

      SafeTenantId(const SafeTenantId& other) : m_tenantId(other.Get()) {}

      std::string Get() const
      {
        std::shared_lock<std::shared_timed_mutex> lock(m_tenantIdMutex);
        return m_tenantId;
      }

      void Set(const std::string& tenantId)
      {
        std::unique_lock<std::shared_timed_mutex> lock(m_tenantIdMutex);
        m_tenantId = tenantId;
      }

    private:
      std::string m_tenantId;
      mutable std::shared_timed_mutex m_tenantIdMutex;
    };

    std::vector<std::string> m_scopes;
    mutable SafeTenantId m_safeTenantId;
    bool m_enableTenantDiscovery;

    std::unique_ptr<Azure::Core::Http::RawResponse> AuthorizeAndSendRequest(
        Azure::Core::Http::Request& request,
        Azure::Core::Http::Policies::NextHttpPolicy& nextPolicy,
        Azure::Core::Context const& context) const override;

    bool AuthorizeRequestOnChallenge(
        std::string const& challenge,
        Azure::Core::Http ::Request& request,
        Azure::Core::Context const& context) const override;
  };

}}} // namespace Azure::Storage::_internal

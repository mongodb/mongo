// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/identity/dll_import_export.hpp"

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/internal/environment.hpp>
#include <azure/core/url.hpp>

#include <string>
#include <vector>

namespace Azure { namespace Identity { namespace _detail {
  constexpr auto AzureAuthorityHostEnvVarName = "AZURE_AUTHORITY_HOST";
  constexpr auto AzureTenantIdEnvVarName = "AZURE_TENANT_ID";
  constexpr auto AzureClientIdEnvVarName = "AZURE_CLIENT_ID";
  constexpr auto AzureFederatedTokenFileEnvVarName = "AZURE_FEDERATED_TOKEN_FILE";
  const std::string OidcRequestUrlEnvVarName = "SYSTEM_OIDCREQUESTURI";
  const std::string AadGlobalAuthority = "https://login.microsoftonline.com/";

  class DefaultOptionValues final {
    DefaultOptionValues() = delete;
    ~DefaultOptionValues() = delete;

  public:
    static std::string GetAuthorityHost()
    {
      const std::string envAuthHost
          = Core::_internal::Environment::GetVariable(AzureAuthorityHostEnvVarName);

      return envAuthHost.empty() ? AadGlobalAuthority : envAuthHost;
    }

    static std::string GetTenantId()
    {
      return Core::_internal::Environment::GetVariable(AzureTenantIdEnvVarName);
    }

    static std::string GetClientId()
    {
      return Core::_internal::Environment::GetVariable(AzureClientIdEnvVarName);
    }

    static std::string GetFederatedTokenFile()
    {
      return Core::_internal::Environment::GetVariable(AzureFederatedTokenFileEnvVarName);
    }

    static std::string GetOidcRequestUrl()
    {
      return Core::_internal::Environment::GetVariable(OidcRequestUrlEnvVarName.c_str());
    }
  };

  class ClientCredentialCore final {
    std::vector<std::string> m_additionallyAllowedTenants;
    Core::Url m_authorityHost;
    std::string m_tenantId;

  public:
    explicit ClientCredentialCore(
        std::string tenantId,
        std::string const& authorityHost,
        std::vector<std::string> additionallyAllowedTenants);

    Core::Url GetRequestUrl(std::string const& tenantId) const;

    std::string GetScopesString(
        std::string const& tenantId,
        decltype(Core::Credentials::TokenRequestContext::Scopes) const& scopes) const;

    std::string const& GetTenantId() const { return m_tenantId; }

    std::vector<std::string> const& GetAdditionallyAllowedTenants() const
    {
      return m_additionallyAllowedTenants;
    }
  };
}}} // namespace Azure::Identity::_detail

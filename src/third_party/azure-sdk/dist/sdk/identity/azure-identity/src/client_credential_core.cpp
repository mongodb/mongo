// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/identity/detail/client_credential_core.hpp"

#include "private/tenant_id_resolver.hpp"
#include "private/token_credential_impl.hpp"

using Azure::Identity::_detail::ClientCredentialCore;

using Azure::Core::Url;
using Azure::Core::Credentials::TokenRequestContext;
using Azure::Identity::_detail::TenantIdResolver;
using Azure::Identity::_detail::TokenCredentialImpl;

// The authority host used by the credentials is in the following order of precedence:
// 1. AuthorityHost option set/overriden by the user.
// 2. The value of AZURE_AUTHORITY_HOST environment variable, which is the default value of the
// option.
// 3. If that environment variable isn't set or is empty, use Azure Public Cloud.
ClientCredentialCore::ClientCredentialCore(
    std::string tenantId,
    std::string const& authorityHost,
    std::vector<std::string> additionallyAllowedTenants)
    : m_additionallyAllowedTenants(std::move(additionallyAllowedTenants)),
      m_authorityHost(Url(authorityHost)), m_tenantId(std::move(tenantId))
{
}

Url ClientCredentialCore::GetRequestUrl(std::string const& tenantId) const
{
  if (m_authorityHost.GetScheme() != "https")
  {
    throw Azure::Core::Credentials::AuthenticationException(
        "Authority host must be a TLS protected (https) endpoint.");
  }

  auto requestUrl = m_authorityHost;
  requestUrl.AppendPath(tenantId);
  requestUrl.AppendPath(TenantIdResolver::IsAdfs(tenantId) ? "oauth2/token" : "oauth2/v2.0/token");

  return requestUrl;
}

std::string ClientCredentialCore::GetScopesString(
    std::string const& tenantId,
    decltype(TokenRequestContext::Scopes) const& scopes) const
{
  return scopes.empty()
      ? std::string()
      : TokenCredentialImpl::FormatScopes(scopes, TenantIdResolver::IsAdfs(tenantId));
}

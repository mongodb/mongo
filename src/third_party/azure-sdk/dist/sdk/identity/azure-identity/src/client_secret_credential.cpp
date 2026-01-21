// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/identity/client_secret_credential.hpp"

#include "private/tenant_id_resolver.hpp"
#include "private/token_credential_impl.hpp"

using Azure::Identity::ClientSecretCredential;

using Azure::Core::Context;
using Azure::Core::Url;
using Azure::Core::Credentials::AccessToken;
using Azure::Core::Credentials::TokenCredentialOptions;
using Azure::Core::Credentials::TokenRequestContext;
using Azure::Core::Http::HttpMethod;
using Azure::Identity::_detail::TenantIdResolver;
using Azure::Identity::_detail::TokenCredentialImpl;

ClientSecretCredential::ClientSecretCredential(
    std::string tenantId,
    std::string const& clientId,
    std::string const& clientSecret,
    std::string const& authorityHost,
    std::vector<std::string> additionallyAllowedTenants,
    Core::Credentials::TokenCredentialOptions const& options)
    : TokenCredential("ClientSecretCredential"),
      m_clientCredentialCore(tenantId, authorityHost, additionallyAllowedTenants),
      m_tokenCredentialImpl(std::make_unique<TokenCredentialImpl>(options)),
      m_requestBody(
          std::string("grant_type=client_credentials&client_id=") + Url::Encode(clientId)
          + "&client_secret=" + Url::Encode(clientSecret))
{
}

ClientSecretCredential::ClientSecretCredential(
    std::string tenantId,
    std::string const& clientId,
    std::string const& clientSecret,
    ClientSecretCredentialOptions const& options)
    : ClientSecretCredential(
        tenantId,
        clientId,
        clientSecret,
        options.AuthorityHost,
        options.AdditionallyAllowedTenants,
        options)
{
}

ClientSecretCredential::ClientSecretCredential(
    std::string tenantId,
    std::string const& clientId,
    std::string const& clientSecret,
    Core::Credentials::TokenCredentialOptions const& options)
    : ClientSecretCredential(
        tenantId,
        clientId,
        clientSecret,
        ClientSecretCredentialOptions{}.AuthorityHost,
        ClientSecretCredentialOptions{}.AdditionallyAllowedTenants,
        options)
{
}

ClientSecretCredential::~ClientSecretCredential() = default;

AccessToken ClientSecretCredential::GetToken(
    TokenRequestContext const& tokenRequestContext,
    Context const& context) const
{

  auto const tenantId = TenantIdResolver::Resolve(
      m_clientCredentialCore.GetTenantId(),
      tokenRequestContext,
      m_clientCredentialCore.GetAdditionallyAllowedTenants());

  auto const scopesStr
      = m_clientCredentialCore.GetScopesString(tenantId, tokenRequestContext.Scopes);

  // TokenCache::GetToken() and m_tokenCredentialImpl->GetToken() can only use the lambda argument
  // when they are being executed. They are not supposed to keep a reference to lambda argument to
  // call it later. Therefore, any capture made here will outlive the possible time frame when the
  // lambda might get called.
  return m_tokenCache.GetToken(scopesStr, tenantId, tokenRequestContext.MinimumExpiration, [&]() {
    return m_tokenCredentialImpl->GetToken(context, false, [&]() {
      auto body = m_requestBody;

      if (!scopesStr.empty())
      {
        body += "&scope=" + scopesStr;
      }

      auto const requestUrl = m_clientCredentialCore.GetRequestUrl(tenantId);

      auto request
          = std::make_unique<TokenCredentialImpl::TokenRequest>(HttpMethod::Post, requestUrl, body);

      request->HttpRequest.SetHeader("Host", requestUrl.GetHost());

      return request;
    });
  });
}

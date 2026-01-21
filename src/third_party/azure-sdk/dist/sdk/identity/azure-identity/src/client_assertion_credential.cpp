// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/identity/client_assertion_credential.hpp"

#include "private/client_assertion_credential_impl.hpp"
#include "private/identity_log.hpp"
#include "private/package_version.hpp"
#include "private/tenant_id_resolver.hpp"

#include <azure/core/internal/json/json.hpp>

using Azure::Identity::ClientAssertionCredential;
using Azure::Identity::ClientAssertionCredentialOptions;
using Azure::Identity::_detail::ClientAssertionCredentialImpl;

using Azure::Core::Context;
using Azure::Core::Url;
using Azure::Core::_internal::StringExtensions;
using Azure::Core::Credentials::AccessToken;
using Azure::Core::Credentials::AuthenticationException;
using Azure::Core::Credentials::TokenRequestContext;
using Azure::Core::Http::HttpMethod;
using Azure::Identity::_detail::IdentityLog;
using Azure::Identity::_detail::TenantIdResolver;
using Azure::Identity::_detail::TokenCredentialImpl;

ClientAssertionCredentialImpl::ClientAssertionCredentialImpl(
    std::string const& credentialName,
    std::string tenantId,
    std::string clientId,
    std::function<std::string(Context const&)> assertionCallback,
    ClientAssertionCredentialOptions const& options)
    : m_assertionCallback(std::move(assertionCallback)),
      m_clientCredentialCore(tenantId, options.AuthorityHost, options.AdditionallyAllowedTenants)
{
  bool isTenantIdValid = TenantIdResolver::IsValidTenantId(tenantId);
  if (!isTenantIdValid)
  {
    IdentityLog::Write(
        IdentityLog::Level::Warning,
        credentialName
            + ": Invalid tenant ID provided. The tenant ID must be a non-empty string containing "
              "only alphanumeric characters, periods, or hyphens. You can locate your tenant ID by "
              "following the instructions listed here: "
              "https://learn.microsoft.com/partner-center/find-ids-and-domain-names");
  }
  if (clientId.empty())
  {
    IdentityLog::Write(IdentityLog::Level::Warning, credentialName + ": No client ID specified.");
  }
  if (!m_assertionCallback)
  {
    IdentityLog::Write(
        IdentityLog::Level::Warning,
        credentialName
            + ": The assertionCallback must be a valid function that returns assertions.");
  }

  if (isTenantIdValid && !clientId.empty() && m_assertionCallback)
  {
    m_tokenCredentialImpl = std::make_unique<TokenCredentialImpl>(options);
    m_requestBody
        = std::string(
              "grant_type=client_credentials"
              "&client_assertion_type="
              "urn%3Aietf%3Aparams%3Aoauth%3Aclient-assertion-type%3Ajwt-bearer" // cspell:disable-line
              "&client_id=")
        + Url::Encode(clientId);

    IdentityLog::Write(
        IdentityLog::Level::Informational, credentialName + " was created successfully.");
  }
  else
  {
    // Rather than throwing an exception in the ctor, following the pattern in existing credentials
    // to log the errors, and defer throwing an exception to the first call of GetToken(). This is
    // primarily needed for credentials that are part of the DefaultAzureCredential, which this
    // credential is not intended for.
    IdentityLog::Write(
        IdentityLog::Level::Warning, credentialName + " was not initialized correctly.");
  }
}

AccessToken ClientAssertionCredentialImpl::GetToken(
    std::string const& credentialName,
    TokenRequestContext const& tokenRequestContext,
    Context const& context) const
{
  if (!m_tokenCredentialImpl)
  {
    auto const AuthUnavailable = credentialName + " authentication unavailable. ";

    IdentityLog::Write(
        IdentityLog::Level::Warning,
        AuthUnavailable + "See earlier " + credentialName + " log messages for details.");

    throw AuthenticationException(AuthUnavailable);
  }

  auto const tenantId = TenantIdResolver::Resolve(
      m_clientCredentialCore.GetTenantId(),
      tokenRequestContext,
      m_clientCredentialCore.GetAdditionallyAllowedTenants());

  auto const scopesStr
      = m_clientCredentialCore.GetScopesString(tenantId, tokenRequestContext.Scopes);

  // TokenCache::GetToken() and m_tokenCredentialImpl->GetToken() can only use the lambda
  // argument when they are being executed. They are not supposed to keep a reference to lambda
  // argument to call it later. Therefore, any capture made here will outlive the possible time
  // frame when the lambda might get called.
  return m_tokenCache.GetToken(scopesStr, tenantId, tokenRequestContext.MinimumExpiration, [&]() {
    return m_tokenCredentialImpl->GetToken(context, false, [&]() {
      auto body = m_requestBody;
      if (!scopesStr.empty())
      {
        body += "&scope=" + scopesStr;
      }

      // Get the request url before calling m_assertionCallback to validate the authority host
      // scheme (GetRequestUrl() will throw if validation fails). This is to avoid calling the
      // assertion callback if the authority host scheme is invalid.
      auto const requestUrl = m_clientCredentialCore.GetRequestUrl(tenantId);

      const std::string assertion = m_assertionCallback(context);

      body += "&client_assertion=" + Azure::Core::Url::Encode(assertion);

      auto request
          = std::make_unique<TokenCredentialImpl::TokenRequest>(HttpMethod::Post, requestUrl, body);

      request->HttpRequest.SetHeader("Host", requestUrl.GetHost());

      return request;
    });
  });
}

ClientAssertionCredential::ClientAssertionCredential(
    std::string tenantId,
    std::string clientId,
    std::function<std::string(Context const&)> assertionCallback,
    ClientAssertionCredentialOptions const& options)
    : TokenCredential("ClientAssertionCredential"),
      m_impl(std::make_unique<ClientAssertionCredentialImpl>(
          GetCredentialName(),
          tenantId,
          clientId,
          assertionCallback,
          options))
{
}

ClientAssertionCredential::~ClientAssertionCredential() = default;

AccessToken ClientAssertionCredential::GetToken(
    TokenRequestContext const& tokenRequestContext,
    Context const& context) const
{
  return m_impl->GetToken(GetCredentialName(), tokenRequestContext, context);
}

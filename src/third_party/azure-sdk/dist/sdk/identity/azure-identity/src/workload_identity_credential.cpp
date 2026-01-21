// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/identity/workload_identity_credential.hpp"

#include "private/client_assertion_credential_impl.hpp"
#include "private/identity_log.hpp"
#include "private/tenant_id_resolver.hpp"

#include <azure/core/internal/environment.hpp>

#include <fstream>
#include <streambuf>

using Azure::Identity::WorkloadIdentityCredential;

using Azure::Core::Context;
using Azure::Core::Url;
using Azure::Core::_internal::Environment;
using Azure::Core::Credentials::AccessToken;
using Azure::Core::Credentials::AuthenticationException;
using Azure::Core::Credentials::TokenRequestContext;
using Azure::Core::Http::HttpMethod;
using Azure::Identity::_detail::IdentityLog;
using Azure::Identity::_detail::TenantIdResolver;

WorkloadIdentityCredential::WorkloadIdentityCredential(
    WorkloadIdentityCredentialOptions const& options)
    : TokenCredential("WorkloadIdentityCredential")
{
  std::string tenantId = options.TenantId;
  std::string clientId = options.ClientId;
  m_tokenFilePath = options.TokenFilePath;

  if (TenantIdResolver::IsValidTenantId(tenantId) && !clientId.empty() && !m_tokenFilePath.empty())
  {
    ClientAssertionCredentialOptions clientAssertionCredentialOptions{};
    // Get the options from the base class (including ClientOptions).
    static_cast<Core::Credentials::TokenCredentialOptions&>(clientAssertionCredentialOptions)
        = options;
    clientAssertionCredentialOptions.AuthorityHost = options.AuthorityHost;
    clientAssertionCredentialOptions.AdditionallyAllowedTenants
        = options.AdditionallyAllowedTenants;

    std::function<std::string(Context const&)> callback
        = [this](Context const& context) { return GetAssertion(context); };

    // ClientAssertionCredential validates the tenant ID, client ID, and assertion callback and logs
    // warning messages otherwise.
    m_clientAssertionCredentialImpl = std::make_unique<_detail::ClientAssertionCredentialImpl>(
        GetCredentialName(), tenantId, clientId, callback, clientAssertionCredentialOptions);
  }
  else
  {
    IdentityLog::Write(
        IdentityLog::Level::Warning,
        "Azure Kubernetes environment is not set up for the " + GetCredentialName()
            + " credential to work.");
  }
}

WorkloadIdentityCredential::WorkloadIdentityCredential(
    Core::Credentials::TokenCredentialOptions const& options)
    : TokenCredential("WorkloadIdentityCredential")
{
  std::string const tenantId = _detail::DefaultOptionValues::GetTenantId();
  std::string const clientId = _detail::DefaultOptionValues::GetClientId();
  m_tokenFilePath = _detail::DefaultOptionValues::GetFederatedTokenFile();

  if (TenantIdResolver::IsValidTenantId(tenantId) && !clientId.empty() && !m_tokenFilePath.empty())
  {
    ClientAssertionCredentialOptions clientAssertionCredentialOptions{};
    // Get the options from the base class (including ClientOptions).
    static_cast<Core::Credentials::TokenCredentialOptions&>(clientAssertionCredentialOptions)
        = options;

    std::function<std::string(Context const&)> callback
        = [this](Context const& context) { return GetAssertion(context); };

    // ClientAssertionCredential validates the tenant ID, client ID, and assertion callback and logs
    // warning messages otherwise.
    m_clientAssertionCredentialImpl = std::make_unique<_detail::ClientAssertionCredentialImpl>(
        GetCredentialName(), tenantId, clientId, callback, clientAssertionCredentialOptions);
  }
  else
  {
    IdentityLog::Write(
        IdentityLog::Level::Warning,
        "Azure Kubernetes environment is not set up for the " + GetCredentialName()
            + " credential to work.");
  }
}

WorkloadIdentityCredential::~WorkloadIdentityCredential() = default;

std::string WorkloadIdentityCredential::GetAssertion(Context const&) const
{
  // Read the specified file's content, which is expected to be a Kubernetes service account
  // token. Kubernetes is responsible for updating the file as service account tokens expire.
  std::ifstream azureFederatedTokenFile(m_tokenFilePath);
  std::string assertion(
      (std::istreambuf_iterator<char>(azureFederatedTokenFile)), std::istreambuf_iterator<char>());
  return assertion;
}

AccessToken WorkloadIdentityCredential::GetToken(
    TokenRequestContext const& tokenRequestContext,
    Context const& context) const
{
  if (!m_clientAssertionCredentialImpl)
  {
    auto const AuthUnavailable = GetCredentialName() + " authentication unavailable. ";

    IdentityLog::Write(
        IdentityLog::Level::Warning,
        AuthUnavailable + "See earlier " + GetCredentialName() + " log messages for details.");

    throw AuthenticationException(
        AuthUnavailable + "Azure Kubernetes environment is not set up correctly.");
  }

  return m_clientAssertionCredentialImpl->GetToken(
      GetCredentialName(), tokenRequestContext, context);
}

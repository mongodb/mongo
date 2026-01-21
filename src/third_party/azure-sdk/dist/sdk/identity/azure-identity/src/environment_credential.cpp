// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/identity/environment_credential.hpp"

#include "azure/identity/client_certificate_credential.hpp"
#include "azure/identity/client_secret_credential.hpp"
#include "azure/identity/detail/client_credential_core.hpp"
#include "private/identity_log.hpp"

#include <azure/core/azure_assert.hpp>
#include <azure/core/internal/environment.hpp>

#include <utility>
#include <vector>

using Azure::Identity::EnvironmentCredential;
using Azure::Identity::EnvironmentCredentialOptions;

using Azure::Core::Context;
using Azure::Core::_internal::Environment;
using Azure::Core::Credentials::AccessToken;
using Azure::Core::Credentials::AuthenticationException;
using Azure::Core::Credentials::TokenCredentialOptions;
using Azure::Core::Credentials::TokenRequestContext;
using Azure::Identity::_detail::IdentityLog;

namespace {
constexpr auto AzureTenantIdEnvVarName = "AZURE_TENANT_ID";
constexpr auto AzureClientIdEnvVarName = "AZURE_CLIENT_ID";
constexpr auto AzureClientSecretEnvVarName = "AZURE_CLIENT_SECRET";
constexpr auto AzureClientCertificatePathEnvVarName = "AZURE_CLIENT_CERTIFICATE_PATH";

void PrintCredentialCreationLogMessage(
    std::string const& logMsgPrefix,
    std::vector<std::pair<char const*, char const*>> const& envVarsToParams,
    char const* credThatGetsCreated);
} // namespace

EnvironmentCredential::EnvironmentCredential(
    Core::Credentials::TokenCredentialOptions const& options,
    std::vector<std::string> const& additionallyAllowedTenants)
    : TokenCredential("EnvironmentCredential")
{
  auto tenantId = Environment::GetVariable(AzureTenantIdEnvVarName);
  auto clientId = Environment::GetVariable(AzureClientIdEnvVarName);

  auto clientSecret = Environment::GetVariable(AzureClientSecretEnvVarName);
  auto authority = Environment::GetVariable(_detail::AzureAuthorityHostEnvVarName);

  auto clientCertificatePath = Environment::GetVariable(AzureClientCertificatePathEnvVarName);

  if (!tenantId.empty() && !clientId.empty())
  {
    std::vector<std::pair<char const*, char const*>> envVarsToParams
        = {{AzureTenantIdEnvVarName, "tenantId"}, {AzureClientIdEnvVarName, "clientId"}};

    if (!clientSecret.empty())
    {
      envVarsToParams.push_back({AzureClientSecretEnvVarName, "clientSecret"});

      ClientSecretCredentialOptions clientSecretCredentialOptions;
      static_cast<TokenCredentialOptions&>(clientSecretCredentialOptions) = options;
      clientSecretCredentialOptions.AdditionallyAllowedTenants = additionallyAllowedTenants;

      if (!authority.empty())
      {
        envVarsToParams.push_back({_detail::AzureAuthorityHostEnvVarName, "authorityHost"});
        clientSecretCredentialOptions.AuthorityHost = authority;
      }

      PrintCredentialCreationLogMessage(
          GetCredentialName(), envVarsToParams, "ClientSecretCredential");

      m_credentialImpl.reset(new ClientSecretCredential(
          tenantId, clientId, clientSecret, clientSecretCredentialOptions));
    }
    else if (!clientCertificatePath.empty())
    {
      envVarsToParams.push_back({AzureClientCertificatePathEnvVarName, "clientCertificatePath"});

      ClientCertificateCredentialOptions clientCertificateCredentialOptions;
      static_cast<TokenCredentialOptions&>(clientCertificateCredentialOptions) = options;
      clientCertificateCredentialOptions.AdditionallyAllowedTenants = additionallyAllowedTenants;

      if (!authority.empty())
      {
        envVarsToParams.push_back({_detail::AzureAuthorityHostEnvVarName, "authorityHost"});
        clientCertificateCredentialOptions.AuthorityHost = authority;
      }

      PrintCredentialCreationLogMessage(
          GetCredentialName(), envVarsToParams, "ClientCertificateCredential");

      m_credentialImpl.reset(new ClientCertificateCredential(
          tenantId, clientId, clientCertificatePath, clientCertificateCredentialOptions));
    }
  }

  if (!m_credentialImpl)
  {
    IdentityLog::Write(
        IdentityLog::Level::Warning,
        GetCredentialName() + " was not initialized with underlying credential.");

    auto const logLevel = IdentityLog::Level::Verbose;
    if (IdentityLog::ShouldWrite(logLevel))
    {
      auto logMsg = GetCredentialName() + ": Both '" + AzureTenantIdEnvVarName + "' and '"
          + AzureClientIdEnvVarName + "', and at least one of '" + AzureClientSecretEnvVarName
          + "', '" + AzureClientCertificatePathEnvVarName + "' needs to be set. Additionally, '"
          + _detail::AzureAuthorityHostEnvVarName
          + "' could be set to override the default authority host. Currently:\n";

      std::pair<char const*, bool> envVarStatus[] = {
          {AzureTenantIdEnvVarName, !tenantId.empty()},
          {AzureClientIdEnvVarName, !clientId.empty()},
          {AzureClientSecretEnvVarName, !clientSecret.empty()},
          {AzureClientCertificatePathEnvVarName, !clientCertificatePath.empty()},
          {_detail::AzureAuthorityHostEnvVarName, !authority.empty()},
      };
      for (auto const& status : envVarStatus)
      {
        logMsg += std::string(" * '") + status.first + "' " + "is" + (status.second ? " " : " NOT ")
            + "set\n";
      }

      IdentityLog::Write(logLevel, logMsg);
    }
  }
}

EnvironmentCredential::EnvironmentCredential(
    Core::Credentials::TokenCredentialOptions const& options)
    : EnvironmentCredential(options, {})
{
}

EnvironmentCredential::EnvironmentCredential(EnvironmentCredentialOptions const& options)
    : EnvironmentCredential(options, options.AdditionallyAllowedTenants)
{
}

AccessToken EnvironmentCredential::GetToken(
    TokenRequestContext const& tokenRequestContext,
    Context const& context) const
{
  if (!m_credentialImpl)
  {
    auto const AuthUnavailable = GetCredentialName() + " authentication unavailable. ";

    IdentityLog::Write(
        IdentityLog::Level::Warning,
        AuthUnavailable + "See earlier " + GetCredentialName() + " log messages for details.");

    throw AuthenticationException(
        AuthUnavailable + "Environment variables are not fully configured.");
  }

  return m_credentialImpl->GetToken(tokenRequestContext, context);
}

namespace {
void PrintCredentialCreationLogMessage(
    std::string const& logMsgPrefix,
    std::vector<std::pair<char const*, char const*>> const& envVarsToParams,
    char const* credThatGetsCreated)
{
  IdentityLog::Write(
      IdentityLog::Level::Informational,
      logMsgPrefix + " gets created with " + credThatGetsCreated + '.');

  auto const logLevel = IdentityLog::Level::Verbose;
  if (!IdentityLog::ShouldWrite(logLevel))
  {
    return;
  }

  auto const envVarsToParamsSize = envVarsToParams.size();

  AZURE_ASSERT(envVarsToParamsSize > 1);

  constexpr auto Tick = "'";
  constexpr auto Comma = ", ";

  std::string const And = "and ";

  std::string envVars;
  std::string credParams;
  for (size_t i = 0; i < envVarsToParamsSize - 1;
       ++i) // not iterating over the last element for ", and".
  {
    envVars += Tick + std::string(envVarsToParams[i].first) + Tick + Comma;
    credParams += std::string(envVarsToParams[i].second) + Comma;
  }

  envVars += And + Tick + envVarsToParams.back().first + Tick;
  credParams += And + envVarsToParams.back().second;

  IdentityLog::Write(
      logLevel,
      logMsgPrefix + ": " + envVars + " environment variables are set, so " + credThatGetsCreated
          + " with corresponding " + credParams + " gets created.");
}
} // namespace

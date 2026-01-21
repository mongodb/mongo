// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/identity/azure_pipelines_credential.hpp"

#include "private/client_assertion_credential_impl.hpp"
#include "private/identity_log.hpp"
#include "private/package_version.hpp"
#include "private/tenant_id_resolver.hpp"

#include <azure/core/internal/json/json.hpp>

using Azure::Identity::AzurePipelinesCredential;
using Azure::Identity::AzurePipelinesCredentialOptions;

using Azure::Core::Context;
using Azure::Core::Url;
using Azure::Core::_internal::StringExtensions;
using Azure::Core::Credentials::AccessToken;
using Azure::Core::Credentials::AuthenticationException;
using Azure::Core::Credentials::TokenRequestContext;
using Azure::Core::Http::HttpMethod;
using Azure::Core::Http::HttpStatusCode;
using Azure::Core::Http::RawResponse;
using Azure::Core::Http::Request;
using Azure::Core::Http::_internal::HttpPipeline;
using Azure::Core::Json::_internal::json;
using Azure::Identity::_detail::IdentityLog;
using Azure::Identity::_detail::PackageVersion;
using Azure::Identity::_detail::TenantIdResolver;

AzurePipelinesCredential::AzurePipelinesCredential(
    std::string tenantId,
    std::string clientId,
    std::string serviceConnectionId,
    std::string systemAccessToken,
    AzurePipelinesCredentialOptions const& options)
    : TokenCredential("AzurePipelinesCredential"), m_serviceConnectionId(serviceConnectionId),
      m_systemAccessToken(systemAccessToken)
{
  // Allow these headers to be logged since they are used for troubleshooting.
  AzurePipelinesCredentialOptions optionsWithLoggableHeaders = options;
  optionsWithLoggableHeaders.Log.AllowedHttpHeaders.insert("x-vss-e2eid");
  optionsWithLoggableHeaders.Log.AllowedHttpHeaders.insert("x-msedge-ref");

  m_httpPipeline = std::make_unique<HttpPipeline>(
      optionsWithLoggableHeaders,
      "identity",
      PackageVersion::ToString(),
      std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>{},
      std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>{});

  m_oidcRequestUrl = _detail::DefaultOptionValues::GetOidcRequestUrl();

  if (serviceConnectionId.empty())
  {
    IdentityLog::Write(
        IdentityLog::Level::Warning,
        "No service connection ID specified for " + GetCredentialName() + ".");
  }
  if (systemAccessToken.empty())
  {
    IdentityLog::Write(
        IdentityLog::Level::Warning,
        "No system access token specified for " + GetCredentialName() + ".");
  }
  if (m_oidcRequestUrl.empty())
  {
    IdentityLog::Write(
        IdentityLog::Level::Warning,
        "No value for environment variable '" + Azure::Identity::_detail::OidcRequestUrlEnvVarName
            + "' needed by " + GetCredentialName() + ". This should be set by Azure Pipelines.");
  }

  if (TenantIdResolver::IsValidTenantId(tenantId) && !clientId.empty()
      && !serviceConnectionId.empty() && !systemAccessToken.empty() && !m_oidcRequestUrl.empty())
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
    // Rather than throwing an exception in the ctor, following the pattern in existing credentials
    // to log the errors, and defer throwing an exception to the first call of GetToken(). This is
    // primarily needed for credentials that are part of the DefaultAzureCredential, which this
    // credential is not intended for.
    IdentityLog::Write(
        IdentityLog::Level::Warning,
        "Azure Pipelines environment is not set up for the " + GetCredentialName()
            + " credential to work.");
  }
}

Request AzurePipelinesCredential::CreateOidcRequestMessage() const
{
  const std::string oidcApiVersion = "7.1";

  Url requestUrl = Url(
      m_oidcRequestUrl + "?api-version=" + Url::Encode(oidcApiVersion)
      + "&serviceConnectionId=" + Url::Encode(m_serviceConnectionId));
  Request request = Request(HttpMethod::Post, requestUrl);
  request.SetHeader("content-type", "application/json");
  request.SetHeader("authorization", "Bearer " + m_systemAccessToken);

  // Prevents the service from responding with a redirect HTTP status code (useful for automation).
  request.SetHeader("X-TFS-FedAuthRedirect", "Suppress");

  return request;
}

std::string AzurePipelinesCredential::GetOidcTokenResponse(
    std::unique_ptr<RawResponse> const& response,
    std::string responseBody) const
{
  auto const statusCode = response->GetStatusCode();
  if (statusCode != HttpStatusCode::Ok)
  {
    // Include the response because its body, if any, probably contains an error message.
    // OK responses aren't included with errors because they probably contain secrets.

    std::string message = GetCredentialName() + " : "
        + std::to_string(static_cast<std::underlying_type<decltype(statusCode)>::type>(statusCode))
        + " (" + response->GetReasonPhrase()
        + ") response from the OIDC endpoint. Check service connection ID and Pipeline "
          "configuration";

    auto responseHeaders = response->GetHeaders();
    auto headerValue = responseHeaders.find("x-vss-e2eid");
    if (headerValue != responseHeaders.end())
    {
      message += "\n" + headerValue->first + ":" + headerValue->second;
    }
    headerValue = responseHeaders.find("x-msedge-ref");
    if (headerValue != responseHeaders.end())
    {
      message += "\n" + headerValue->first + ":" + headerValue->second;
    }
    message += "\n\n" + responseBody;

    IdentityLog::Write(IdentityLog::Level::Verbose, message);

    throw AuthenticationException(message);
  }

  json parsedJson;
  try
  {
    parsedJson = Azure::Core::Json::_internal::json::parse(responseBody);
  }
  catch (json::exception const&)
  {
    std::string message = GetCredentialName() + " : Cannot parse the response string as JSON.";
    IdentityLog::Write(IdentityLog::Level::Verbose, message);

    throw AuthenticationException(message);
  }

  const std::string oidcTokenPropertyName = "oidcToken";
  if (!parsedJson.contains(oidcTokenPropertyName) || !parsedJson[oidcTokenPropertyName].is_string())
  {
    std::string message = GetCredentialName()
        + " : OIDC token not found in response. \nSee Azure::Core::Diagnostics::Logger for details "
          "(https://aka.ms/azsdk/cpp/identity/troubleshooting).";
    IdentityLog::Write(IdentityLog::Level::Verbose, message);
    throw AuthenticationException(message);
  }
  return parsedJson[oidcTokenPropertyName].get<std::string>();
}

AzurePipelinesCredential::~AzurePipelinesCredential() = default;

std::string AzurePipelinesCredential::GetAssertion(Context const& context) const
{
  Azure::Core::Http::Request oidcRequest = CreateOidcRequestMessage();
  std::unique_ptr<RawResponse> response = m_httpPipeline->Send(oidcRequest, context);

  if (!response)
  {
    throw AuthenticationException(
        GetCredentialName() + " couldn't send OIDC token request: null response.");
  }

  auto const bodyStream = response->ExtractBodyStream();
  auto const bodyVec = bodyStream ? bodyStream->ReadToEnd(context) : response->GetBody();
  auto const responseBody
      = std::string(reinterpret_cast<char const*>(bodyVec.data()), bodyVec.size());

  return GetOidcTokenResponse(response, responseBody);
}

AccessToken AzurePipelinesCredential::GetToken(
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
        AuthUnavailable + "Azure Pipelines environment is not set up correctly.");
  }

  return m_clientAssertionCredentialImpl->GetToken(
      GetCredentialName(), tokenRequestContext, context);
}

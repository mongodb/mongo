// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/credentials/credentials.hpp"
#include "azure/core/http/policies/policy.hpp"
#include "azure/core/internal/credentials/authorization_challenge_parser.hpp"

#include <chrono>

using Azure::Core::Http::Policies::_internal::BearerTokenAuthenticationPolicy;

using Azure::Core::Context;
using Azure::Core::Credentials::AuthenticationException;
using Azure::Core::Credentials::TokenRequestContext;
using Azure::Core::Credentials::_detail::AuthorizationChallengeHelper;
using Azure::Core::Http::RawResponse;
using Azure::Core::Http::Request;
using Azure::Core::Http::Policies::NextHttpPolicy;

std::unique_ptr<RawResponse> BearerTokenAuthenticationPolicy::Send(
    Request& request,
    NextHttpPolicy nextPolicy,
    Context const& context) const
{
  if (request.GetUrl().GetScheme() != "https")
  {
    throw AuthenticationException(
        "Bearer token authentication is not permitted for non TLS protected (https) endpoints.");
  }

  auto result = AuthorizeAndSendRequest(request, nextPolicy, context);
  {
    auto const& response = *result;
    auto const& challenge = AuthorizationChallengeHelper::GetChallenge(response);
    if (!challenge.empty() && AuthorizeRequestOnChallenge(challenge, request, context))
    {
      result = nextPolicy.Send(request, context);
    }
  }

  return result;
}

std::unique_ptr<RawResponse> BearerTokenAuthenticationPolicy::AuthorizeAndSendRequest(
    Request& request,
    NextHttpPolicy& nextPolicy,
    Context const& context) const
{
  AuthenticateAndAuthorizeRequest(request, m_tokenRequestContext, context);
  return nextPolicy.Send(request, context);
}

bool BearerTokenAuthenticationPolicy::AuthorizeRequestOnChallenge(
    std::string const& challenge,
    Request& request,
    Context const& context) const
{
  static_cast<void>(challenge);
  static_cast<void>(request);
  static_cast<void>(context);

  return false;
}

namespace {
bool TokenNeedsRefresh(
    Azure::Core::Credentials::AccessToken const& cachedToken,
    Azure::Core::Credentials::TokenRequestContext const& cachedTokenRequestContext,
    Azure::DateTime const& currentTime,
    Azure::Core::Credentials::TokenRequestContext const& newTokenRequestContext)
{
  return newTokenRequestContext.TenantId != cachedTokenRequestContext.TenantId
      || newTokenRequestContext.Scopes != cachedTokenRequestContext.Scopes
      || currentTime > (cachedToken.ExpiresOn - newTokenRequestContext.MinimumExpiration);
}

void ApplyBearerToken(
    Azure::Core::Http::Request& request,
    Azure::Core::Credentials::AccessToken const& token)
{
  request.SetHeader("authorization", "Bearer " + token.Token);
}
} // namespace

void BearerTokenAuthenticationPolicy::AuthenticateAndAuthorizeRequest(
    Request& request,
    TokenRequestContext const& tokenRequestContext,
    Context const& context) const
{
  DateTime const currentTime = std::chrono::system_clock::now();

  {
    std::shared_lock<std::shared_timed_mutex> readLock(m_accessTokenMutex);
    if (!TokenNeedsRefresh(m_accessToken, m_accessTokenContext, currentTime, tokenRequestContext))
    {
      ApplyBearerToken(request, m_accessToken);
      return;
    }
  }

  std::unique_lock<std::shared_timed_mutex> writeLock(m_accessTokenMutex);
  // Check if token needs refresh for the second time in case another thread has just updated it.
  if (TokenNeedsRefresh(m_accessToken, m_accessTokenContext, currentTime, tokenRequestContext))
  {
    m_accessToken = m_credential->GetToken(tokenRequestContext, context);
    m_accessTokenContext = tokenRequestContext;
  }

  ApplyBearerToken(request, m_accessToken);
}

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "private/tenant_id_resolver.hpp"

#include <azure/core/internal/environment.hpp>
#include <azure/core/internal/strings.hpp>

using Azure::Identity::_detail::TenantIdResolver;

using Azure::Core::_internal::Environment;
using Azure::Core::_internal::StringExtensions;
using Azure::Core::Credentials::AuthenticationException;
using Azure::Core::Credentials::TokenRequestContext;

namespace {
bool IsMultitenantAuthDisabled()
{
  auto const envVar = Environment::GetVariable("AZURE_IDENTITY_DISABLE_MULTITENANTAUTH");
  return envVar == "1" || StringExtensions::LocaleInvariantCaseInsensitiveEqual(envVar, "true");
}
} // namespace

std::string TenantIdResolver::Resolve(
    std::string const& explicitTenantId,
    TokenRequestContext const& tokenRequestContext,
    std::vector<std::string> const& additionallyAllowedTenants)
{
  auto const& requestedTenantId = tokenRequestContext.TenantId;

  if (requestedTenantId.empty()
      || StringExtensions::LocaleInvariantCaseInsensitiveEqual(requestedTenantId, explicitTenantId)
      || IsAdfs(explicitTenantId) || IsMultitenantAuthDisabled())
  {
    return explicitTenantId;
  }

  for (auto const& allowedTenantId : additionallyAllowedTenants)
  {
    if (allowedTenantId == "*"
        || StringExtensions::LocaleInvariantCaseInsensitiveEqual(
            allowedTenantId, requestedTenantId))
    {
      return requestedTenantId;
    }
  }

  throw AuthenticationException(
      "The current credential is not configured to acquire tokens for tenant '" + requestedTenantId
      + "'. To enable acquiring tokens for this tenant add it to the AdditionallyAllowedTenants on "
        "the credential options, or add \"*\" to AdditionallyAllowedTenants to allow acquiring "
        "tokens for any tenant.");
}

bool TenantIdResolver::IsAdfs(std::string const& tenantId)
{
  return StringExtensions::LocaleInvariantCaseInsensitiveEqual(tenantId, "adfs");
}

bool TenantIdResolver::IsValidTenantId(std::string const& tenantId)
{
  const std::string allowedChars = ".-";
  if (tenantId.empty())
  {
    return false;
  }
  for (auto const c : tenantId)
  {
    if (allowedChars.find(c) != std::string::npos)
    {
      continue;
    }
    if (!StringExtensions::IsAlphaNumeric(c))
    {
      return false;
    }
  }
  return true;
}

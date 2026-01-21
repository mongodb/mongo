// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <azure/core/credentials/credentials.hpp>

#include <string>
#include <vector>

namespace Azure { namespace Identity { namespace _detail {
  /**
   * @brief Implements an access token cache.
   *
   */
  class TenantIdResolver final {
    TenantIdResolver() = delete;
    ~TenantIdResolver() = delete;

  public:
    static std::string Resolve(
        std::string const& explicitTenantId,
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        std::vector<std::string> const& additionallyAllowedTenants);

    // ADFS is the Active Directory Federation Service, a tenant ID that is used in Azure Stack.
    static bool IsAdfs(std::string const& tenantId);

    static bool IsValidTenantId(std::string const& tenantId);
  };
}}} // namespace Azure::Identity::_detail

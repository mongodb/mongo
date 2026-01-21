// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Client Assertion Credential and options.
 */

#pragma once

#include "azure/identity/client_assertion_credential.hpp"
#include "azure/identity/detail/client_credential_core.hpp"
#include "azure/identity/detail/token_cache.hpp"
#include "token_credential_impl.hpp"

#include <string>

namespace Azure { namespace Identity { namespace _detail {
  class TokenCredentialImpl;

  class ClientAssertionCredentialImpl final {
  private:
    std::function<std::string(Core::Context const&)> m_assertionCallback;
    _detail::ClientCredentialCore m_clientCredentialCore;
    std::unique_ptr<TokenCredentialImpl> m_tokenCredentialImpl;
    std::string m_requestBody;
    _detail::TokenCache m_tokenCache;

  public:
    ClientAssertionCredentialImpl(
        std::string const& credentialName,
        std::string tenantId,
        std::string clientId,
        std::function<std::string(Core::Context const&)> assertionCallback,
        ClientAssertionCredentialOptions const& options = {});

    Core::Credentials::AccessToken GetToken(
        std::string const& credentialName,
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        Core::Context const& context) const;
  };
}}} // namespace Azure::Identity::_detail

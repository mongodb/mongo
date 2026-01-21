// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Client Assertion Credential and options.
 */

#pragma once

#include "azure/identity/detail/client_credential_core.hpp"

#include <azure/core/credentials/token_credential_options.hpp>

#include <string>
#include <vector>

namespace Azure { namespace Identity {
  namespace _detail {
    class ClientAssertionCredentialImpl;
  } // namespace _detail

  /**
   * @brief Options used to configure the Client Assertion credential.
   *
   */
  struct ClientAssertionCredentialOptions final : public Core::Credentials::TokenCredentialOptions
  {
    /**
     * @brief Authentication authority URL.
     * @note Defaults to the value of the environment variable 'AZURE_AUTHORITY_HOST'. If that's not
     * set, the default value is Microsoft Entra global authority
     * (https://login.microsoftonline.com/).
     *
     * @note Example of an authority host string: "https://login.microsoftonline.us/". See national
     * clouds' Microsoft Entra authentication endpoints:
     * https://learn.microsoft.com/entra/identity-platform/authentication-national-cloud.
     */
    std::string AuthorityHost = _detail::DefaultOptionValues::GetAuthorityHost();

    /**
     * @brief For multi-tenant applications, specifies additional tenants for which the credential
     * may acquire tokens. Add the wildcard value `"*"` to allow the credential to acquire tokens
     * for any tenant in which the application is installed.
     */
    std::vector<std::string> AdditionallyAllowedTenants;
  };

  /**
   * @brief Credential which authenticates a Microsoft Entra service principal using a signed client
   * assertion.
   *
   */
  class ClientAssertionCredential final : public Core::Credentials::TokenCredential {
  private:
    std::unique_ptr<_detail::ClientAssertionCredentialImpl> m_impl;

  public:
    /**
     * @brief Creates an instance of the Client Assertion Credential with a callback that provides a
     * signed client assertion to authenticate against Microsoft Entra ID.
     *
     * @param tenantId The Microsoft Entra tenant (directory) ID of the service principal.
     * @param clientId The client (application) ID of the service principal.
     * @param assertionCallback A callback returning a valid client assertion used to authenticate
     * the service principal.
     * @param options Options that allow to configure the management of the requests sent to
     * Microsoft Entra ID for token retrieval.
     */
    explicit ClientAssertionCredential(
        std::string tenantId,
        std::string clientId,
        std::function<std::string(Core::Context const&)> assertionCallback,
        ClientAssertionCredentialOptions const& options = {});

    /**
     * @brief Destructs `%ClientAssertionCredential`.
     *
     */
    ~ClientAssertionCredential() override;

    /**
     * @brief Obtains an authentication token from Microsoft Entra ID, by calling the
     * assertionCallback specified when constructing the credential to obtain a client assertion for
     * authentication.
     *
     * @param tokenRequestContext A context to get the token in.
     * @param context A context to control the request lifetime.
     *
     * @throw Azure::Core::Credentials::AuthenticationException Authentication error occurred.
     */
    Core::Credentials::AccessToken GetToken(
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        Core::Context const& context) const override;
  };

}} // namespace Azure::Identity

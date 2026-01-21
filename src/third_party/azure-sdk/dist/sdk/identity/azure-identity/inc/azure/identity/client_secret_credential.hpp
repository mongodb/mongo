// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Client Secret Credential and options.
 */

#pragma once

#include "azure/identity/detail/client_credential_core.hpp"
#include "azure/identity/detail/token_cache.hpp"

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/credentials/token_credential_options.hpp>
#include <azure/core/url.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Azure { namespace Identity {
  namespace _detail {
    class TokenCredentialImpl;
  } // namespace _detail

  /**
   * @brief Options for token authentication.
   *
   */
  struct ClientSecretCredentialOptions final : public Core::Credentials::TokenCredentialOptions
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
   * @brief Client Secret Credential authenticates with the Azure services using a Tenant ID, Client
   * ID and a client secret.
   *
   */
  class ClientSecretCredential final : public Core::Credentials::TokenCredential {
  private:
    _detail::TokenCache m_tokenCache;
    _detail::ClientCredentialCore m_clientCredentialCore;
    std::unique_ptr<_detail::TokenCredentialImpl> m_tokenCredentialImpl;
    std::string m_requestBody;

    ClientSecretCredential(
        std::string tenantId,
        std::string const& clientId,
        std::string const& clientSecret,
        std::string const& authorityHost,
        std::vector<std::string> additionallyAllowedTenants,
        Core::Credentials::TokenCredentialOptions const& options);

  public:
    /**
     * @brief Constructs a Client Secret Credential.
     *
     * @param tenantId Tenant ID.
     * @param clientId Client ID.
     * @param clientSecret Client secret.
     * @param options Options for token retrieval.
     */
    explicit ClientSecretCredential(
        std::string tenantId,
        std::string const& clientId,
        std::string const& clientSecret,
        ClientSecretCredentialOptions const& options);

    /**
     * @brief Constructs a Client Secret Credential.
     *
     * @param tenantId Tenant ID.
     * @param clientId Client ID.
     * @param clientSecret Client Secret.
     * @param options Options for token retrieval.
     */
    explicit ClientSecretCredential(
        std::string tenantId,
        std::string const& clientId,
        std::string const& clientSecret,
        Core::Credentials::TokenCredentialOptions const& options
        = Core::Credentials::TokenCredentialOptions());

    /**
     * @brief Destructs `%ClientSecretCredential`.
     *
     */
    ~ClientSecretCredential() override;

    /**
     * @brief Gets an authentication token.
     *
     * @param tokenRequestContext A context to get the token in.
     * @param context A context to control the request lifetime.
     *
     * @return Authentication token.
     *
     * @throw Azure::Core::Credentials::AuthenticationException Authentication error occurred.
     */
    Core::Credentials::AccessToken GetToken(
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        Core::Context const& context) const override;
  };

}} // namespace Azure::Identity

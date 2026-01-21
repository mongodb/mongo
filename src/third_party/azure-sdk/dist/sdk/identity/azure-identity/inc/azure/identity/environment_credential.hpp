// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Environment Credential initializes an Azure credential from system environment variables.
 */

#pragma once

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/credentials/token_credential_options.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Azure { namespace Identity {
  /**
   * @brief Options for token authentication.
   *
   */
  struct EnvironmentCredentialOptions final : public Core::Credentials::TokenCredentialOptions
  {
    /**
     * @brief For multi-tenant applications, specifies additional tenants for which the credential
     * may acquire tokens. Add the wildcard value `"*"` to allow the credential to acquire tokens
     * for any tenant in which the application is installed.
     */
    std::vector<std::string> AdditionallyAllowedTenants;
  };

  /**
   * @brief Environment Credential initializes an Azure credential, based on the system environment
   * variables being set.
   *
   * @note May read from the following environment variables:
   * - `AZURE_TENANT_ID`
   * - `AZURE_CLIENT_ID`
   * - `AZURE_CLIENT_SECRET`
   * - `AZURE_CLIENT_CERTIFICATE_PATH`
   * - `AZURE_CLIENT_CERTIFICATE_PASSWORD`
   * - `AZURE_CLIENT_SEND_CERTIFICATE_CHAIN`
   * - `AZURE_USERNAME`
   * - `AZURE_PASSWORD`
   * - `AZURE_AUTHORITY_HOST`
   */
  class EnvironmentCredential final : public Core::Credentials::TokenCredential {
  private:
    std::unique_ptr<TokenCredential> m_credentialImpl;

    explicit EnvironmentCredential(
        Core::Credentials::TokenCredentialOptions const& options,
        std::vector<std::string> const& additionallyAllowedTenants);

  public:
    /**
     * @brief Constructs an Environment Credential.
     * @param options Options for token retrieval.
     */
    explicit EnvironmentCredential(
        Core::Credentials::TokenCredentialOptions const& options
        = Core::Credentials::TokenCredentialOptions());

    /**
     * @brief Constructs an Environment Credential.
     * @param options Options for token retrieval.
     */
    explicit EnvironmentCredential(EnvironmentCredentialOptions const& options);

    /**
     * @brief Gets an authentication token.
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

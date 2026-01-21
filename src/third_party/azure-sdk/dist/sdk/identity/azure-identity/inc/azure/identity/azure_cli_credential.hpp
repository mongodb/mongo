// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Azure CLI Credential uses Azure CLI to obtain an access token.
 */

#pragma once

#include "azure/identity/detail/token_cache.hpp"

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/credentials/token_credential_options.hpp>
#include <azure/core/datetime.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace Azure { namespace Identity {
  /**
   * @brief Options for configuring the #Azure::Identity::AzureCliCredential.
   */
  struct AzureCliCredentialOptions final : public Core::Credentials::TokenCredentialOptions
  {
    /**
     * @brief The ID of the tenant to which the credential will authenticate by default. If not
     * specified, the credential will authenticate to any requested tenant, and will default to the
     * tenant provided to the 'az login' command.
     */
    std::string TenantId;

    /**
     * @brief The CLI process timeout.
     */
    DateTime::duration CliProcessTimeout
        = std::chrono::seconds(13); // Value was taken from .NET SDK.

    /**
     * @brief For multi-tenant applications, specifies additional tenants for which the credential
     * may acquire tokens. Add the wildcard value `"*"` to allow the credential to acquire tokens
     * for any tenant in which the application is installed.
     */
    std::vector<std::string> AdditionallyAllowedTenants;
  };

  /**
   * @brief Enables authentication to Microsoft Entra ID using Azure CLI to obtain an access
   * token.
   */
  class AzureCliCredential
#if !defined(_azure_TESTING_BUILD)
      final
#endif
      : public Core::Credentials::TokenCredential {
  protected:
    /** @brief The cache for the access token. */
    _detail::TokenCache m_tokenCache;

    /** @brief Additional tenants which will be allowed for this credential. */
    std::vector<std::string> m_additionallyAllowedTenants;

    /** @brief The ID of the tenant to which the credential will authenticate by default. */
    std::string m_tenantId;

    /** @brief The CLI process timeout. */
    DateTime::duration m_cliProcessTimeout;

  private:
    explicit AzureCliCredential(
        Core::Credentials::TokenCredentialOptions const& options,
        std::string tenantId,
        DateTime::duration cliProcessTimeout,
        std::vector<std::string> additionallyAllowedTenants);

    void ThrowIfNotSafeCmdLineInput(
        std::string const& input,
        std::string const& allowedChars,
        std::string const& description) const;

  public:
    /**
     * @brief Constructs an Azure CLI Credential.
     *
     * @param options Options for token retrieval.
     */
    explicit AzureCliCredential(AzureCliCredentialOptions const& options = {});

    /**
     * @brief Constructs an Azure CLI Credential.
     *
     * @param options Options for token retrieval.
     */
    explicit AzureCliCredential(Core::Credentials::TokenCredentialOptions const& options);

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

#if !defined(_azure_TESTING_BUILD)
  private:
#else
  protected:
#endif
    virtual std::string GetAzCommand(std::string const& scopes, std::string const& tenantId) const;
    virtual int GetLocalTimeToUtcDiffSeconds() const;
  };

}} // namespace Azure::Identity

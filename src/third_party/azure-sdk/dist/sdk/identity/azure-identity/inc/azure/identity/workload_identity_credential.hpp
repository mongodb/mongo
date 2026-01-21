// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Workload Identity Credential and options.
 */

#pragma once

#include "azure/identity/client_assertion_credential.hpp"
#include "azure/identity/detail/client_credential_core.hpp"
#include "azure/identity/detail/token_cache.hpp"

#include <azure/core/credentials/token_credential_options.hpp>

#include <string>
#include <vector>

namespace Azure { namespace Identity {
  namespace _detail {
    class ClientAssertionCredentialImpl;
  } // namespace _detail

  /**
   * @brief Options for workload identity credential.
   *
   */
  struct WorkloadIdentityCredentialOptions final : public Core::Credentials::TokenCredentialOptions
  {
    /**
     * @brief The TenantID of the service principal. Defaults to the value of the environment
     * variable AZURE_TENANT_ID.
     */
    std::string TenantId = _detail::DefaultOptionValues::GetTenantId();

    /**
     * @brief The ClientID of the service principal. Defaults to the value of the environment
     * variable AZURE_CLIENT_ID.
     */
    std::string ClientId = _detail::DefaultOptionValues::GetClientId();

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
     * @brief The path of a file containing a Kubernetes service account token. Defaults to the
     * value of the environment variable AZURE_FEDERATED_TOKEN_FILE.
     */
    std::string TokenFilePath = _detail::DefaultOptionValues::GetFederatedTokenFile();

    /**
     * @brief For multi-tenant applications, specifies additional tenants for which the credential
     * may acquire tokens. Add the wildcard value `"*"` to allow the credential to acquire tokens
     * for any tenant in which the application is installed.
     */
    std::vector<std::string> AdditionallyAllowedTenants;
  };

  /**
   * @brief Workload Identity Credential supports Azure workload identity authentication on
   * Kubernetes and other hosts supporting workload identity. See the Azure Kubernetes Service
   * documentation at https://learn.microsoft.com/azure/aks/workload-identity-overview for more
   * information.
   *
   */
  class WorkloadIdentityCredential final : public Core::Credentials::TokenCredential {
  private:
    std::unique_ptr<_detail::ClientAssertionCredentialImpl> m_clientAssertionCredentialImpl;
    std::string m_tokenFilePath;

    std::string GetAssertion(Core::Context const& context) const;

  public:
    /**
     * @brief Constructs a Workload Identity Credential.
     *
     * @param options Options for token retrieval.
     */
    explicit WorkloadIdentityCredential(
        Core::Credentials::TokenCredentialOptions const& options
        = Core::Credentials::TokenCredentialOptions());

    /**
     * @brief Constructs a Workload Identity Credential.
     *
     * @param options Options for token retrieval.
     */
    explicit WorkloadIdentityCredential(WorkloadIdentityCredentialOptions const& options);

    /**
     * @brief Destructs `%WorkloadIdentityCredential`.
     *
     */
    ~WorkloadIdentityCredential() override;

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

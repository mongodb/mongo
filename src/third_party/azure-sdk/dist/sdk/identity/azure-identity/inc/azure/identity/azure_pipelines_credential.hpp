// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Azure Pipelines Credential and options.
 */

#pragma once

#include "azure/identity/client_assertion_credential.hpp"
#include "azure/identity/detail/client_credential_core.hpp"
#include "azure/identity/detail/token_cache.hpp"

#include <azure/core/credentials/token_credential_options.hpp>
#include <azure/core/http/http.hpp>
#include <azure/core/internal/http/pipeline.hpp>

#include <string>
#include <vector>

namespace Azure { namespace Identity {
  namespace _detail {
    class ClientAssertionCredentialImpl;
  } // namespace _detail

  /**
   * @brief Options for Azure Pipelines credential.
   *
   */
  struct AzurePipelinesCredentialOptions final : public Core::Credentials::TokenCredentialOptions
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
   * @brief Credential which authenticates using an Azure Pipelines service connection.
   *
   */
  class AzurePipelinesCredential final : public Core::Credentials::TokenCredential {
  private:
    std::string m_serviceConnectionId;
    std::string m_systemAccessToken;
    std::unique_ptr<Azure::Core::Http::_internal::HttpPipeline> m_httpPipeline;
    std::string m_oidcRequestUrl;
    std::unique_ptr<_detail::ClientAssertionCredentialImpl> m_clientAssertionCredentialImpl;

    std::string GetAssertion(Core::Context const& context) const;
    Azure::Core::Http::Request CreateOidcRequestMessage() const;
    std::string GetOidcTokenResponse(
        std::unique_ptr<Azure::Core::Http::RawResponse> const& response,
        std::string responseBody) const;

  public:
    /**
     * @brief Constructs an Azure Pipelines Credential.
     *
     * @param tenantId The tenant ID for the service connection.
     * @param clientId The client ID for the service connection.
     * @param serviceConnectionId The service connection ID for the service connection associated
     * with the pipeline.
     * @param systemAccessToken The pipeline's System.AccessToken value. See
     * https://learn.microsoft.com/azure/devops/pipelines/build/variables?view=azure-devops%26tabs=yaml#systemaccesstoken
     * for more details.
     * @param options Options for token retrieval.
     */
    explicit AzurePipelinesCredential(
        std::string tenantId,
        std::string clientId,
        std::string serviceConnectionId,
        std::string systemAccessToken,
        AzurePipelinesCredentialOptions const& options = {});

    /**
     * @brief Destructs `%AzurePipelinesCredential`.
     *
     */
    ~AzurePipelinesCredential() override;

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

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Client Certificate Credential and options.
 */

#pragma once

#include "azure/identity/detail/client_credential_core.hpp"
#include "azure/identity/detail/token_cache.hpp"

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/credentials/token_credential_options.hpp>
#include <azure/core/internal/unique_handle.hpp>
#include <azure/core/url.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Azure { namespace Identity {
  namespace _detail {
    class TokenCredentialImpl;

    void FreePrivateKeyImpl(void* pkey);

    template <typename> struct UniquePrivateKeyHelper;
    template <> struct UniquePrivateKeyHelper<void*>
    {
      static void FreePrivateKey(void* pkey) { FreePrivateKeyImpl(pkey); }
      using type = Azure::Core::_internal::BasicUniqueHandle<void, FreePrivateKey>;
    };

    using UniquePrivateKey = Azure::Core::_internal::UniqueHandle<void*, UniquePrivateKeyHelper>;
  } // namespace _detail

  /**
   * @brief Options for client certificate authentication.
   *
   */
  struct ClientCertificateCredentialOptions final : public Core::Credentials::TokenCredentialOptions
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

    /**
     * @brief SendCertificateChain controls whether the credential sends the public certificate
     * chain in the x5c header of each token request's JWT. This is required for Subject Name/Issuer
     * (SNI) authentication.
     *
     * @note Defaults to false.
     *
     */
    bool SendCertificateChain = false;
  };

  /**
   * @brief Client Certificate Credential authenticates with the Azure services using a Tenant ID,
   * Client ID and a client certificate.
   *
   */
  class ClientCertificateCredential final : public Core::Credentials::TokenCredential {
  private:
    _detail::TokenCache m_tokenCache;
    _detail::ClientCredentialCore m_clientCredentialCore;
    std::unique_ptr<_detail::TokenCredentialImpl> m_tokenCredentialImpl;
    std::string m_requestBody;
    std::string m_tokenPayloadStaticPart;
    std::string m_tokenHeaderEncoded;
    _detail::UniquePrivateKey m_pkey;

    explicit ClientCertificateCredential(
        std::string tenantId,
        std::string const& clientId,
        std::string const& clientCertificatePath,
        std::string const& authorityHost,
        std::vector<std::string> additionallyAllowedTenants,
        bool sendCertificateChain,
        Core::Credentials::TokenCredentialOptions const& options);

    explicit ClientCertificateCredential(
        std::string tenantId,
        std::string const& clientId,
        std::string const& clientCertificate,
        std::string const& privateKey,
        std::string const& authorityHost,
        std::vector<std::string> additionallyAllowedTenants,
        bool sendCertificateChain,
        Core::Credentials::TokenCredentialOptions const& options);

  public:
    /**
     * @brief Constructs a Client Certificate Credential.
     *
     * @param tenantId Tenant ID.
     * @param clientId Client ID.
     * @param clientCertificatePath The path to a PEM file containing exactly one certificate which
     * is used for signing along with its corresponding private key.
     * @param options Options for token retrieval.
     */
    explicit ClientCertificateCredential(
        std::string tenantId,
        std::string const& clientId,
        std::string const& clientCertificatePath,
        Core::Credentials::TokenCredentialOptions const& options
        = Core::Credentials::TokenCredentialOptions());

    /**
     * @brief Constructs a Client Certificate Credential.
     *
     * @param tenantId Tenant ID.
     * @param clientId Client ID.
     * @param clientCertificate The PEM encoded x509 certificate which is used for signing, in
     * base64 string format, including the begin and end headers.
     * @param privateKey The PEM encoded representation of the corresponding
     * RSA private key of the certificate.
     * @param options Options for token retrieval.
     */
    explicit ClientCertificateCredential(
        std::string tenantId,
        std::string const& clientId,
        std::string const& clientCertificate,
        std::string const& privateKey,
        ClientCertificateCredentialOptions const& options = {});

    /**
     * @brief Constructs a Client Certificate Credential.
     *
     * @param tenantId Tenant ID.
     * @param clientId Client ID.
     * @param clientCertificatePath The path to a PEM file containing exactly one certificate which
     * is used for signing along with its corresponding private key.
     * @param options Options for token retrieval.
     */
    explicit ClientCertificateCredential(
        std::string tenantId,
        std::string const& clientId,
        std::string const& clientCertificatePath,
        ClientCertificateCredentialOptions const& options);

    /**
     * @brief Destructs `%ClientCertificateCredential`.
     *
     */
    ~ClientCertificateCredential() override;

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

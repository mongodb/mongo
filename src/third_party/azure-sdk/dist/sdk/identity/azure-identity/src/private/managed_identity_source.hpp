// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/identity/detail/token_cache.hpp"
#include "token_credential_impl.hpp"

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/credentials/token_credential_options.hpp>
#include <azure/core/url.hpp>

#include <memory>
#include <string>
#include <utility>

namespace Azure { namespace Identity { namespace _detail {
  class ManagedIdentitySource : protected TokenCredentialImpl {
  private:
    std::string m_clientId;
    std::string m_authorityHost;

  public:
    virtual Core::Credentials::AccessToken GetToken(
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        Core::Context const& context) const = 0;

  protected:
    _detail::TokenCache m_tokenCache;

    static Core::Url ParseEndpointUrl(
        std::string const& credName,
        std::string const& url,
        char const* envVarName,
        std::string const& credSource);

    explicit ManagedIdentitySource(
        std::string clientId,
        std::string authorityHost,
        Core::Credentials::TokenCredentialOptions const& options)
        : TokenCredentialImpl(options), m_clientId(std::move(clientId)),
          m_authorityHost(std::move(authorityHost))
    {
    }

    std::string const& GetClientId() const { return m_clientId; }
    std::string const& GetAuthorityHost() const { return m_authorityHost; }
  };

  class AppServiceManagedIdentitySource : public ManagedIdentitySource {
  private:
    Core::Http::Request m_request;

  protected:
    explicit AppServiceManagedIdentitySource(
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Credentials::TokenCredentialOptions const& options,
        Core::Url endpointUrl,
        std::string const& secret,
        std::string const& apiVersion,
        std::string const& secretHeaderName,
        std::string const& clientIdHeaderName);

    template <typename T>
    static std::unique_ptr<ManagedIdentitySource> Create(
        std::string const& credName,
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Credentials::TokenCredentialOptions const& options,
        char const* endpointVarName,
        char const* secretVarName,
        char const* appServiceVersion);

  public:
    Core::Credentials::AccessToken GetToken(
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        Core::Context const& context) const override final;
  };

  class AppServiceV2017ManagedIdentitySource final : public AppServiceManagedIdentitySource {
    friend class AppServiceManagedIdentitySource;

  private:
    explicit AppServiceV2017ManagedIdentitySource(
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Credentials::TokenCredentialOptions const& options,
        Core::Url endpointUrl,
        std::string const& secret)
        : AppServiceManagedIdentitySource(
            clientId,
            objectId,
            resourceId,
            options,
            endpointUrl,
            secret,
            "2017-09-01",
            "secret",
            "clientid")
    {
    }

  public:
    static std::unique_ptr<ManagedIdentitySource> Create(
        std::string const& credName,
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Credentials::TokenCredentialOptions const& options);
  };

  class AppServiceV2019ManagedIdentitySource final : public AppServiceManagedIdentitySource {
    friend class AppServiceManagedIdentitySource;

  private:
    explicit AppServiceV2019ManagedIdentitySource(
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Credentials::TokenCredentialOptions const& options,
        Core::Url endpointUrl,
        std::string const& secret)
        : AppServiceManagedIdentitySource(
            clientId,
            objectId,
            resourceId,
            options,
            endpointUrl,
            secret,
            "2019-08-01",
            "X-IDENTITY-HEADER",
            "client_id")
    {
    }

  public:
    static std::unique_ptr<ManagedIdentitySource> Create(
        std::string const& credName,
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Credentials::TokenCredentialOptions const& options);
  };

  class CloudShellManagedIdentitySource final : public ManagedIdentitySource {
  private:
    Core::Url m_url;

    explicit CloudShellManagedIdentitySource(
        std::string const& clientId,
        Core::Credentials::TokenCredentialOptions const& options,
        Core::Url endpointUrl);

  public:
    static std::unique_ptr<ManagedIdentitySource> Create(
        std::string const& credName,
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Credentials::TokenCredentialOptions const& options);

    Core::Credentials::AccessToken GetToken(
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        Core::Context const& context) const override;
  };

  class AzureArcManagedIdentitySource final : public ManagedIdentitySource {
  private:
    Core::Url m_url;

    explicit AzureArcManagedIdentitySource(
        Core::Credentials::TokenCredentialOptions const& options,
        Core::Url endpointUrl);

  public:
    static std::unique_ptr<ManagedIdentitySource> Create(
        std::string const& credName,
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Credentials::TokenCredentialOptions const& options);

    Core::Credentials::AccessToken GetToken(
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        Core::Context const& context) const override;
  };

  class ImdsManagedIdentitySource final : public ManagedIdentitySource {
  private:
    Core::Http::Request m_request;

    explicit ImdsManagedIdentitySource(
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Url const& imdsUrl,
        Core::Credentials::TokenCredentialOptions const& options);

  public:
    static std::unique_ptr<ManagedIdentitySource> Create(
        std::string const& credName,
        std::string const& clientId,
        std::string const& objectId,
        std::string const& resourceId,
        Core::Credentials::TokenCredentialOptions const& options);

    Core::Credentials::AccessToken GetToken(
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        Core::Context const& context) const override;
  };
}}} // namespace Azure::Identity::_detail

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Default Azure Credential.
 */

#pragma once

#include <azure/core/credentials/credentials.hpp>
#include <azure/core/credentials/token_credential_options.hpp>

#include <memory>

#if defined(_azure_TESTING_BUILD)
class DefaultAzureCredential_CachingCredential_Test;
#endif

namespace Azure { namespace Identity {
  namespace _detail {
    class ChainedTokenCredentialImpl;
  }

  /**
   * @brief Default Azure Credential combines multiple credentials that depend on the setup
   * environment and require no parameters into a single chain. If the environment is set up
   * sufficiently for at least one of such credentials to work, `DefaultAzureCredential` will work
   * as well.
   *
   * @details This credential is using several credentials in the following order:
   * #Azure::Identity::EnvironmentCredential, #Azure::Identity::WorkloadIdentityCredential,
   * #Azure::Identity::AzureCliCredential, and #Azure::Identity::ManagedIdentityCredential. Even
   * though the credentials being used and their order is documented, it may be changed in the
   * future versions of the SDK, potentially introducing breaking changes in its behavior.
   *
   * @note This credential is intended to be used at the early stages of development, to allow the
   * developer some time to work with the other aspects of the SDK, and later to replace this
   * credential with the exact credential that is the best fit for the application. It is not
   * intended to be used in a production environment.
   *
   */
  class DefaultAzureCredential final : public Core::Credentials::TokenCredential {

#if defined(_azure_TESTING_BUILD)
    //  make tests classes friends to validate caching
    friend class ::DefaultAzureCredential_CachingCredential_Test;
#endif

  public:
    /**
     * @brief Constructs `%DefaultAzureCredential`.
     *
     */
    explicit DefaultAzureCredential()
        : DefaultAzureCredential(Core::Credentials::TokenCredentialOptions{}){};

    /**
     * @brief Constructs `%DefaultAzureCredential`.
     *
     * @param options Generic Token Credential Options.
     */
    explicit DefaultAzureCredential(Core::Credentials::TokenCredentialOptions const& options);

    /**
     * @brief Destructs `%DefaultAzureCredential`.
     *
     */
    ~DefaultAzureCredential() override;

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

  private:
    std::unique_ptr<_detail::ChainedTokenCredentialImpl> m_impl;
  };

}} // namespace Azure::Identity

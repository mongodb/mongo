// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Chained Token Credential.
 */

#pragma once

#include <azure/core/credentials/credentials.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Azure { namespace Identity {
  namespace _detail {
    class ChainedTokenCredentialImpl;
  }

  /**
   * @brief Chained Token Credential provides a token credential implementation which chains
   * multiple Azure::Core::Credentials::TokenCredential implementations to be tried in order until
   * one of the GetToken() methods returns an access token.
   *
   */
  class ChainedTokenCredential final : public Core::Credentials::TokenCredential {
  public:
    /**
     * @brief A container type to store the ordered chain of credentials.
     *
     */
    using Sources = std::vector<std::shared_ptr<Core::Credentials::TokenCredential>>;

    /**
     * @brief Constructs a Chained Token Credential.
     *
     * @param sources The ordered chain of Azure::Core::Credentials::TokenCredential implementations
     * to try when calling GetToken().
     */
    explicit ChainedTokenCredential(Sources sources);

    /**
     * @brief Destructs `%ChainedTokenCredential`.
     *
     */
    ~ChainedTokenCredential() override;

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

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/identity/chained_token_credential.hpp"

#include <atomic>
#include <limits>
#include <mutex>

#if defined(_azure_TESTING_BUILD)
class DefaultAzureCredential_CachingCredential_Test;
#endif

namespace Azure { namespace Identity { namespace _detail {

  class ChainedTokenCredentialImpl final {

#if defined(_azure_TESTING_BUILD)
    //  make tests classes friends to validate caching
    friend class ::DefaultAzureCredential_CachingCredential_Test;
#endif

  public:
    ChainedTokenCredentialImpl(
        std::string const& credentialName,
        ChainedTokenCredential::Sources&& sources,
        bool reuseSuccessfulSource = false);

    Core::Credentials::AccessToken GetToken(
        std::string const& credentialName,
        Core::Credentials::TokenRequestContext const& tokenRequestContext,
        Core::Context const& context) const;

  private:
    ChainedTokenCredential::Sources m_sources;
    mutable std::mutex m_sourcesMutex;
    // Used as a sentinel value to indicate that the index of the source being used for future calls
    // hasn't been found yet.
    constexpr static std::size_t SuccessfulSourceNotSet = (std::numeric_limits<std::size_t>::max)();
    // This needs to be atomic so that sentinel comparison is thread safe.
    mutable std::atomic<std::size_t> m_successfulSourceIndex = {SuccessfulSourceNotSet};
    bool m_reuseSuccessfulSource;
  };

}}} // namespace Azure::Identity::_detail

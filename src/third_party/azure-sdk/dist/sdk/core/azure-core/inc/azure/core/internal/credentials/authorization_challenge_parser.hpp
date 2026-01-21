// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Parser for challenge-based authentication policy.
 */

#pragma once

#include "azure/core/http/raw_response.hpp"

#include <string>

namespace Azure { namespace Core { namespace Credentials {
  namespace _detail {
    class AuthorizationChallengeHelper final {
    public:
      static std::string const& GetChallenge(Http::RawResponse const& response);
    };
  } // namespace _detail

  namespace _internal {
    class AuthorizationChallengeParser final {
    private:
      AuthorizationChallengeParser() = delete;
      ~AuthorizationChallengeParser() = delete;

    public:
      /**
       * @brief Gets challenge parameter from authentication challenge.
       *
       * @param challenge Authentication challenge.
       * @param challengeScheme The challenge scheme containing the \p challengeParameter.
       * @param challengeParameter The parameter key name containing the value to return.
       *
       * @return Challenge parameter value.
       */
      static std::string GetChallengeParameter(
          std::string const& challenge,
          std::string const& challengeScheme,
          std::string const& challengeParameter);
    };
  } // namespace _internal
}}} // namespace Azure::Core::Credentials

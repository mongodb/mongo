// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Declaration of the UserAgentGenerator type.
 */

#pragma once

#include <string>

namespace Azure { namespace Core { namespace Http { namespace _detail {
  class UserAgentGenerator {
  public:
    static std::string GenerateUserAgent(
        std::string const& componentName,
        std::string const& componentVersion,
        std::string const& applicationId,
        long cplusplusValue);
  };
}}}} // namespace Azure::Core::Http::_detail

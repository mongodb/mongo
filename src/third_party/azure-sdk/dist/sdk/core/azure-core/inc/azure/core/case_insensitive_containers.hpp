// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief A `map<string, string>` with case-insensitive key comparison.
 */

#pragma once

#include "azure/core/internal/strings.hpp"

#include <map>
#include <set>
#include <string>

namespace Azure { namespace Core {

  /**
   * @brief A type alias of `std::map<std::string, std::string>` with case-insensitive key
   * comparison.
   */
  using CaseInsensitiveMap
      = std::map<std::string, std::string, _internal::StringExtensions::CaseInsensitiveComparator>;

  /**
   * @brief A type alias of `std::set<std::string>` with case-insensitive element comparison.
   *
   */
  using CaseInsensitiveSet
      = std::set<std::string, _internal::StringExtensions::CaseInsensitiveComparator>;

}} // namespace Azure::Core

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Define MatchConditions
 */

#pragma once

#include "azure/core/etag.hpp"

#include <string>

namespace Azure {

/**
 * @brief Specifies HTTP options for conditional requests.
 *
 */
struct MatchConditions
{
  /**
   * @brief Destructs `%MatchConditions`.
   *
   */
  virtual ~MatchConditions() = default;

  /**
   * @brief Optionally limit requests to resources that match the value specified.
   *
   */
  ETag IfMatch;

  /**
   * @brief Optionally limit requests to resources that do not match the value specified. Specify
   * ETag::Any() to limit requests to resources that do not exist.
   */
  ETag IfNoneMatch;
};
} // namespace Azure

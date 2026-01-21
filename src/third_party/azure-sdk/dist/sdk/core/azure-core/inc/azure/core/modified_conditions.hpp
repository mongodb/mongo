// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Define ModifiedConditions
 */

#pragma once

#include "azure/core/datetime.hpp"
#include "azure/core/nullable.hpp"

#include <string>

namespace Azure {

/**
 * @brief Specifies HTTP options for conditional requests based on modification time.
 *
 */
struct ModifiedConditions
{
  /**
   * @brief Destructs `%ModifiedConditions`.
   *
   */
  virtual ~ModifiedConditions() = default;

  /**
   * @brief Optionally limit requests to resources that have only been modified since this point
   * in time.
   */
  Azure::Nullable<Azure::DateTime> IfModifiedSince;

  /**
   * @brief Optionally limit requests to resources that have remained unmodified.
   *
   */
  Azure::Nullable<Azure::DateTime> IfUnmodifiedSince;
};
} // namespace Azure

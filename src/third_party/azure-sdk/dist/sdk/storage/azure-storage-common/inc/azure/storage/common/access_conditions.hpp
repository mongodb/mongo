// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/common/storage_common.hpp"

#include <azure/core/etag.hpp>
#include <azure/core/nullable.hpp>

#include <string>

namespace Azure { namespace Storage {

  /**
   * @brief Specifies HTTP options for conditional requests based on lease.
   */
  struct LeaseAccessConditions
  {
    /**
     * @brief Destructor.
     *
     */
    virtual ~LeaseAccessConditions() = default;

    /**
     * @brief Specify this header to perform the operation only if the resource has an
     * active lease mathing this ID.
     */
    Azure::Nullable<std::string> LeaseId;
  };

  /**
   * @brief Specifies HTTP options for conditional requests based on ContentHash.
   */
  struct ContentHashAccessConditions final
  {
    /**
     * @brief Specify this header to perform the operation only if the resource's ContentHash
     * matches the value specified.
     */
    Azure::Nullable<ContentHash> IfMatchContentHash;

    /**
     * @brief Specify this header to perform the operation only if the resource's ContentHash does
     * not match the value specified.
     */
    Azure::Nullable<ContentHash> IfNoneMatchContentHash;
  };

}} // namespace Azure::Storage

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief An Azure Resource Manager resource identifier.
 */

#pragma once

#include <string>

namespace Azure { namespace Core {

  /**
   * @brief An Azure Resource Manager resource identifier.
   */
  class ResourceIdentifier final {
    std::string m_resourceId;

  public:
    /**
     * @brief Constructs a resource identifier.
     *
     * @param resourceId The id string to create the ResourceIdentifier from.
     */
    explicit ResourceIdentifier(std::string const& resourceId) : m_resourceId(resourceId){};

    /**
     * @brief The string representation of this resource identifier.
     *
     * @return The resource identifier string.
     */
    std::string ToString() const { return m_resourceId; }
  };

}} // namespace Azure::Core

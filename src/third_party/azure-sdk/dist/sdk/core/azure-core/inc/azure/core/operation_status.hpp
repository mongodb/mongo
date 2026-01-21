// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Valid states for long-running Operations. Services can extend upon the default set of
 * values.
 */

#pragma once

#include "azure/core/dll_import_export.hpp"
#include "azure/core/internal/strings.hpp"

#include <string>
#include <utility> // for std::move

namespace Azure { namespace Core {

  /**
   * @brief Long-running operation states.
   */
  class OperationStatus final {
    std::string m_value;

  public:
    /**
     * @brief Constructs an `%OperationStatus` with \p value.
     *
     * @param value The value to initialize with.
     */
    explicit OperationStatus(const std::string& value) : m_value(value) {}

    /**
     * @brief Constructs an `%OperationStatus` with \p value.
     *
     * @param value The value to initialize with.
     */
    explicit OperationStatus(std::string&& value) : m_value(std::move(value)) {}

    /**
     * @brief Constructs an `%OperationStatus` with \p value.
     *
     * @param value The value to initialize with.
     */
    explicit OperationStatus(const char* value) : m_value(value) {}

    /**
     * @brief Compares two `%OperationStatus` objects for equality.
     *
     * @param other An `%OperationStatus` to compare with.
     *
     * @return `true` if the states have the same string representation; otherwise,`false`.
     */
    bool operator==(const OperationStatus& other) const noexcept
    {
      return _internal::StringExtensions::LocaleInvariantCaseInsensitiveEqual(
          m_value, other.m_value);
    }

    /**
     * @brief Compares two `%OperationStatus` objects for equality.
     *
     * @param other A `%OperationStatus` to compare with.
     *
     * @return `false` if the states have the same string representation; otherwise, `true`.
     */
    bool operator!=(const OperationStatus& other) const noexcept { return !(*this == other); }

    /**
     * @brief Gets the `std::string` representation of the operation status.
     *
     */
    const std::string& Get() const noexcept { return m_value; }

    /**
     * @brief The #Azure::Core::Operation is Not Started.
     *
     */
    AZ_CORE_DLLEXPORT static const OperationStatus NotStarted;
    /**
     * @brief The #Azure::Core::Operation is Running.
     *
     */
    AZ_CORE_DLLEXPORT static const OperationStatus Running;
    /**
     * @brief The #Azure::Core::Operation Succeeded.
     *
     */
    AZ_CORE_DLLEXPORT static const OperationStatus Succeeded;
    /**
     * @brief The #Azure::Core::Operation was Cancelled.
     *
     */
    AZ_CORE_DLLEXPORT static const OperationStatus Cancelled;
    /**
     * @brief The #Azure::Core::Operation Failed.
     *
     */
    AZ_CORE_DLLEXPORT static const OperationStatus Failed;
  };

}} // namespace Azure::Core

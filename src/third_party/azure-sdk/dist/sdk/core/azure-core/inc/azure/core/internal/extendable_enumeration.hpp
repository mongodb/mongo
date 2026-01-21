// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

/**
 * @file
 * @brief Internal utility functions for extendable enumerations.
 *
 */
#pragma once

#include <string>

namespace Azure { namespace Core { namespace _internal {
  /** @brief Template base class helper for implementing extendable enumerations.
   *
   * This template exists to simplify the experience of authoring ["extendable
   * enumerations"](https://azure.github.io/azure-sdk/cpp_implementation.html#cpp-enums).
   *
   * An extendable enumeration derives publicly from the #ExtendableEnumeration base class passing
   * in the extendable enumeration type as the template argument.
   *
   * Example:
   *
   * \code{.cpp}
   * class MyEnumeration final : public ExtendableEnumeration<MyEnumeration> {
   * public:
   *   explicit MyEnumeration(std::string value) :
   *     ExtendableEnumeration(std::move(value)) {}
   *   MyEnumeration() = default;
   *   static const MyEnumeration Enumerator1;
   *   static const MyEnumeration Enumerator2;
   *   static const MyEnumeration Enumerator3;
   * };
   * \endcode
   *
   */
  template <class T> class ExtendableEnumeration {
  private:
    std::string m_enumerationValue;

  protected:
    ~ExtendableEnumeration() = default;

  public:
    /**
     * @brief Construct a new extensable enumeration object
     *
     * @param enumerationValue The string enumerationValue used for the value.
     */
    explicit ExtendableEnumeration(std::string enumerationValue)
        : m_enumerationValue(std::move(enumerationValue))
    {
    }

    /**
     * @brief Construct a default extendable enumeration.
     */
    ExtendableEnumeration() = default;

    /**
     * @brief Enable comparing the ext enum.
     *
     * @param other Another extendable enumeration to be compared.
     */
    bool operator==(ExtendableEnumeration<T> const& other) const noexcept
    {
      return m_enumerationValue == other.m_enumerationValue;
    }

    /**
     * @brief Enable comparing the ext enum.
     *
     * @param other Another extendable enumeration to be compared.
     */
    bool operator!=(ExtendableEnumeration<T> const& other) const noexcept
    {
      return !operator==(other);
    }

    /**
     * @brief Return the ExtendableEnumeration string representation.
     *
     */
    std::string const& ToString() const { return m_enumerationValue; }
  };
}}} // namespace Azure::Core::_internal

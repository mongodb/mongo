// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Universally unique identifier.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace Azure { namespace Core {
  /**
   * @brief Universally unique identifier.
   */
  class Uuid final {
  public:
    /**
     * @brief Represents a byte array where the UUID value can be stored.
     *
     */
    using ValueArray = std::array<std::uint8_t, 16>;

  private:
    ValueArray m_uuid{};

  private:
    constexpr Uuid(ValueArray const& uuid) : m_uuid(uuid) {}

  public:
    /**
     * @brief Constructs a Nil UUID (`00000000-0000-0000-0000-000000000000`).
     *
     */
    // Nil UUID, per RFC9562, consists of all zeros:
    // https://www.rfc-editor.org/rfc/rfc9562.html#name-nil-uuid
    constexpr explicit Uuid() : m_uuid{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} {}

    /**
     * @brief Gets Uuid as a string.
     * @details A string is in canonical format (`8-4-4-4-12` lowercase hex and dashes only).
     */
    std::string ToString() const;

    /**
     * @brief Returns the binary value of the Uuid for consumption by clients who need non-string
     * representation of the Uuid.
     * @returns An array with the binary representation of the Uuid.
     */
    constexpr ValueArray const& AsArray() const { return m_uuid; }

    /**
     * @brief Creates a new random UUID.
     *
     */
    static Uuid CreateUuid();

    /**
     * @brief Construct a Uuid from an existing UUID represented as an array of bytes.
     * @details Creates a Uuid from a UUID created in an external scope.
     */
    static constexpr Uuid CreateFromArray(ValueArray const& uuid) { return Uuid{uuid}; }

    /**
     * @brief Construct a Uuid by parsing its representation.
     * @param uuidString a string in `8-4-4-4-12` hex characters format.
     * @throw `std::invalid_argument` if \p uuidString cannot be parsed.
     */
    static Uuid Parse(std::string const& uuidString);

    /**
     * @brief Compares with another instance of Uuid for equality.
     * @param other another instance of Uuid.
     * @return `true` if values of two Uuids are equal, `false` otherwise.
     *
     */
    constexpr bool operator==(Uuid const& other) const
    {
      if (this != &other)
      {
        // std::array::operator==() is not a constexpr until C++20
        for (size_t i = 0; i < m_uuid.size(); ++i)
        {
          if (m_uuid[i] != other.m_uuid[i])
          {
            return false;
          }
        }
      }

      return true;
    }

    /**
     * @brief Compares with another instance of Uuid for inequality.
     * @param other another instance of Uuid.
     * @return `true` if values of two Uuids are not equal, `false` otherwise.
     *
     */
    constexpr bool operator!=(Uuid const& other) const { return !(*this == other); }

    /**
     * @brief Checks if the value represents a Nil UUID (`00000000-0000-0000-0000-000000000000`).
     *
     */
    constexpr bool IsNil() const
    {
      // Nil UUID, per RFC9562, consists of all zeros:
      // https://www.rfc-editor.org/rfc/rfc9562.html#name-nil-uuid
      for (size_t i = 0; i < m_uuid.size(); ++i)
      {
        if (m_uuid[i] != 0)
        {
          return false;
        }
      }

      return true;
    }
  };
}} // namespace Azure::Core

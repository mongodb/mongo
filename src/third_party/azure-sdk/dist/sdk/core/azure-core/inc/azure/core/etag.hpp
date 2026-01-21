// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Define ETag.
 */

#pragma once

#include "azure/core/azure_assert.hpp"
#include "azure/core/nullable.hpp"

#include <string>

namespace Azure {

/**
 * @brief Represents an HTTP validator.
 */
class ETag final {
  // ETag is a validator based on https://tools.ietf.org/html/rfc7232#section-2.3.2
private:
  Azure::Nullable<std::string> m_value;

public:
  /**
   * @brief The comparison type.
   *
   */
  enum class ETagComparison
  {
    Strong,
    Weak
  };

  /*
  2.3.2.  Comparison

    There are two entity-tag comparison functions, depending on whether
    or not the comparison context allows the use of weak validators:

    o  Strong comparison: two entity-tags are equivalent if both are not
        weak and their opaque-tags match character-by-character.

    o  Weak comparison: two entity-tags are equivalent if their
        opaque-tags match character-by-character, regardless of either or
        both being tagged as "weak".

     +--------+--------+-------------------+-----------------+
     | ETag 1 | ETag 2 | Strong Comparison | Weak Comparison |
     +--------+--------+-------------------+-----------------+
     | W/"1"  | W/"1"  | no match          | match           |
     | W/"1"  | W/"2"  | no match          | no match        |
     | W/"1"  | "1"    | no match          | match           |
     | "1"    | "1"    | match             | match           |
     +--------+--------+-------------------+-----------------+

  // etag:                            //This is possible and means no etag is present
  // etag:""
  // etag:"*"                         //This means the etag is value '*'
  // etag:"some value"                //This means the etag is value 'some value'
  // etag:/W""                        //Weak eTag
  // etag:*                           //This is special, means any etag
  // If-Match header can do this
  // If-Match:"value1","value2","value3"  // Do this if any of these match

  */

  /**
   * @brief Indicates whether two #Azure::ETag values are equal.
   *
   * @param left #Azure::ETag to compare.
   * @param right #Azure::ETag to compare.
   * @param comparisonKind Determines what #Azure::ETag::ETagComparison to perform, default
   * is #Azure::ETag::ETagComparison Strong.
   * @return `true` if `%ETag` matches; otherwise, `false`.
   */
  static bool Equals(
      const ETag& left,
      const ETag& right,
      const ETagComparison comparisonKind = ETagComparison::Strong)
  {
    // ETags are != if one of the values is null
    if (!left.m_value || !right.m_value)
    {
      // Caveat, If both values are null then we consider the ETag equal
      return !left.m_value && !right.m_value;
    }

    switch (comparisonKind)
    {
      case ETagComparison::Strong:
        // Strong comparison
        // If either is weak then there is no match
        //  else tags must match character for character
        return !left.IsWeak() && !right.IsWeak()
            && (left.m_value.Value().compare(right.m_value.Value()) == 0);
        break;

      case ETagComparison::Weak:

        auto leftStart = left.IsWeak() ? 2 : 0;
        auto rightStart = right.IsWeak() ? 2 : 0;

        auto leftVal = left.m_value.Value();
        auto rightVal = right.m_value.Value();

        // Compare if lengths are equal
        //   Compare the strings character by character
        return ((leftVal.length() - leftStart) == (rightVal.length() - rightStart))
            && (leftVal.compare(leftStart, leftVal.length() - leftStart, &rightVal[rightStart])
                == 0);
        break;
    }
    // Unknown comparison
    AZURE_UNREACHABLE_CODE();
  }

  /**
   * @brief Constructs an empty (null) `%ETag`.
   *
   */
  ETag() = default;

  /**
   * @brief Constructs an `%ETag` with string representation.
   * @param etag The string value representation.
   */
  explicit ETag(std::string etag) : m_value(std::move(etag)) {}

  /**
   * @brief Whether `%ETag` is present.
   * @return `true` if `%ETag` has a value; otherwise, `false`.
   */
  bool HasValue() const { return m_value.HasValue(); }

  /**
   * @brief Returns the resource metadata represented as a string.
   * @return std::string
   */
  const std::string& ToString() const
  {
    AZURE_ASSERT_MSG(m_value.HasValue(), "Empty ETag, check HasValue() before calling ToString().");
    return m_value.Value();
  }

  /**
   * @brief Compare with \p other `%ETag` for equality.
   * @param other Other `%ETag` to compare with.
   * @return `true` if `%ETag` instances are equal according to strong validation; otherwise,
   * `false`.
   */
  bool operator==(const ETag& other) const { return Equals(*this, other, ETagComparison::Strong); }

  /**
   * @brief Compare with \p other `%ETag` for inequality.
   * @param other Other `%ETag` to compare with.
   * @return `true` if `%ETag` instances are not equal according to strong validation; otherwise,
   * `false`.
   */
  bool operator!=(const ETag& other) const { return !(*this == other); }

  /**
   * @brief Specifies whether the #Azure::ETag is strong or weak.
   * @return `true` if #Azure::ETag is a weak validator; otherwise, `false`.
   */
  bool IsWeak() const
  {
    // Null ETag is considered Strong
    // Shortest valid weak etag has length of 4
    //  W/""
    // Valid weak format must start with W/"
    //   Must end with a /"
    const bool weak = m_value && (m_value.Value().length() >= 4)
        && ((m_value.Value()[0] == 'W') && (m_value.Value()[1] == '/')
            && (m_value.Value()[2] == '"') && (m_value.Value()[m_value.Value().size() - 1] == '"'));

    return weak;
  }

  /**
   * @brief #Azure::ETag representing everything.
   * @note The any #Azure::ETag is *, (unquoted).  It is NOT the same as * in quotes.
   */
  static const ETag& Any();
};
} // namespace Azure

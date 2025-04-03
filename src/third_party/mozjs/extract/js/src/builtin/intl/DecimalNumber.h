/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_DecimalNumber_h
#define builtin_intl_DecimalNumber_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "mozilla/Variant.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

#include "js/TypeDecls.h"

class JSLinearString;

namespace JS {
class JS_PUBLIC_API AutoCheckCannotGC;
}

namespace js::intl {

/**
 * Representation of a decimal number in normalized form.
 *
 * Examples of normalized forms:
 * - "123" is normalized to "0.123e3".
 * - "0.01e-4" is normalized to "0.1e-5".
 * - "12.3" is normalized to "0.123e2".
 *
 * Note: Internally we leave the decimal point where it lies to avoid copying
 * the string, but otherwise ignore it once we calculate the normalized
 * exponent.
 *
 * TODO: Remove unused capabilities once there's a concrete PR for
 * <https://github.com/tc39/proposal-intl-numberformat-v3/issues/98>.
 */
class MOZ_STACK_CLASS DecimalNumber final {
  using Latin1String = mozilla::Span<const JS::Latin1Char>;
  using TwoByteString = mozilla::Span<const char16_t>;

  mozilla::Variant<Latin1String, TwoByteString> string_;

  char charAt(size_t i) const {
    if (string_.is<Latin1String>()) {
      return static_cast<char>(string_.as<Latin1String>()[i]);
    }
    return static_cast<char>(string_.as<TwoByteString>()[i]);
  }

  // Decimal exponent. Valid range is (INT32_MIN, INT_MAX32].
  int32_t exponent_ = 0;

  // Start and end position of the significand.
  size_t significandStart_ = 0;
  size_t significandEnd_ = 0;

  // Flag if the number is zero.
  bool zero_ = false;

  // Flag for negative numbers.
  bool negative_ = false;

  // Error flag when the exponent is too large.
  bool exponentTooLarge_ = false;

  template <typename CharT>
  explicit DecimalNumber(mozilla::Span<const CharT> string) : string_(string) {}

 public:
  /** Return true if this decimal is zero. */
  bool isZero() const { return zero_; }

  /** Return true if this decimal is negative. */
  bool isNegative() const { return negative_; }

  /** Return true if the exponent is too large. */
  bool exponentTooLarge() const { return exponentTooLarge_; }

  /** Return the exponent of this decimal. */
  int32_t exponent() const { return exponent_; }

  // Exposed for testing.
  size_t significandStart() const { return significandStart_; }
  size_t significandEnd() const { return significandEnd_; }

  /**
   * Compare this decimal to another decimal. Returns a negative value if this
   * decimal is smaller; zero if this decimal is equal; or a positive value if
   * this decimal is larger than the input.
   */
  int32_t compareTo(const DecimalNumber& other) const;

  /**
   * Create a decimal number from the input. Returns |mozilla::Nothing| if the
   * input can't be parsed.
   */
  template <typename CharT>
  static mozilla::Maybe<DecimalNumber> from(mozilla::Span<const CharT> chars);

  /**
   * Create a decimal number from the input. Returns |mozilla::Nothing| if the
   * input can't be parsed.
   */
  static mozilla::Maybe<DecimalNumber> from(JSLinearString* str,
                                            JS::AutoCheckCannotGC& nogc);
};
}  // namespace js::intl

#endif /* builtin_intl_DecimalNumber_h */

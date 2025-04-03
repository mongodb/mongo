/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/intl/DecimalNumber.h"

#include "mozilla/Assertions.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/TextUtils.h"

#include "js/GCAPI.h"
#include "util/Text.h"
#include "util/Unicode.h"
#include "vm/StringType.h"

int32_t js::intl::DecimalNumber::compareTo(const DecimalNumber& other) const {
  // Can't compare if the exponent is too large.
  MOZ_ASSERT(!exponentTooLarge());
  MOZ_ASSERT(!other.exponentTooLarge());

  // If the signs don't match, the negative number is smaller.
  if (isNegative() != other.isNegative()) {
    return isNegative() ? -1 : 1;
  }

  // Next handle the case when one of the numbers is zero.
  if (isZero()) {
    return other.isZero() ? 0 : other.isNegative() ? 1 : -1;
  }
  if (other.isZero()) {
    return isNegative() ? -1 : 1;
  }

  // If the exponent is different, the number with the smaller exponent is
  // smaller in total, unless the numbers are negative.
  if (exponent() != other.exponent()) {
    return (exponent() < other.exponent() ? -1 : 1) * (isNegative() ? -1 : 1);
  }

  class Significand {
    const DecimalNumber& decimal_;
    size_t index_;

   public:
    explicit Significand(const DecimalNumber& decimal) : decimal_(decimal) {
      index_ = decimal.significandStart_;
    }

    int32_t next() {
      // Any remaining digits in the significand are implicit zeros.
      if (index_ >= decimal_.significandEnd_) {
        return 0;
      }

      char ch = decimal_.charAt(index_++);

      // Skip over the decimal point.
      if (ch == '.') {
        if (index_ >= decimal_.significandEnd_) {
          return 0;
        }
        ch = decimal_.charAt(index_++);
      }

      MOZ_ASSERT(mozilla::IsAsciiDigit(ch));
      return AsciiDigitToNumber(ch);
    }
  };

  // Both numbers have the same sign, neither of them is zero, and they have the
  // same exponent. Next compare the significand digit by digit until we find
  // the first difference.

  Significand s1(*this);
  Significand s2(other);
  for (int32_t e = std::abs(exponent()); e >= 0; e--) {
    int32_t x = s1.next();
    int32_t y = s2.next();
    if (int32_t r = x - y) {
      return r * (isNegative() ? -1 : 1);
    }
  }

  // No different significand digit was found, so the numbers are equal.
  return 0;
}

mozilla::Maybe<js::intl::DecimalNumber> js::intl::DecimalNumber::from(
    JSLinearString* str, JS::AutoCheckCannotGC& nogc) {
  return str->hasLatin1Chars() ? from<Latin1Char>(str->latin1Range(nogc))
                               : from<char16_t>(str->twoByteRange(nogc));
}

template <typename CharT>
mozilla::Maybe<js::intl::DecimalNumber> js::intl::DecimalNumber::from(
    mozilla::Span<const CharT> chars) {
  // This algorithm matches a subset of the `StringNumericLiteral` grammar
  // production of ECMAScript. In particular, we do *not* allow:
  // - NonDecimalIntegerLiteral (eg. "0x10")
  // - NumericLiteralSeparator (eg. "123_456")
  // - Infinity (eg. "-Infinity")

  DecimalNumber number(chars);

  // Skip over leading whitespace.
  size_t i = 0;
  while (i < chars.size() && unicode::IsSpace(chars[i])) {
    i++;
  }

  // The number is only whitespace, treat as zero.
  if (i == chars.size()) {
    number.zero_ = true;
    return mozilla::Some(number);
  }

  // Read the optional sign.
  if (auto ch = chars[i]; ch == '-' || ch == '+') {
    i++;
    number.negative_ = ch == '-';

    if (i == chars.size()) {
      return mozilla::Nothing();
    }
  }

  // Must start with either a digit or the decimal point.
  size_t startInteger = i;
  size_t endInteger = i;
  if (auto ch = chars[i]; mozilla::IsAsciiDigit(ch)) {
    // Skip over leading zeros.
    while (i < chars.size() && chars[i] == '0') {
      i++;
    }

    // Read the integer part.
    startInteger = i;
    while (i < chars.size() && mozilla::IsAsciiDigit(chars[i])) {
      i++;
    }
    endInteger = i;
  } else if (ch == '.') {
    // There must be a digit when the number starts with the decimal point.
    if (i + 1 == chars.size() || !mozilla::IsAsciiDigit(chars[i + 1])) {
      return mozilla::Nothing();
    }
  } else {
    return mozilla::Nothing();
  }

  // Read the fractional part.
  size_t startFraction = i;
  size_t endFraction = i;
  if (i < chars.size() && chars[i] == '.') {
    i++;

    startFraction = i;
    while (i < chars.size() && mozilla::IsAsciiDigit(chars[i])) {
      i++;
    }
    endFraction = i;

    // Ignore trailing zeros in the fractional part.
    while (startFraction <= endFraction && chars[endFraction - 1] == '0') {
      endFraction--;
    }
  }

  // Read the exponent.
  if (i < chars.size() && (chars[i] == 'e' || chars[i] == 'E')) {
    i++;

    if (i == chars.size()) {
      return mozilla::Nothing();
    }

    int32_t exponentSign = 1;
    if (auto ch = chars[i]; ch == '-' || ch == '+') {
      i++;
      exponentSign = ch == '-' ? -1 : +1;

      if (i == chars.size()) {
        return mozilla::Nothing();
      }
    }

    if (!mozilla::IsAsciiDigit(chars[i])) {
      return mozilla::Nothing();
    }

    mozilla::CheckedInt32 exp = 0;
    while (i < chars.size() && mozilla::IsAsciiDigit(chars[i])) {
      exp *= 10;
      exp += AsciiDigitToNumber(chars[i]);

      i++;
    }

    // Check for exponent overflow.
    if (exp.isValid()) {
      number.exponent_ = exp.value() * exponentSign;
    } else {
      number.exponentTooLarge_ = true;
    }
  }

  // Skip over trailing whitespace.
  while (i < chars.size() && unicode::IsSpace(chars[i])) {
    i++;
  }

  // The complete string must have been parsed.
  if (i != chars.size()) {
    return mozilla::Nothing();
  }

  if (startInteger < endInteger) {
    // We have a non-zero integer part.

    mozilla::CheckedInt32 integerExponent = number.exponent_;
    integerExponent += size_t(endInteger - startInteger);

    if (integerExponent.isValid()) {
      number.exponent_ = integerExponent.value();
    } else {
      number.exponent_ = 0;
      number.exponentTooLarge_ = true;
    }

    number.significandStart_ = startInteger;
    number.significandEnd_ = endFraction;
  } else if (startFraction < endFraction) {
    // We have a non-zero fractional part.

    // Skip over leading zeros
    size_t i = startFraction;
    while (i < endFraction && chars[i] == '0') {
      i++;
    }

    mozilla::CheckedInt32 fractionExponent = number.exponent_;
    fractionExponent -= size_t(i - startFraction);

    if (fractionExponent.isValid() && fractionExponent.value() != INT32_MIN) {
      number.exponent_ = fractionExponent.value();
    } else {
      number.exponent_ = 0;
      number.exponentTooLarge_ = true;
    }

    number.significandStart_ = i;
    number.significandEnd_ = endFraction;
  } else {
    // The number is zero, clear the error flag if it was set.
    number.zero_ = true;
    number.exponent_ = 0;
    number.exponentTooLarge_ = false;
  }

  return mozilla::Some(number);
}

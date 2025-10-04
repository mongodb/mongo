/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/Int96.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"

#include <cmath>
#include <stddef.h>
#include <stdint.h>

#include "jsnum.h"

using namespace js;
using namespace js::temporal;

mozilla::Maybe<Int96> Int96::fromInteger(double value) {
  MOZ_ASSERT(IsInteger(value));

  // Fast path for the common case.
  int64_t intValue;
  if (mozilla::NumberEqualsInt64(value, &intValue)) {
    return mozilla::Some(Int96{intValue});
  }

  // First double integer which requires more than three digits.
  constexpr double maximum = 0x1p+96;

  // Reject if the value needs more than 96 bits.
  if (std::abs(value) >= maximum) {
    return mozilla::Nothing();
  }

  // Inlined version of |BigInt::createFromDouble()| for DigitBits=32. See the
  // comments in |BigInt::createFromDouble()| for how this code works.
  constexpr int DigitBits = 32;

  // The number can't have more than three digits when it's below |maximum|.
  Int96::Digits digits = {};

  int exponent = int(mozilla::ExponentComponent(value));
  MOZ_ASSERT(0 <= exponent && exponent <= 95,
             "exponent is lower than exponent of 0x1p+96");

  int length = exponent / DigitBits + 1;
  MOZ_ASSERT(1 <= length && length <= 3);

  using Double = mozilla::FloatingPoint<double>;
  uint64_t mantissa =
      mozilla::BitwiseCast<uint64_t>(value) & Double::kSignificandBits;

  // Add implicit high bit.
  mantissa |= 1ull << Double::kSignificandWidth;

  // 0-indexed position of the double's most significant bit within the `msd`.
  int msdTopBit = exponent % DigitBits;

  // First, build the MSD by shifting the mantissa appropriately.
  int remainingMantissaBits = int(Double::kSignificandWidth - msdTopBit);
  digits[--length] = mantissa >> remainingMantissaBits;

  // Fill in digits containing mantissa contributions.
  mantissa = mantissa << (64 - remainingMantissaBits);
  if (mantissa) {
    MOZ_ASSERT(length > 0);
    digits[--length] = uint32_t(mantissa >> 32);

    if (uint32_t(mantissa)) {
      MOZ_ASSERT(length > 0);
      digits[--length] = uint32_t(mantissa);
    }
  }

  return mozilla::Some(Int96{digits, value < 0});
}

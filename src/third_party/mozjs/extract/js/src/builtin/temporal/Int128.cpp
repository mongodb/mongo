/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/Int128.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"

#include <stdint.h>

using namespace js;
using namespace js::temporal;

double Uint128::toDouble(const Uint128& x, bool negative) {
  // Simplified version of |BigInt::numberValue()| for DigitBits=64. See the
  // comments in |BigInt::numberValue()| for how this code works.

  using Double = mozilla::FloatingPoint<double>;
  constexpr uint8_t ExponentShift = Double::kExponentShift;
  constexpr uint8_t SignificandWidth = Double::kSignificandWidth;
  constexpr unsigned ExponentBias = Double::kExponentBias;
  constexpr uint8_t SignShift = Double::kExponentWidth + SignificandWidth;

  constexpr uint64_t MaxIntegralPrecisionDouble = uint64_t(1)
                                                  << (SignificandWidth + 1);

  // We compute the final mantissa of the result, shifted upward to the top of
  // the `uint64_t` space -- plus an extra bit to detect potential rounding.
  constexpr uint8_t BitsNeededForShiftedMantissa = SignificandWidth + 1;

  uint64_t shiftedMantissa = 0;
  uint64_t exponent = 0;

  // If the extra bit is set, correctly rounding the result may require
  // examining all lower-order bits. Also compute 1) the index of the Digit
  // storing the extra bit, and 2) whether bits beneath the extra bit in that
  // Digit are nonzero so we can round if needed.
  uint64_t bitsBeneathExtraBitInDigitContainingExtraBit = 0;

  if (x.high == 0) {
    uint64_t msd = x.low;

    // Fast path for the likely-common case of up to a uint64_t of magnitude not
    // exceeding integral precision in IEEE-754.
    if (msd <= MaxIntegralPrecisionDouble) {
      return negative ? -double(msd) : +double(msd);
    }

    const uint8_t msdLeadingZeroes = mozilla::CountLeadingZeroes64(msd);
    MOZ_ASSERT(msdLeadingZeroes <= 10,
               "leading zeroes is at most 10 when the fast path isn't taken");

    exponent = 64 - msdLeadingZeroes - 1;

    // Omit the most significant bit: the IEEE-754 format includes this bit
    // implicitly for all double-precision integers.
    const uint8_t msdIgnoredBits = msdLeadingZeroes + 1;
    MOZ_ASSERT(1 <= msdIgnoredBits && msdIgnoredBits <= 11);

    const uint8_t msdIncludedBits = 64 - msdIgnoredBits;
    MOZ_ASSERT(53 <= msdIncludedBits && msdIncludedBits <= 63);
    MOZ_ASSERT(msdIncludedBits >= BitsNeededForShiftedMantissa);

    // Munge the most significant bits of the number into proper
    // position in an IEEE-754 double and go to town.

    // Shift `msd`'s contributed bits upward to remove high-order zeroes and the
    // highest set bit (which is implicit in IEEE-754 integral values so must be
    // removed) and to add low-order zeroes.  (Lower-order garbage bits are
    // discarded when `shiftedMantissa` is converted to a real mantissa.)
    shiftedMantissa = msd << (64 - msdIncludedBits);

    // Add shifted bits to `shiftedMantissa` until we have a complete mantissa
    // and an extra bit.
    const uint8_t countOfBitsInDigitBelowExtraBit =
        64 - BitsNeededForShiftedMantissa - msdIgnoredBits;
    bitsBeneathExtraBitInDigitContainingExtraBit =
        msd & ((uint64_t(1) << countOfBitsInDigitBelowExtraBit) - 1);
  } else {
    uint64_t msd = x.high;
    uint64_t second = x.low;

    uint8_t msdLeadingZeroes = mozilla::CountLeadingZeroes64(msd);

    exponent = 2 * 64 - msdLeadingZeroes - 1;

    // Munge the most significant bits of the number into proper
    // position in an IEEE-754 double and go to town.

    // Omit the most significant bit: the IEEE-754 format includes this bit
    // implicitly for all double-precision integers.
    const uint8_t msdIgnoredBits = msdLeadingZeroes + 1;
    const uint8_t msdIncludedBits = 64 - msdIgnoredBits;

    // Shift `msd`'s contributed bits upward to remove high-order zeroes and the
    // highest set bit (which is implicit in IEEE-754 integral values so must be
    // removed) and to add low-order zeroes.  (Lower-order garbage bits are
    // discarded when `shiftedMantissa` is converted to a real mantissa.)
    shiftedMantissa = msdIncludedBits == 0 ? 0 : msd << (64 - msdIncludedBits);

    // Add shifted bits to `shiftedMantissa` until we have a complete mantissa
    // and an extra bit.
    if (msdIncludedBits >= BitsNeededForShiftedMantissa) {
      const uint8_t countOfBitsInDigitBelowExtraBit =
          64 - BitsNeededForShiftedMantissa - msdIgnoredBits;
      bitsBeneathExtraBitInDigitContainingExtraBit =
          msd & ((uint64_t(1) << countOfBitsInDigitBelowExtraBit) - 1);

      if (bitsBeneathExtraBitInDigitContainingExtraBit == 0) {
        bitsBeneathExtraBitInDigitContainingExtraBit = second;
      }
    } else {
      shiftedMantissa |= second >> msdIncludedBits;

      const uint8_t countOfBitsInSecondDigitBelowExtraBit =
          (msdIncludedBits + 64) - BitsNeededForShiftedMantissa;
      bitsBeneathExtraBitInDigitContainingExtraBit =
          second << (64 - countOfBitsInSecondDigitBelowExtraBit);
    }
  }

  constexpr uint64_t LeastSignificantBit = uint64_t(1)
                                           << (64 - SignificandWidth);
  constexpr uint64_t ExtraBit = LeastSignificantBit >> 1;

  // The extra bit must be set for rounding to change the mantissa.
  if ((shiftedMantissa & ExtraBit) != 0) {
    bool shouldRoundUp;
    if (shiftedMantissa & LeastSignificantBit) {
      // If the lowest mantissa bit is set, it doesn't matter what lower bits
      // are: nearest-even rounds up regardless.
      shouldRoundUp = true;
    } else {
      // If the lowest mantissa bit is unset, *all* lower bits are relevant.
      // All-zero bits below the extra bit situates `x` halfway between two
      // values, and the nearest *even* value lies downward.  But if any bit
      // below the extra bit is set, `x` is closer to the rounded-up value.
      shouldRoundUp = bitsBeneathExtraBitInDigitContainingExtraBit != 0;
    }

    if (shouldRoundUp) {
      // Add one to the significand bits.  If they overflow, the exponent must
      // also be increased.  If *that* overflows, return the correct infinity.
      uint64_t before = shiftedMantissa;
      shiftedMantissa += ExtraBit;
      if (shiftedMantissa < before) {
        exponent++;
      }
    }
  }

  uint64_t significandBits = shiftedMantissa >> (64 - SignificandWidth);
  uint64_t signBit = uint64_t(negative ? 1 : 0) << SignShift;
  uint64_t exponentBits = (exponent + ExponentBias) << ExponentShift;
  return mozilla::BitwiseCast<double>(signBit | exponentBits | significandBits);
}

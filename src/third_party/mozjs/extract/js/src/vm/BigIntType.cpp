/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Portions of this code taken from WebKit, whose copyright is as follows:
 *
 *   Copyright (C) 2017 Caio Lima <ticaiolima@gmail.com>
 *   Copyright (C) 2017-2018 Apple Inc. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 *   EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *   PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 *   OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Portions of this code taken from V8, whose copyright notice is as follows:
 *
 *   Copyright 2017 the V8 project authors. All rights reserved.
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are
 *   met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer in the documentation and/or other materials provided
 *         with the distribution.
 *       * Neither the name of Google Inc. nor the names of its
 *         contributors may be used to endorse or promote products derived
 *         from this software without specific prior written permission.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Portions of this code taken from Dart, whose copyright notice is as follows:
 *
 *   Copyright (c) 2014 the Dart project authors.  Please see the AUTHORS file
 * [1] for details. All rights reserved. Use of this source code is governed by
 * a BSD-style license that can be found in the LICENSE file [2].
 *
 *   [1] https://github.com/dart-lang/sdk/blob/master/AUTHORS
 *   [2] https://github.com/dart-lang/sdk/blob/master/LICENSE
 *
 * Portions of this code taken from Go, whose copyright notice is as follows:
 *
 *   Copyright 2009 The Go Authors. All rights reserved.
 *   Use of this source code is governed by a BSD-style
 *   license that can be found in the LICENSE file [3].
 *
 *   [3] https://golang.org/LICENSE
 */

#include "vm/BigIntType.h"

#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/Range.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/Span.h"  // mozilla::Span
#include "mozilla/WrappingOperations.h"

#include <functional>
#include <limits>
#include <math.h>
#include <memory>
#include <type_traits>  // std::is_same_v

#include "jsapi.h"
#include "jsnum.h"

#include "builtin/BigInt.h"
#include "gc/Allocator.h"
#include "js/BigInt.h"
#include "js/Conversions.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Initialization.h"
#include "js/StableStringChars.h"
#include "js/Utility.h"
#include "util/CheckedArithmetic.h"
#include "vm/JSContext.h"
#include "vm/SelfHosting.h"

#include "gc/FreeOp-inl.h"
#include "gc/Nursery-inl.h"
#include "vm/JSContext-inl.h"

using namespace js;

using JS::AutoStableStringChars;
using mozilla::Abs;
using mozilla::AssertedCast;
using mozilla::BitwiseCast;
using mozilla::IsFinite;
using mozilla::Maybe;
using mozilla::NegativeInfinity;
using mozilla::Nothing;
using mozilla::PositiveInfinity;
using mozilla::Range;
using mozilla::RangedPtr;
using mozilla::Some;
using mozilla::WrapToSigned;

static inline unsigned DigitLeadingZeroes(BigInt::Digit x) {
  return sizeof(x) == 4 ? mozilla::CountLeadingZeroes32(x)
                        : mozilla::CountLeadingZeroes64(x);
}

#ifdef DEBUG
static bool HasLeadingZeroes(BigInt* bi) {
  return bi->digitLength() > 0 && bi->digit(bi->digitLength() - 1) == 0;
}
#endif

BigInt* BigInt::createUninitialized(JSContext* cx, size_t digitLength,
                                    bool isNegative, gc::InitialHeap heap) {
  if (digitLength > MaxDigitLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_TOO_LARGE);
    return nullptr;
  }

  BigInt* x = AllocateBigInt(cx, heap);
  if (!x) {
    return nullptr;
  }

  x->setLengthAndFlags(digitLength, isNegative ? SignBit : 0);

  MOZ_ASSERT(x->digitLength() == digitLength);
  MOZ_ASSERT(x->isNegative() == isNegative);

  if (digitLength > InlineDigitsLength) {
    x->heapDigits_ = js::AllocateBigIntDigits(cx, x, digitLength);
    if (!x->heapDigits_) {
      // |x| is partially initialized, expose it as a BigInt using inline digits
      // to the GC.
      x->setLengthAndFlags(0, 0);
      return nullptr;
    }

    AddCellMemory(x, digitLength * sizeof(Digit), js::MemoryUse::BigIntDigits);
  }

  return x;
}

void BigInt::initializeDigitsToZero() {
  auto digs = digits();
  std::uninitialized_fill_n(digs.begin(), digs.Length(), 0);
}

void BigInt::finalize(JSFreeOp* fop) {
  MOZ_ASSERT(isTenured());
  if (hasHeapDigits()) {
    size_t size = digitLength() * sizeof(Digit);
    fop->free_(this, heapDigits_, size, js::MemoryUse::BigIntDigits);
  }
}

js::HashNumber BigInt::hash() const {
  js::HashNumber h =
      mozilla::HashBytes(digits().data(), digitLength() * sizeof(Digit));
  return mozilla::AddToHash(h, isNegative());
}

size_t BigInt::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
  return hasInlineDigits() ? 0 : mallocSizeOf(heapDigits_);
}

size_t BigInt::sizeOfExcludingThisInNursery(
    mozilla::MallocSizeOf mallocSizeOf) const {
  MOZ_ASSERT(!isTenured());

  if (hasInlineDigits()) {
    return 0;
  }

  const Nursery& nursery = runtimeFromMainThread()->gc.nursery();
  if (nursery.isInside(heapDigits_)) {
    // See |AllocateBigIntDigits()|.
    return RoundUp(digitLength() * sizeof(Digit), sizeof(Value));
  }

  return mallocSizeOf(heapDigits_);
}

BigInt* BigInt::zero(JSContext* cx, gc::InitialHeap heap) {
  return createUninitialized(cx, 0, false, heap);
}

BigInt* BigInt::createFromDigit(JSContext* cx, Digit d, bool isNegative) {
  MOZ_ASSERT(d != 0);
  BigInt* res = createUninitialized(cx, 1, isNegative);
  if (!res) {
    return nullptr;
  }
  res->setDigit(0, d);
  return res;
}

BigInt* BigInt::one(JSContext* cx) { return createFromDigit(cx, 1, false); }

BigInt* BigInt::negativeOne(JSContext* cx) {
  return createFromDigit(cx, 1, true);
}

BigInt* BigInt::createFromNonZeroRawUint64(JSContext* cx, uint64_t n,
                                           bool isNegative) {
  MOZ_ASSERT(n != 0);

  size_t resultLength = 1;
  if (DigitBits == 32 && (n >> 32) != 0) {
    resultLength = 2;
  }

  BigInt* result = createUninitialized(cx, resultLength, isNegative);
  if (!result) {
    return nullptr;
  }
  result->setDigit(0, n);
  if (DigitBits == 32 && resultLength > 1) {
    result->setDigit(1, n >> 32);
  }

  MOZ_ASSERT(!HasLeadingZeroes(result));
  return result;
}

BigInt* BigInt::neg(JSContext* cx, HandleBigInt x) {
  if (x->isZero()) {
    return x;
  }

  BigInt* result = copy(cx, x);
  if (!result) {
    return nullptr;
  }
  result->toggleHeaderFlagBit(SignBit);
  return result;
}

#if !defined(JS_64BIT)
#  define HAVE_TWO_DIGIT 1
using TwoDigit = uint64_t;
#elif defined(__SIZEOF_INT128__)
#  define HAVE_TWO_DIGIT 1
using TwoDigit = __uint128_t;
#endif

inline BigInt::Digit BigInt::digitMul(Digit a, Digit b, Digit* high) {
#if defined(HAVE_TWO_DIGIT)
  TwoDigit result = static_cast<TwoDigit>(a) * static_cast<TwoDigit>(b);
  *high = result >> DigitBits;

  return static_cast<Digit>(result);
#else
  // Multiply in half-pointer-sized chunks.
  // For inputs [AH AL]*[BH BL], the result is:
  //
  //            [AL*BL]  // rLow
  //    +    [AL*BH]     // rMid1
  //    +    [AH*BL]     // rMid2
  //    + [AH*BH]        // rHigh
  //    = [R4 R3 R2 R1]  // high = [R4 R3], low = [R2 R1]
  //
  // Where of course we must be careful with carries between the columns.
  Digit aLow = a & HalfDigitMask;
  Digit aHigh = a >> HalfDigitBits;
  Digit bLow = b & HalfDigitMask;
  Digit bHigh = b >> HalfDigitBits;

  Digit rLow = aLow * bLow;
  Digit rMid1 = aLow * bHigh;
  Digit rMid2 = aHigh * bLow;
  Digit rHigh = aHigh * bHigh;

  Digit carry = 0;
  Digit low = digitAdd(rLow, rMid1 << HalfDigitBits, &carry);
  low = digitAdd(low, rMid2 << HalfDigitBits, &carry);

  *high = (rMid1 >> HalfDigitBits) + (rMid2 >> HalfDigitBits) + rHigh + carry;

  return low;
#endif
}

BigInt::Digit BigInt::digitDiv(Digit high, Digit low, Digit divisor,
                               Digit* remainder) {
  MOZ_ASSERT(high < divisor, "division must not overflow");
#if defined(__x86_64__)
  Digit quotient;
  Digit rem;
  __asm__("divq  %[divisor]"
          // Outputs: `quotient` will be in rax, `rem` in rdx.
          : "=a"(quotient), "=d"(rem)
          // Inputs: put `high` into rdx, `low` into rax, and `divisor` into
          // any register or stack slot.
          : "d"(high), "a"(low), [divisor] "rm"(divisor));
  *remainder = rem;
  return quotient;
#elif defined(__i386__)
  Digit quotient;
  Digit rem;
  __asm__("divl  %[divisor]"
          // Outputs: `quotient` will be in eax, `rem` in edx.
          : "=a"(quotient), "=d"(rem)
          // Inputs: put `high` into edx, `low` into eax, and `divisor` into
          // any register or stack slot.
          : "d"(high), "a"(low), [divisor] "rm"(divisor));
  *remainder = rem;
  return quotient;
#else
  static constexpr Digit HalfDigitBase = 1ull << HalfDigitBits;
  // Adapted from Warren, Hacker's Delight, p. 152.
  unsigned s = DigitLeadingZeroes(divisor);
  // If `s` is DigitBits here, it causes an undefined behavior.
  // But `s` is never DigitBits since `divisor` is never zero here.
  MOZ_ASSERT(s != DigitBits);
  divisor <<= s;

  Digit vn1 = divisor >> HalfDigitBits;
  Digit vn0 = divisor & HalfDigitMask;

  // `sZeroMask` which is 0 if s == 0 and all 1-bits otherwise.
  //
  // `s` can be 0. If `s` is 0, performing "low >> (DigitBits - s)" must not
  // be done since it causes an undefined behavior since `>> DigitBits` is
  // undefined in C++. Quoted from C++ spec, "The type of the result is that of
  // the promoted left operand.
  //
  // The behavior is undefined if the right operand is negative, or greater
  // than or equal to the length in bits of the promoted left operand". We
  // mask the right operand of the shift by `shiftMask` (`DigitBits - 1`),
  // which makes `DigitBits - 0` zero.
  //
  // This shifting produces a value which covers 0 < `s` <= (DigitBits - 1)
  // cases. `s` == DigitBits never happen as we asserted.  Since `sZeroMask`
  // clears the value in the case of `s` == 0, `s` == 0 case is also covered.
  static_assert(sizeof(intptr_t) == sizeof(Digit),
                "unexpected size of BigInt::Digit");
  Digit sZeroMask =
      static_cast<Digit>((-static_cast<intptr_t>(s)) >> (DigitBits - 1));
  static constexpr unsigned shiftMask = DigitBits - 1;
  Digit un32 =
      (high << s) | ((low >> ((DigitBits - s) & shiftMask)) & sZeroMask);

  Digit un10 = low << s;
  Digit un1 = un10 >> HalfDigitBits;
  Digit un0 = un10 & HalfDigitMask;
  Digit q1 = un32 / vn1;
  Digit rhat = un32 - q1 * vn1;

  while (q1 >= HalfDigitBase || q1 * vn0 > rhat * HalfDigitBase + un1) {
    q1--;
    rhat += vn1;
    if (rhat >= HalfDigitBase) {
      break;
    }
  }

  Digit un21 = un32 * HalfDigitBase + un1 - q1 * divisor;
  Digit q0 = un21 / vn1;
  rhat = un21 - q0 * vn1;

  while (q0 >= HalfDigitBase || q0 * vn0 > rhat * HalfDigitBase + un0) {
    q0--;
    rhat += vn1;
    if (rhat >= HalfDigitBase) {
      break;
    }
  }

  *remainder = (un21 * HalfDigitBase + un0 - q0 * divisor) >> s;
  return q1 * HalfDigitBase + q0;
#endif
}

// Multiplies `source` with `factor` and adds `summand` to the result.
// `result` and `source` may be the same BigInt for inplace modification.
void BigInt::internalMultiplyAdd(BigInt* source, Digit factor, Digit summand,
                                 unsigned n, BigInt* result) {
  MOZ_ASSERT(source->digitLength() >= n);
  MOZ_ASSERT(result->digitLength() >= n);

  Digit carry = summand;
  Digit high = 0;
  for (unsigned i = 0; i < n; i++) {
    Digit current = source->digit(i);
    Digit newCarry = 0;

    // Compute this round's multiplication.
    Digit newHigh = 0;
    current = digitMul(current, factor, &newHigh);

    // Add last round's carryovers.
    current = digitAdd(current, high, &newCarry);
    current = digitAdd(current, carry, &newCarry);

    // Store result and prepare for next round.
    result->setDigit(i, current);
    carry = newCarry;
    high = newHigh;
  }

  if (result->digitLength() > n) {
    result->setDigit(n++, carry + high);

    // Current callers don't pass in such large results, but let's be robust.
    while (n < result->digitLength()) {
      result->setDigit(n++, 0);
    }
  } else {
    MOZ_ASSERT(!(carry + high));
  }
}

// Multiplies `this` with `factor` and adds `summand` to the result.
void BigInt::inplaceMultiplyAdd(Digit factor, Digit summand) {
  internalMultiplyAdd(this, factor, summand, digitLength(), this);
}

// Multiplies `multiplicand` with `multiplier` and adds the result to
// `accumulator`, starting at `accumulatorIndex` for the least-significant
// digit.  Callers must ensure that `accumulator`'s digitLength and
// corresponding digit storage is long enough to hold the result.
void BigInt::multiplyAccumulate(BigInt* multiplicand, Digit multiplier,
                                BigInt* accumulator,
                                unsigned accumulatorIndex) {
  MOZ_ASSERT(accumulator->digitLength() >
             multiplicand->digitLength() + accumulatorIndex);
  if (!multiplier) {
    return;
  }

  Digit carry = 0;
  Digit high = 0;
  for (unsigned i = 0; i < multiplicand->digitLength();
       i++, accumulatorIndex++) {
    Digit acc = accumulator->digit(accumulatorIndex);
    Digit newCarry = 0;

    // Add last round's carryovers.
    acc = digitAdd(acc, high, &newCarry);
    acc = digitAdd(acc, carry, &newCarry);

    // Compute this round's multiplication.
    Digit multiplicandDigit = multiplicand->digit(i);
    Digit low = digitMul(multiplier, multiplicandDigit, &high);
    acc = digitAdd(acc, low, &newCarry);

    // Store result and prepare for next round.
    accumulator->setDigit(accumulatorIndex, acc);
    carry = newCarry;
  }

  while (carry || high) {
    MOZ_ASSERT(accumulatorIndex < accumulator->digitLength());
    Digit acc = accumulator->digit(accumulatorIndex);
    Digit newCarry = 0;
    acc = digitAdd(acc, high, &newCarry);
    high = 0;
    acc = digitAdd(acc, carry, &newCarry);
    accumulator->setDigit(accumulatorIndex, acc);
    carry = newCarry;
    accumulatorIndex++;
  }
}

inline int8_t BigInt::absoluteCompare(BigInt* x, BigInt* y) {
  MOZ_ASSERT(!HasLeadingZeroes(x));
  MOZ_ASSERT(!HasLeadingZeroes(y));

  // Sanity checks to catch negative zeroes escaping to the wild.
  MOZ_ASSERT(!x->isNegative() || !x->isZero());
  MOZ_ASSERT(!y->isNegative() || !y->isZero());

  int diff = x->digitLength() - y->digitLength();
  if (diff) {
    return diff < 0 ? -1 : 1;
  }

  int i = x->digitLength() - 1;
  while (i >= 0 && x->digit(i) == y->digit(i)) {
    i--;
  }

  if (i < 0) {
    return 0;
  }

  return x->digit(i) > y->digit(i) ? 1 : -1;
}

BigInt* BigInt::absoluteAdd(JSContext* cx, HandleBigInt x, HandleBigInt y,
                            bool resultNegative) {
  bool swap = x->digitLength() < y->digitLength();
  // Ensure `left` has at least as many digits as `right`.
  HandleBigInt& left = swap ? y : x;
  HandleBigInt& right = swap ? x : y;

  if (left->isZero()) {
    MOZ_ASSERT(right->isZero());
    return left;
  }

  if (right->isZero()) {
    return resultNegative == left->isNegative() ? left : neg(cx, left);
  }

  // Fast path for the likely-common case of up to a uint64_t of magnitude.
  if (left->absFitsInUint64()) {
    MOZ_ASSERT(right->absFitsInUint64());

    uint64_t lhs = left->uint64FromAbsNonZero();
    uint64_t rhs = right->uint64FromAbsNonZero();

    uint64_t res = lhs + rhs;
    bool overflow = res < lhs;
    MOZ_ASSERT(res != 0 || overflow);

    size_t resultLength = 1;
    if (DigitBits == 32) {
      if (overflow) {
        resultLength = 3;
      } else if (res >> 32) {
        resultLength = 2;
      }
    } else {
      if (overflow) {
        resultLength = 2;
      }
    }
    BigInt* result = createUninitialized(cx, resultLength, resultNegative);
    if (!result) {
      return nullptr;
    }
    result->setDigit(0, res);
    if (DigitBits == 32 && resultLength > 1) {
      result->setDigit(1, res >> 32);
    }
    if (overflow) {
      constexpr size_t overflowIndex = DigitBits == 32 ? 2 : 1;
      result->setDigit(overflowIndex, 1);
    }

    MOZ_ASSERT(!HasLeadingZeroes(result));
    return result;
  }

  BigInt* result =
      createUninitialized(cx, left->digitLength() + 1, resultNegative);
  if (!result) {
    return nullptr;
  }
  Digit carry = 0;
  unsigned i = 0;
  for (; i < right->digitLength(); i++) {
    Digit newCarry = 0;
    Digit sum = digitAdd(left->digit(i), right->digit(i), &newCarry);
    sum = digitAdd(sum, carry, &newCarry);
    result->setDigit(i, sum);
    carry = newCarry;
  }

  for (; i < left->digitLength(); i++) {
    Digit newCarry = 0;
    Digit sum = digitAdd(left->digit(i), carry, &newCarry);
    result->setDigit(i, sum);
    carry = newCarry;
  }

  result->setDigit(i, carry);

  return destructivelyTrimHighZeroDigits(cx, result);
}

BigInt* BigInt::absoluteSub(JSContext* cx, HandleBigInt x, HandleBigInt y,
                            bool resultNegative) {
  MOZ_ASSERT(x->digitLength() >= y->digitLength());
  MOZ_ASSERT(absoluteCompare(x, y) > 0);
  MOZ_ASSERT(!x->isZero());

  if (y->isZero()) {
    return resultNegative == x->isNegative() ? x : neg(cx, x);
  }

  // Fast path for the likely-common case of up to a uint64_t of magnitude.
  if (x->absFitsInUint64()) {
    MOZ_ASSERT(y->absFitsInUint64());

    uint64_t lhs = x->uint64FromAbsNonZero();
    uint64_t rhs = y->uint64FromAbsNonZero();
    MOZ_ASSERT(lhs > rhs);

    uint64_t res = lhs - rhs;
    MOZ_ASSERT(res != 0);

    return createFromNonZeroRawUint64(cx, res, resultNegative);
  }

  BigInt* result = createUninitialized(cx, x->digitLength(), resultNegative);
  if (!result) {
    return nullptr;
  }
  Digit borrow = 0;
  unsigned i = 0;
  for (; i < y->digitLength(); i++) {
    Digit newBorrow = 0;
    Digit difference = digitSub(x->digit(i), y->digit(i), &newBorrow);
    difference = digitSub(difference, borrow, &newBorrow);
    result->setDigit(i, difference);
    borrow = newBorrow;
  }

  for (; i < x->digitLength(); i++) {
    Digit newBorrow = 0;
    Digit difference = digitSub(x->digit(i), borrow, &newBorrow);
    result->setDigit(i, difference);
    borrow = newBorrow;
  }

  MOZ_ASSERT(!borrow);
  return destructivelyTrimHighZeroDigits(cx, result);
}

// Divides `x` by `divisor`, returning the result in `quotient` and `remainder`.
// Mathematically, the contract is:
//
//   quotient = (x - remainder) / divisor, with 0 <= remainder < divisor.
//
// If `quotient` is an empty handle, an appropriately sized BigInt will be
// allocated for it; otherwise the caller must ensure that it is big enough.
// `quotient` can be the same as `x` for an in-place division. `quotient` can
// also be `Nothing()` if the caller is only interested in the remainder.
//
// This function returns false if `quotient` is an empty handle, but allocating
// the quotient failed.  Otherwise it returns true, indicating success.
bool BigInt::absoluteDivWithDigitDivisor(
    JSContext* cx, HandleBigInt x, Digit divisor,
    const Maybe<MutableHandleBigInt>& quotient, Digit* remainder,
    bool quotientNegative) {
  MOZ_ASSERT(divisor);

  MOZ_ASSERT(!x->isZero());
  *remainder = 0;
  if (divisor == 1) {
    if (quotient) {
      BigInt* q;
      if (x->isNegative() == quotientNegative) {
        q = x;
      } else {
        q = neg(cx, x);
        if (!q) {
          return false;
        }
      }
      quotient.value().set(q);
    }
    return true;
  }

  unsigned length = x->digitLength();
  if (quotient) {
    if (!quotient.value()) {
      BigInt* q = createUninitialized(cx, length, quotientNegative);
      if (!q) {
        return false;
      }
      quotient.value().set(q);
    }

    for (int i = length - 1; i >= 0; i--) {
      Digit q = digitDiv(*remainder, x->digit(i), divisor, remainder);
      quotient.value()->setDigit(i, q);
    }
  } else {
    for (int i = length - 1; i >= 0; i--) {
      digitDiv(*remainder, x->digit(i), divisor, remainder);
    }
  }

  return true;
}

// Adds `summand` onto `this`, starting with `summand`'s 0th digit
// at `this`'s `startIndex`'th digit. Returns the "carry" (0 or 1).
BigInt::Digit BigInt::absoluteInplaceAdd(BigInt* summand, unsigned startIndex) {
  Digit carry = 0;
  unsigned n = summand->digitLength();
  MOZ_ASSERT(digitLength() > startIndex,
             "must start adding at an in-range digit");
  MOZ_ASSERT(digitLength() - startIndex >= n,
             "digits being added to must not extend above the digits in "
             "this (except for the returned carry digit)");
  for (unsigned i = 0; i < n; i++) {
    Digit newCarry = 0;
    Digit sum = digitAdd(digit(startIndex + i), summand->digit(i), &newCarry);
    sum = digitAdd(sum, carry, &newCarry);
    setDigit(startIndex + i, sum);
    carry = newCarry;
  }

  return carry;
}

// Subtracts `subtrahend` from this, starting with `subtrahend`'s 0th digit
// at `this`'s `startIndex`-th digit. Returns the "borrow" (0 or 1).
BigInt::Digit BigInt::absoluteInplaceSub(BigInt* subtrahend,
                                         unsigned startIndex) {
  Digit borrow = 0;
  unsigned n = subtrahend->digitLength();
  MOZ_ASSERT(digitLength() > startIndex,
             "must start subtracting from an in-range digit");
  MOZ_ASSERT(digitLength() - startIndex >= n,
             "digits being subtracted from must not extend above the "
             "digits in this (except for the returned borrow digit)");
  for (unsigned i = 0; i < n; i++) {
    Digit newBorrow = 0;
    Digit difference =
        digitSub(digit(startIndex + i), subtrahend->digit(i), &newBorrow);
    difference = digitSub(difference, borrow, &newBorrow);
    setDigit(startIndex + i, difference);
    borrow = newBorrow;
  }

  return borrow;
}

// Returns whether (factor1 * factor2) > (high << kDigitBits) + low.
inline bool BigInt::productGreaterThan(Digit factor1, Digit factor2, Digit high,
                                       Digit low) {
  Digit resultHigh;
  Digit resultLow = digitMul(factor1, factor2, &resultHigh);
  return resultHigh > high || (resultHigh == high && resultLow > low);
}

void BigInt::inplaceRightShiftLowZeroBits(unsigned shift) {
  MOZ_ASSERT(shift < DigitBits);
  MOZ_ASSERT(!(digit(0) & ((static_cast<Digit>(1) << shift) - 1)),
             "should only be shifting away zeroes");

  if (!shift) {
    return;
  }

  Digit carry = digit(0) >> shift;
  unsigned last = digitLength() - 1;
  for (unsigned i = 0; i < last; i++) {
    Digit d = digit(i + 1);
    setDigit(i, (d << (DigitBits - shift)) | carry);
    carry = d >> shift;
  }
  setDigit(last, carry);
}

// Always copies the input, even when `shift` == 0.
BigInt* BigInt::absoluteLeftShiftAlwaysCopy(JSContext* cx, HandleBigInt x,
                                            unsigned shift,
                                            LeftShiftMode mode) {
  MOZ_ASSERT(shift < DigitBits);
  MOZ_ASSERT(!x->isZero());

  unsigned n = x->digitLength();
  unsigned resultLength = mode == LeftShiftMode::AlwaysAddOneDigit ? n + 1 : n;
  BigInt* result = createUninitialized(cx, resultLength, x->isNegative());
  if (!result) {
    return nullptr;
  }

  if (!shift) {
    for (unsigned i = 0; i < n; i++) {
      result->setDigit(i, x->digit(i));
    }
    if (mode == LeftShiftMode::AlwaysAddOneDigit) {
      result->setDigit(n, 0);
    }

    return result;
  }

  Digit carry = 0;
  for (unsigned i = 0; i < n; i++) {
    Digit d = x->digit(i);
    result->setDigit(i, (d << shift) | carry);
    carry = d >> (DigitBits - shift);
  }

  if (mode == LeftShiftMode::AlwaysAddOneDigit) {
    result->setDigit(n, carry);
  } else {
    MOZ_ASSERT(mode == LeftShiftMode::SameSizeResult);
    MOZ_ASSERT(!carry);
  }

  return result;
}

// Divides `dividend` by `divisor`, returning the result in `quotient` and
// `remainder`. Mathematically, the contract is:
//
//   quotient = (dividend - remainder) / divisor, with 0 <= remainder < divisor.
//
// Both `quotient` and `remainder` are optional, for callers that are only
// interested in one of them.  See Knuth, Volume 2, section 4.3.1, Algorithm D.
// Also see the overview of the algorithm by Jan Marthedal Rasmussen over at
// https://janmr.com/blog/2014/04/basic-multiple-precision-long-division/.
bool BigInt::absoluteDivWithBigIntDivisor(
    JSContext* cx, HandleBigInt dividend, HandleBigInt divisor,
    const Maybe<MutableHandleBigInt>& quotient,
    const Maybe<MutableHandleBigInt>& remainder, bool isNegative) {
  MOZ_ASSERT(divisor->digitLength() >= 2);
  MOZ_ASSERT(dividend->digitLength() >= divisor->digitLength());

  // Any early error return is detectable by checking the quotient and/or
  // remainder output values.
  MOZ_ASSERT(!quotient || !quotient.value());
  MOZ_ASSERT(!remainder || !remainder.value());

  // The unusual variable names inside this function are consistent with
  // Knuth's book, as well as with Go's implementation of this algorithm.
  // Maintaining this consistency is probably more useful than trying to
  // come up with more descriptive names for them.
  const unsigned n = divisor->digitLength();
  const unsigned m = dividend->digitLength() - n;

  // The quotient to be computed.
  RootedBigInt q(cx);
  if (quotient) {
    q = createUninitialized(cx, m + 1, isNegative);
    if (!q) {
      return false;
    }
  }

  // In each iteration, `qhatv` holds `divisor` * `current quotient digit`.
  // "v" is the book's name for `divisor`, `qhat` the current quotient digit.
  RootedBigInt qhatv(cx, createUninitialized(cx, n + 1, isNegative));
  if (!qhatv) {
    return false;
  }

  // D1.
  // Left-shift inputs so that the divisor's MSB is set. This is necessary to
  // prevent the digit-wise divisions (see digitDiv call below) from
  // overflowing (they take a two digits wide input, and return a one digit
  // result).
  Digit lastDigit = divisor->digit(n - 1);
  unsigned shift = DigitLeadingZeroes(lastDigit);

  RootedBigInt shiftedDivisor(cx);
  if (shift > 0) {
    shiftedDivisor = absoluteLeftShiftAlwaysCopy(cx, divisor, shift,
                                                 LeftShiftMode::SameSizeResult);
    if (!shiftedDivisor) {
      return false;
    }
  } else {
    shiftedDivisor = divisor;
  }

  // Holds the (continuously updated) remaining part of the dividend, which
  // eventually becomes the remainder.
  RootedBigInt u(cx,
                 absoluteLeftShiftAlwaysCopy(cx, dividend, shift,
                                             LeftShiftMode::AlwaysAddOneDigit));
  if (!u) {
    return false;
  }

  // D2.
  // Iterate over the dividend's digit (like the "grade school" algorithm).
  // `vn1` is the divisor's most significant digit.
  Digit vn1 = shiftedDivisor->digit(n - 1);
  for (int j = m; j >= 0; j--) {
    // D3.
    // Estimate the current iteration's quotient digit (see Knuth for details).
    // `qhat` is the current quotient digit.
    Digit qhat = std::numeric_limits<Digit>::max();

    // `ujn` is the dividend's most significant remaining digit.
    Digit ujn = u->digit(j + n);
    if (ujn != vn1) {
      // `rhat` is the current iteration's remainder.
      Digit rhat = 0;
      // Estimate the current quotient digit by dividing the most significant
      // digits of dividend and divisor. The result will not be too small,
      // but could be a bit too large.
      qhat = digitDiv(ujn, u->digit(j + n - 1), vn1, &rhat);

      // Decrement the quotient estimate as needed by looking at the next
      // digit, i.e. by testing whether
      // qhat * v_{n-2} > (rhat << DigitBits) + u_{j+n-2}.
      Digit vn2 = shiftedDivisor->digit(n - 2);
      Digit ujn2 = u->digit(j + n - 2);
      while (productGreaterThan(qhat, vn2, rhat, ujn2)) {
        qhat--;
        Digit prevRhat = rhat;
        rhat += vn1;
        // v[n-1] >= 0, so this tests for overflow.
        if (rhat < prevRhat) {
          break;
        }
      }
    }

    // D4.
    // Multiply the divisor with the current quotient digit, and subtract
    // it from the dividend. If there was "borrow", then the quotient digit
    // was one too high, so we must correct it and undo one subtraction of
    // the (shifted) divisor.
    internalMultiplyAdd(shiftedDivisor, qhat, 0, n, qhatv);
    Digit c = u->absoluteInplaceSub(qhatv, j);
    if (c) {
      c = u->absoluteInplaceAdd(shiftedDivisor, j);
      u->setDigit(j + n, u->digit(j + n) + c);
      qhat--;
    }

    if (quotient) {
      q->setDigit(j, qhat);
    }
  }

  if (quotient) {
    BigInt* bi = destructivelyTrimHighZeroDigits(cx, q);
    if (!bi) {
      return false;
    }
    quotient.value().set(q);
  }

  if (remainder) {
    u->inplaceRightShiftLowZeroBits(shift);
    remainder.value().set(u);
  }

  return true;
}

// Helper for Absolute{And,AndNot,Or,Xor}.
// Performs the given binary `op` on digit pairs of `x` and `y`; when the
// end of the shorter of the two is reached, `kind` configures how
// remaining digits are handled.
// Example:
//       y:             [ y2 ][ y1 ][ y0 ]
//       x:       [ x3 ][ x2 ][ x1 ][ x0 ]
//                   |     |     |     |
//                (Fill)  (op)  (op)  (op)
//                   |     |     |     |
//                   v     v     v     v
//  result: [  0 ][ x3 ][ r2 ][ r1 ][ r0 ]
template <BigInt::BitwiseOpKind kind, typename BitwiseOp>
inline BigInt* BigInt::absoluteBitwiseOp(JSContext* cx, HandleBigInt x,
                                         HandleBigInt y, BitwiseOp&& op) {
  unsigned xLength = x->digitLength();
  unsigned yLength = y->digitLength();
  unsigned numPairs = std::min(xLength, yLength);
  unsigned resultLength;
  if (kind == BitwiseOpKind::SymmetricTrim) {
    resultLength = numPairs;
  } else if (kind == BitwiseOpKind::SymmetricFill) {
    resultLength = std::max(xLength, yLength);
  } else {
    MOZ_ASSERT(kind == BitwiseOpKind::AsymmetricFill);
    resultLength = xLength;
  }
  bool resultNegative = false;

  BigInt* result = createUninitialized(cx, resultLength, resultNegative);
  if (!result) {
    return nullptr;
  }

  unsigned i = 0;
  for (; i < numPairs; i++) {
    result->setDigit(i, op(x->digit(i), y->digit(i)));
  }

  if (kind != BitwiseOpKind::SymmetricTrim) {
    BigInt* source = kind == BitwiseOpKind::AsymmetricFill ? x
                     : xLength == i                        ? y
                                                           : x;
    for (; i < resultLength; i++) {
      result->setDigit(i, source->digit(i));
    }
  }

  MOZ_ASSERT(i == resultLength);

  return destructivelyTrimHighZeroDigits(cx, result);
}

BigInt* BigInt::absoluteAnd(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  return absoluteBitwiseOp<BitwiseOpKind::SymmetricTrim>(cx, x, y,
                                                         std::bit_and<Digit>());
}

BigInt* BigInt::absoluteOr(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  return absoluteBitwiseOp<BitwiseOpKind::SymmetricFill>(cx, x, y,
                                                         std::bit_or<Digit>());
}

BigInt* BigInt::absoluteAndNot(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  auto digitOperation = [](Digit a, Digit b) { return a & ~b; };
  return absoluteBitwiseOp<BitwiseOpKind::AsymmetricFill>(cx, x, y,
                                                          digitOperation);
}

BigInt* BigInt::absoluteXor(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  return absoluteBitwiseOp<BitwiseOpKind::SymmetricFill>(cx, x, y,
                                                         std::bit_xor<Digit>());
}

BigInt* BigInt::absoluteAddOne(JSContext* cx, HandleBigInt x,
                               bool resultNegative) {
  unsigned inputLength = x->digitLength();
  // The addition will overflow into a new digit if all existing digits are
  // at maximum.
  bool willOverflow = true;
  for (unsigned i = 0; i < inputLength; i++) {
    if (std::numeric_limits<Digit>::max() != x->digit(i)) {
      willOverflow = false;
      break;
    }
  }

  unsigned resultLength = inputLength + willOverflow;
  BigInt* result = createUninitialized(cx, resultLength, resultNegative);
  if (!result) {
    return nullptr;
  }

  Digit carry = 1;
  for (unsigned i = 0; i < inputLength; i++) {
    Digit newCarry = 0;
    result->setDigit(i, digitAdd(x->digit(i), carry, &newCarry));
    carry = newCarry;
  }
  if (resultLength > inputLength) {
    MOZ_ASSERT(carry == 1);
    result->setDigit(inputLength, 1);
  } else {
    MOZ_ASSERT(!carry);
  }

  return destructivelyTrimHighZeroDigits(cx, result);
}

BigInt* BigInt::absoluteSubOne(JSContext* cx, HandleBigInt x,
                               bool resultNegative) {
  MOZ_ASSERT(!x->isZero());

  unsigned length = x->digitLength();

  if (length == 1) {
    Digit d = x->digit(0);
    if (d == 1) {
      // Ignore resultNegative.
      return zero(cx);
    }
    return createFromDigit(cx, d - 1, resultNegative);
  }

  BigInt* result = createUninitialized(cx, length, resultNegative);
  if (!result) {
    return nullptr;
  }

  Digit borrow = 1;
  for (unsigned i = 0; i < length; i++) {
    Digit newBorrow = 0;
    result->setDigit(i, digitSub(x->digit(i), borrow, &newBorrow));
    borrow = newBorrow;
  }
  MOZ_ASSERT(!borrow);

  return destructivelyTrimHighZeroDigits(cx, result);
}

BigInt* BigInt::inc(JSContext* cx, HandleBigInt x) {
  if (x->isZero()) {
    return one(cx);
  }

  bool isNegative = x->isNegative();
  if (isNegative) {
    return absoluteSubOne(cx, x, isNegative);
  }

  return absoluteAddOne(cx, x, isNegative);
}

BigInt* BigInt::dec(JSContext* cx, HandleBigInt x) {
  if (x->isZero()) {
    return negativeOne(cx);
  }

  bool isNegative = x->isNegative();
  if (isNegative) {
    return absoluteAddOne(cx, x, isNegative);
  }

  return absoluteSubOne(cx, x, isNegative);
}

// Lookup table for the maximum number of bits required per character of a
// base-N string representation of a number. To increase accuracy, the array
// value is the actual value multiplied by 32. To generate this table:
// for (var i = 0; i <= 36; i++) { print(Math.ceil(Math.log2(i) * 32) + ","); }
static constexpr uint8_t maxBitsPerCharTable[] = {
    0,   0,   32,  51,  64,  75,  83,  90,  96,  // 0..8
    102, 107, 111, 115, 119, 122, 126, 128,      // 9..16
    131, 134, 136, 139, 141, 143, 145, 147,      // 17..24
    149, 151, 153, 154, 156, 158, 159, 160,      // 25..32
    162, 163, 165, 166,                          // 33..36
};

static constexpr unsigned bitsPerCharTableShift = 5;
static constexpr size_t bitsPerCharTableMultiplier = 1u
                                                     << bitsPerCharTableShift;
static constexpr char radixDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

static inline uint64_t CeilDiv(uint64_t numerator, uint64_t denominator) {
  MOZ_ASSERT(numerator != 0);
  return 1 + (numerator - 1) / denominator;
};

// Compute (an overapproximation of) the length of the string representation of
// a BigInt.  In base B an X-digit number has maximum value:
//
//   B**X - 1
//
// We're trying to find N for an N-digit number in base |radix| full
// representing a |bitLength|-digit number in base 2, so we have:
//
//   radix**N - 1 ≥ 2**bitLength - 1
//   radix**N ≥ 2**bitLength
//   N ≥ log2(2**bitLength) / log2(radix)
//   N ≥ bitLength / log2(radix)
//
// so the smallest N is:
//
//   N = ⌈bitLength / log2(radix)⌉
//
// We want to avoid floating-point computations and precompute the logarithm, so
// we multiply both sides of the division by |bitsPerCharTableMultiplier|:
//
//   N = ⌈(bPCTM * bitLength) / (bPCTM * log2(radix))⌉
//
// and then because |maxBitsPerChar| representing the denominator may have been
// rounded *up* -- which could produce an overall under-computation -- we reduce
// by one to undo any rounding and conservatively compute:
//
//   N ≥ ⌈(bPCTM * bitLength) / (maxBitsPerChar - 1)⌉
//
size_t BigInt::calculateMaximumCharactersRequired(HandleBigInt x,
                                                  unsigned radix) {
  MOZ_ASSERT(!x->isZero());
  MOZ_ASSERT(radix >= 2 && radix <= 36);

  size_t length = x->digitLength();
  Digit lastDigit = x->digit(length - 1);
  size_t bitLength = length * DigitBits - DigitLeadingZeroes(lastDigit);

  uint8_t maxBitsPerChar = maxBitsPerCharTable[radix];
  uint64_t maximumCharactersRequired =
      CeilDiv(static_cast<uint64_t>(bitsPerCharTableMultiplier) * bitLength,
              maxBitsPerChar - 1);
  maximumCharactersRequired += x->isNegative();

  return AssertedCast<size_t>(maximumCharactersRequired);
}

template <AllowGC allowGC>
JSLinearString* BigInt::toStringBasePowerOfTwo(JSContext* cx, HandleBigInt x,
                                               unsigned radix) {
  MOZ_ASSERT(mozilla::IsPowerOfTwo(radix));
  MOZ_ASSERT(radix >= 2 && radix <= 32);
  MOZ_ASSERT(!x->isZero());

  const unsigned length = x->digitLength();
  const bool sign = x->isNegative();
  const unsigned bitsPerChar = mozilla::CountTrailingZeroes32(radix);
  const unsigned charMask = radix - 1;
  // Compute the length of the resulting string: divide the bit length of the
  // BigInt by the number of bits representable per character (rounding up).
  const Digit msd = x->digit(length - 1);

  const size_t bitLength = length * DigitBits - DigitLeadingZeroes(msd);
  const size_t charsRequired = CeilDiv(bitLength, bitsPerChar) + sign;

  if (charsRequired > JSString::MAX_LENGTH) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  auto resultChars = cx->make_pod_array<char>(charsRequired);
  if (!resultChars) {
    return nullptr;
  }

  Digit digit = 0;
  // Keeps track of how many unprocessed bits there are in |digit|.
  unsigned availableBits = 0;
  size_t pos = charsRequired;
  for (unsigned i = 0; i < length - 1; i++) {
    Digit newDigit = x->digit(i);
    // Take any leftover bits from the last iteration into account.
    unsigned current = (digit | (newDigit << availableBits)) & charMask;
    MOZ_ASSERT(pos);
    resultChars[--pos] = radixDigits[current];
    unsigned consumedBits = bitsPerChar - availableBits;
    digit = newDigit >> consumedBits;
    availableBits = DigitBits - consumedBits;
    while (availableBits >= bitsPerChar) {
      MOZ_ASSERT(pos);
      resultChars[--pos] = radixDigits[digit & charMask];
      digit >>= bitsPerChar;
      availableBits -= bitsPerChar;
    }
  }

  // Write out the character containing the lowest-order bit of |msd|.
  //
  // This character may include leftover bits from the Digit below |msd|.  For
  // example, if |x === 2n**64n| and |radix == 32|: the preceding loop writes
  // twelve zeroes for low-order bits 0-59 in |x->digit(0)| (and |x->digit(1)|
  // on 32-bit); then the highest 4 bits of of |x->digit(0)| (or |x->digit(1)|
  // on 32-bit) and bit 0 of |x->digit(1)| (|x->digit(2)| on 32-bit) will
  // comprise the |current == 0b1'0000| computed below for the high-order 'g'
  // character.
  unsigned current = (digit | (msd << availableBits)) & charMask;
  MOZ_ASSERT(pos);
  resultChars[--pos] = radixDigits[current];

  // Write out remaining characters represented by |msd|.  (There may be none,
  // as in the example above.)
  digit = msd >> (bitsPerChar - availableBits);
  while (digit != 0) {
    MOZ_ASSERT(pos);
    resultChars[--pos] = radixDigits[digit & charMask];
    digit >>= bitsPerChar;
  }

  if (sign) {
    MOZ_ASSERT(pos);
    resultChars[--pos] = '-';
  }

  MOZ_ASSERT(pos == 0);
  return NewStringCopyN<allowGC>(cx, resultChars.get(), charsRequired);
}

template <AllowGC allowGC>
JSLinearString* BigInt::toStringSingleDigitBaseTen(JSContext* cx, Digit digit,
                                                   bool isNegative) {
  if (digit <= Digit(INT32_MAX)) {
    int32_t val = AssertedCast<int32_t>(digit);
    return Int32ToString<allowGC>(cx, isNegative ? -val : val);
  }

  MOZ_ASSERT(digit != 0, "zero case should have been handled in toString");

  constexpr size_t maxLength = 1 + (std::numeric_limits<Digit>::digits10 + 1);
  static_assert(maxLength == 11 || maxLength == 21,
                "unexpected decimal string length");

  char resultChars[maxLength];
  size_t writePos = maxLength;

  while (digit != 0) {
    MOZ_ASSERT(writePos > 0);
    resultChars[--writePos] = radixDigits[digit % 10];
    digit /= 10;
  }
  MOZ_ASSERT(writePos < maxLength);
  MOZ_ASSERT(resultChars[writePos] != '0');

  if (isNegative) {
    MOZ_ASSERT(writePos > 0);
    resultChars[--writePos] = '-';
  }

  MOZ_ASSERT(writePos < maxLength);
  return NewStringCopyN<allowGC>(cx, resultChars + writePos,
                                 maxLength - writePos);
}

static constexpr BigInt::Digit MaxPowerInDigit(uint8_t radix) {
  BigInt::Digit result = 1;
  while (result < BigInt::Digit(-1) / radix) {
    result *= radix;
  }
  return result;
}

static constexpr uint8_t MaxExponentInDigit(uint8_t radix) {
  uint8_t exp = 0;
  BigInt::Digit result = 1;
  while (result < BigInt::Digit(-1) / radix) {
    result *= radix;
    exp += 1;
  }
  return exp;
}

struct RadixInfo {
  BigInt::Digit maxPowerInDigit;
  uint8_t maxExponentInDigit;

  constexpr RadixInfo(BigInt::Digit maxPower, uint8_t maxExponent)
      : maxPowerInDigit(maxPower), maxExponentInDigit(maxExponent) {}

  explicit constexpr RadixInfo(uint8_t radix)
      : RadixInfo(MaxPowerInDigit(radix), MaxExponentInDigit(radix)) {}
};

static constexpr const RadixInfo toStringInfo[37] = {
    {0, 0},        {0, 0},        RadixInfo(2),  RadixInfo(3),  RadixInfo(4),
    RadixInfo(5),  RadixInfo(6),  RadixInfo(7),  RadixInfo(8),  RadixInfo(9),
    RadixInfo(10), RadixInfo(11), RadixInfo(12), RadixInfo(13), RadixInfo(14),
    RadixInfo(15), RadixInfo(16), RadixInfo(17), RadixInfo(18), RadixInfo(19),
    RadixInfo(20), RadixInfo(21), RadixInfo(22), RadixInfo(23), RadixInfo(24),
    RadixInfo(25), RadixInfo(26), RadixInfo(27), RadixInfo(28), RadixInfo(29),
    RadixInfo(30), RadixInfo(31), RadixInfo(32), RadixInfo(33), RadixInfo(34),
    RadixInfo(35), RadixInfo(36),
};

JSLinearString* BigInt::toStringGeneric(JSContext* cx, HandleBigInt x,
                                        unsigned radix) {
  MOZ_ASSERT(radix >= 2 && radix <= 36);
  MOZ_ASSERT(!x->isZero());

  size_t maximumCharactersRequired =
      calculateMaximumCharactersRequired(x, radix);
  if (maximumCharactersRequired > JSString::MAX_LENGTH) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  UniqueChars resultString(js_pod_malloc<char>(maximumCharactersRequired));
  if (!resultString) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  size_t writePos = maximumCharactersRequired;
  unsigned length = x->digitLength();
  Digit lastDigit;
  if (length == 1) {
    lastDigit = x->digit(0);
  } else {
    unsigned chunkChars = toStringInfo[radix].maxExponentInDigit;
    Digit chunkDivisor = toStringInfo[radix].maxPowerInDigit;

    unsigned nonZeroDigit = length - 1;
    MOZ_ASSERT(x->digit(nonZeroDigit) != 0);

    // `rest` holds the part of the BigInt that we haven't looked at yet.
    // Not to be confused with "remainder"!
    RootedBigInt rest(cx);

    // In the first round, divide the input, allocating a new BigInt for
    // the result == rest; from then on divide the rest in-place.
    //
    // FIXME: absoluteDivWithDigitDivisor doesn't
    // destructivelyTrimHighZeroDigits for in-place divisions, leading to
    // worse constant factors.  See
    // https://bugzilla.mozilla.org/show_bug.cgi?id=1510213.
    RootedBigInt dividend(cx, x);
    do {
      Digit chunk;
      if (!absoluteDivWithDigitDivisor(cx, dividend, chunkDivisor, Some(&rest),
                                       &chunk, dividend->isNegative())) {
        return nullptr;
      }

      dividend = rest;
      for (unsigned i = 0; i < chunkChars; i++) {
        MOZ_ASSERT(writePos > 0);
        resultString[--writePos] = radixDigits[chunk % radix];
        chunk /= radix;
      }
      MOZ_ASSERT(!chunk);

      if (!rest->digit(nonZeroDigit)) {
        nonZeroDigit--;
      }

      MOZ_ASSERT(rest->digit(nonZeroDigit) != 0,
                 "division by a single digit can't remove more than one "
                 "digit from a number");
    } while (nonZeroDigit > 0);

    lastDigit = rest->digit(0);
  }

  do {
    MOZ_ASSERT(writePos > 0);
    resultString[--writePos] = radixDigits[lastDigit % radix];
    lastDigit /= radix;
  } while (lastDigit > 0);
  MOZ_ASSERT(writePos < maximumCharactersRequired);
  MOZ_ASSERT(maximumCharactersRequired - writePos <=
             static_cast<size_t>(maximumCharactersRequired));

  // Remove leading zeroes.
  while (writePos + 1 < maximumCharactersRequired &&
         resultString[writePos] == '0') {
    writePos++;
  }

  if (x->isNegative()) {
    MOZ_ASSERT(writePos > 0);
    resultString[--writePos] = '-';
  }

  MOZ_ASSERT(writePos < maximumCharactersRequired);
  // Would be better to somehow adopt resultString directly.
  return NewStringCopyN<CanGC>(cx, resultString.get() + writePos,
                               maximumCharactersRequired - writePos);
}

static void FreeDigits(JSContext* cx, BigInt* bi, BigInt::Digit* digits,
                       size_t nbytes) {
  if (cx->isHelperThreadContext()) {
    js_free(digits);
  } else if (bi->isTenured()) {
    MOZ_ASSERT(!cx->nursery().isInside(digits));
    js_free(digits);
  } else {
    cx->nursery().freeBuffer(digits, nbytes);
  }
}

BigInt* BigInt::destructivelyTrimHighZeroDigits(JSContext* cx, BigInt* x) {
  if (x->isZero()) {
    MOZ_ASSERT(!x->isNegative());
    return x;
  }
  MOZ_ASSERT(x->digitLength());

  int nonZeroIndex = x->digitLength() - 1;
  while (nonZeroIndex >= 0 && x->digit(nonZeroIndex) == 0) {
    nonZeroIndex--;
  }

  if (nonZeroIndex < 0) {
    return zero(cx);
  }

  if (nonZeroIndex == static_cast<int>(x->digitLength() - 1)) {
    return x;
  }

  unsigned newLength = nonZeroIndex + 1;

  if (newLength > InlineDigitsLength) {
    MOZ_ASSERT(x->hasHeapDigits());

    size_t oldLength = x->digitLength();
    Digit* newdigits =
        js::ReallocateBigIntDigits(cx, x, x->heapDigits_, oldLength, newLength);
    if (!newdigits) {
      return nullptr;
    }
    x->heapDigits_ = newdigits;

    RemoveCellMemory(x, oldLength * sizeof(Digit), js::MemoryUse::BigIntDigits);
    AddCellMemory(x, newLength * sizeof(Digit), js::MemoryUse::BigIntDigits);
  } else {
    if (x->hasHeapDigits()) {
      Digit digits[InlineDigitsLength];
      std::copy_n(x->heapDigits_, InlineDigitsLength, digits);

      size_t nbytes = x->digitLength() * sizeof(Digit);
      FreeDigits(cx, x, x->heapDigits_, nbytes);
      RemoveCellMemory(x, nbytes, js::MemoryUse::BigIntDigits);

      std::copy_n(digits, InlineDigitsLength, x->inlineDigits_);
    }
  }

  x->setLengthAndFlags(newLength, x->isNegative() ? SignBit : 0);

  return x;
}

// The maximum value `radix**charCount - 1` must be represented as a max number
// `2**(N * DigitBits) - 1` for `N` digits, so
//
//   2**(N * DigitBits) - 1 ≥ radix**charcount - 1
//   2**(N * DigitBits) ≥ radix**charcount
//   N * DigitBits ≥ log2(radix**charcount)
//   N * DigitBits ≥ charcount * log2(radix)
//   N ≥ ⌈charcount * log2(radix) / DigitBits⌉ (conservatively)
//
// or in the code's terms (all numbers promoted to exact mathematical values),
//
//   N ≥ ⌈charcount * bitsPerChar / (DigitBits * bitsPerCharTableMultiplier)⌉
//
// Note that `N` is computed even more conservatively here because `bitsPerChar`
// is rounded up.
bool BigInt::calculateMaximumDigitsRequired(JSContext* cx, uint8_t radix,
                                            size_t charcount, size_t* result) {
  MOZ_ASSERT(2 <= radix && radix <= 36);

  uint8_t bitsPerChar = maxBitsPerCharTable[radix];

  MOZ_ASSERT(charcount > 0);
  MOZ_ASSERT(charcount <= std::numeric_limits<uint64_t>::max() / bitsPerChar);
  static_assert(
      MaxDigitLength < std::numeric_limits<size_t>::max(),
      "can't safely cast calculateMaximumDigitsRequired result to size_t");

  uint64_t n = CeilDiv(static_cast<uint64_t>(charcount) * bitsPerChar,
                       DigitBits * bitsPerCharTableMultiplier);
  if (n > MaxDigitLength) {
    ReportOutOfMemory(cx);
    return false;
  }

  *result = n;
  return true;
}

template <typename CharT>
BigInt* BigInt::parseLiteralDigits(JSContext* cx,
                                   const Range<const CharT> chars,
                                   unsigned radix, bool isNegative,
                                   bool* haveParseError, gc::InitialHeap heap) {
  static_assert(
      std::is_same_v<CharT, JS::Latin1Char> || std::is_same_v<CharT, char16_t>,
      "only the bare minimum character types are supported, to avoid "
      "excessively instantiating this template");

  MOZ_ASSERT(chars.length());

  RangedPtr<const CharT> start = chars.begin();
  RangedPtr<const CharT> end = chars.end();

  // Skipping leading zeroes.
  while (start[0] == '0') {
    start++;
    if (start == end) {
      return zero(cx, heap);
    }
  }

  unsigned limit0 = '0' + std::min(radix, 10u);
  unsigned limita = 'a' + (radix - 10);
  unsigned limitA = 'A' + (radix - 10);

  size_t length;
  if (!calculateMaximumDigitsRequired(cx, radix, end - start, &length)) {
    return nullptr;
  }
  BigInt* result = createUninitialized(cx, length, isNegative, heap);
  if (!result) {
    return nullptr;
  }

  result->initializeDigitsToZero();

  for (; start < end; start++) {
    uint32_t digit;
    CharT c = *start;
    if (c >= '0' && c < limit0) {
      digit = c - '0';
    } else if (c >= 'a' && c < limita) {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c < limitA) {
      digit = c - 'A' + 10;
    } else {
      *haveParseError = true;
      return nullptr;
    }

    result->inplaceMultiplyAdd(static_cast<Digit>(radix),
                               static_cast<Digit>(digit));
  }

  return destructivelyTrimHighZeroDigits(cx, result);
}

// BigInt proposal section 7.2
template <typename CharT>
BigInt* BigInt::parseLiteral(JSContext* cx, const Range<const CharT> chars,
                             bool* haveParseError) {
  RangedPtr<const CharT> start = chars.begin();
  const RangedPtr<const CharT> end = chars.end();
  bool isNegative = false;

  MOZ_ASSERT(chars.length());

  // This function is only called from the frontend when parsing BigInts. Parsed
  // BigInts are stored in the script's data vector and therefore need to be
  // allocated in the tenured heap.
  constexpr gc::InitialHeap heap = gc::TenuredHeap;

  if (end - start > 2 && start[0] == '0') {
    if (start[1] == 'b' || start[1] == 'B') {
      // StringNumericLiteral ::: BinaryIntegerLiteral
      return parseLiteralDigits(cx, Range<const CharT>(start + 2, end), 2,
                                isNegative, haveParseError, heap);
    }
    if (start[1] == 'x' || start[1] == 'X') {
      // StringNumericLiteral ::: HexIntegerLiteral
      return parseLiteralDigits(cx, Range<const CharT>(start + 2, end), 16,
                                isNegative, haveParseError, heap);
    }
    if (start[1] == 'o' || start[1] == 'O') {
      // StringNumericLiteral ::: OctalIntegerLiteral
      return parseLiteralDigits(cx, Range<const CharT>(start + 2, end), 8,
                                isNegative, haveParseError, heap);
    }
  }

  return parseLiteralDigits(cx, Range<const CharT>(start, end), 10, isNegative,
                            haveParseError, heap);
}

template <typename CharT>
bool BigInt::literalIsZeroNoRadix(const Range<const CharT> chars) {
  MOZ_ASSERT(chars.length());

  RangedPtr<const CharT> start = chars.begin();
  RangedPtr<const CharT> end = chars.end();

  // Skipping leading zeroes.
  while (start[0] == '0') {
    start++;
    if (start == end) {
      return true;
    }
  }

  return false;
}

// trim and remove radix selection prefix.
template <typename CharT>
bool BigInt::literalIsZero(const Range<const CharT> chars) {
  RangedPtr<const CharT> start = chars.begin();
  const RangedPtr<const CharT> end = chars.end();

  MOZ_ASSERT(chars.length());

  // Skip over radix selector.
  if (end - start > 2 && start[0] == '0') {
    if (start[1] == 'b' || start[1] == 'B' || start[1] == 'x' ||
        start[1] == 'X' || start[1] == 'o' || start[1] == 'O') {
      return literalIsZeroNoRadix(Range<const CharT>(start + 2, end));
    }
  }

  return literalIsZeroNoRadix(Range<const CharT>(start, end));
}

template bool BigInt::literalIsZero(const Range<const char16_t> chars);

BigInt* BigInt::createFromDouble(JSContext* cx, double d) {
  MOZ_ASSERT(IsInteger(d), "Only integer-valued doubles can convert to BigInt");

  if (d == 0) {
    return zero(cx);
  }

  int exponent = mozilla::ExponentComponent(d);
  MOZ_ASSERT(exponent >= 0);
  int length = exponent / DigitBits + 1;
  BigInt* result = createUninitialized(cx, length, d < 0);
  if (!result) {
    return nullptr;
  }

  // We construct a BigInt from the double `d` by shifting its mantissa
  // according to its exponent and mapping the bit pattern onto digits.
  //
  //               <----------- bitlength = exponent + 1 ----------->
  //                <----- 52 ------> <------ trailing zeroes ------>
  // mantissa:     1yyyyyyyyyyyyyyyyy 0000000000000000000000000000000
  // digits:    0001xxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
  //                <-->          <------>
  //          msdTopBits          DigitBits
  //
  using Double = mozilla::FloatingPoint<double>;
  uint64_t mantissa =
      mozilla::BitwiseCast<uint64_t>(d) & Double::kSignificandBits;
  // Add implicit high bit.
  mantissa |= 1ull << Double::kSignificandWidth;

  const int mantissaTopBit = Double::kSignificandWidth;  // 0-indexed.

  // 0-indexed position of `d`'s most significant bit within the `msd`.
  int msdTopBit = exponent % DigitBits;

  // Next digit under construction.
  Digit digit;

  // First, build the MSD by shifting the mantissa appropriately.
  if (msdTopBit < mantissaTopBit) {
    int remainingMantissaBits = mantissaTopBit - msdTopBit;
    digit = mantissa >> remainingMantissaBits;
    mantissa = mantissa << (64 - remainingMantissaBits);
  } else {
    MOZ_ASSERT(msdTopBit >= mantissaTopBit);
    digit = mantissa << (msdTopBit - mantissaTopBit);
    mantissa = 0;
  }
  MOZ_ASSERT(digit != 0, "most significant digit should not be zero");
  result->setDigit(--length, digit);

  // Fill in digits containing mantissa contributions.
  while (mantissa) {
    MOZ_ASSERT(length > 0,
               "double bits were all non-fractional, so there must be "
               "digits present to hold them");

    if (DigitBits == 64) {
      result->setDigit(--length, mantissa);
      break;
    }

    MOZ_ASSERT(DigitBits == 32);
    Digit current = mantissa >> 32;
    mantissa = mantissa << 32;
    result->setDigit(--length, current);
  }

  // Fill in low-order zeroes.
  for (int i = length - 1; i >= 0; i--) {
    result->setDigit(i, 0);
  }

  return result;
}

BigInt* BigInt::createFromUint64(JSContext* cx, uint64_t n) {
  if (n == 0) {
    return zero(cx);
  }

  const bool isNegative = false;

  if (DigitBits == 32) {
    Digit low = n;
    Digit high = n >> 32;
    size_t length = high ? 2 : 1;

    BigInt* res = createUninitialized(cx, length, isNegative);
    if (!res) {
      return nullptr;
    }
    res->setDigit(0, low);
    if (high) {
      res->setDigit(1, high);
    }
    return res;
  }

  return createFromDigit(cx, n, isNegative);
}

BigInt* BigInt::createFromInt64(JSContext* cx, int64_t n) {
  BigInt* res = createFromUint64(cx, Abs(n));
  if (!res) {
    return nullptr;
  }

  if (n < 0) {
    res->setHeaderFlagBit(SignBit);
  }
  MOZ_ASSERT(res->isNegative() == (n < 0));

  return res;
}

// BigInt proposal section 5.1.2
BigInt* js::NumberToBigInt(JSContext* cx, double d) {
  // Step 1 is an assertion checked by the caller.
  // Step 2.
  if (!IsInteger(d)) {
    char str[JS::MaximumNumberToStringLength];
    JS::NumberToString(d, str);

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NONINTEGER_NUMBER_TO_BIGINT, str);
    return nullptr;
  }

  // Step 3.
  return BigInt::createFromDouble(cx, d);
}

BigInt* BigInt::copy(JSContext* cx, HandleBigInt x, gc::InitialHeap heap) {
  if (x->isZero()) {
    return zero(cx, heap);
  }

  BigInt* result =
      createUninitialized(cx, x->digitLength(), x->isNegative(), heap);
  if (!result) {
    return nullptr;
  }
  for (size_t i = 0; i < x->digitLength(); i++) {
    result->setDigit(i, x->digit(i));
  }
  return result;
}

// BigInt proposal section 1.1.7
BigInt* BigInt::add(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  bool xNegative = x->isNegative();

  // x + y == x + y
  // -x + -y == -(x + y)
  if (xNegative == y->isNegative()) {
    return absoluteAdd(cx, x, y, xNegative);
  }

  // x + -y == x - y == -(y - x)
  // -x + y == y - x == -(x - y)
  int8_t compare = absoluteCompare(x, y);
  if (compare == 0) {
    return zero(cx);
  }

  if (compare > 0) {
    return absoluteSub(cx, x, y, xNegative);
  }

  return absoluteSub(cx, y, x, !xNegative);
}

// BigInt proposal section 1.1.8
BigInt* BigInt::sub(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  bool xNegative = x->isNegative();
  if (xNegative != y->isNegative()) {
    // x - (-y) == x + y
    // (-x) - y == -(x + y)
    return absoluteAdd(cx, x, y, xNegative);
  }

  // x - y == -(y - x)
  // (-x) - (-y) == y - x == -(x - y)
  int8_t compare = absoluteCompare(x, y);
  if (compare == 0) {
    return zero(cx);
  }

  if (compare > 0) {
    return absoluteSub(cx, x, y, xNegative);
  }

  return absoluteSub(cx, y, x, !xNegative);
}

// BigInt proposal section 1.1.4
BigInt* BigInt::mul(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  if (x->isZero()) {
    return x;
  }
  if (y->isZero()) {
    return y;
  }

  bool resultNegative = x->isNegative() != y->isNegative();

  // Fast path for the likely-common case of up to a uint64_t of magnitude.
  if (x->absFitsInUint64() && y->absFitsInUint64()) {
    uint64_t lhs = x->uint64FromAbsNonZero();
    uint64_t rhs = y->uint64FromAbsNonZero();

    uint64_t res;
    if (js::SafeMul(lhs, rhs, &res)) {
      MOZ_ASSERT(res != 0);
      return createFromNonZeroRawUint64(cx, res, resultNegative);
    }
  }

  unsigned resultLength = x->digitLength() + y->digitLength();
  BigInt* result = createUninitialized(cx, resultLength, resultNegative);
  if (!result) {
    return nullptr;
  }
  result->initializeDigitsToZero();

  for (size_t i = 0; i < x->digitLength(); i++) {
    multiplyAccumulate(y, x->digit(i), result, i);
  }

  return destructivelyTrimHighZeroDigits(cx, result);
}

// BigInt proposal section 1.1.5
BigInt* BigInt::div(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  // 1. If y is 0n, throw a RangeError exception.
  if (y->isZero()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_DIVISION_BY_ZERO);
    return nullptr;
  }

  // 2. Let quotient be the mathematical value of x divided by y.
  // 3. Return a BigInt representing quotient rounded towards 0 to the next
  //    integral value.
  if (x->isZero()) {
    return x;
  }

  if (absoluteCompare(x, y) < 0) {
    return zero(cx);
  }

  RootedBigInt quotient(cx);
  bool resultNegative = x->isNegative() != y->isNegative();
  if (y->digitLength() == 1) {
    Digit divisor = y->digit(0);
    if (divisor == 1) {
      return resultNegative == x->isNegative() ? x : neg(cx, x);
    }

    Digit remainder;
    if (!absoluteDivWithDigitDivisor(cx, x, divisor, Some(&quotient),
                                     &remainder, resultNegative)) {
      return nullptr;
    }
  } else {
    if (!absoluteDivWithBigIntDivisor(cx, x, y, Some(&quotient), Nothing(),
                                      resultNegative)) {
      return nullptr;
    }
  }

  return destructivelyTrimHighZeroDigits(cx, quotient);
}

// BigInt proposal section 1.1.6
BigInt* BigInt::mod(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  // 1. If y is 0n, throw a RangeError exception.
  if (y->isZero()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_DIVISION_BY_ZERO);
    return nullptr;
  }

  // 2. If x is 0n, return x.
  if (x->isZero()) {
    return x;
  }
  // 3. Let r be the BigInt defined by the mathematical relation r = x - (y ×
  // q) where q is a BigInt that is negative only if x/y is negative and
  // positive only if x/y is positive, and whose magnitude is as large as
  // possible without exceeding the magnitude of the true mathematical
  // quotient of x and y.
  if (absoluteCompare(x, y) < 0) {
    return x;
  }

  if (y->digitLength() == 1) {
    Digit divisor = y->digit(0);
    if (divisor == 1) {
      return zero(cx);
    }

    Digit remainderDigit;
    bool unusedQuotientNegative = false;
    if (!absoluteDivWithDigitDivisor(cx, x, divisor, Nothing(), &remainderDigit,
                                     unusedQuotientNegative)) {
      MOZ_CRASH("BigInt div by digit failed unexpectedly");
    }

    if (!remainderDigit) {
      return zero(cx);
    }

    return createFromDigit(cx, remainderDigit, x->isNegative());
  } else {
    RootedBigInt remainder(cx);
    if (!absoluteDivWithBigIntDivisor(cx, x, y, Nothing(), Some(&remainder),
                                      x->isNegative())) {
      return nullptr;
    }
    MOZ_ASSERT(remainder);
    return destructivelyTrimHighZeroDigits(cx, remainder);
  }
}

// BigInt proposal section 1.1.3
BigInt* BigInt::pow(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  // 1. If exponent is < 0, throw a RangeError exception.
  if (y->isNegative()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_NEGATIVE_EXPONENT);
    return nullptr;
  }

  // 2. If base is 0n and exponent is 0n, return 1n.
  if (y->isZero()) {
    return one(cx);
  }

  if (x->isZero()) {
    return x;
  }

  // 3. Return a BigInt representing the mathematical value of base raised
  //    to the power exponent.
  if (x->digitLength() == 1 && x->digit(0) == 1) {
    // (-1) ** even_number == 1.
    if (x->isNegative() && (y->digit(0) & 1) == 0) {
      return neg(cx, x);
    }
    // (-1) ** odd_number == -1; 1 ** anything == 1.
    return x;
  }

  // For all bases >= 2, very large exponents would lead to unrepresentable
  // results.
  static_assert(MaxBitLength < std::numeric_limits<Digit>::max(),
                "unexpectedly large MaxBitLength");
  if (y->digitLength() > 1) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_TOO_LARGE);
    return nullptr;
  }
  Digit exponent = y->digit(0);
  if (exponent == 1) {
    return x;
  }
  if (exponent >= MaxBitLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_TOO_LARGE);
    return nullptr;
  }

  static_assert(MaxBitLength <= std::numeric_limits<int>::max(),
                "unexpectedly large MaxBitLength");
  int n = static_cast<int>(exponent);
  bool isOddPower = n & 1;

  if (x->digitLength() == 1 && mozilla::IsPowerOfTwo(x->digit(0))) {
    // Fast path for (2^m)^n.

    // Result is negative for odd powers.
    bool resultNegative = x->isNegative() && isOddPower;

    unsigned m = mozilla::FloorLog2(x->digit(0));
    MOZ_ASSERT(m < DigitBits);

    static_assert(MaxBitLength * DigitBits > MaxBitLength,
                  "n * m can't overflow");
    n *= int(m);

    int length = 1 + (n / DigitBits);
    BigInt* result = createUninitialized(cx, length, resultNegative);
    if (!result) {
      return nullptr;
    }
    result->initializeDigitsToZero();
    result->setDigit(length - 1, static_cast<Digit>(1) << (n % DigitBits));
    return result;
  }

  RootedBigInt runningSquare(cx, x);
  RootedBigInt result(cx, isOddPower ? x : nullptr);
  n /= 2;

  // Fast path for the likely-common case of up to a uint64_t of magnitude.
  if (x->absFitsInUint64()) {
    bool resultNegative = x->isNegative() && isOddPower;

    uint64_t runningSquareInt = x->uint64FromAbsNonZero();
    uint64_t resultInt = isOddPower ? runningSquareInt : 1;
    while (true) {
      uint64_t runningSquareStart = runningSquareInt;
      uint64_t r;
      if (!js::SafeMul(runningSquareInt, runningSquareInt, &r)) {
        break;
      }
      runningSquareInt = r;

      if (n & 1) {
        if (!js::SafeMul(resultInt, runningSquareInt, &r)) {
          // Recover |runningSquare| before we restart the loop.
          runningSquareInt = runningSquareStart;
          break;
        }
        resultInt = r;
      }

      n /= 2;
      if (n == 0) {
        return createFromNonZeroRawUint64(cx, resultInt, resultNegative);
      }
    }

    runningSquare = createFromNonZeroRawUint64(cx, runningSquareInt, false);
    if (!runningSquare) {
      return nullptr;
    }

    result = createFromNonZeroRawUint64(cx, resultInt, resultNegative);
    if (!result) {
      return nullptr;
    }
  }

  // This implicitly sets the result's sign correctly.
  while (true) {
    runningSquare = mul(cx, runningSquare, runningSquare);
    if (!runningSquare) {
      return nullptr;
    }

    if (n & 1) {
      if (!result) {
        result = runningSquare;
      } else {
        result = mul(cx, result, runningSquare);
        if (!result) {
          return nullptr;
        }
      }
    }

    n /= 2;
    if (n == 0) {
      return result;
    }
  }
}

BigInt* BigInt::lshByAbsolute(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  if (x->isZero() || y->isZero()) {
    return x;
  }

  if (y->digitLength() > 1 || y->digit(0) > MaxBitLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_TOO_LARGE);
    return nullptr;
  }
  Digit shift = y->digit(0);
  int digitShift = static_cast<int>(shift / DigitBits);
  int bitsShift = static_cast<int>(shift % DigitBits);
  int length = x->digitLength();
  bool grow = bitsShift && (x->digit(length - 1) >> (DigitBits - bitsShift));
  int resultLength = length + digitShift + grow;
  BigInt* result = createUninitialized(cx, resultLength, x->isNegative());
  if (!result) {
    return nullptr;
  }

  int i = 0;
  for (; i < digitShift; i++) {
    result->setDigit(i, 0);
  }

  if (bitsShift == 0) {
    for (int j = 0; i < resultLength; i++, j++) {
      result->setDigit(i, x->digit(j));
    }
  } else {
    Digit carry = 0;
    for (int j = 0; j < length; i++, j++) {
      Digit d = x->digit(j);
      result->setDigit(i, (d << bitsShift) | carry);
      carry = d >> (DigitBits - bitsShift);
    }
    if (grow) {
      result->setDigit(i, carry);
    } else {
      MOZ_ASSERT(!carry);
    }
  }
  return result;
}

BigInt* BigInt::rshByMaximum(JSContext* cx, bool isNegative) {
  return isNegative ? negativeOne(cx) : zero(cx);
}

BigInt* BigInt::rshByAbsolute(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  if (x->isZero() || y->isZero()) {
    return x;
  }

  if (y->digitLength() > 1 || y->digit(0) >= MaxBitLength) {
    return rshByMaximum(cx, x->isNegative());
  }
  Digit shift = y->digit(0);
  int length = x->digitLength();
  int digitShift = static_cast<int>(shift / DigitBits);
  int bitsShift = static_cast<int>(shift % DigitBits);
  int resultLength = length - digitShift;
  if (resultLength <= 0) {
    return rshByMaximum(cx, x->isNegative());
  }
  // For negative numbers, round down if any bit was shifted out (so that e.g.
  // -5n >> 1n == -3n and not -2n). Check now whether this will happen and
  // whether it can cause overflow into a new digit. If we allocate the result
  // large enough up front, it avoids having to do a second allocation later.
  bool mustRoundDown = false;
  if (x->isNegative()) {
    const Digit mask = (static_cast<Digit>(1) << bitsShift) - 1;
    if ((x->digit(digitShift) & mask)) {
      mustRoundDown = true;
    } else {
      for (int i = 0; i < digitShift; i++) {
        if (x->digit(i)) {
          mustRoundDown = true;
          break;
        }
      }
    }
  }
  // If bits_shift is non-zero, it frees up bits, preventing overflow.
  if (mustRoundDown && bitsShift == 0) {
    // Overflow cannot happen if the most significant digit has unset bits.
    Digit msd = x->digit(length - 1);
    bool roundingCanOverflow = msd == std::numeric_limits<Digit>::max();
    if (roundingCanOverflow) {
      resultLength++;
    }
  }

  MOZ_ASSERT(resultLength <= length);
  RootedBigInt result(cx,
                      createUninitialized(cx, resultLength, x->isNegative()));
  if (!result) {
    return nullptr;
  }
  if (!bitsShift) {
    // If roundingCanOverflow, manually initialize the overflow digit.
    result->setDigit(resultLength - 1, 0);
    for (int i = digitShift; i < length; i++) {
      result->setDigit(i - digitShift, x->digit(i));
    }
  } else {
    Digit carry = x->digit(digitShift) >> bitsShift;
    int last = length - digitShift - 1;
    for (int i = 0; i < last; i++) {
      Digit d = x->digit(i + digitShift + 1);
      result->setDigit(i, (d << (DigitBits - bitsShift)) | carry);
      carry = d >> bitsShift;
    }
    result->setDigit(last, carry);
  }

  if (mustRoundDown) {
    MOZ_ASSERT(x->isNegative());
    // Since the result is negative, rounding down means adding one to
    // its absolute value. This cannot overflow.  TODO: modify the result in
    // place.
    return absoluteAddOne(cx, result, x->isNegative());
  }
  return destructivelyTrimHighZeroDigits(cx, result);
}

// BigInt proposal section 1.1.9. BigInt::leftShift ( x, y )
BigInt* BigInt::lsh(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  if (y->isNegative()) {
    return rshByAbsolute(cx, x, y);
  }
  return lshByAbsolute(cx, x, y);
}

// BigInt proposal section 1.1.10. BigInt::signedRightShift ( x, y )
BigInt* BigInt::rsh(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  if (y->isNegative()) {
    return lshByAbsolute(cx, x, y);
  }
  return rshByAbsolute(cx, x, y);
}

// BigInt proposal section 1.1.17. BigInt::bitwiseAND ( x, y )
BigInt* BigInt::bitAnd(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  if (x->isZero()) {
    return x;
  }

  if (y->isZero()) {
    return y;
  }

  if (!x->isNegative() && !y->isNegative()) {
    return absoluteAnd(cx, x, y);
  }

  if (x->isNegative() && y->isNegative()) {
    // (-x) & (-y) == ~(x-1) & ~(y-1) == ~((x-1) | (y-1))
    // == -(((x-1) | (y-1)) + 1)
    RootedBigInt x1(cx, absoluteSubOne(cx, x));
    if (!x1) {
      return nullptr;
    }
    RootedBigInt y1(cx, absoluteSubOne(cx, y));
    if (!y1) {
      return nullptr;
    }
    RootedBigInt result(cx, absoluteOr(cx, x1, y1));
    if (!result) {
      return nullptr;
    }
    bool resultNegative = true;
    return absoluteAddOne(cx, result, resultNegative);
  }

  MOZ_ASSERT(x->isNegative() != y->isNegative());
  HandleBigInt& pos = x->isNegative() ? y : x;
  HandleBigInt& neg = x->isNegative() ? x : y;

  RootedBigInt neg1(cx, absoluteSubOne(cx, neg));
  if (!neg1) {
    return nullptr;
  }

  // x & (-y) == x & ~(y-1) == x & ~(y-1)
  return absoluteAndNot(cx, pos, neg1);
}

// BigInt proposal section 1.1.18. BigInt::bitwiseXOR ( x, y )
BigInt* BigInt::bitXor(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  if (x->isZero()) {
    return y;
  }

  if (y->isZero()) {
    return x;
  }

  if (!x->isNegative() && !y->isNegative()) {
    return absoluteXor(cx, x, y);
  }

  if (x->isNegative() && y->isNegative()) {
    // (-x) ^ (-y) == ~(x-1) ^ ~(y-1) == (x-1) ^ (y-1)
    RootedBigInt x1(cx, absoluteSubOne(cx, x));
    if (!x1) {
      return nullptr;
    }
    RootedBigInt y1(cx, absoluteSubOne(cx, y));
    if (!y1) {
      return nullptr;
    }
    return absoluteXor(cx, x1, y1);
  }
  MOZ_ASSERT(x->isNegative() != y->isNegative());

  HandleBigInt& pos = x->isNegative() ? y : x;
  HandleBigInt& neg = x->isNegative() ? x : y;

  // x ^ (-y) == x ^ ~(y-1) == ~(x ^ (y-1)) == -((x ^ (y-1)) + 1)
  RootedBigInt result(cx, absoluteSubOne(cx, neg));
  if (!result) {
    return nullptr;
  }
  result = absoluteXor(cx, result, pos);
  if (!result) {
    return nullptr;
  }
  bool resultNegative = true;
  return absoluteAddOne(cx, result, resultNegative);
}

// BigInt proposal section 1.1.19. BigInt::bitwiseOR ( x, y )
BigInt* BigInt::bitOr(JSContext* cx, HandleBigInt x, HandleBigInt y) {
  if (x->isZero()) {
    return y;
  }

  if (y->isZero()) {
    return x;
  }

  bool resultNegative = x->isNegative() || y->isNegative();

  if (!resultNegative) {
    return absoluteOr(cx, x, y);
  }

  if (x->isNegative() && y->isNegative()) {
    // (-x) | (-y) == ~(x-1) | ~(y-1) == ~((x-1) & (y-1))
    // == -(((x-1) & (y-1)) + 1)
    RootedBigInt result(cx, absoluteSubOne(cx, x));
    if (!result) {
      return nullptr;
    }
    RootedBigInt y1(cx, absoluteSubOne(cx, y));
    if (!y1) {
      return nullptr;
    }
    result = absoluteAnd(cx, result, y1);
    if (!result) {
      return nullptr;
    }
    return absoluteAddOne(cx, result, resultNegative);
  }

  MOZ_ASSERT(x->isNegative() != y->isNegative());
  HandleBigInt& pos = x->isNegative() ? y : x;
  HandleBigInt& neg = x->isNegative() ? x : y;

  // x | (-y) == x | ~(y-1) == ~((y-1) &~ x) == -(((y-1) &~ x) + 1)
  RootedBigInt result(cx, absoluteSubOne(cx, neg));
  if (!result) {
    return nullptr;
  }
  result = absoluteAndNot(cx, result, pos);
  if (!result) {
    return nullptr;
  }
  return absoluteAddOne(cx, result, resultNegative);
}

// BigInt proposal section 1.1.2. BigInt::bitwiseNOT ( x )
BigInt* BigInt::bitNot(JSContext* cx, HandleBigInt x) {
  if (x->isNegative()) {
    // ~(-x) == ~(~(x-1)) == x-1
    return absoluteSubOne(cx, x);
  } else {
    // ~x == -x-1 == -(x+1)
    bool resultNegative = true;
    return absoluteAddOne(cx, x, resultNegative);
  }
}

int64_t BigInt::toInt64(BigInt* x) { return WrapToSigned(toUint64(x)); }

uint64_t BigInt::toUint64(BigInt* x) {
  if (x->isZero()) {
    return 0;
  }

  uint64_t digit = x->uint64FromAbsNonZero();

  // Return the two's complement if x is negative.
  if (x->isNegative()) {
    return ~(digit - 1);
  }

  return digit;
}

bool BigInt::isInt64(BigInt* x, int64_t* result) {
  MOZ_MAKE_MEM_UNDEFINED(result, sizeof(*result));

  if (!x->absFitsInUint64()) {
    return false;
  }

  if (x->isZero()) {
    *result = 0;
    return true;
  }

  uint64_t magnitude = x->uint64FromAbsNonZero();

  if (x->isNegative()) {
    constexpr uint64_t Int64MinMagnitude = uint64_t(1) << 63;
    if (magnitude <= Int64MinMagnitude) {
      *result = magnitude == Int64MinMagnitude
                    ? std::numeric_limits<int64_t>::min()
                    : -AssertedCast<int64_t>(magnitude);
      return true;
    }
  } else {
    if (magnitude <=
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      *result = AssertedCast<int64_t>(magnitude);
      return true;
    }
  }

  return false;
}

bool BigInt::isUint64(BigInt* x, uint64_t* result) {
  MOZ_MAKE_MEM_UNDEFINED(result, sizeof(*result));

  if (!x->absFitsInUint64() || x->isNegative()) {
    return false;
  }

  if (x->isZero()) {
    *result = 0;
    return true;
  }

  *result = x->uint64FromAbsNonZero();
  return true;
}

bool BigInt::isNumber(BigInt* x, double* result) {
  MOZ_MAKE_MEM_UNDEFINED(result, sizeof(*result));

  if (!x->absFitsInUint64()) {
    return false;
  }

  if (x->isZero()) {
    *result = 0;
    return true;
  }

  uint64_t magnitude = x->uint64FromAbsNonZero();
  if (magnitude < uint64_t(DOUBLE_INTEGRAL_PRECISION_LIMIT)) {
    *result = x->isNegative() ? -double(magnitude) : double(magnitude);
    return true;
  }

  return false;
}

// Compute `2**bits - (x & (2**bits - 1))`.  Used when treating BigInt values as
// arbitrary-precision two's complement signed integers.
BigInt* BigInt::truncateAndSubFromPowerOfTwo(JSContext* cx, HandleBigInt x,
                                             uint64_t bits,
                                             bool resultNegative) {
  MOZ_ASSERT(bits != 0);
  MOZ_ASSERT(!x->isZero());

  if (bits > MaxBitLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_TOO_LARGE);
    return nullptr;
  }

  size_t resultLength = CeilDiv(bits, DigitBits);
  BigInt* result = createUninitialized(cx, resultLength, resultNegative);
  if (!result) {
    return nullptr;
  }

  // Process all digits except the MSD.
  size_t xLength = x->digitLength();
  Digit borrow = 0;
  // Take digits from `x` until its length is exhausted.
  for (size_t i = 0; i < std::min(resultLength - 1, xLength); i++) {
    Digit newBorrow = 0;
    Digit difference = digitSub(0, x->digit(i), &newBorrow);
    difference = digitSub(difference, borrow, &newBorrow);
    result->setDigit(i, difference);
    borrow = newBorrow;
  }
  // Then simulate leading zeroes in `x` as needed.
  for (size_t i = xLength; i < resultLength - 1; i++) {
    Digit newBorrow = 0;
    Digit difference = digitSub(0, borrow, &newBorrow);
    result->setDigit(i, difference);
    borrow = newBorrow;
  }

  // The MSD might contain extra bits that we don't want.
  Digit xMSD = resultLength <= xLength ? x->digit(resultLength - 1) : 0;
  Digit resultMSD;
  if (bits % DigitBits == 0) {
    Digit newBorrow = 0;
    resultMSD = digitSub(0, xMSD, &newBorrow);
    resultMSD = digitSub(resultMSD, borrow, &newBorrow);
  } else {
    size_t drop = DigitBits - (bits % DigitBits);
    xMSD = (xMSD << drop) >> drop;
    Digit minuendMSD = Digit(1) << (DigitBits - drop);
    Digit newBorrow = 0;
    resultMSD = digitSub(minuendMSD, xMSD, &newBorrow);
    resultMSD = digitSub(resultMSD, borrow, &newBorrow);
    MOZ_ASSERT(newBorrow == 0, "result < 2^bits");
    // If all subtracted bits were zero, we have to get rid of the
    // materialized minuendMSD again.
    resultMSD &= (minuendMSD - 1);
  }
  result->setDigit(resultLength - 1, resultMSD);

  return destructivelyTrimHighZeroDigits(cx, result);
}

BigInt* BigInt::asUintN(JSContext* cx, HandleBigInt x, uint64_t bits) {
  if (x->isZero()) {
    return x;
  }

  if (bits == 0) {
    return zero(cx);
  }

  // When truncating a negative number, simulate two's complement.
  if (x->isNegative()) {
    bool resultNegative = false;
    return truncateAndSubFromPowerOfTwo(cx, x, bits, resultNegative);
  }

  if (bits <= 64) {
    uint64_t u64 = toUint64(x);
    uint64_t mask = uint64_t(-1) >> (64 - bits);
    uint64_t n = u64 & mask;
    if (u64 == n && x->absFitsInUint64()) {
      return x;
    }
    return createFromUint64(cx, n);
  }

  if (bits >= MaxBitLength) {
    return x;
  }

  Digit msd = x->digit(x->digitLength() - 1);
  size_t msdBits = DigitBits - DigitLeadingZeroes(msd);
  size_t bitLength = msdBits + (x->digitLength() - 1) * DigitBits;

  if (bits >= bitLength) {
    return x;
  }

  size_t length = CeilDiv(bits, DigitBits);
  MOZ_ASSERT(length >= 2, "single-digit cases should be handled above");
  MOZ_ASSERT(length <= x->digitLength());

  // Eagerly trim high zero digits.
  const size_t highDigitBits = ((bits - 1) % DigitBits) + 1;
  const Digit highDigitMask = Digit(-1) >> (DigitBits - highDigitBits);
  Digit mask = highDigitMask;
  while (length > 0) {
    if (x->digit(length - 1) & mask) {
      break;
    }

    mask = Digit(-1);
    length--;
  }

  const bool isNegative = false;
  BigInt* res = createUninitialized(cx, length, isNegative);
  if (res == nullptr) {
    return nullptr;
  }

  while (length-- > 0) {
    res->setDigit(length, x->digit(length) & mask);
    mask = Digit(-1);
  }
  MOZ_ASSERT_IF(length == 0, res->isZero());

  return res;
}

BigInt* BigInt::asIntN(JSContext* cx, HandleBigInt x, uint64_t bits) {
  if (x->isZero()) {
    return x;
  }

  if (bits == 0) {
    return zero(cx);
  }

  if (bits == 64) {
    int64_t n = toInt64(x);
    if (((n < 0) == x->isNegative()) && x->absFitsInUint64()) {
      return x;
    }
    return createFromInt64(cx, n);
  }

  if (bits > MaxBitLength) {
    return x;
  }

  Digit msd = x->digit(x->digitLength() - 1);
  size_t msdBits = DigitBits - DigitLeadingZeroes(msd);
  size_t bitLength = msdBits + (x->digitLength() - 1) * DigitBits;

  if (bits > bitLength) {
    return x;
  }

  Digit signBit = Digit(1) << ((bits - 1) % DigitBits);
  if (bits == bitLength && msd < signBit) {
    return x;
  }

  // All the cases above were the trivial cases: truncating zero, or to zero
  // bits, or to more bits than are in `x` (so we return `x` directly), or we
  // already have the 64-bit fast path.  If we get here, follow the textbook
  // algorithm from the specification.

  // BigInt.asIntN step 3:  Let `mod` be `x` modulo `2**bits`.
  RootedBigInt mod(cx, asUintN(cx, x, bits));
  if (!mod) {
    return nullptr;
  }

  // Step 4: If `mod >= 2**(bits - 1)`, return `mod - 2**bits`; otherwise,
  // return `mod`.
  if (mod->digitLength() == CeilDiv(bits, DigitBits)) {
    MOZ_ASSERT(!mod->isZero(),
               "nonzero bits implies nonzero digit length which implies "
               "nonzero overall");

    if ((mod->digit(mod->digitLength() - 1) & signBit) != 0) {
      bool resultNegative = true;
      return truncateAndSubFromPowerOfTwo(cx, mod, bits, resultNegative);
    }
  }

  return mod;
}

static bool ValidBigIntOperands(JSContext* cx, HandleValue lhs,
                                HandleValue rhs) {
  MOZ_ASSERT(lhs.isBigInt() || rhs.isBigInt());

  if (!lhs.isBigInt() || !rhs.isBigInt()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_TO_NUMBER);
    return false;
  }

  return true;
}

bool BigInt::addValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                      MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::add(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::subValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                      MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::sub(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::mulValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                      MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::mul(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::divValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                      MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::div(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::modValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                      MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::mod(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::powValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                      MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::pow(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::negValue(JSContext* cx, HandleValue operand,
                      MutableHandleValue res) {
  MOZ_ASSERT(operand.isBigInt());

  RootedBigInt operandBigInt(cx, operand.toBigInt());
  BigInt* resBigInt = BigInt::neg(cx, operandBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::incValue(JSContext* cx, HandleValue operand,
                      MutableHandleValue res) {
  MOZ_ASSERT(operand.isBigInt());

  RootedBigInt operandBigInt(cx, operand.toBigInt());
  BigInt* resBigInt = BigInt::inc(cx, operandBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::decValue(JSContext* cx, HandleValue operand,
                      MutableHandleValue res) {
  MOZ_ASSERT(operand.isBigInt());

  RootedBigInt operandBigInt(cx, operand.toBigInt());
  BigInt* resBigInt = BigInt::dec(cx, operandBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::lshValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                      MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::lsh(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::rshValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                      MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::rsh(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::bitAndValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                         MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::bitAnd(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::bitXorValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                         MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::bitXor(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::bitOrValue(JSContext* cx, HandleValue lhs, HandleValue rhs,
                        MutableHandleValue res) {
  if (!ValidBigIntOperands(cx, lhs, rhs)) {
    return false;
  }

  RootedBigInt lhsBigInt(cx, lhs.toBigInt());
  RootedBigInt rhsBigInt(cx, rhs.toBigInt());
  BigInt* resBigInt = BigInt::bitOr(cx, lhsBigInt, rhsBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

bool BigInt::bitNotValue(JSContext* cx, HandleValue operand,
                         MutableHandleValue res) {
  MOZ_ASSERT(operand.isBigInt());

  RootedBigInt operandBigInt(cx, operand.toBigInt());
  BigInt* resBigInt = BigInt::bitNot(cx, operandBigInt);
  if (!resBigInt) {
    return false;
  }
  res.setBigInt(resBigInt);
  return true;
}

// BigInt proposal section 7.3
BigInt* js::ToBigInt(JSContext* cx, HandleValue val) {
  RootedValue v(cx, val);

  // Step 1.
  if (!ToPrimitive(cx, JSTYPE_NUMBER, &v)) {
    return nullptr;
  }

  // Step 2.
  if (v.isBigInt()) {
    return v.toBigInt();
  }

  if (v.isBoolean()) {
    return v.toBoolean() ? BigInt::one(cx) : BigInt::zero(cx);
  }

  if (v.isString()) {
    RootedString str(cx, v.toString());
    BigInt* bi;
    JS_TRY_VAR_OR_RETURN_NULL(cx, bi, StringToBigInt(cx, str));
    if (!bi) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BIGINT_INVALID_SYNTAX);
      return nullptr;
    }
    return bi;
  }

  ReportValueError(cx, JSMSG_CANT_CONVERT_TO, JSDVG_IGNORE_STACK, v, nullptr,
                   "BigInt");
  return nullptr;
}

JS::Result<int64_t> js::ToBigInt64(JSContext* cx, HandleValue v) {
  BigInt* bi = js::ToBigInt(cx, v);
  if (!bi) {
    return cx->alreadyReportedError();
  }
  return BigInt::toInt64(bi);
}

JS::Result<uint64_t> js::ToBigUint64(JSContext* cx, HandleValue v) {
  BigInt* bi = js::ToBigInt(cx, v);
  if (!bi) {
    return cx->alreadyReportedError();
  }
  return BigInt::toUint64(bi);
}

double BigInt::numberValue(BigInt* x) {
  if (x->isZero()) {
    return 0.0;
  }

  using Double = mozilla::FloatingPoint<double>;
  constexpr uint8_t ExponentShift = Double::kExponentShift;
  constexpr uint8_t SignificandWidth = Double::kSignificandWidth;
  constexpr unsigned ExponentBias = Double::kExponentBias;
  constexpr uint8_t SignShift = Double::kExponentWidth + SignificandWidth;

  MOZ_ASSERT(x->digitLength() > 0);

  // Fast path for the likely-common case of up to a uint64_t of magnitude not
  // exceeding integral precision in IEEE-754.  (Note that we *depend* on this
  // optimization being performed further down.)
  if (x->absFitsInUint64()) {
    uint64_t magnitude = x->uint64FromAbsNonZero();
    const uint64_t MaxIntegralPrecisionDouble = uint64_t(1)
                                                << (SignificandWidth + 1);
    if (magnitude <= MaxIntegralPrecisionDouble) {
      return x->isNegative() ? -double(magnitude) : +double(magnitude);
    }
  }

  size_t length = x->digitLength();
  Digit msd = x->digit(length - 1);
  uint8_t msdLeadingZeroes = DigitLeadingZeroes(msd);

  // `2**ExponentBias` is the largest power of two in a finite IEEE-754
  // double.  If this bigint has a greater power of two, it'll round to
  // infinity.
  uint64_t exponent = length * DigitBits - msdLeadingZeroes - 1;
  if (exponent > ExponentBias) {
    return x->isNegative() ? mozilla::NegativeInfinity<double>()
                           : mozilla::PositiveInfinity<double>();
  }

  // Otherwise munge the most significant bits of the number into proper
  // position in an IEEE-754 double and go to town.

  // Omit the most significant bit: the IEEE-754 format includes this bit
  // implicitly for all double-precision integers.
  const uint8_t msdIgnoredBits = msdLeadingZeroes + 1;
  const uint8_t msdIncludedBits = DigitBits - msdIgnoredBits;

  // We compute the final mantissa of the result, shifted upward to the top of
  // the `uint64_t` space -- plus an extra bit to detect potential rounding.
  constexpr uint8_t BitsNeededForShiftedMantissa = SignificandWidth + 1;

  // Shift `msd`'s contributed bits upward to remove high-order zeroes and the
  // highest set bit (which is implicit in IEEE-754 integral values so must be
  // removed) and to add low-order zeroes.  (Lower-order garbage bits are
  // discarded when `shiftedMantissa` is converted to a real mantissa.)
  uint64_t shiftedMantissa =
      msdIncludedBits == 0 ? 0 : uint64_t(msd) << (64 - msdIncludedBits);

  // If the extra bit is set, correctly rounding the result may require
  // examining all lower-order bits.  Also compute 1) the index of the Digit
  // storing the extra bit, and 2) whether bits beneath the extra bit in that
  // Digit are nonzero so we can round if needed.
  size_t digitContainingExtraBit;
  Digit bitsBeneathExtraBitInDigitContainingExtraBit;

  // Add shifted bits to `shiftedMantissa` until we have a complete mantissa and
  // an extra bit.
  if (msdIncludedBits >= BitsNeededForShiftedMantissa) {
    //       DigitBits=64 (necessarily for msdIncludedBits ≥ SignificandWidth+1;
    //            |        C++ compiler range analysis ought eliminate this
    //            |        check on 32-bit)
    //   _________|__________
    //  /                    |
    //        msdIncludedBits
    //      ________|________
    //     /                 |
    // [001···················|
    //  \_/\_____________/\__|
    //   |            |    |
    // msdIgnoredBits |   bits below the extra bit (may be no bits)
    //      BitsNeededForShiftedMantissa=SignificandWidth+1
    digitContainingExtraBit = length - 1;

    const uint8_t countOfBitsInDigitBelowExtraBit =
        DigitBits - BitsNeededForShiftedMantissa - msdIgnoredBits;
    bitsBeneathExtraBitInDigitContainingExtraBit =
        msd & ((Digit(1) << countOfBitsInDigitBelowExtraBit) - 1);
  } else {
    MOZ_ASSERT(length >= 2,
               "single-Digit numbers with this few bits should have been "
               "handled by the fast-path above");

    Digit second = x->digit(length - 2);
    if (DigitBits == 64) {
      shiftedMantissa |= second >> msdIncludedBits;

      digitContainingExtraBit = length - 2;

      //  msdIncludedBits + DigitBits
      //      ________|_________
      //     /                  |
      //             DigitBits=64
      // msdIncludedBits    |
      //      __|___   _____|___
      //     /      \ /         |
      // [001········|···········|
      //  \_/\_____________/\___|
      //   |            |     |
      // msdIgnoredBits | bits below the extra bit (always more than one)
      //                |
      //      BitsNeededForShiftedMantissa=SignificandWidth+1
      const uint8_t countOfBitsInSecondDigitBelowExtraBit =
          (msdIncludedBits + DigitBits) - BitsNeededForShiftedMantissa;

      bitsBeneathExtraBitInDigitContainingExtraBit =
          second << (DigitBits - countOfBitsInSecondDigitBelowExtraBit);
    } else {
      shiftedMantissa |= uint64_t(second) << msdIgnoredBits;

      if (msdIncludedBits + DigitBits >= BitsNeededForShiftedMantissa) {
        digitContainingExtraBit = length - 2;

        //  msdIncludedBits + DigitBits
        //      ______|________
        //     /               |
        //             DigitBits=32
        // msdIncludedBits |
        //      _|_   _____|___
        //     /   \ /         |
        // [001·····|···········|
        //     \___________/\__|
        //          |        |
        //          |      bits below the extra bit (may be no bits)
        //      BitsNeededForShiftedMantissa=SignificandWidth+1
        const uint8_t countOfBitsInSecondDigitBelowExtraBit =
            (msdIncludedBits + DigitBits) - BitsNeededForShiftedMantissa;

        bitsBeneathExtraBitInDigitContainingExtraBit =
            second & ((Digit(1) << countOfBitsInSecondDigitBelowExtraBit) - 1);
      } else {
        MOZ_ASSERT(length >= 3,
                   "we must have at least three digits here, because "
                   "`msdIncludedBits + 32 < BitsNeededForShiftedMantissa` "
                   "guarantees `x < 2**53` -- and therefore the "
                   "MaxIntegralPrecisionDouble optimization above will have "
                   "handled two-digit cases");

        Digit third = x->digit(length - 3);
        shiftedMantissa |= uint64_t(third) >> msdIncludedBits;

        digitContainingExtraBit = length - 3;

        //    msdIncludedBits + DigitBits + DigitBits
        //      ____________|______________
        //     /                           |
        //             DigitBits=32
        // msdIncludedBits |     DigitBits=32
        //      _|_   _____|___   ____|____
        //     /   \ /         \ /         |
        // [001·····|···········|···········|
        //     \____________________/\_____|
        //               |               |
        //               |      bits below the extra bit
        //      BitsNeededForShiftedMantissa=SignificandWidth+1
        static_assert(2 * DigitBits > BitsNeededForShiftedMantissa,
                      "two 32-bit digits should more than fill a mantissa");
        const uint8_t countOfBitsInThirdDigitBelowExtraBit =
            msdIncludedBits + 2 * DigitBits - BitsNeededForShiftedMantissa;

        // Shift out the mantissa bits and the extra bit.
        bitsBeneathExtraBitInDigitContainingExtraBit =
            third << (DigitBits - countOfBitsInThirdDigitBelowExtraBit);
      }
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
      if (!shouldRoundUp) {
        while (digitContainingExtraBit-- > 0) {
          if (x->digit(digitContainingExtraBit) != 0) {
            shouldRoundUp = true;
            break;
          }
        }
      }
    }

    if (shouldRoundUp) {
      // Add one to the significand bits.  If they overflow, the exponent must
      // also be increased.  If *that* overflows, return the correct infinity.
      uint64_t before = shiftedMantissa;
      shiftedMantissa += ExtraBit;
      if (shiftedMantissa < before) {
        exponent++;
        if (exponent > ExponentBias) {
          return x->isNegative() ? NegativeInfinity<double>()
                                 : PositiveInfinity<double>();
        }
      }
    }
  }

  uint64_t significandBits = shiftedMantissa >> (64 - SignificandWidth);
  uint64_t signBit = uint64_t(x->isNegative() ? 1 : 0) << SignShift;
  uint64_t exponentBits = (exponent + ExponentBias) << ExponentShift;
  return mozilla::BitwiseCast<double>(signBit | exponentBits | significandBits);
}

int8_t BigInt::compare(BigInt* x, BigInt* y) {
  // Sanity checks to catch negative zeroes escaping to the wild.
  MOZ_ASSERT(!x->isNegative() || !x->isZero());
  MOZ_ASSERT(!y->isNegative() || !y->isZero());

  bool xSign = x->isNegative();

  if (xSign != y->isNegative()) {
    return xSign ? -1 : 1;
  }

  if (xSign) {
    std::swap(x, y);
  }

  return absoluteCompare(x, y);
}

bool BigInt::equal(BigInt* lhs, BigInt* rhs) {
  if (lhs == rhs) {
    return true;
  }
  if (lhs->digitLength() != rhs->digitLength()) {
    return false;
  }
  if (lhs->isNegative() != rhs->isNegative()) {
    return false;
  }
  for (size_t i = 0; i < lhs->digitLength(); i++) {
    if (lhs->digit(i) != rhs->digit(i)) {
      return false;
    }
  }
  return true;
}

int8_t BigInt::compare(BigInt* x, double y) {
  MOZ_ASSERT(!mozilla::IsNaN(y));

  constexpr int LessThan = -1, Equal = 0, GreaterThan = 1;

  // ±Infinity exceeds a finite bigint value.
  if (!mozilla::IsFinite(y)) {
    return y > 0 ? LessThan : GreaterThan;
  }

  // Handle `x === 0n` and `y == 0` special cases.
  if (x->isZero()) {
    if (y == 0) {
      // -0 and +0 are treated identically.
      return Equal;
    }

    return y > 0 ? LessThan : GreaterThan;
  }

  const bool xNegative = x->isNegative();
  if (y == 0) {
    return xNegative ? LessThan : GreaterThan;
  }

  // Nonzero `x` and `y` with different signs are trivially compared.
  const bool yNegative = y < 0;
  if (xNegative != yNegative) {
    return xNegative ? LessThan : GreaterThan;
  }

  // `x` and `y` are same-signed.  Determine which has greater magnitude,
  // then combine that with the signedness just computed to reach a result.
  const int exponent = mozilla::ExponentComponent(y);
  if (exponent < 0) {
    // `y` is a nonzero fraction of magnitude less than 1.
    return xNegative ? LessThan : GreaterThan;
  }

  size_t xLength = x->digitLength();
  MOZ_ASSERT(xLength > 0);

  Digit xMSD = x->digit(xLength - 1);
  const int shift = DigitLeadingZeroes(xMSD);
  int xBitLength = xLength * DigitBits - shift;

  // Differing bit-length makes for a simple comparison.
  int yBitLength = exponent + 1;
  if (xBitLength < yBitLength) {
    return xNegative ? GreaterThan : LessThan;
  }
  if (xBitLength > yBitLength) {
    return xNegative ? LessThan : GreaterThan;
  }

  // Compare the high 64 bits of both numbers.  (Lower-order bits not present
  // in either number are zeroed.)  Either that distinguishes `x` and `y`, or
  // `x` and `y` differ only if a subsequent nonzero bit in `x` means `x` has
  // larger magnitude.

  using Double = mozilla::FloatingPoint<double>;
  constexpr uint8_t SignificandWidth = Double::kSignificandWidth;
  constexpr uint64_t SignificandBits = Double::kSignificandBits;

  const uint64_t doubleBits = mozilla::BitwiseCast<uint64_t>(y);
  const uint64_t significandBits = doubleBits & SignificandBits;

  // Readd the implicit-one bit when constructing `y`'s high 64 bits.
  const uint64_t yHigh64Bits =
      ((uint64_t(1) << SignificandWidth) | significandBits)
      << (64 - SignificandWidth - 1);

  // Cons up `x`'s high 64 bits, backfilling zeroes for binary fractions of 1
  // if `x` doesn't have 64 bits.
  uint8_t xBitsFilled = DigitBits - shift;
  uint64_t xHigh64Bits = uint64_t(xMSD) << (64 - xBitsFilled);

  // At this point we no longer need to look at the most significant digit.
  xLength--;

  // The high 64 bits from `x` will probably not align to a digit boundary.
  // `xHasNonZeroLeftoverBits` will be set to true if any remaining
  // least-significant bit from the digit holding xHigh64Bits's
  // least-significant bit is nonzero.
  bool xHasNonZeroLeftoverBits = false;

  if (xBitsFilled < std::min(xBitLength, 64)) {
    MOZ_ASSERT(xLength >= 1,
               "If there are more bits to fill, there should be "
               "more digits to fill them from");

    Digit second = x->digit(--xLength);
    if (DigitBits == 32) {
      xBitsFilled += 32;
      xHigh64Bits |= uint64_t(second) << (64 - xBitsFilled);
      if (xBitsFilled < 64 && xLength >= 1) {
        Digit third = x->digit(--xLength);
        const uint8_t neededBits = 64 - xBitsFilled;
        xHigh64Bits |= uint64_t(third) >> (DigitBits - neededBits);
        xHasNonZeroLeftoverBits = (third << neededBits) != 0;
      }
    } else {
      const uint8_t neededBits = 64 - xBitsFilled;
      xHigh64Bits |= uint64_t(second) >> (DigitBits - neededBits);
      xHasNonZeroLeftoverBits = (second << neededBits) != 0;
    }
  }

  // If high bits are unequal, the larger one has greater magnitude.
  if (yHigh64Bits > xHigh64Bits) {
    return xNegative ? GreaterThan : LessThan;
  }
  if (xHigh64Bits > yHigh64Bits) {
    return xNegative ? LessThan : GreaterThan;
  }

  // Otherwise the top 64 bits of both are equal.  If the values differ, a
  // lower-order bit in `x` is nonzero and `x` has greater magnitude than
  // `y`; otherwise `x == y`.
  if (xHasNonZeroLeftoverBits) {
    return xNegative ? LessThan : GreaterThan;
  }
  while (xLength != 0) {
    if (x->digit(--xLength) != 0) {
      return xNegative ? LessThan : GreaterThan;
    }
  }

  return Equal;
}

bool BigInt::equal(BigInt* lhs, double rhs) {
  if (mozilla::IsNaN(rhs)) {
    return false;
  }
  return compare(lhs, rhs) == 0;
}

JS::Result<bool> BigInt::equal(JSContext* cx, Handle<BigInt*> lhs,
                               HandleString rhs) {
  BigInt* rhsBigInt;
  MOZ_TRY_VAR(rhsBigInt, StringToBigInt(cx, rhs));
  if (!rhsBigInt) {
    return false;
  }
  return equal(lhs, rhsBigInt);
}

// BigInt proposal section 3.2.5
JS::Result<bool> BigInt::looselyEqual(JSContext* cx, HandleBigInt lhs,
                                      HandleValue rhs) {
  // Step 1.
  if (rhs.isBigInt()) {
    return equal(lhs, rhs.toBigInt());
  }

  // Steps 2-5 (not applicable).

  // Steps 6-7.
  if (rhs.isString()) {
    RootedString rhsString(cx, rhs.toString());
    return equal(cx, lhs, rhsString);
  }

  // Steps 8-9 (not applicable).

  // Steps 10-11.
  if (rhs.isObject()) {
    RootedValue rhsPrimitive(cx, rhs);
    if (!ToPrimitive(cx, &rhsPrimitive)) {
      return cx->alreadyReportedError();
    }
    return looselyEqual(cx, lhs, rhsPrimitive);
  }

  // Step 12.
  if (rhs.isNumber()) {
    return equal(lhs, rhs.toNumber());
  }

  // Step 13.
  return false;
}

// BigInt proposal section 1.1.12. BigInt::lessThan ( x, y )
bool BigInt::lessThan(BigInt* x, BigInt* y) { return compare(x, y) < 0; }

Maybe<bool> BigInt::lessThan(BigInt* lhs, double rhs) {
  if (mozilla::IsNaN(rhs)) {
    return Maybe<bool>(Nothing());
  }
  return Some(compare(lhs, rhs) < 0);
}

Maybe<bool> BigInt::lessThan(double lhs, BigInt* rhs) {
  if (mozilla::IsNaN(lhs)) {
    return Maybe<bool>(Nothing());
  }
  return Some(-compare(rhs, lhs) < 0);
}

bool BigInt::lessThan(JSContext* cx, HandleBigInt lhs, HandleString rhs,
                      Maybe<bool>& res) {
  BigInt* rhsBigInt;
  JS_TRY_VAR_OR_RETURN_FALSE(cx, rhsBigInt, StringToBigInt(cx, rhs));
  if (!rhsBigInt) {
    res = Nothing();
    return true;
  }
  res = Some(lessThan(lhs, rhsBigInt));
  return true;
}

bool BigInt::lessThan(JSContext* cx, HandleString lhs, HandleBigInt rhs,
                      Maybe<bool>& res) {
  BigInt* lhsBigInt;
  JS_TRY_VAR_OR_RETURN_FALSE(cx, lhsBigInt, StringToBigInt(cx, lhs));
  if (!lhsBigInt) {
    res = Nothing();
    return true;
  }
  res = Some(lessThan(lhsBigInt, rhs));
  return true;
}

bool BigInt::lessThan(JSContext* cx, HandleValue lhs, HandleValue rhs,
                      Maybe<bool>& res) {
  if (lhs.isBigInt()) {
    if (rhs.isString()) {
      RootedBigInt lhsBigInt(cx, lhs.toBigInt());
      RootedString rhsString(cx, rhs.toString());
      return lessThan(cx, lhsBigInt, rhsString, res);
    }

    if (rhs.isNumber()) {
      res = lessThan(lhs.toBigInt(), rhs.toNumber());
      return true;
    }

    MOZ_ASSERT(rhs.isBigInt());
    res = Some(lessThan(lhs.toBigInt(), rhs.toBigInt()));
    return true;
  }

  MOZ_ASSERT(rhs.isBigInt());
  if (lhs.isString()) {
    RootedString lhsString(cx, lhs.toString());
    RootedBigInt rhsBigInt(cx, rhs.toBigInt());
    return lessThan(cx, lhsString, rhsBigInt, res);
  }

  MOZ_ASSERT(lhs.isNumber());
  res = lessThan(lhs.toNumber(), rhs.toBigInt());
  return true;
}

template <js::AllowGC allowGC>
JSLinearString* BigInt::toString(JSContext* cx, HandleBigInt x, uint8_t radix) {
  MOZ_ASSERT(2 <= radix && radix <= 36);

  if (x->isZero()) {
    return cx->staticStrings().getInt(0);
  }

  if (mozilla::IsPowerOfTwo(radix)) {
    return toStringBasePowerOfTwo<allowGC>(cx, x, radix);
  }

  if (radix == 10 && x->digitLength() == 1) {
    return toStringSingleDigitBaseTen<allowGC>(cx, x->digit(0),
                                               x->isNegative());
  }

  // Punt on doing generic toString without GC.
  if (!allowGC) {
    return nullptr;
  }

  return toStringGeneric(cx, x, radix);
}

template JSLinearString* BigInt::toString<js::CanGC>(JSContext* cx,
                                                     HandleBigInt x,
                                                     uint8_t radix);
template JSLinearString* BigInt::toString<js::NoGC>(JSContext* cx,
                                                    HandleBigInt x,
                                                    uint8_t radix);

template <typename CharT>
static inline BigInt* ParseStringBigIntLiteral(JSContext* cx,
                                               Range<const CharT> range,
                                               bool* haveParseError) {
  auto start = range.begin();
  auto end = range.end();

  while (start < end && unicode::IsSpace(start[0])) {
    start++;
  }

  while (start < end && unicode::IsSpace(end[-1])) {
    end--;
  }

  if (start == end) {
    return BigInt::zero(cx);
  }

  // StringNumericLiteral ::: StrDecimalLiteral, but without Infinity, decimal
  // points, or exponents.  Note that the raw '+' or '-' cases fall through
  // because the string is too short, and eventually signal a parse error.
  if (end - start > 1) {
    if (start[0] == '+') {
      bool isNegative = false;
      start++;
      return BigInt::parseLiteralDigits(cx, Range<const CharT>(start, end), 10,
                                        isNegative, haveParseError);
    }
    if (start[0] == '-') {
      bool isNegative = true;
      start++;
      return BigInt::parseLiteralDigits(cx, Range<const CharT>(start, end), 10,
                                        isNegative, haveParseError);
    }
  }

  return BigInt::parseLiteral(cx, Range<const CharT>(start, end),
                              haveParseError);
}

// Called from BigInt constructor.
JS::Result<BigInt*, JS::OOM> js::StringToBigInt(JSContext* cx,
                                                HandleString str) {
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return cx->alreadyReportedOOM();
  }

  AutoStableStringChars chars(cx);
  if (!chars.init(cx, str)) {
    return cx->alreadyReportedOOM();
  }

  BigInt* res;
  bool parseError = false;
  if (chars.isLatin1()) {
    res = ParseStringBigIntLiteral(cx, chars.latin1Range(), &parseError);
  } else {
    res = ParseStringBigIntLiteral(cx, chars.twoByteRange(), &parseError);
  }

  // A nullptr result can indicate either a parse error or out-of-memory.
  if (!res && !parseError) {
    return cx->alreadyReportedOOM();
  }

  return res;
}

// Called from parser with already trimmed and validated token.
BigInt* js::ParseBigIntLiteral(JSContext* cx,
                               const Range<const char16_t>& chars) {
  bool parseError = false;
  BigInt* res = BigInt::parseLiteral(cx, chars, &parseError);
  if (!res) {
    return nullptr;
  }
  MOZ_ASSERT(res->isTenured());
  MOZ_RELEASE_ASSERT(!parseError);
  return res;
}

// Check a already validated numeric literal for a non-zero value. Used by
// the parsers node folder in deferred mode.
bool js::BigIntLiteralIsZero(const mozilla::Range<const char16_t>& chars) {
  return BigInt::literalIsZero(chars);
}

template <js::AllowGC allowGC>
JSAtom* js::BigIntToAtom(JSContext* cx, HandleBigInt bi) {
  JSString* str = BigInt::toString<allowGC>(cx, bi, 10);
  if (!str) {
    return nullptr;
  }
  return AtomizeString(cx, str);
}

template JSAtom* js::BigIntToAtom<js::CanGC>(JSContext* cx, HandleBigInt bi);
template JSAtom* js::BigIntToAtom<js::NoGC>(JSContext* cx, HandleBigInt bi);

#if defined(DEBUG) || defined(JS_JITSPEW)
void BigInt::dump() const {
  js::Fprinter out(stderr);
  dump(out);
}

void BigInt::dump(js::GenericPrinter& out) const {
  if (isNegative()) {
    out.putChar('-');
  }

  if (digitLength() == 0) {
    out.put("0");
  } else if (digitLength() == 1) {
    uint64_t d = digit(0);
    out.printf("%" PRIu64, d);
  } else {
    out.put("0x");
    for (size_t i = 0; i < digitLength(); i++) {
      uint64_t d = digit(digitLength() - i - 1);
      if (sizeof(Digit) == 4) {
        out.printf("%.8" PRIX32, uint32_t(d));
      } else {
        out.printf("%.16" PRIX64, d);
      }
    }
  }

  out.putChar('n');
}
#endif

JS::ubi::Node::Size JS::ubi::Concrete<BigInt>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  BigInt& bi = get();
  size_t size = sizeof(JS::BigInt);
  if (IsInsideNursery(&bi)) {
    size += Nursery::nurseryCellHeaderSize();
    size += bi.sizeOfExcludingThisInNursery(mallocSizeOf);
  } else {
    size += bi.sizeOfExcludingThis(mallocSizeOf);
  }
  return size;
}

template <XDRMode mode>
XDRResult js::XDRBigInt(XDRState<mode>* xdr, MutableHandleBigInt bi) {
  JSContext* cx = xdr->cx();

  uint8_t sign;
  uint32_t length;

  if (mode == XDR_ENCODE) {
    cx->check(bi);
    sign = static_cast<uint8_t>(bi->isNegative());
    uint64_t sz = bi->digitLength() * sizeof(BigInt::Digit);
    // As the maximum source code size is currently UINT32_MAX code units
    // (see BytecodeCompiler::checkLength), any bigint literal's length in
    // word-sized digits will be less than UINT32_MAX as well.  That could
    // change or FoldConstants could start creating these though, so leave
    // this as a release-enabled assert.
    MOZ_RELEASE_ASSERT(sz <= UINT32_MAX);
    length = static_cast<uint32_t>(sz);
  }

  MOZ_TRY(xdr->codeUint8(&sign));
  MOZ_TRY(xdr->codeUint32(&length));

  MOZ_RELEASE_ASSERT(length % sizeof(BigInt::Digit) == 0);
  uint32_t digitLength = length / sizeof(BigInt::Digit);
  auto buf = cx->make_pod_array<BigInt::Digit>(digitLength);
  if (!buf) {
    return xdr->fail(JS::TranscodeResult::Throw);
  }

  if (mode == XDR_ENCODE) {
    std::uninitialized_copy_n(bi->digits().Elements(), digitLength, buf.get());
  }

  MOZ_TRY(xdr->codeBytes(buf.get(), length));

  if (mode == XDR_DECODE) {
    BigInt* res =
        BigInt::createUninitialized(cx, digitLength, sign, gc::TenuredHeap);
    if (!res) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    std::uninitialized_copy_n(buf.get(), digitLength, res->digits().Elements());
    bi.set(res);
  }

  return Ok();
}

template XDRResult js::XDRBigInt(XDRState<XDR_ENCODE>* xdr,
                                 MutableHandleBigInt bi);

template XDRResult js::XDRBigInt(XDRState<XDR_DECODE>* xdr,
                                 MutableHandleBigInt bi);

// Public API

BigInt* JS::NumberToBigInt(JSContext* cx, double num) {
  return js::NumberToBigInt(cx, num);
}

template <typename CharT>
static inline BigInt* StringToBigIntHelper(JSContext* cx,
                                           Range<const CharT>& chars) {
  bool parseError = false;
  BigInt* bi = ParseStringBigIntLiteral(cx, chars, &parseError);
  if (!bi) {
    if (parseError) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BIGINT_INVALID_SYNTAX);
    }
    return nullptr;
  }
  MOZ_RELEASE_ASSERT(!parseError);
  return bi;
}

BigInt* JS::StringToBigInt(JSContext* cx, Range<const Latin1Char> chars) {
  return StringToBigIntHelper(cx, chars);
}

BigInt* JS::StringToBigInt(JSContext* cx, Range<const char16_t> chars) {
  return StringToBigIntHelper(cx, chars);
}

static inline BigInt* SimpleStringToBigIntHelper(
    JSContext* cx, mozilla::Span<const Latin1Char> chars, uint8_t radix,
    bool* haveParseError) {
  if (chars.Length() > 1) {
    if (chars[0] == '+') {
      return BigInt::parseLiteralDigits(
          cx, Range<const Latin1Char>{chars.From(1)}, radix,
          /* isNegative = */ false, haveParseError);
    }
    if (chars[0] == '-') {
      return BigInt::parseLiteralDigits(
          cx, Range<const Latin1Char>{chars.From(1)}, radix,
          /* isNegative = */ true, haveParseError);
    }
  }

  return BigInt::parseLiteralDigits(cx, Range<const Latin1Char>{chars}, radix,
                                    /* isNegative = */ false, haveParseError);
}

BigInt* JS::SimpleStringToBigInt(JSContext* cx, mozilla::Span<const char> chars,
                                 uint8_t radix) {
  if (chars.empty()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BIGINT_INVALID_SYNTAX);
    return nullptr;
  }
  if (radix < 2 || radix > 36) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_RADIX);
    return nullptr;
  }

  mozilla::Span<const Latin1Char> latin1{
      reinterpret_cast<const Latin1Char*>(chars.data()), chars.size()};
  bool haveParseError = false;
  BigInt* bi = SimpleStringToBigIntHelper(cx, latin1, radix, &haveParseError);
  if (!bi) {
    if (haveParseError) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BIGINT_INVALID_SYNTAX);
    }
    return nullptr;
  }
  MOZ_RELEASE_ASSERT(!haveParseError);
  return bi;
}

BigInt* JS::ToBigInt(JSContext* cx, HandleValue val) {
  return js::ToBigInt(cx, val);
}

int64_t JS::ToBigInt64(JS::BigInt* bi) { return BigInt::toInt64(bi); }

uint64_t JS::ToBigUint64(JS::BigInt* bi) { return BigInt::toUint64(bi); }

double JS::BigIntToNumber(JS::BigInt* bi) { return BigInt::numberValue(bi); }

bool JS::BigIntIsNegative(BigInt* bi) {
  return !bi->isZero() && bi->isNegative();
}

bool JS::BigIntFitsNumber(BigInt* bi, double* out) {
  return bi->isNumber(bi, out);
}

JSString* JS::BigIntToString(JSContext* cx, Handle<BigInt*> bi, uint8_t radix) {
  if (radix < 2 || radix > 36) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_RADIX);
    return nullptr;
  }
  return BigInt::toString<CanGC>(cx, bi, radix);
}

// Semi-public template details

BigInt* JS::detail::BigIntFromInt64(JSContext* cx, int64_t num) {
  return BigInt::createFromInt64(cx, num);
}

BigInt* JS::detail::BigIntFromUint64(JSContext* cx, uint64_t num) {
  return BigInt::createFromUint64(cx, num);
}

BigInt* JS::detail::BigIntFromBool(JSContext* cx, bool b) {
  return b ? BigInt::one(cx) : BigInt::zero(cx);
}

bool JS::detail::BigIntIsInt64(BigInt* bi, int64_t* result) {
  return BigInt::isInt64(bi, result);
}

bool JS::detail::BigIntIsUint64(BigInt* bi, uint64_t* result) {
  return BigInt::isUint64(bi, result);
}

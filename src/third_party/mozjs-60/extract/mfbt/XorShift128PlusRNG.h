/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* The xorshift128+ pseudo-random number generator. */

#ifndef mozilla_XorShift128Plus_h
#define mozilla_XorShift128Plus_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/FloatingPoint.h"

#include <inttypes.h>

namespace mozilla {
namespace non_crypto {

/*
 * A stream of pseudo-random numbers generated using the xorshift+ technique
 * described here:
 *
 * Vigna, Sebastiano (2014). "Further scramblings of Marsaglia's xorshift
 * generators". arXiv:1404.0390 (http://arxiv.org/abs/1404.0390)
 *
 * That paper says:
 *
 *     In particular, we propose a tightly coded xorshift128+ generator that
 *     does not fail systematically any test from the BigCrush suite of TestU01
 *     (even reversed) and generates 64 pseudorandom bits in 1.10 ns on an
 *     Intel(R) Core(TM) i7-4770 CPU @3.40GHz (Haswell). It is the fastest
 *     generator we are aware of with such empirical statistical properties.
 *
 * The stream of numbers produced by this method repeats every 2**128 - 1 calls
 * (i.e. never, for all practical purposes). Zero appears 2**64 - 1 times in
 * this period; all other numbers appear 2**64 times. Additionally, each *bit*
 * in the produced numbers repeats every 2**128 - 1 calls.
 *
 * This generator is not suitable as a cryptographically secure random number
 * generator.
 */
class XorShift128PlusRNG {
  uint64_t mState[2];

 public:
  /*
   * Construct a xorshift128+ pseudo-random number stream using |aInitial0| and
   * |aInitial1| as the initial state.  These MUST NOT both be zero.
   *
   * If the initial states contain many zeros, for a few iterations you'll see
   * many zeroes in the generated numbers.  It's suggested to seed a SplitMix64
   * generator <http://xorshift.di.unimi.it/splitmix64.c> and use its first two
   * outputs to seed xorshift128+.
   */
  XorShift128PlusRNG(uint64_t aInitial0, uint64_t aInitial1) {
    setState(aInitial0, aInitial1);
  }

  /**
   * Return a pseudo-random 64-bit number.
   */
  MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW
  uint64_t next() {
    /*
     * The offsetOfState*() methods below are provided so that exceedingly-rare
     * callers that want to observe or poke at RNG state in C++ type-system-
     * ignoring means can do so. Don't change the next() or nextDouble()
     * algorithms without altering code that uses offsetOfState*()!
     */
    uint64_t s1 = mState[0];
    const uint64_t s0 = mState[1];
    mState[0] = s0;
    s1 ^= s1 << 23;
    mState[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
    return mState[1] + s0;
  }

  /*
   * Return a pseudo-random floating-point value in the range [0, 1). More
   * precisely, choose an integer in the range [0, 2**53) and divide it by
   * 2**53. Given the 2**128 - 1 period noted above, the produced doubles are
   * all but uniformly distributed in this range.
   */
  double nextDouble() {
    /*
     * Because the IEEE 64-bit floating point format stores the leading '1' bit
     * of the mantissa implicitly, it effectively represents a mantissa in the
     * range [0, 2**53) in only 52 bits. FloatingPoint<double>::kExponentShift
     * is the width of the bitfield in the in-memory format, so we must add one
     * to get the mantissa's range.
     */
    static constexpr int kMantissaBits =
      mozilla::FloatingPoint<double>::kExponentShift + 1;
    uint64_t mantissa = next() & ((UINT64_C(1) << kMantissaBits) - 1);
    return double(mantissa) / (UINT64_C(1) << kMantissaBits);
  }

  /*
   * Set the stream's current state to |aState0| and |aState1|. These must not
   * both be zero; ideally, they should have an almost even mix of zero and one
   * bits.
   */
  void setState(uint64_t aState0, uint64_t aState1) {
    MOZ_ASSERT(aState0 || aState1);
    mState[0] = aState0;
    mState[1] = aState1;
  }

  static size_t offsetOfState0() {
    return offsetof(XorShift128PlusRNG, mState[0]);
  }
  static size_t offsetOfState1() {
    return offsetof(XorShift128PlusRNG, mState[1]);
  }
};

} // namespace non_crypto
} // namespace mozilla

#endif // mozilla_XorShift128Plus_h

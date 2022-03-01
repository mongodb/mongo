/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Utilities for hashing. */

/*
 * This file exports functions for hashing data down to a uint32_t (a.k.a.
 * mozilla::HashNumber), including:
 *
 *  - HashString    Hash a char* or char16_t/wchar_t* of known or unknown
 *                  length.
 *
 *  - HashBytes     Hash a byte array of known length.
 *
 *  - HashGeneric   Hash one or more values.  Currently, we support uint32_t,
 *                  types which can be implicitly cast to uint32_t, data
 *                  pointers, and function pointers.
 *
 *  - AddToHash     Add one or more values to the given hash.  This supports the
 *                  same list of types as HashGeneric.
 *
 *
 * You can chain these functions together to hash complex objects.  For example:
 *
 *  class ComplexObject
 *  {
 *    char* mStr;
 *    uint32_t mUint1, mUint2;
 *    void (*mCallbackFn)();
 *
 *  public:
 *    HashNumber hash()
 *    {
 *      HashNumber hash = HashString(mStr);
 *      hash = AddToHash(hash, mUint1, mUint2);
 *      return AddToHash(hash, mCallbackFn);
 *    }
 *  };
 *
 * If you want to hash an nsAString or nsACString, use the HashString functions
 * in nsHashKeys.h.
 */

#ifndef mozilla_HashFunctions_h
#define mozilla_HashFunctions_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Char16.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Types.h"
#include "mozilla/WrappingOperations.h"

#include <stdint.h>
#include <type_traits>

namespace mozilla {

using HashNumber = uint32_t;
static const uint32_t kHashNumberBits = 32;

/**
 * The golden ratio as a 32-bit fixed-point value.
 */
static const HashNumber kGoldenRatioU32 = 0x9E3779B9U;

/*
 * Given a raw hash code, h, return a number that can be used to select a hash
 * bucket.
 *
 * This function aims to produce as uniform an output distribution as possible,
 * especially in the most significant (leftmost) bits, even though the input
 * distribution may be highly nonrandom, given the constraints that this must
 * be deterministic and quick to compute.
 *
 * Since the leftmost bits of the result are best, the hash bucket index is
 * computed by doing ScrambleHashCode(h) / (2^32/N) or the equivalent
 * right-shift, not ScrambleHashCode(h) % N or the equivalent bit-mask.
 *
 * FIXME: OrderedHashTable uses a bit-mask; see bug 775896.
 */
constexpr HashNumber ScrambleHashCode(HashNumber h) {
  /*
   * Simply returning h would not cause any hash tables to produce wrong
   * answers. But it can produce pathologically bad performance: The caller
   * right-shifts the result, keeping only the highest bits. The high bits of
   * hash codes are very often completely entropy-free. (So are the lowest
   * bits.)
   *
   * So we use Fibonacci hashing, as described in Knuth, The Art of Computer
   * Programming, 6.4. This mixes all the bits of the input hash code h.
   *
   * The value of goldenRatio is taken from the hex expansion of the golden
   * ratio, which starts 1.9E3779B9.... This value is especially good if
   * values with consecutive hash codes are stored in a hash table; see Knuth
   * for details.
   */
  return mozilla::WrappingMultiply(h, kGoldenRatioU32);
}

namespace detail {

MOZ_NO_SANITIZE_UNSIGNED_OVERFLOW
constexpr HashNumber RotateLeft5(HashNumber aValue) {
  return (aValue << 5) | (aValue >> 27);
}

constexpr HashNumber AddU32ToHash(HashNumber aHash, uint32_t aValue) {
  /*
   * This is the meat of all our hash routines.  This hash function is not
   * particularly sophisticated, but it seems to work well for our mostly
   * plain-text inputs.  Implementation notes follow.
   *
   * Our use of the golden ratio here is arbitrary; we could pick almost any
   * number which:
   *
   *  * is odd (because otherwise, all our hash values will be even)
   *
   *  * has a reasonably-even mix of 1's and 0's (consider the extreme case
   *    where we multiply by 0x3 or 0xeffffff -- this will not produce good
   *    mixing across all bits of the hash).
   *
   * The rotation length of 5 is also arbitrary, although an odd number is again
   * preferable so our hash explores the whole universe of possible rotations.
   *
   * Finally, we multiply by the golden ratio *after* xor'ing, not before.
   * Otherwise, if |aHash| is 0 (as it often is for the beginning of a
   * message), the expression
   *
   *   mozilla::WrappingMultiply(kGoldenRatioU32, RotateLeft5(aHash))
   *   |xor|
   *   aValue
   *
   * evaluates to |aValue|.
   *
   * (Number-theoretic aside: Because any odd number |m| is relatively prime to
   * our modulus (2**32), the list
   *
   *    [x * m (mod 2**32) for 0 <= x < 2**32]
   *
   * has no duplicate elements.  This means that multiplying by |m| does not
   * cause us to skip any possible hash values.
   *
   * It's also nice if |m| has large-ish order mod 2**32 -- that is, if the
   * smallest k such that m**k == 1 (mod 2**32) is large -- so we can safely
   * multiply our hash value by |m| a few times without negating the
   * multiplicative effect.  Our golden ratio constant has order 2**29, which is
   * more than enough for our purposes.)
   */
  return mozilla::WrappingMultiply(kGoldenRatioU32,
                                   RotateLeft5(aHash) ^ aValue);
}

/**
 * AddUintptrToHash takes sizeof(uintptr_t) as a template parameter.
 */
template <size_t PtrSize>
constexpr HashNumber AddUintptrToHash(HashNumber aHash, uintptr_t aValue) {
  return AddU32ToHash(aHash, static_cast<uint32_t>(aValue));
}

template <>
inline HashNumber AddUintptrToHash<8>(HashNumber aHash, uintptr_t aValue) {
  uint32_t v1 = static_cast<uint32_t>(aValue);
  uint32_t v2 = static_cast<uint32_t>(static_cast<uint64_t>(aValue) >> 32);
  return AddU32ToHash(AddU32ToHash(aHash, v1), v2);
}

} /* namespace detail */

/**
 * AddToHash takes a hash and some values and returns a new hash based on the
 * inputs.
 *
 * Currently, we support hashing uint32_t's, values which we can implicitly
 * convert to uint32_t, data pointers, and function pointers.
 */
template <typename T, bool TypeIsNotIntegral = !std::is_integral_v<T>,
          bool TypeIsNotEnum = !std::is_enum_v<T>,
          std::enable_if_t<TypeIsNotIntegral && TypeIsNotEnum, int> = 0>
[[nodiscard]] inline HashNumber AddToHash(HashNumber aHash, T aA) {
  /*
   * Try to convert |A| to uint32_t implicitly.  If this works, great.  If not,
   * we'll error out.
   */
  return detail::AddU32ToHash(aHash, aA);
}

template <typename A>
[[nodiscard]] inline HashNumber AddToHash(HashNumber aHash, A* aA) {
  /*
   * You might think this function should just take a void*.  But then we'd only
   * catch data pointers and couldn't handle function pointers.
   */

  static_assert(sizeof(aA) == sizeof(uintptr_t), "Strange pointer!");

  return detail::AddUintptrToHash<sizeof(uintptr_t)>(aHash, uintptr_t(aA));
}

// We use AddUintptrToHash() for hashing all integral types.  8-byte integral
// types are treated the same as 64-bit pointers, and smaller integral types are
// first implicitly converted to 32 bits and then passed to AddUintptrToHash()
// to be hashed.
template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
[[nodiscard]] constexpr HashNumber AddToHash(HashNumber aHash, T aA) {
  return detail::AddUintptrToHash<sizeof(T)>(aHash, aA);
}

template <typename T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
[[nodiscard]] constexpr HashNumber AddToHash(HashNumber aHash, T aA) {
  // Hash using AddUintptrToHash with the underlying type of the enum type
  using UnderlyingType = typename std::underlying_type<T>::type;
  return detail::AddUintptrToHash<sizeof(UnderlyingType)>(
      aHash, static_cast<UnderlyingType>(aA));
}

template <typename A, typename... Args>
[[nodiscard]] HashNumber AddToHash(HashNumber aHash, A aArg, Args... aArgs) {
  return AddToHash(AddToHash(aHash, aArg), aArgs...);
}

/**
 * The HashGeneric class of functions let you hash one or more values.
 *
 * If you want to hash together two values x and y, calling HashGeneric(x, y) is
 * much better than calling AddToHash(x, y), because AddToHash(x, y) assumes
 * that x has already been hashed.
 */
template <typename... Args>
[[nodiscard]] inline HashNumber HashGeneric(Args... aArgs) {
  return AddToHash(0, aArgs...);
}

/**
 * Hash successive |*aIter| until |!*aIter|, i.e. til null-termination.
 *
 * This function is *not* named HashString like the non-template overloads
 * below.  Some users define HashString overloads and pass inexactly-matching
 * values to them -- but an inexactly-matching value would match this overload
 * instead!  We follow the general rule and don't mix and match template and
 * regular overloads to avoid this.
 *
 * If you have the string's length, call HashStringKnownLength: it may be
 * marginally faster.
 */
template <typename Iterator>
[[nodiscard]] constexpr HashNumber HashStringUntilZero(Iterator aIter) {
  HashNumber hash = 0;
  for (; auto c = *aIter; ++aIter) {
    hash = AddToHash(hash, c);
  }
  return hash;
}

/**
 * Hash successive |aIter[i]| up to |i == aLength|.
 */
template <typename Iterator>
[[nodiscard]] constexpr HashNumber HashStringKnownLength(Iterator aIter,
                                                         size_t aLength) {
  HashNumber hash = 0;
  for (size_t i = 0; i < aLength; i++) {
    hash = AddToHash(hash, aIter[i]);
  }
  return hash;
}

/**
 * The HashString overloads below do just what you'd expect.
 *
 * These functions are non-template functions so that users can 1) overload them
 * with their own types 2) in a way that allows implicit conversions to happen.
 */
[[nodiscard]] inline HashNumber HashString(const char* aStr) {
  // Use the |const unsigned char*| version of the above so that all ordinary
  // character data hashes identically.
  return HashStringUntilZero(reinterpret_cast<const unsigned char*>(aStr));
}

[[nodiscard]] inline HashNumber HashString(const char* aStr, size_t aLength) {
  // Delegate to the |const unsigned char*| version of the above to share
  // template instantiations.
  return HashStringKnownLength(reinterpret_cast<const unsigned char*>(aStr),
                               aLength);
}

[[nodiscard]] inline HashNumber HashString(const unsigned char* aStr,
                                           size_t aLength) {
  return HashStringKnownLength(aStr, aLength);
}

[[nodiscard]] constexpr HashNumber HashString(const char16_t* aStr) {
  return HashStringUntilZero(aStr);
}

[[nodiscard]] inline HashNumber HashString(const char16_t* aStr,
                                           size_t aLength) {
  return HashStringKnownLength(aStr, aLength);
}

/**
 * HashString overloads for |wchar_t| on platforms where it isn't |char16_t|.
 */
template <typename WCharT, typename = typename std::enable_if<
                               std::is_same<WCharT, wchar_t>::value &&
                               !std::is_same<wchar_t, char16_t>::value>::type>
[[nodiscard]] inline HashNumber HashString(const WCharT* aStr) {
  return HashStringUntilZero(aStr);
}

template <typename WCharT, typename = typename std::enable_if<
                               std::is_same<WCharT, wchar_t>::value &&
                               !std::is_same<wchar_t, char16_t>::value>::type>
[[nodiscard]] inline HashNumber HashString(const WCharT* aStr, size_t aLength) {
  return HashStringKnownLength(aStr, aLength);
}

/**
 * Hash some number of bytes.
 *
 * This hash walks word-by-word, rather than byte-by-byte, so you won't get the
 * same result out of HashBytes as you would out of HashString.
 */
[[nodiscard]] extern MFBT_API HashNumber HashBytes(const void* bytes,
                                                   size_t aLength);

/**
 * A pseudorandom function mapping 32-bit integers to 32-bit integers.
 *
 * This is for when you're feeding private data (like pointer values or credit
 * card numbers) to a non-crypto hash function (like HashBytes) and then using
 * the hash code for something that untrusted parties could observe (like a JS
 * Map). Plug in a HashCodeScrambler before that last step to avoid leaking the
 * private data.
 *
 * By itself, this does not prevent hash-flooding DoS attacks, because an
 * attacker can still generate many values with exactly equal hash codes by
 * attacking the non-crypto hash function alone. Equal hash codes will, of
 * course, still be equal however much you scramble them.
 *
 * The algorithm is SipHash-1-3. See <https://131002.net/siphash/>.
 */
class HashCodeScrambler {
  struct SipHasher;

  uint64_t mK0, mK1;

 public:
  /** Creates a new scrambler with the given 128-bit key. */
  constexpr HashCodeScrambler(uint64_t aK0, uint64_t aK1)
      : mK0(aK0), mK1(aK1) {}

  /**
   * Scramble a hash code. Always produces the same result for the same
   * combination of key and hash code.
   */
  HashNumber scramble(HashNumber aHashCode) const {
    SipHasher hasher(mK0, mK1);
    return HashNumber(hasher.sipHash(aHashCode));
  }

 private:
  struct SipHasher {
    SipHasher(uint64_t aK0, uint64_t aK1) {
      // 1. Initialization.
      mV0 = aK0 ^ UINT64_C(0x736f6d6570736575);
      mV1 = aK1 ^ UINT64_C(0x646f72616e646f6d);
      mV2 = aK0 ^ UINT64_C(0x6c7967656e657261);
      mV3 = aK1 ^ UINT64_C(0x7465646279746573);
    }

    uint64_t sipHash(uint64_t aM) {
      // 2. Compression.
      mV3 ^= aM;
      sipRound();
      mV0 ^= aM;

      // 3. Finalization.
      mV2 ^= 0xff;
      for (int i = 0; i < 3; i++) sipRound();
      return mV0 ^ mV1 ^ mV2 ^ mV3;
    }

    void sipRound() {
      mV0 = WrappingAdd(mV0, mV1);
      mV1 = RotateLeft(mV1, 13);
      mV1 ^= mV0;
      mV0 = RotateLeft(mV0, 32);
      mV2 = WrappingAdd(mV2, mV3);
      mV3 = RotateLeft(mV3, 16);
      mV3 ^= mV2;
      mV0 = WrappingAdd(mV0, mV3);
      mV3 = RotateLeft(mV3, 21);
      mV3 ^= mV0;
      mV2 = WrappingAdd(mV2, mV1);
      mV1 = RotateLeft(mV1, 17);
      mV1 ^= mV2;
      mV2 = RotateLeft(mV2, 32);
    }

    uint64_t mV0, mV1, mV2, mV3;
  };
};

} /* namespace mozilla */

#endif /* mozilla_HashFunctions_h */

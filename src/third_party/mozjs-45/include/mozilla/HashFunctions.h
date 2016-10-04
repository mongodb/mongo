/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Utilities for hashing. */

/*
 * This file exports functions for hashing data down to a 32-bit value,
 * including:
 *
 *  - HashString    Hash a char* or uint16_t/wchar_t* of known or unknown
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
 *    uint32_t hash()
 *    {
 *      uint32_t hash = HashString(mStr);
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
#include "mozilla/Types.h"

#include <stdint.h>

#ifdef __cplusplus
namespace mozilla {

/**
 * The golden ratio as a 32-bit fixed-point value.
 */
static const uint32_t kGoldenRatioU32 = 0x9E3779B9U;

inline uint32_t
RotateBitsLeft32(uint32_t aValue, uint8_t aBits)
{
  MOZ_ASSERT(aBits < 32);
  return (aValue << aBits) | (aValue >> (32 - aBits));
}

namespace detail {

inline uint32_t
AddU32ToHash(uint32_t aHash, uint32_t aValue)
{
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
   *   (kGoldenRatioU32 * RotateBitsLeft(aHash, 5)) |xor| aValue
   *
   * evaluates to |aValue|.
   *
   * (Number-theoretic aside: Because any odd number |m| is relatively prime to
   * our modulus (2^32), the list
   *
   *    [x * m (mod 2^32) for 0 <= x < 2^32]
   *
   * has no duplicate elements.  This means that multiplying by |m| does not
   * cause us to skip any possible hash values.
   *
   * It's also nice if |m| has large-ish order mod 2^32 -- that is, if the
   * smallest k such that m^k == 1 (mod 2^32) is large -- so we can safely
   * multiply our hash value by |m| a few times without negating the
   * multiplicative effect.  Our golden ratio constant has order 2^29, which is
   * more than enough for our purposes.)
   */
  return kGoldenRatioU32 * (RotateBitsLeft32(aHash, 5) ^ aValue);
}

/**
 * AddUintptrToHash takes sizeof(uintptr_t) as a template parameter.
 */
template<size_t PtrSize>
inline uint32_t
AddUintptrToHash(uint32_t aHash, uintptr_t aValue);

template<>
inline uint32_t
AddUintptrToHash<4>(uint32_t aHash, uintptr_t aValue)
{
  return AddU32ToHash(aHash, static_cast<uint32_t>(aValue));
}

template<>
inline uint32_t
AddUintptrToHash<8>(uint32_t aHash, uintptr_t aValue)
{
  /*
   * The static cast to uint64_t below is necessary because this function
   * sometimes gets compiled on 32-bit platforms (yes, even though it's a
   * template and we never call this particular override in a 32-bit build).  If
   * we do aValue >> 32 on a 32-bit machine, we're shifting a 32-bit uintptr_t
   * right 32 bits, and the compiler throws an error.
   */
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
template<typename A>
MOZ_WARN_UNUSED_RESULT inline uint32_t
AddToHash(uint32_t aHash, A aA)
{
  /*
   * Try to convert |A| to uint32_t implicitly.  If this works, great.  If not,
   * we'll error out.
   */
  return detail::AddU32ToHash(aHash, aA);
}

template<typename A>
MOZ_WARN_UNUSED_RESULT inline uint32_t
AddToHash(uint32_t aHash, A* aA)
{
  /*
   * You might think this function should just take a void*.  But then we'd only
   * catch data pointers and couldn't handle function pointers.
   */

  static_assert(sizeof(aA) == sizeof(uintptr_t), "Strange pointer!");

  return detail::AddUintptrToHash<sizeof(uintptr_t)>(aHash, uintptr_t(aA));
}

template<>
MOZ_WARN_UNUSED_RESULT inline uint32_t
AddToHash(uint32_t aHash, uintptr_t aA)
{
  return detail::AddUintptrToHash<sizeof(uintptr_t)>(aHash, aA);
}

template<typename A, typename... Args>
MOZ_WARN_UNUSED_RESULT uint32_t
AddToHash(uint32_t aHash, A aArg, Args... aArgs)
{
  return AddToHash(AddToHash(aHash, aArg), aArgs...);
}

/**
 * The HashGeneric class of functions let you hash one or more values.
 *
 * If you want to hash together two values x and y, calling HashGeneric(x, y) is
 * much better than calling AddToHash(x, y), because AddToHash(x, y) assumes
 * that x has already been hashed.
 */
template<typename... Args>
MOZ_WARN_UNUSED_RESULT inline uint32_t
HashGeneric(Args... aArgs)
{
  return AddToHash(0, aArgs...);
}

namespace detail {

template<typename T>
uint32_t
HashUntilZero(const T* aStr)
{
  uint32_t hash = 0;
  for (T c; (c = *aStr); aStr++) {
    hash = AddToHash(hash, c);
  }
  return hash;
}

template<typename T>
uint32_t
HashKnownLength(const T* aStr, size_t aLength)
{
  uint32_t hash = 0;
  for (size_t i = 0; i < aLength; i++) {
    hash = AddToHash(hash, aStr[i]);
  }
  return hash;
}

} /* namespace detail */

/**
 * The HashString overloads below do just what you'd expect.
 *
 * If you have the string's length, you might as well call the overload which
 * includes the length.  It may be marginally faster.
 */
MOZ_WARN_UNUSED_RESULT inline uint32_t
HashString(const char* aStr)
{
  return detail::HashUntilZero(reinterpret_cast<const unsigned char*>(aStr));
}

MOZ_WARN_UNUSED_RESULT inline uint32_t
HashString(const char* aStr, size_t aLength)
{
  return detail::HashKnownLength(reinterpret_cast<const unsigned char*>(aStr), aLength);
}

MOZ_WARN_UNUSED_RESULT
inline uint32_t
HashString(const unsigned char* aStr, size_t aLength)
{
  return detail::HashKnownLength(aStr, aLength);
}

MOZ_WARN_UNUSED_RESULT inline uint32_t
HashString(const uint16_t* aStr)
{
  return detail::HashUntilZero(aStr);
}

MOZ_WARN_UNUSED_RESULT inline uint32_t
HashString(const uint16_t* aStr, size_t aLength)
{
  return detail::HashKnownLength(aStr, aLength);
}

#ifdef MOZ_CHAR16_IS_NOT_WCHAR
MOZ_WARN_UNUSED_RESULT inline uint32_t
HashString(const char16_t* aStr)
{
  return detail::HashUntilZero(aStr);
}

MOZ_WARN_UNUSED_RESULT inline uint32_t
HashString(const char16_t* aStr, size_t aLength)
{
  return detail::HashKnownLength(aStr, aLength);
}
#endif

/*
 * On Windows, wchar_t (char16_t) is not the same as uint16_t, even though it's
 * the same width!
 */
#ifdef WIN32
MOZ_WARN_UNUSED_RESULT inline uint32_t
HashString(const wchar_t* aStr)
{
  return detail::HashUntilZero(aStr);
}

MOZ_WARN_UNUSED_RESULT inline uint32_t
HashString(const wchar_t* aStr, size_t aLength)
{
  return detail::HashKnownLength(aStr, aLength);
}
#endif

/**
 * Hash some number of bytes.
 *
 * This hash walks word-by-word, rather than byte-by-byte, so you won't get the
 * same result out of HashBytes as you would out of HashString.
 */
MOZ_WARN_UNUSED_RESULT extern MFBT_API uint32_t
HashBytes(const void* bytes, size_t aLength);

} /* namespace mozilla */
#endif /* __cplusplus */

#endif /* mozilla_HashFunctions_h */

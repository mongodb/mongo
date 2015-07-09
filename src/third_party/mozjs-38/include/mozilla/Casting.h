/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Cast operations to supplement the built-in casting operations. */

#ifndef mozilla_Casting_h
#define mozilla_Casting_h

#include "mozilla/Assertions.h"
#include "mozilla/TypeTraits.h"

#include <limits.h>

namespace mozilla {

/**
 * Return a value of type |To|, containing the underlying bit pattern of
 * |aFrom|.
 *
 * |To| and |From| must be types of the same size; be careful of cross-platform
 * size differences, or this might fail to compile on some but not all
 * platforms.
 */
template<typename To, typename From>
inline To
BitwiseCast(const From aFrom)
{
  static_assert(sizeof(From) == sizeof(To),
                "To and From must have the same size");
  union
  {
    From mFrom;
    To mTo;
  } u;
  u.mFrom = aFrom;
  return u.mTo;
}

namespace detail {

enum ToSignedness { ToIsSigned, ToIsUnsigned };
enum FromSignedness { FromIsSigned, FromIsUnsigned };

template<typename From,
         typename To,
         FromSignedness = IsSigned<From>::value ? FromIsSigned : FromIsUnsigned,
         ToSignedness = IsSigned<To>::value ? ToIsSigned : ToIsUnsigned>
struct BoundsCheckImpl;

// Implicit conversions on operands to binary operations make this all a bit
// hard to verify.  Attempt to ease the pain below by *only* comparing values
// that are obviously the same type (and will undergo no further conversions),
// even when it's not strictly necessary, for explicitness.

enum UUComparison { FromIsBigger, FromIsNotBigger };

// Unsigned-to-unsigned range check

template<typename From, typename To,
         UUComparison = (sizeof(From) > sizeof(To))
                        ? FromIsBigger
                        : FromIsNotBigger>
struct UnsignedUnsignedCheck;

template<typename From, typename To>
struct UnsignedUnsignedCheck<From, To, FromIsBigger>
{
public:
  static bool checkBounds(const From aFrom)
  {
    return aFrom <= From(To(-1));
  }
};

template<typename From, typename To>
struct UnsignedUnsignedCheck<From, To, FromIsNotBigger>
{
public:
  static bool checkBounds(const From aFrom)
  {
    return true;
  }
};

template<typename From, typename To>
struct BoundsCheckImpl<From, To, FromIsUnsigned, ToIsUnsigned>
{
public:
  static bool checkBounds(const From aFrom)
  {
    return UnsignedUnsignedCheck<From, To>::checkBounds(aFrom);
  }
};

// Signed-to-unsigned range check

template<typename From, typename To>
struct BoundsCheckImpl<From, To, FromIsSigned, ToIsUnsigned>
{
public:
  static bool checkBounds(const From aFrom)
  {
    if (aFrom < 0) {
      return false;
    }
    if (sizeof(To) >= sizeof(From)) {
      return true;
    }
    return aFrom <= From(To(-1));
  }
};

// Unsigned-to-signed range check

enum USComparison { FromIsSmaller, FromIsNotSmaller };

template<typename From, typename To,
         USComparison = (sizeof(From) < sizeof(To))
                        ? FromIsSmaller
                        : FromIsNotSmaller>
struct UnsignedSignedCheck;

template<typename From, typename To>
struct UnsignedSignedCheck<From, To, FromIsSmaller>
{
public:
  static bool checkBounds(const From aFrom)
  {
    return true;
  }
};

template<typename From, typename To>
struct UnsignedSignedCheck<From, To, FromIsNotSmaller>
{
public:
  static bool checkBounds(const From aFrom)
  {
    const To MaxValue = To((1ULL << (CHAR_BIT * sizeof(To) - 1)) - 1);
    return aFrom <= From(MaxValue);
  }
};

template<typename From, typename To>
struct BoundsCheckImpl<From, To, FromIsUnsigned, ToIsSigned>
{
public:
  static bool checkBounds(const From aFrom)
  {
    return UnsignedSignedCheck<From, To>::checkBounds(aFrom);
  }
};

// Signed-to-signed range check

template<typename From, typename To>
struct BoundsCheckImpl<From, To, FromIsSigned, ToIsSigned>
{
public:
  static bool checkBounds(const From aFrom)
  {
    if (sizeof(From) <= sizeof(To)) {
      return true;
    }
    const To MaxValue = To((1ULL << (CHAR_BIT * sizeof(To) - 1)) - 1);
    const To MinValue = -MaxValue - To(1);
    return From(MinValue) <= aFrom &&
           From(aFrom) <= From(MaxValue);
  }
};

template<typename From, typename To,
         bool TypesAreIntegral = IsIntegral<From>::value &&
                                 IsIntegral<To>::value>
class BoundsChecker;

template<typename From>
class BoundsChecker<From, From, true>
{
public:
  static bool checkBounds(const From aFrom) { return true; }
};

template<typename From, typename To>
class BoundsChecker<From, To, true>
{
public:
  static bool checkBounds(const From aFrom)
  {
    return BoundsCheckImpl<From, To>::checkBounds(aFrom);
  }
};

template<typename From, typename To>
inline bool
IsInBounds(const From aFrom)
{
  return BoundsChecker<From, To>::checkBounds(aFrom);
}

} // namespace detail

/**
 * Cast a value of integral type |From| to a value of integral type |To|,
 * asserting that the cast will be a safe cast per C++ (that is, that |to| is in
 * the range of values permitted for the type |From|).
 */
template<typename To, typename From>
inline To
AssertedCast(const From aFrom)
{
  MOZ_ASSERT((detail::IsInBounds<From, To>(aFrom)));
  return static_cast<To>(aFrom);
}

} // namespace mozilla

#endif /* mozilla_Casting_h */

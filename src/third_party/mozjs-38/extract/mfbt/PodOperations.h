/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Operations for zeroing POD types, arrays, and so on.
 *
 * These operations are preferable to memset, memcmp, and the like because they
 * don't require remembering to multiply by sizeof(T), array lengths, and so on
 * everywhere.
 */

#ifndef mozilla_PodOperations_h
#define mozilla_PodOperations_h

#include "mozilla/Array.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/Attributes.h"

#include <stdint.h>
#include <string.h>

namespace mozilla {

/** Set the contents of |aT| to 0. */
template<typename T>
static MOZ_ALWAYS_INLINE void
PodZero(T* aT)
{
  memset(aT, 0, sizeof(T));
}

/** Set the contents of |aNElem| elements starting at |aT| to 0. */
template<typename T>
static MOZ_ALWAYS_INLINE void
PodZero(T* aT, size_t aNElem)
{
  /*
   * This function is often called with 'aNElem' small; we use an inline loop
   * instead of calling 'memset' with a non-constant length.  The compiler
   * should inline the memset call with constant size, though.
   */
  for (T* end = aT + aNElem; aT < end; aT++) {
    memset(aT, 0, sizeof(T));
  }
}

/*
 * Arrays implicitly convert to pointers to their first element, which is
 * dangerous when combined with the above PodZero definitions.  Adding an
 * overload for arrays is ambiguous, so we need another identifier.  The
 * ambiguous overload is left to catch mistaken uses of PodZero; if you get a
 * compile error involving PodZero and array types, use PodArrayZero instead.
 */
template<typename T, size_t N>
static void PodZero(T (&aT)[N]) = delete;
template<typename T, size_t N>
static void PodZero(T (&aT)[N], size_t aNElem) = delete;

/** Set the contents of the array |aT| to zero. */
template <class T, size_t N>
static MOZ_ALWAYS_INLINE void
PodArrayZero(T (&aT)[N])
{
  memset(aT, 0, N * sizeof(T));
}

template <typename T, size_t N>
static MOZ_ALWAYS_INLINE void
PodArrayZero(Array<T, N>& aArr)
{
  memset(&aArr[0], 0, N * sizeof(T));
}

/**
 * Assign |*aSrc| to |*aDst|.  The locations must not be the same and must not
 * overlap.
 */
template<typename T>
static MOZ_ALWAYS_INLINE void
PodAssign(T* aDst, const T* aSrc)
{
  MOZ_ASSERT(aDst + 1 <= aSrc || aSrc + 1 <= aDst,
             "destination and source must not overlap");
  memcpy(reinterpret_cast<char*>(aDst), reinterpret_cast<const char*>(aSrc),
         sizeof(T));
}

/**
 * Copy |aNElem| T elements from |aSrc| to |aDst|.  The two memory ranges must
 * not overlap!
 */
template<typename T>
static MOZ_ALWAYS_INLINE void
PodCopy(T* aDst, const T* aSrc, size_t aNElem)
{
  MOZ_ASSERT(aDst + aNElem <= aSrc || aSrc + aNElem <= aDst,
             "destination and source must not overlap");
  if (aNElem < 128) {
    /*
     * Avoid using operator= in this loop, as it may have been
     * intentionally deleted by the POD type.
     */
    for (const T* srcend = aSrc + aNElem; aSrc < srcend; aSrc++, aDst++) {
      PodAssign(aDst, aSrc);
    }
  } else {
    memcpy(aDst, aSrc, aNElem * sizeof(T));
  }
}

template<typename T>
static MOZ_ALWAYS_INLINE void
PodCopy(volatile T* aDst, const volatile T* aSrc, size_t aNElem)
{
  MOZ_ASSERT(aDst + aNElem <= aSrc || aSrc + aNElem <= aDst,
             "destination and source must not overlap");

  /*
   * Volatile |aDst| requires extra work, because it's undefined behavior to
   * modify volatile objects using the mem* functions.  Just write out the
   * loops manually, using operator= rather than memcpy for the same reason,
   * and let the compiler optimize to the extent it can.
   */
  for (const volatile T* srcend = aSrc + aNElem;
       aSrc < srcend;
       aSrc++, aDst++) {
    *aDst = *aSrc;
  }
}

/*
 * Copy the contents of the array |aSrc| into the array |aDst|, both of size N.
 * The arrays must not overlap!
 */
template <class T, size_t N>
static MOZ_ALWAYS_INLINE void
PodArrayCopy(T (&aDst)[N], const T (&aSrc)[N])
{
  PodCopy(aDst, aSrc, N);
}

/**
 * Copy the memory for |aNElem| T elements from |aSrc| to |aDst|.  If the two
 * memory ranges overlap, then the effect is as if the |aNElem| elements are
 * first copied from |aSrc| to a temporary array, and then from the temporary
 * array to |aDst|.
 */
template<typename T>
static MOZ_ALWAYS_INLINE void
PodMove(T* aDst, const T* aSrc, size_t aNElem)
{
  MOZ_ASSERT(aNElem <= SIZE_MAX / sizeof(T),
             "trying to move an impossible number of elements");
  memmove(aDst, aSrc, aNElem * sizeof(T));
}

/**
 * Determine whether the |len| elements at |one| are memory-identical to the
 * |len| elements at |two|.
 */
template<typename T>
static MOZ_ALWAYS_INLINE bool
PodEqual(const T* one, const T* two, size_t len)
{
  if (len < 128) {
    const T* p1end = one + len;
    const T* p1 = one;
    const T* p2 = two;
    for (; p1 < p1end; p1++, p2++) {
      if (*p1 != *p2) {
        return false;
      }
    }
    return true;
  }

  return !memcmp(one, two, len * sizeof(T));
}

} // namespace mozilla

#endif /* mozilla_PodOperations_h */

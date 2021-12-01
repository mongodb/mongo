/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implements a smart pointer asserted to remain within a range specified at
 * construction.
 */

#ifndef mozilla_RangedPtr_h
#define mozilla_RangedPtr_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stdint.h>

namespace mozilla {

/*
 * RangedPtr is a smart pointer restricted to an address range specified at
 * creation.  The pointer (and any smart pointers derived from it) must remain
 * within the range [start, end] (inclusive of end to facilitate use as
 * sentinels).  Dereferencing or indexing into the pointer (or pointers derived
 * from it) must remain within the range [start, end).  All the standard pointer
 * operators are defined on it; in debug builds these operations assert that the
 * range specified at construction is respected.
 *
 * In theory passing a smart pointer instance as an argument can be slightly
 * slower than passing a T* (due to ABI requirements for passing structs versus
 * passing pointers), if the method being called isn't inlined.  If you are in
 * extremely performance-critical code, you may want to be careful using this
 * smart pointer as an argument type.
 *
 * RangedPtr<T> intentionally does not implicitly convert to T*.  Use get() to
 * explicitly convert to T*.  Keep in mind that the raw pointer of course won't
 * implement bounds checking in debug builds.
 */
template<typename T>
class RangedPtr
{
  T* mPtr;

#ifdef DEBUG
  T* const mRangeStart;
  T* const mRangeEnd;
#endif

  void checkSanity()
  {
    MOZ_ASSERT(mRangeStart <= mPtr);
    MOZ_ASSERT(mPtr <= mRangeEnd);
  }

  /* Creates a new pointer for |aPtr|, restricted to this pointer's range. */
  RangedPtr<T> create(T* aPtr) const
  {
#ifdef DEBUG
    return RangedPtr<T>(aPtr, mRangeStart, mRangeEnd);
#else
    return RangedPtr<T>(aPtr, nullptr, size_t(0));
#endif
  }

  uintptr_t asUintptr() const { return reinterpret_cast<uintptr_t>(mPtr); }

public:
  RangedPtr(T* aPtr, T* aStart, T* aEnd)
    : mPtr(aPtr)
#ifdef DEBUG
    , mRangeStart(aStart), mRangeEnd(aEnd)
#endif
  {
    MOZ_ASSERT(mRangeStart <= mRangeEnd);
    checkSanity();
  }
  RangedPtr(T* aPtr, T* aStart, size_t aLength)
    : mPtr(aPtr)
#ifdef DEBUG
    , mRangeStart(aStart), mRangeEnd(aStart + aLength)
#endif
  {
    MOZ_ASSERT(aLength <= size_t(-1) / sizeof(T));
    MOZ_ASSERT(reinterpret_cast<uintptr_t>(mRangeStart) + aLength * sizeof(T) >=
               reinterpret_cast<uintptr_t>(mRangeStart));
    checkSanity();
  }

  /* Equivalent to RangedPtr(aPtr, aPtr, aLength). */
  RangedPtr(T* aPtr, size_t aLength)
    : mPtr(aPtr)
#ifdef DEBUG
    , mRangeStart(aPtr), mRangeEnd(aPtr + aLength)
#endif
  {
    MOZ_ASSERT(aLength <= size_t(-1) / sizeof(T));
    MOZ_ASSERT(reinterpret_cast<uintptr_t>(mRangeStart) + aLength * sizeof(T) >=
               reinterpret_cast<uintptr_t>(mRangeStart));
    checkSanity();
  }

  /* Equivalent to RangedPtr(aArr, aArr, N). */
  template<size_t N>
  explicit RangedPtr(T (&aArr)[N])
    : mPtr(aArr)
#ifdef DEBUG
    , mRangeStart(aArr), mRangeEnd(aArr + N)
#endif
  {
    checkSanity();
  }

  T* get() const { return mPtr; }

  explicit operator bool() const { return mPtr != nullptr; }

  void checkIdenticalRange(const RangedPtr<T>& aOther) const
  {
    MOZ_ASSERT(mRangeStart == aOther.mRangeStart);
    MOZ_ASSERT(mRangeEnd == aOther.mRangeEnd);
  }

  /*
   * You can only assign one RangedPtr into another if the two pointers have
   * the same valid range:
   *
   *   char arr1[] = "hi";
   *   char arr2[] = "bye";
   *   RangedPtr<char> p1(arr1, 2);
   *   p1 = RangedPtr<char>(arr1 + 1, arr1, arr1 + 2); // works
   *   p1 = RangedPtr<char>(arr2, 3);                  // asserts
   */
  RangedPtr<T>& operator=(const RangedPtr<T>& aOther)
  {
    checkIdenticalRange(aOther);
    mPtr = aOther.mPtr;
    checkSanity();
    return *this;
  }

  RangedPtr<T> operator+(size_t aInc) const
  {
    MOZ_ASSERT(aInc <= size_t(-1) / sizeof(T));
    MOZ_ASSERT(asUintptr() + aInc * sizeof(T) >= asUintptr());
    return create(mPtr + aInc);
  }

  RangedPtr<T> operator-(size_t aDec) const
  {
    MOZ_ASSERT(aDec <= size_t(-1) / sizeof(T));
    MOZ_ASSERT(asUintptr() - aDec * sizeof(T) <= asUintptr());
    return create(mPtr - aDec);
  }

  /*
   * You can assign a raw pointer into a RangedPtr if the raw pointer is
   * within the range specified at creation.
   */
  template <typename U>
  RangedPtr<T>& operator=(U* aPtr)
  {
    *this = create(aPtr);
    return *this;
  }

  template <typename U>
  RangedPtr<T>& operator=(const RangedPtr<U>& aPtr)
  {
    MOZ_ASSERT(mRangeStart <= aPtr.mPtr);
    MOZ_ASSERT(aPtr.mPtr <= mRangeEnd);
    mPtr = aPtr.mPtr;
    checkSanity();
    return *this;
  }

  RangedPtr<T>& operator++()
  {
    return (*this += 1);
  }

  RangedPtr<T> operator++(int)
  {
    RangedPtr<T> rcp = *this;
    ++*this;
    return rcp;
  }

  RangedPtr<T>& operator--()
  {
    return (*this -= 1);
  }

  RangedPtr<T> operator--(int)
  {
    RangedPtr<T> rcp = *this;
    --*this;
    return rcp;
  }

  RangedPtr<T>& operator+=(size_t aInc)
  {
    *this = *this + aInc;
    return *this;
  }

  RangedPtr<T>& operator-=(size_t aDec)
  {
    *this = *this - aDec;
    return *this;
  }

  T& operator[](int aIndex) const
  {
    MOZ_ASSERT(size_t(aIndex > 0 ? aIndex : -aIndex) <= size_t(-1) / sizeof(T));
    return *create(mPtr + aIndex);
  }

  T& operator*() const
  {
    MOZ_ASSERT(mPtr >= mRangeStart);
    MOZ_ASSERT(mPtr < mRangeEnd);
    return *mPtr;
  }

  T* operator->() const
  {
    MOZ_ASSERT(mPtr >= mRangeStart);
    MOZ_ASSERT(mPtr < mRangeEnd);
    return mPtr;
  }

  template <typename U>
  bool operator==(const RangedPtr<U>& aOther) const
  {
    return mPtr == aOther.mPtr;
  }
  template <typename U>
  bool operator!=(const RangedPtr<U>& aOther) const
  {
    return !(*this == aOther);
  }

  template<typename U>
  bool operator==(const U* u) const
  {
    return mPtr == u;
  }
  template<typename U>
  bool operator!=(const U* u) const
  {
    return !(*this == u);
  }

  template <typename U>
  bool operator<(const RangedPtr<U>& aOther) const
  {
    return mPtr < aOther.mPtr;
  }
  template <typename U>
  bool operator<=(const RangedPtr<U>& aOther) const
  {
    return mPtr <= aOther.mPtr;
  }

  template <typename U>
  bool operator>(const RangedPtr<U>& aOther) const
  {
    return mPtr > aOther.mPtr;
  }
  template <typename U>
  bool operator>=(const RangedPtr<U>& aOther) const
  {
    return mPtr >= aOther.mPtr;
  }

  size_t operator-(const RangedPtr<T>& aOther) const
  {
    MOZ_ASSERT(mPtr >= aOther.mPtr);
    return PointerRangeSize(aOther.mPtr, mPtr);
  }

private:
  RangedPtr() = delete;
};

} /* namespace mozilla */

#endif /* mozilla_RangedPtr_h */

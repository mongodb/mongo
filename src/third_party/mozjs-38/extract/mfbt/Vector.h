/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A type/length-parametrized vector class. */

#ifndef mozilla_Vector_h
#define mozilla_Vector_h

#include "mozilla/Alignment.h"
#include "mozilla/AllocPolicy.h"
#include "mozilla/ArrayUtils.h" // for PointerRangeSize
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"
#include "mozilla/ReentrancyGuard.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/TypeTraits.h"

#include <new> // for placement new

/* Silence dire "bugs in previous versions of MSVC have been fixed" warnings */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4345)
#endif

namespace mozilla {

template<typename T, size_t N, class AllocPolicy, class ThisVector>
class VectorBase;

namespace detail {

/*
 * Check that the given capacity wastes the minimal amount of space if
 * allocated on the heap. This means that aCapacity*sizeof(T) is as close to a
 * power-of-two as possible. growStorageBy() is responsible for ensuring this.
 */
template<typename T>
static bool CapacityHasExcessSpace(size_t aCapacity)
{
  size_t size = aCapacity * sizeof(T);
  return RoundUpPow2(size) - size >= sizeof(T);
}

/*
 * This template class provides a default implementation for vector operations
 * when the element type is not known to be a POD, as judged by IsPod.
 */
template<typename T, size_t N, class AP, class ThisVector, bool IsPod>
struct VectorImpl
{
  /*
   * Constructs a default object in the uninitialized memory at *aDst.
   */
  MOZ_NONNULL(1)
  static inline void new_(T* aDst)
  {
    new(aDst) T();
  }

  /*
   * Constructs an object in the uninitialized memory at *aDst from aSrc.
   */
  template<typename U>
  MOZ_NONNULL(1)
  static inline void new_(T* aDst, U&& aU)
  {
    new(aDst) T(Forward<U>(aU));
  }

  /* Destroys constructed objects in the range [aBegin, aEnd). */
  static inline void destroy(T* aBegin, T* aEnd)
  {
    MOZ_ASSERT(aBegin <= aEnd);
    for (T* p = aBegin; p < aEnd; ++p) {
      p->~T();
    }
  }

  /* Constructs objects in the uninitialized range [aBegin, aEnd). */
  static inline void initialize(T* aBegin, T* aEnd)
  {
    MOZ_ASSERT(aBegin <= aEnd);
    for (T* p = aBegin; p < aEnd; ++p) {
      new_(p);
    }
  }

  /*
   * Copy-constructs objects in the uninitialized range
   * [aDst, aDst+(aSrcEnd-aSrcStart)) from the range [aSrcStart, aSrcEnd).
   */
  template<typename U>
  static inline void copyConstruct(T* aDst,
                                   const U* aSrcStart, const U* aSrcEnd)
  {
    MOZ_ASSERT(aSrcStart <= aSrcEnd);
    for (const U* p = aSrcStart; p < aSrcEnd; ++p, ++aDst) {
      new_(aDst, *p);
    }
  }

  /*
   * Move-constructs objects in the uninitialized range
   * [aDst, aDst+(aSrcEnd-aSrcStart)) from the range [aSrcStart, aSrcEnd).
   */
  template<typename U>
  static inline void moveConstruct(T* aDst, U* aSrcStart, U* aSrcEnd)
  {
    MOZ_ASSERT(aSrcStart <= aSrcEnd);
    for (U* p = aSrcStart; p < aSrcEnd; ++p, ++aDst) {
      new_(aDst, Move(*p));
    }
  }

  /*
   * Copy-constructs objects in the uninitialized range [aDst, aDst+aN) from
   * the same object aU.
   */
  template<typename U>
  static inline void copyConstructN(T* aDst, size_t aN, const U& aU)
  {
    for (T* end = aDst + aN; aDst < end; ++aDst) {
      new_(aDst, aU);
    }
  }

  /*
   * Grows the given buffer to have capacity aNewCap, preserving the objects
   * constructed in the range [begin, end) and updating aV. Assumes that (1)
   * aNewCap has not overflowed, and (2) multiplying aNewCap by sizeof(T) will
   * not overflow.
   */
  static inline bool
  growTo(VectorBase<T, N, AP, ThisVector>& aV, size_t aNewCap)
  {
    MOZ_ASSERT(!aV.usingInlineStorage());
    MOZ_ASSERT(!CapacityHasExcessSpace<T>(aNewCap));
    T* newbuf = aV.template pod_malloc<T>(aNewCap);
    if (MOZ_UNLIKELY(!newbuf)) {
      return false;
    }
    T* dst = newbuf;
    T* src = aV.beginNoCheck();
    for (; src < aV.endNoCheck(); ++dst, ++src) {
      new_(dst, Move(*src));
    }
    VectorImpl::destroy(aV.beginNoCheck(), aV.endNoCheck());
    aV.free_(aV.mBegin);
    aV.mBegin = newbuf;
    /* aV.mLength is unchanged. */
    aV.mCapacity = aNewCap;
    return true;
  }
};

/*
 * This partial template specialization provides a default implementation for
 * vector operations when the element type is known to be a POD, as judged by
 * IsPod.
 */
template<typename T, size_t N, class AP, class ThisVector>
struct VectorImpl<T, N, AP, ThisVector, true>
{
  static inline void new_(T* aDst)
  {
    *aDst = T();
  }

  template<typename U>
  static inline void new_(T* aDst, U&& aU)
  {
    *aDst = Forward<U>(aU);
  }

  static inline void destroy(T*, T*) {}

  static inline void initialize(T* aBegin, T* aEnd)
  {
    /*
     * You would think that memset would be a big win (or even break even)
     * when we know T is a POD. But currently it's not. This is probably
     * because |append| tends to be given small ranges and memset requires
     * a function call that doesn't get inlined.
     *
     * memset(aBegin, 0, sizeof(T) * (aEnd - aBegin));
     */
    MOZ_ASSERT(aBegin <= aEnd);
    for (T* p = aBegin; p < aEnd; ++p) {
      new_(p);
    }
  }

  template<typename U>
  static inline void copyConstruct(T* aDst,
                                   const U* aSrcStart, const U* aSrcEnd)
  {
    /*
     * See above memset comment. Also, notice that copyConstruct is
     * currently templated (T != U), so memcpy won't work without
     * requiring T == U.
     *
     * memcpy(aDst, aSrcStart, sizeof(T) * (aSrcEnd - aSrcStart));
     */
    MOZ_ASSERT(aSrcStart <= aSrcEnd);
    for (const U* p = aSrcStart; p < aSrcEnd; ++p, ++aDst) {
      new_(aDst, *p);
    }
  }

  template<typename U>
  static inline void moveConstruct(T* aDst,
                                   const U* aSrcStart, const U* aSrcEnd)
  {
    copyConstruct(aDst, aSrcStart, aSrcEnd);
  }

  static inline void copyConstructN(T* aDst, size_t aN, const T& aT)
  {
    for (T* end = aDst + aN; aDst < end; ++aDst) {
      new_(aDst, aT);
    }
  }

  static inline bool
  growTo(VectorBase<T, N, AP, ThisVector>& aV, size_t aNewCap)
  {
    MOZ_ASSERT(!aV.usingInlineStorage());
    MOZ_ASSERT(!CapacityHasExcessSpace<T>(aNewCap));
    T* newbuf = aV.template pod_realloc<T>(aV.mBegin, aV.mCapacity, aNewCap);
    if (MOZ_UNLIKELY(!newbuf)) {
      return false;
    }
    aV.mBegin = newbuf;
    /* aV.mLength is unchanged. */
    aV.mCapacity = aNewCap;
    return true;
  }
};

// A struct for TestVector.cpp to access private internal fields.
// DO NOT DEFINE IN YOUR OWN CODE.
struct VectorTesting;

} // namespace detail

/*
 * A CRTP base class for vector-like classes.  Unless you really really want
 * your own vector class -- and you almost certainly don't -- you should use
 * mozilla::Vector instead!
 *
 * See mozilla::Vector for interface requirements.
 */
template<typename T, size_t N, class AllocPolicy, class ThisVector>
class VectorBase : private AllocPolicy
{
  /* utilities */

  static const bool kElemIsPod = IsPod<T>::value;
  typedef detail::VectorImpl<T, N, AllocPolicy, ThisVector, kElemIsPod> Impl;
  friend struct detail::VectorImpl<T, N, AllocPolicy, ThisVector, kElemIsPod>;

  friend struct detail::VectorTesting;

  bool growStorageBy(size_t aIncr);
  bool convertToHeapStorage(size_t aNewCap);

  /* magic constants */

  static const int kMaxInlineBytes = 1024;

  /* compute constants */

  /*
   * Consider element size to be 1 for buffer sizing if there are 0 inline
   * elements.  This allows us to compile when the definition of the element
   * type is not visible here.
   *
   * Explicit specialization is only allowed at namespace scope, so in order
   * to keep everything here, we use a dummy template parameter with partial
   * specialization.
   */
  template<int M, int Dummy>
  struct ElemSize
  {
    static const size_t value = sizeof(T);
  };
  template<int Dummy>
  struct ElemSize<0, Dummy>
  {
    static const size_t value = 1;
  };

  static const size_t kInlineCapacity =
    tl::Min<N, kMaxInlineBytes / ElemSize<N, 0>::value>::value;

  /* Calculate inline buffer size; avoid 0-sized array. */
  static const size_t kInlineBytes =
    tl::Max<1, kInlineCapacity * ElemSize<N, 0>::value>::value;

  /* member data */

  /*
   * Pointer to the buffer, be it inline or heap-allocated. Only [mBegin,
   * mBegin + mLength) hold valid constructed T objects. The range [mBegin +
   * mLength, mBegin + mCapacity) holds uninitialized memory. The range
   * [mBegin + mLength, mBegin + mReserved) also holds uninitialized memory
   * previously allocated by a call to reserve().
   */
  T* mBegin;

  /* Number of elements in the vector. */
  size_t mLength;

  /* Max number of elements storable in the vector without resizing. */
  size_t mCapacity;

#ifdef DEBUG
  /* Max elements of reserved or used space in this vector. */
  size_t mReserved;
#endif

  /* Memory used for inline storage. */
  AlignedStorage<kInlineBytes> mStorage;

#ifdef DEBUG
  friend class ReentrancyGuard;
  bool mEntered;
#endif

  /* private accessors */

  bool usingInlineStorage() const
  {
    return mBegin == const_cast<VectorBase*>(this)->inlineStorage();
  }

  T* inlineStorage()
  {
    return static_cast<T*>(mStorage.addr());
  }

  T* beginNoCheck() const
  {
    return mBegin;
  }

  T* endNoCheck()
  {
    return mBegin + mLength;
  }

  const T* endNoCheck() const
  {
    return mBegin + mLength;
  }

#ifdef DEBUG
  /**
   * The amount of explicitly allocated space in this vector that is immediately
   * available to be filled by appending additional elements.  This value is
   * always greater than or equal to |length()| -- the vector's actual elements
   * are implicitly reserved.  This value is always less than or equal to
   * |capacity()|.  It may be explicitly increased using the |reserve()| method.
   */
  size_t reserved() const
  {
    MOZ_ASSERT(mLength <= mReserved);
    MOZ_ASSERT(mReserved <= mCapacity);
    return mReserved;
  }
#endif

  /* Append operations guaranteed to succeed due to pre-reserved space. */
  template<typename U> void internalAppend(U&& aU);
  template<typename U, size_t O, class BP, class UV>
  void internalAppendAll(const VectorBase<U, O, BP, UV>& aU);
  void internalAppendN(const T& aT, size_t aN);
  template<typename U> void internalAppend(const U* aBegin, size_t aLength);

public:
  static const size_t sMaxInlineStorage = N;

  typedef T ElementType;

  explicit VectorBase(AllocPolicy = AllocPolicy());
  explicit VectorBase(ThisVector&&); /* Move constructor. */
  ThisVector& operator=(ThisVector&&); /* Move assignment. */
  ~VectorBase();

  /* accessors */

  const AllocPolicy& allocPolicy() const { return *this; }

  AllocPolicy& allocPolicy() { return *this; }

  enum { InlineLength = N };

  size_t length() const { return mLength; }

  bool empty() const { return mLength == 0; }

  size_t capacity() const { return mCapacity; }

  T* begin()
  {
    MOZ_ASSERT(!mEntered);
    return mBegin;
  }

  const T* begin() const
  {
    MOZ_ASSERT(!mEntered);
    return mBegin;
  }

  T* end()
  {
    MOZ_ASSERT(!mEntered);
    return mBegin + mLength;
  }

  const T* end() const
  {
    MOZ_ASSERT(!mEntered);
    return mBegin + mLength;
  }

  T& operator[](size_t aIndex)
  {
    MOZ_ASSERT(!mEntered);
    MOZ_ASSERT(aIndex < mLength);
    return begin()[aIndex];
  }

  const T& operator[](size_t aIndex) const
  {
    MOZ_ASSERT(!mEntered);
    MOZ_ASSERT(aIndex < mLength);
    return begin()[aIndex];
  }

  T& back()
  {
    MOZ_ASSERT(!mEntered);
    MOZ_ASSERT(!empty());
    return *(end() - 1);
  }

  const T& back() const
  {
    MOZ_ASSERT(!mEntered);
    MOZ_ASSERT(!empty());
    return *(end() - 1);
  }

  class Range
  {
    friend class VectorBase;
    T* mCur;
    T* mEnd;
    Range(T* aCur, T* aEnd)
      : mCur(aCur)
      , mEnd(aEnd)
    {
      MOZ_ASSERT(aCur <= aEnd);
    }

  public:
    Range() {}
    bool empty() const { return mCur == mEnd; }
    size_t remain() const { return PointerRangeSize(mCur, mEnd); }
    T& front() const { MOZ_ASSERT(!empty()); return *mCur; }
    void popFront() { MOZ_ASSERT(!empty()); ++mCur; }
    T popCopyFront() { MOZ_ASSERT(!empty()); return *mCur++; }
  };

  Range all() { return Range(begin(), end()); }

  /* mutators */

  /**
   * Given that the vector is empty and has no inline storage, grow to
   * |capacity|.
   */
  bool initCapacity(size_t aRequest);

  /**
   * If reserve(aRequest) succeeds and |aRequest >= length()|, then appending
   * |aRequest - length()| elements, in any sequence of append/appendAll calls,
   * is guaranteed to succeed.
   *
   * A request to reserve an amount less than the current length does not affect
   * reserved space.
   */
  bool reserve(size_t aRequest);

  /**
   * Destroy elements in the range [end() - aIncr, end()). Does not deallocate
   * or unreserve storage for those elements.
   */
  void shrinkBy(size_t aIncr);

  /** Grow the vector by aIncr elements. */
  bool growBy(size_t aIncr);

  /** Call shrinkBy or growBy based on whether newSize > length(). */
  bool resize(size_t aNewLength);

  /**
   * Increase the length of the vector, but don't initialize the new elements
   * -- leave them as uninitialized memory.
   */
  bool growByUninitialized(size_t aIncr);
  void infallibleGrowByUninitialized(size_t aIncr);
  bool resizeUninitialized(size_t aNewLength);

  /** Shorthand for shrinkBy(length()). */
  void clear();

  /** Clears and releases any heap-allocated storage. */
  void clearAndFree();

  /**
   * If true, appending |aNeeded| elements won't reallocate elements storage.
   * This *doesn't* mean that infallibleAppend may be used!  You still must
   * reserve the extra space, even if this method indicates that appends won't
   * need to reallocate elements storage.
   */
  bool canAppendWithoutRealloc(size_t aNeeded) const;

  /** Potentially fallible append operations. */

  /**
   * This can take either a T& or a T&&. Given a T&&, it moves |aU| into the
   * vector, instead of copying it. If it fails, |aU| is left unmoved. ("We are
   * not amused.")
   */
  template<typename U> bool append(U&& aU);

  template<typename U, size_t O, class BP, class UV>
  bool appendAll(const VectorBase<U, O, BP, UV>& aU);
  bool appendN(const T& aT, size_t aN);
  template<typename U> bool append(const U* aBegin, const U* aEnd);
  template<typename U> bool append(const U* aBegin, size_t aLength);

  /*
   * Guaranteed-infallible append operations for use upon vectors whose
   * memory has been pre-reserved.  Don't use this if you haven't reserved the
   * memory!
   */
  template<typename U> void infallibleAppend(U&& aU)
  {
    internalAppend(Forward<U>(aU));
  }
  void infallibleAppendN(const T& aT, size_t aN)
  {
    internalAppendN(aT, aN);
  }
  template<typename U> void infallibleAppend(const U* aBegin, const U* aEnd)
  {
    internalAppend(aBegin, PointerRangeSize(aBegin, aEnd));
  }
  template<typename U> void infallibleAppend(const U* aBegin, size_t aLength)
  {
    internalAppend(aBegin, aLength);
  }

  void popBack();

  T popCopy();

  /**
   * Transfers ownership of the internal buffer used by this vector to the
   * caller.  (It's the caller's responsibility to properly deallocate this
   * buffer, in accordance with this vector's AllocPolicy.)  After this call,
   * the vector is empty.  Since the returned buffer may need to be allocated
   * (if the elements are currently stored in-place), the call can fail,
   * returning nullptr.
   *
   * N.B. Although a T*, only the range [0, length()) is constructed.
   */
  T* extractRawBuffer();

  /**
   * Transfer ownership of an array of objects into the vector.  The caller
   * must have allocated the array in accordance with this vector's
   * AllocPolicy.
   *
   * N.B. This call assumes that there are no uninitialized elements in the
   *      passed array.
   */
  void replaceRawBuffer(T* aP, size_t aLength);

  /**
   * Places |aVal| at position |aP|, shifting existing elements from |aP| onward
   * one position higher.  On success, |aP| should not be reused because it'll
   * be a dangling pointer if reallocation of the vector storage occurred; the
   * return value should be used instead.  On failure, nullptr is returned.
   *
   * Example usage:
   *
   *   if (!(p = vec.insert(p, val))) {
   *     <handle failure>
   *   }
   *   <keep working with p>
   *
   * This is inherently a linear-time operation.  Be careful!
   */
  template<typename U>
  T* insert(T* aP, U&& aVal);

  /**
   * Removes the element |aT|, which must fall in the bounds [begin, end),
   * shifting existing elements from |aT + 1| onward one position lower.
   */
  void erase(T* aT);

  /**
   * Removes the elements [|aBegin|, |aEnd|), which must fall in the bounds
   * [begin, end), shifting existing elements from |aEnd + 1| onward to aBegin's
   * old position.
   */
  void erase(T* aBegin, T* aEnd);

  /**
   * Measure the size of the vector's heap-allocated storage.
   */
  size_t sizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const;

  /**
   * Like sizeOfExcludingThis, but also measures the size of the vector
   * object (which must be heap-allocated) itself.
   */
  size_t sizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const;

  void swap(ThisVector& aOther);

private:
  VectorBase(const VectorBase&) = delete;
  void operator=(const VectorBase&) = delete;

  /* Move-construct/assign only from our derived class, ThisVector. */
  VectorBase(VectorBase&&) = delete;
  void operator=(VectorBase&&) = delete;
};

/* This does the re-entrancy check plus several other sanity checks. */
#define MOZ_REENTRANCY_GUARD_ET_AL \
  ReentrancyGuard g(*this); \
  MOZ_ASSERT_IF(usingInlineStorage(), mCapacity == kInlineCapacity); \
  MOZ_ASSERT(reserved() <= mCapacity); \
  MOZ_ASSERT(mLength <= reserved()); \
  MOZ_ASSERT(mLength <= mCapacity)

/* Vector Implementation */

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE
VectorBase<T, N, AP, TV>::VectorBase(AP aAP)
  : AP(aAP)
  , mLength(0)
  , mCapacity(kInlineCapacity)
#ifdef DEBUG
  , mReserved(0)
  , mEntered(false)
#endif
{
  mBegin = static_cast<T*>(mStorage.addr());
}

/* Move constructor. */
template<typename T, size_t N, class AllocPolicy, class TV>
MOZ_ALWAYS_INLINE
VectorBase<T, N, AllocPolicy, TV>::VectorBase(TV&& aRhs)
  : AllocPolicy(Move(aRhs))
#ifdef DEBUG
  , mEntered(false)
#endif
{
  mLength = aRhs.mLength;
  mCapacity = aRhs.mCapacity;
#ifdef DEBUG
  mReserved = aRhs.mReserved;
#endif

  if (aRhs.usingInlineStorage()) {
    /* We can't move the buffer over in this case, so copy elements. */
    mBegin = static_cast<T*>(mStorage.addr());
    Impl::moveConstruct(mBegin, aRhs.beginNoCheck(), aRhs.endNoCheck());
    /*
     * Leave aRhs's mLength, mBegin, mCapacity, and mReserved as they are.
     * The elements in its in-line storage still need to be destroyed.
     */
  } else {
    /*
     * Take src's buffer, and turn src into an empty vector using
     * in-line storage.
     */
    mBegin = aRhs.mBegin;
    aRhs.mBegin = static_cast<T*>(aRhs.mStorage.addr());
    aRhs.mCapacity = kInlineCapacity;
    aRhs.mLength = 0;
#ifdef DEBUG
    aRhs.mReserved = 0;
#endif
  }
}

/* Move assignment. */
template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE TV&
VectorBase<T, N, AP, TV>::operator=(TV&& aRhs)
{
  MOZ_ASSERT(this != &aRhs, "self-move assignment is prohibited");
  TV* tv = static_cast<TV*>(this);
  tv->~TV();
  new(tv) TV(Move(aRhs));
  return *tv;
}

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE
VectorBase<T, N, AP, TV>::~VectorBase()
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  Impl::destroy(beginNoCheck(), endNoCheck());
  if (!usingInlineStorage()) {
    this->free_(beginNoCheck());
  }
}

/*
 * This function will create a new heap buffer with capacity aNewCap,
 * move all elements in the inline buffer to this new buffer,
 * and fail on OOM.
 */
template<typename T, size_t N, class AP, class TV>
inline bool
VectorBase<T, N, AP, TV>::convertToHeapStorage(size_t aNewCap)
{
  MOZ_ASSERT(usingInlineStorage());

  /* Allocate buffer. */
  MOZ_ASSERT(!detail::CapacityHasExcessSpace<T>(aNewCap));
  T* newBuf = this->template pod_malloc<T>(aNewCap);
  if (MOZ_UNLIKELY(!newBuf)) {
    return false;
  }

  /* Copy inline elements into heap buffer. */
  Impl::moveConstruct(newBuf, beginNoCheck(), endNoCheck());
  Impl::destroy(beginNoCheck(), endNoCheck());

  /* Switch in heap buffer. */
  mBegin = newBuf;
  /* mLength is unchanged. */
  mCapacity = aNewCap;
  return true;
}

template<typename T, size_t N, class AP, class TV>
MOZ_NEVER_INLINE bool
VectorBase<T, N, AP, TV>::growStorageBy(size_t aIncr)
{
  MOZ_ASSERT(mLength + aIncr > mCapacity);

  /*
   * When choosing a new capacity, its size should is as close to 2**N bytes
   * as possible.  2**N-sized requests are best because they are unlikely to
   * be rounded up by the allocator.  Asking for a 2**N number of elements
   * isn't as good, because if sizeof(T) is not a power-of-two that would
   * result in a non-2**N request size.
   */

  size_t newCap;

  if (aIncr == 1) {
    if (usingInlineStorage()) {
      /* This case occurs in ~70--80% of the calls to this function. */
      size_t newSize =
        tl::RoundUpPow2<(kInlineCapacity + 1) * sizeof(T)>::value;
      newCap = newSize / sizeof(T);
      goto convert;
    }

    if (mLength == 0) {
      /* This case occurs in ~0--10% of the calls to this function. */
      newCap = 1;
      goto grow;
    }

    /* This case occurs in ~15--20% of the calls to this function. */

    /*
     * Will mLength * 4 *sizeof(T) overflow?  This condition limits a vector
     * to 1GB of memory on a 32-bit system, which is a reasonable limit.  It
     * also ensures that
     *
     *   static_cast<char*>(end()) - static_cast<char*>(begin())
     *
     * doesn't overflow ptrdiff_t (see bug 510319).
     */
    if (MOZ_UNLIKELY(mLength & tl::MulOverflowMask<4 * sizeof(T)>::value)) {
      this->reportAllocOverflow();
      return false;
    }

    /*
     * If we reach here, the existing capacity will have a size that is already
     * as close to 2^N as sizeof(T) will allow.  Just double the capacity, and
     * then there might be space for one more element.
     */
    newCap = mLength * 2;
    if (detail::CapacityHasExcessSpace<T>(newCap)) {
      newCap += 1;
    }
  } else {
    /* This case occurs in ~2% of the calls to this function. */
    size_t newMinCap = mLength + aIncr;

    /* Did mLength + aIncr overflow?  Will newCap * sizeof(T) overflow? */
    if (MOZ_UNLIKELY(newMinCap < mLength ||
                     newMinCap & tl::MulOverflowMask<2 * sizeof(T)>::value))
    {
      this->reportAllocOverflow();
      return false;
    }

    size_t newMinSize = newMinCap * sizeof(T);
    size_t newSize = RoundUpPow2(newMinSize);
    newCap = newSize / sizeof(T);
  }

  if (usingInlineStorage()) {
convert:
    return convertToHeapStorage(newCap);
  }

grow:
  return Impl::growTo(*this, newCap);
}

template<typename T, size_t N, class AP, class TV>
inline bool
VectorBase<T, N, AP, TV>::initCapacity(size_t aRequest)
{
  MOZ_ASSERT(empty());
  MOZ_ASSERT(usingInlineStorage());
  if (aRequest == 0) {
    return true;
  }
  T* newbuf = this->template pod_malloc<T>(aRequest);
  if (MOZ_UNLIKELY(!newbuf)) {
    return false;
  }
  mBegin = newbuf;
  mCapacity = aRequest;
#ifdef DEBUG
  mReserved = aRequest;
#endif
  return true;
}

template<typename T, size_t N, class AP, class TV>
inline bool
VectorBase<T, N, AP, TV>::reserve(size_t aRequest)
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (aRequest > mCapacity && MOZ_UNLIKELY(!growStorageBy(aRequest - mLength))) {
    return false;
  }
#ifdef DEBUG
  if (aRequest > mReserved) {
    mReserved = aRequest;
  }
  MOZ_ASSERT(mLength <= mReserved);
  MOZ_ASSERT(mReserved <= mCapacity);
#endif
  return true;
}

template<typename T, size_t N, class AP, class TV>
inline void
VectorBase<T, N, AP, TV>::shrinkBy(size_t aIncr)
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  MOZ_ASSERT(aIncr <= mLength);
  Impl::destroy(endNoCheck() - aIncr, endNoCheck());
  mLength -= aIncr;
}

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE bool
VectorBase<T, N, AP, TV>::growBy(size_t aIncr)
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (aIncr > mCapacity - mLength && MOZ_UNLIKELY(!growStorageBy(aIncr))) {
    return false;
  }
  MOZ_ASSERT(mLength + aIncr <= mCapacity);
  T* newend = endNoCheck() + aIncr;
  Impl::initialize(endNoCheck(), newend);
  mLength += aIncr;
#ifdef DEBUG
  if (mLength > mReserved) {
    mReserved = mLength;
  }
#endif
  return true;
}

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE bool
VectorBase<T, N, AP, TV>::growByUninitialized(size_t aIncr)
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (aIncr > mCapacity - mLength && MOZ_UNLIKELY(!growStorageBy(aIncr))) {
    return false;
  }
  infallibleGrowByUninitialized(aIncr);
  return true;
}

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE void
VectorBase<T, N, AP, TV>::infallibleGrowByUninitialized(size_t aIncr)
{
  MOZ_ASSERT(mLength + aIncr <= mCapacity);
  mLength += aIncr;
#ifdef DEBUG
  if (mLength > mReserved) {
    mReserved = mLength;
  }
#endif
}

template<typename T, size_t N, class AP, class TV>
inline bool
VectorBase<T, N, AP, TV>::resize(size_t aNewLength)
{
  size_t curLength = mLength;
  if (aNewLength > curLength) {
    return growBy(aNewLength - curLength);
  }
  shrinkBy(curLength - aNewLength);
  return true;
}

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE bool
VectorBase<T, N, AP, TV>::resizeUninitialized(size_t aNewLength)
{
  size_t curLength = mLength;
  if (aNewLength > curLength) {
    return growByUninitialized(aNewLength - curLength);
  }
  shrinkBy(curLength - aNewLength);
  return true;
}

template<typename T, size_t N, class AP, class TV>
inline void
VectorBase<T, N, AP, TV>::clear()
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  Impl::destroy(beginNoCheck(), endNoCheck());
  mLength = 0;
}

template<typename T, size_t N, class AP, class TV>
inline void
VectorBase<T, N, AP, TV>::clearAndFree()
{
  clear();

  if (usingInlineStorage()) {
    return;
  }
  this->free_(beginNoCheck());
  mBegin = static_cast<T*>(mStorage.addr());
  mCapacity = kInlineCapacity;
#ifdef DEBUG
  mReserved = 0;
#endif
}

template<typename T, size_t N, class AP, class TV>
inline bool
VectorBase<T, N, AP, TV>::canAppendWithoutRealloc(size_t aNeeded) const
{
  return mLength + aNeeded <= mCapacity;
}

template<typename T, size_t N, class AP, class TV>
template<typename U, size_t O, class BP, class UV>
MOZ_ALWAYS_INLINE void
VectorBase<T, N, AP, TV>::internalAppendAll(
  const VectorBase<U, O, BP, UV>& aOther)
{
  internalAppend(aOther.begin(), aOther.length());
}

template<typename T, size_t N, class AP, class TV>
template<typename U>
MOZ_ALWAYS_INLINE void
VectorBase<T, N, AP, TV>::internalAppend(U&& aU)
{
  MOZ_ASSERT(mLength + 1 <= mReserved);
  MOZ_ASSERT(mReserved <= mCapacity);
  Impl::new_(endNoCheck(), Forward<U>(aU));
  ++mLength;
}

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE bool
VectorBase<T, N, AP, TV>::appendN(const T& aT, size_t aNeeded)
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (mLength + aNeeded > mCapacity && MOZ_UNLIKELY(!growStorageBy(aNeeded))) {
    return false;
  }
#ifdef DEBUG
  if (mLength + aNeeded > mReserved) {
    mReserved = mLength + aNeeded;
  }
#endif
  internalAppendN(aT, aNeeded);
  return true;
}

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE void
VectorBase<T, N, AP, TV>::internalAppendN(const T& aT, size_t aNeeded)
{
  MOZ_ASSERT(mLength + aNeeded <= mReserved);
  MOZ_ASSERT(mReserved <= mCapacity);
  Impl::copyConstructN(endNoCheck(), aNeeded, aT);
  mLength += aNeeded;
}

template<typename T, size_t N, class AP, class TV>
template<typename U>
inline T*
VectorBase<T, N, AP, TV>::insert(T* aP, U&& aVal)
{
  MOZ_ASSERT(begin() <= aP);
  MOZ_ASSERT(aP <= end());
  size_t pos = aP - begin();
  MOZ_ASSERT(pos <= mLength);
  size_t oldLength = mLength;
  if (pos == oldLength) {
    if (!append(Forward<U>(aVal))) {
      return nullptr;
    }
  } else {
    T oldBack = Move(back());
    if (!append(Move(oldBack))) { /* Dup the last element. */
      return nullptr;
    }
    for (size_t i = oldLength; i > pos; --i) {
      (*this)[i] = Move((*this)[i - 1]);
    }
    (*this)[pos] = Forward<U>(aVal);
  }
  return begin() + pos;
}

template<typename T, size_t N, class AP, class TV>
inline void
VectorBase<T, N, AP, TV>::erase(T* aIt)
{
  MOZ_ASSERT(begin() <= aIt);
  MOZ_ASSERT(aIt < end());
  while (aIt + 1 < end()) {
    *aIt = Move(*(aIt + 1));
    ++aIt;
  }
  popBack();
}

template<typename T, size_t N, class AP, class TV>
inline void
VectorBase<T, N, AP, TV>::erase(T* aBegin, T* aEnd)
{
  MOZ_ASSERT(begin() <= aBegin);
  MOZ_ASSERT(aBegin <= aEnd);
  MOZ_ASSERT(aEnd <= end());
  while (aEnd < end()) {
    *aBegin++ = Move(*aEnd++);
  }
  shrinkBy(aEnd - aBegin);
}

template<typename T, size_t N, class AP, class TV>
template<typename U>
MOZ_ALWAYS_INLINE bool
VectorBase<T, N, AP, TV>::append(const U* aInsBegin, const U* aInsEnd)
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  size_t aNeeded = PointerRangeSize(aInsBegin, aInsEnd);
  if (mLength + aNeeded > mCapacity && MOZ_UNLIKELY(!growStorageBy(aNeeded))) {
    return false;
  }
#ifdef DEBUG
  if (mLength + aNeeded > mReserved) {
    mReserved = mLength + aNeeded;
  }
#endif
  internalAppend(aInsBegin, aNeeded);
  return true;
}

template<typename T, size_t N, class AP, class TV>
template<typename U>
MOZ_ALWAYS_INLINE void
VectorBase<T, N, AP, TV>::internalAppend(const U* aInsBegin, size_t aInsLength)
{
  MOZ_ASSERT(mLength + aInsLength <= mReserved);
  MOZ_ASSERT(mReserved <= mCapacity);
  Impl::copyConstruct(endNoCheck(), aInsBegin, aInsBegin + aInsLength);
  mLength += aInsLength;
}

template<typename T, size_t N, class AP, class TV>
template<typename U>
MOZ_ALWAYS_INLINE bool
VectorBase<T, N, AP, TV>::append(U&& aU)
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (mLength == mCapacity && MOZ_UNLIKELY(!growStorageBy(1))) {
    return false;
  }
#ifdef DEBUG
  if (mLength + 1 > mReserved) {
    mReserved = mLength + 1;
  }
#endif
  internalAppend(Forward<U>(aU));
  return true;
}

template<typename T, size_t N, class AP, class TV>
template<typename U, size_t O, class BP, class UV>
MOZ_ALWAYS_INLINE bool
VectorBase<T, N, AP, TV>::appendAll(const VectorBase<U, O, BP, UV>& aOther)
{
  return append(aOther.begin(), aOther.length());
}

template<typename T, size_t N, class AP, class TV>
template<class U>
MOZ_ALWAYS_INLINE bool
VectorBase<T, N, AP, TV>::append(const U* aInsBegin, size_t aInsLength)
{
  return append(aInsBegin, aInsBegin + aInsLength);
}

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE void
VectorBase<T, N, AP, TV>::popBack()
{
  MOZ_REENTRANCY_GUARD_ET_AL;
  MOZ_ASSERT(!empty());
  --mLength;
  endNoCheck()->~T();
}

template<typename T, size_t N, class AP, class TV>
MOZ_ALWAYS_INLINE T
VectorBase<T, N, AP, TV>::popCopy()
{
  T ret = back();
  popBack();
  return ret;
}

template<typename T, size_t N, class AP, class TV>
inline T*
VectorBase<T, N, AP, TV>::extractRawBuffer()
{
  T* ret;
  if (usingInlineStorage()) {
    ret = this->template pod_malloc<T>(mLength);
    if (!ret) {
      return nullptr;
    }
    Impl::copyConstruct(ret, beginNoCheck(), endNoCheck());
    Impl::destroy(beginNoCheck(), endNoCheck());
    /* mBegin, mCapacity are unchanged. */
    mLength = 0;
  } else {
    ret = mBegin;
    mBegin = static_cast<T*>(mStorage.addr());
    mLength = 0;
    mCapacity = kInlineCapacity;
#ifdef DEBUG
    mReserved = 0;
#endif
  }
  return ret;
}

template<typename T, size_t N, class AP, class TV>
inline void
VectorBase<T, N, AP, TV>::replaceRawBuffer(T* aP, size_t aLength)
{
  MOZ_REENTRANCY_GUARD_ET_AL;

  /* Destroy what we have. */
  Impl::destroy(beginNoCheck(), endNoCheck());
  if (!usingInlineStorage()) {
    this->free_(beginNoCheck());
  }

  /* Take in the new buffer. */
  if (aLength <= kInlineCapacity) {
    /*
     * We convert to inline storage if possible, even though aP might
     * otherwise be acceptable.  Maybe this behaviour should be
     * specifiable with an argument to this function.
     */
    mBegin = static_cast<T*>(mStorage.addr());
    mLength = aLength;
    mCapacity = kInlineCapacity;
    Impl::moveConstruct(mBegin, aP, aP + aLength);
    Impl::destroy(aP, aP + aLength);
    this->free_(aP);
  } else {
    mBegin = aP;
    mLength = aLength;
    mCapacity = aLength;
  }
#ifdef DEBUG
  mReserved = aLength;
#endif
}

template<typename T, size_t N, class AP, class TV>
inline size_t
VectorBase<T, N, AP, TV>::sizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const
{
  return usingInlineStorage() ? 0 : aMallocSizeOf(beginNoCheck());
}

template<typename T, size_t N, class AP, class TV>
inline size_t
VectorBase<T, N, AP, TV>::sizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const
{
  return aMallocSizeOf(this) + sizeOfExcludingThis(aMallocSizeOf);
}

template<typename T, size_t N, class AP, class TV>
inline void
VectorBase<T, N, AP, TV>::swap(TV& aOther)
{
  static_assert(N == 0,
                "still need to implement this for N != 0");

  // This only works when inline storage is always empty.
  if (!usingInlineStorage() && aOther.usingInlineStorage()) {
    aOther.mBegin = mBegin;
    mBegin = inlineStorage();
  } else if (usingInlineStorage() && !aOther.usingInlineStorage()) {
    mBegin = aOther.mBegin;
    aOther.mBegin = aOther.inlineStorage();
  } else if (!usingInlineStorage() && !aOther.usingInlineStorage()) {
    Swap(mBegin, aOther.mBegin);
  } else {
    // This case is a no-op, since we'd set both to use their inline storage.
  }

  Swap(mLength, aOther.mLength);
  Swap(mCapacity, aOther.mCapacity);
#ifdef DEBUG
  Swap(mReserved, aOther.mReserved);
#endif
}

/*
 * STL-like container providing a short-lived, dynamic buffer.  Vector calls the
 * constructors/destructors of all elements stored in its internal buffer, so
 * non-PODs may be safely used.  Additionally, Vector will store the first N
 * elements in-place before resorting to dynamic allocation.
 *
 * T requirements:
 *  - default and copy constructible, assignable, destructible
 *  - operations do not throw
 * N requirements:
 *  - any value, however, N is clamped to min/max values
 * AllocPolicy:
 *  - see "Allocation policies" in AllocPolicy.h (defaults to
 *    mozilla::MallocAllocPolicy)
 *
 * Vector is not reentrant: T member functions called during Vector member
 * functions must not call back into the same object!
 */
template<typename T,
         size_t MinInlineCapacity = 0,
         class AllocPolicy = MallocAllocPolicy>
class Vector
  : public VectorBase<T,
                      MinInlineCapacity,
                      AllocPolicy,
                      Vector<T, MinInlineCapacity, AllocPolicy> >
{
  typedef VectorBase<T, MinInlineCapacity, AllocPolicy, Vector> Base;

public:
  explicit Vector(AllocPolicy alloc = AllocPolicy()) : Base(alloc) {}
  Vector(Vector&& vec) : Base(Move(vec)) {}
  Vector& operator=(Vector&& aOther)
  {
    return Base::operator=(Move(aOther));
  }
};

} // namespace mozilla

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /* mozilla_Vector_h */

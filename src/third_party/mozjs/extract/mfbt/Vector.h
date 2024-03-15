/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A type/length-parametrized vector class. */

#ifndef mozilla_Vector_h
#define mozilla_Vector_h

#include <new>  // for placement new
#include <type_traits>
#include <utility>

#include "mozilla/Alignment.h"
#include "mozilla/AllocPolicy.h"
#include "mozilla/ArrayUtils.h"  // for PointerRangeSize
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/OperatorNewExtensions.h"
#include "mozilla/ReentrancyGuard.h"
#include "mozilla/Span.h"
#include "mozilla/TemplateLib.h"

namespace mozilla {

template <typename T, size_t N, class AllocPolicy>
class Vector;

namespace detail {

/*
 * Check that the given capacity wastes the minimal amount of space if
 * allocated on the heap. This means that aCapacity*EltSize is as close to a
 * power-of-two as possible. growStorageBy() is responsible for ensuring this.
 */
template <size_t EltSize>
static bool CapacityHasExcessSpace(size_t aCapacity) {
  size_t size = aCapacity * EltSize;
  return RoundUpPow2(size) - size >= EltSize;
}

/*
 * AllocPolicy can optionally provide a `computeGrowth<T>(size_t aOldElts,
 * size_t aIncr)` method that returns the new number of elements to allocate
 * when the current capacity is `aOldElts` and `aIncr` more are being
 * requested. If the AllocPolicy does not have such a method, a fallback
 * will be used that mostly will just round the new requested capacity up to
 * the next power of two, which results in doubling capacity for the most part.
 *
 * If the new size would overflow some limit, `computeGrowth` returns 0.
 *
 * A simpler way would be to make computeGrowth() part of the API for all
 * AllocPolicy classes, but this turns out to be rather complex because
 * mozalloc.h defines a very widely-used InfallibleAllocPolicy, and yet it
 * can only be compiled in limited contexts, eg within `extern "C"` and with
 * -std=c++11 rather than a later version. That makes the headers that are
 * necessary for the computation unavailable (eg mfbt/MathAlgorithms.h).
 */

// Fallback version.
template <size_t EltSize>
inline size_t GrowEltsByDoubling(size_t aOldElts, size_t aIncr) {
  /*
   * When choosing a new capacity, its size in bytes should is as close to 2**N
   * bytes as possible.  2**N-sized requests are best because they are unlikely
   * to be rounded up by the allocator.  Asking for a 2**N number of elements
   * isn't as good, because if EltSize is not a power-of-two that would
   * result in a non-2**N request size.
   */

  if (aIncr == 1) {
    if (aOldElts == 0) {
      return 1;
    }

    /* This case occurs in ~15--20% of the calls to Vector::growStorageBy. */

    /*
     * Will aOldSize * 4 * sizeof(T) overflow?  This condition limits a
     * collection to 1GB of memory on a 32-bit system, which is a reasonable
     * limit.  It also ensures that
     *
     *   static_cast<char*>(end()) - static_cast<char*>(begin())
     *
     * for a Vector doesn't overflow ptrdiff_t (see bug 510319).
     */
    if (MOZ_UNLIKELY(aOldElts &
                     mozilla::tl::MulOverflowMask<4 * EltSize>::value)) {
      return 0;
    }

    /*
     * If we reach here, the existing capacity will have a size that is already
     * as close to 2^N as sizeof(T) will allow.  Just double the capacity, and
     * then there might be space for one more element.
     */
    size_t newElts = aOldElts * 2;
    if (CapacityHasExcessSpace<EltSize>(newElts)) {
      newElts += 1;
    }
    return newElts;
  }

  /* This case occurs in ~2% of the calls to Vector::growStorageBy. */
  size_t newMinCap = aOldElts + aIncr;

  /* Did aOldElts + aIncr overflow?  Will newMinCap * EltSize rounded up to the
   * next power of two overflow PTRDIFF_MAX? */
  if (MOZ_UNLIKELY(newMinCap < aOldElts ||
                   newMinCap & tl::MulOverflowMask<4 * EltSize>::value)) {
    return 0;
  }

  size_t newMinSize = newMinCap * EltSize;
  size_t newSize = RoundUpPow2(newMinSize);
  return newSize / EltSize;
};

// Fallback version.
template <typename AP, size_t EltSize>
static size_t ComputeGrowth(size_t aOldElts, size_t aIncr, int) {
  return GrowEltsByDoubling<EltSize>(aOldElts, aIncr);
}

// If the AllocPolicy provides its own computeGrowth<EltSize> implementation,
// use that.
template <typename AP, size_t EltSize>
static size_t ComputeGrowth(
    size_t aOldElts, size_t aIncr,
    decltype(std::declval<AP>().template computeGrowth<EltSize>(0, 0),
             bool()) aOverloadSelector) {
  size_t newElts = AP::template computeGrowth<EltSize>(aOldElts, aIncr);
  MOZ_ASSERT(newElts <= PTRDIFF_MAX && newElts * EltSize <= PTRDIFF_MAX,
             "invalid Vector size (see bug 510319)");
  return newElts;
}

/*
 * This template class provides a default implementation for vector operations
 * when the element type is not known to be a POD, as judged by IsPod.
 */
template <typename T, size_t N, class AP, bool IsPod>
struct VectorImpl {
  /*
   * Constructs an object in the uninitialized memory at *aDst with aArgs.
   */
  template <typename... Args>
  MOZ_NONNULL(1)
  static inline void new_(T* aDst, Args&&... aArgs) {
    new (KnownNotNull, aDst) T(std::forward<Args>(aArgs)...);
  }

  /* Destroys constructed objects in the range [aBegin, aEnd). */
  static inline void destroy(T* aBegin, T* aEnd) {
    MOZ_ASSERT(aBegin <= aEnd);
    for (T* p = aBegin; p < aEnd; ++p) {
      p->~T();
    }
  }

  /* Constructs objects in the uninitialized range [aBegin, aEnd). */
  static inline void initialize(T* aBegin, T* aEnd) {
    MOZ_ASSERT(aBegin <= aEnd);
    for (T* p = aBegin; p < aEnd; ++p) {
      new_(p);
    }
  }

  /*
   * Copy-constructs objects in the uninitialized range
   * [aDst, aDst+(aSrcEnd-aSrcStart)) from the range [aSrcStart, aSrcEnd).
   */
  template <typename U>
  static inline void copyConstruct(T* aDst, const U* aSrcStart,
                                   const U* aSrcEnd) {
    MOZ_ASSERT(aSrcStart <= aSrcEnd);
    for (const U* p = aSrcStart; p < aSrcEnd; ++p, ++aDst) {
      new_(aDst, *p);
    }
  }

  /*
   * Move-constructs objects in the uninitialized range
   * [aDst, aDst+(aSrcEnd-aSrcStart)) from the range [aSrcStart, aSrcEnd).
   */
  template <typename U>
  static inline void moveConstruct(T* aDst, U* aSrcStart, U* aSrcEnd) {
    MOZ_ASSERT(aSrcStart <= aSrcEnd);
    for (U* p = aSrcStart; p < aSrcEnd; ++p, ++aDst) {
      new_(aDst, std::move(*p));
    }
  }

  /*
   * Copy-constructs objects in the uninitialized range [aDst, aDst+aN) from
   * the same object aU.
   */
  template <typename U>
  static inline void copyConstructN(T* aDst, size_t aN, const U& aU) {
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
  [[nodiscard]] static inline bool growTo(Vector<T, N, AP>& aV,
                                          size_t aNewCap) {
    MOZ_ASSERT(!aV.usingInlineStorage());
    MOZ_ASSERT(!CapacityHasExcessSpace<sizeof(T)>(aNewCap));
    T* newbuf = aV.template pod_malloc<T>(aNewCap);
    if (MOZ_UNLIKELY(!newbuf)) {
      return false;
    }
    T* dst = newbuf;
    T* src = aV.beginNoCheck();
    for (; src < aV.endNoCheck(); ++dst, ++src) {
      new_(dst, std::move(*src));
    }
    VectorImpl::destroy(aV.beginNoCheck(), aV.endNoCheck());
    aV.free_(aV.mBegin, aV.mTail.mCapacity);
    aV.mBegin = newbuf;
    /* aV.mLength is unchanged. */
    aV.mTail.mCapacity = aNewCap;
    return true;
  }
};

/*
 * This partial template specialization provides a default implementation for
 * vector operations when the element type is known to be a POD, as judged by
 * IsPod.
 */
template <typename T, size_t N, class AP>
struct VectorImpl<T, N, AP, true> {
  template <typename... Args>
  MOZ_NONNULL(1)
  static inline void new_(T* aDst, Args&&... aArgs) {
    // Explicitly construct a local object instead of using a temporary since
    // T(args...) will be treated like a C-style cast in the unary case and
    // allow unsafe conversions. Both forms should be equivalent to an
    // optimizing compiler.
    T temp(std::forward<Args>(aArgs)...);
    *aDst = temp;
  }

  static inline void destroy(T*, T*) {}

  static inline void initialize(T* aBegin, T* aEnd) {
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

  template <typename U>
  static inline void copyConstruct(T* aDst, const U* aSrcStart,
                                   const U* aSrcEnd) {
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

  template <typename U>
  static inline void moveConstruct(T* aDst, const U* aSrcStart,
                                   const U* aSrcEnd) {
    copyConstruct(aDst, aSrcStart, aSrcEnd);
  }

  static inline void copyConstructN(T* aDst, size_t aN, const T& aT) {
    for (T* end = aDst + aN; aDst < end; ++aDst) {
      new_(aDst, aT);
    }
  }

  [[nodiscard]] static inline bool growTo(Vector<T, N, AP>& aV,
                                          size_t aNewCap) {
    MOZ_ASSERT(!aV.usingInlineStorage());
    MOZ_ASSERT(!CapacityHasExcessSpace<sizeof(T)>(aNewCap));
    T* newbuf =
        aV.template pod_realloc<T>(aV.mBegin, aV.mTail.mCapacity, aNewCap);
    if (MOZ_UNLIKELY(!newbuf)) {
      return false;
    }
    aV.mBegin = newbuf;
    /* aV.mLength is unchanged. */
    aV.mTail.mCapacity = aNewCap;
    return true;
  }
};

// A struct for TestVector.cpp to access private internal fields.
// DO NOT DEFINE IN YOUR OWN CODE.
struct VectorTesting;

}  // namespace detail

/*
 * STL-like container providing a short-lived, dynamic buffer.  Vector calls the
 * constructors/destructors of all elements stored in its internal buffer, so
 * non-PODs may be safely used.  Additionally, Vector will store the first N
 * elements in-place before resorting to dynamic allocation.
 *
 * T requirements:
 *  - default and copy constructible, assignable, destructible
 *  - operations do not throw
 * MinInlineCapacity requirements:
 *  - any value, however, MinInlineCapacity is clamped to min/max values
 * AllocPolicy:
 *  - see "Allocation policies" in AllocPolicy.h (defaults to
 *    mozilla::MallocAllocPolicy)
 *
 * Vector is not reentrant: T member functions called during Vector member
 * functions must not call back into the same object!
 */
template <typename T, size_t MinInlineCapacity = 0,
          class AllocPolicy = MallocAllocPolicy>
class MOZ_NON_PARAM Vector final : private AllocPolicy {
  /* utilities */
  static constexpr bool kElemIsPod =
      std::is_trivial_v<T> && std::is_standard_layout_v<T>;
  typedef detail::VectorImpl<T, MinInlineCapacity, AllocPolicy, kElemIsPod>
      Impl;
  friend struct detail::VectorImpl<T, MinInlineCapacity, AllocPolicy,
                                   kElemIsPod>;

  friend struct detail::VectorTesting;

  [[nodiscard]] bool growStorageBy(size_t aIncr);
  [[nodiscard]] bool convertToHeapStorage(size_t aNewCap);
  [[nodiscard]] bool maybeCheckSimulatedOOM(size_t aRequestedSize);

  /* magic constants */

  /**
   * The maximum space allocated for inline element storage.
   *
   * We reduce space by what the AllocPolicy base class and prior Vector member
   * fields likely consume to attempt to play well with binary size classes.
   */
  static constexpr size_t kMaxInlineBytes =
      1024 -
      (sizeof(AllocPolicy) + sizeof(T*) + sizeof(size_t) + sizeof(size_t));

  /**
   * The number of T elements of inline capacity built into this Vector.  This
   * is usually |MinInlineCapacity|, but it may be less (or zero!) for large T.
   *
   * We use a partially-specialized template (not explicit specialization, which
   * is only allowed at namespace scope) to compute this value.  The benefit is
   * that |sizeof(T)| need not be computed, and |T| doesn't have to be fully
   * defined at the time |Vector<T>| appears, if no inline storage is requested.
   */
  template <size_t MinimumInlineCapacity, size_t Dummy>
  struct ComputeCapacity {
    static constexpr size_t value =
        tl::Min<MinimumInlineCapacity, kMaxInlineBytes / sizeof(T)>::value;
  };

  template <size_t Dummy>
  struct ComputeCapacity<0, Dummy> {
    static constexpr size_t value = 0;
  };

  /** The actual inline capacity in number of elements T.  This may be zero! */
  static constexpr size_t kInlineCapacity =
      ComputeCapacity<MinInlineCapacity, 0>::value;

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

  /*
   * Memory used to store capacity, reserved element count (debug builds only),
   * and inline storage.  The simple "answer" is:
   *
   *   size_t mCapacity;
   *   #ifdef DEBUG
   *   size_t mReserved;
   *   #endif
   *   alignas(T) unsigned char mBytes[kInlineCapacity * sizeof(T)];
   *
   * but there are complications.  First, C++ forbids zero-sized arrays that
   * might result.  Second, we don't want zero capacity to affect Vector's size
   * (even empty classes take up a byte, unless they're base classes).
   *
   * Yet again, we eliminate the zero-sized array using partial specialization.
   * And we eliminate potential size hit by putting capacity/reserved in one
   * struct, then putting the array (if any) in a derived struct.  If no array
   * is needed, the derived struct won't consume extra space.
   */
  struct CapacityAndReserved {
    explicit CapacityAndReserved(size_t aCapacity, size_t aReserved)
        : mCapacity(aCapacity)
#ifdef DEBUG
          ,
          mReserved(aReserved)
#endif
    {
    }
    CapacityAndReserved() = default;

    /* Max number of elements storable in the vector without resizing. */
    size_t mCapacity;

#ifdef DEBUG
    /* Max elements of reserved or used space in this vector. */
    size_t mReserved;
#endif
  };

// Silence warnings about this struct possibly being padded dued to the
// alignas() in it -- there's nothing we can do to avoid it.
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable : 4324)
#endif  // _MSC_VER

  template <size_t Capacity, size_t Dummy>
  struct CRAndStorage : CapacityAndReserved {
    explicit CRAndStorage(size_t aCapacity, size_t aReserved)
        : CapacityAndReserved(aCapacity, aReserved) {}
    CRAndStorage() = default;

    alignas(T) unsigned char mBytes[Capacity * sizeof(T)];

    // GCC fails due to -Werror=strict-aliasing if |mBytes| is directly cast to
    // T*.  Indirecting through this function addresses the problem.
    void* data() { return mBytes; }

    T* storage() { return static_cast<T*>(data()); }
  };

  template <size_t Dummy>
  struct CRAndStorage<0, Dummy> : CapacityAndReserved {
    explicit CRAndStorage(size_t aCapacity, size_t aReserved)
        : CapacityAndReserved(aCapacity, aReserved) {}
    CRAndStorage() = default;

    T* storage() {
      // If this returns |nullptr|, functions like |Vector::begin()| would too,
      // breaking callers that pass a vector's elements as pointer/length to
      // code that bounds its operation by length but (even just as a sanity
      // check) always wants a non-null pointer.  Fake up an aligned, non-null
      // pointer to support these callers.
      return reinterpret_cast<T*>(sizeof(T));
    }
  };

  CRAndStorage<kInlineCapacity, 0> mTail;

#ifdef _MSC_VER
#  pragma warning(pop)
#endif  // _MSC_VER

#ifdef DEBUG
  friend class ReentrancyGuard;
  bool mEntered;
#endif

  /* private accessors */

  bool usingInlineStorage() const {
    return mBegin == const_cast<Vector*>(this)->inlineStorage();
  }

  T* inlineStorage() { return mTail.storage(); }

  T* beginNoCheck() const { return mBegin; }

  T* endNoCheck() { return mBegin + mLength; }

  const T* endNoCheck() const { return mBegin + mLength; }

#ifdef DEBUG
  /**
   * The amount of explicitly allocated space in this vector that is immediately
   * available to be filled by appending additional elements.  This value is
   * always greater than or equal to |length()| -- the vector's actual elements
   * are implicitly reserved.  This value is always less than or equal to
   * |capacity()|.  It may be explicitly increased using the |reserve()| method.
   */
  size_t reserved() const {
    MOZ_ASSERT(mLength <= mTail.mReserved);
    MOZ_ASSERT(mTail.mReserved <= mTail.mCapacity);
    return mTail.mReserved;
  }
#endif

  bool internalEnsureCapacity(size_t aNeeded);

  /* Append operations guaranteed to succeed due to pre-reserved space. */
  template <typename U>
  void internalAppend(U&& aU);
  template <typename U, size_t O, class BP>
  void internalAppendAll(const Vector<U, O, BP>& aU);
  void internalAppendN(const T& aT, size_t aN);
  template <typename U>
  void internalAppend(const U* aBegin, size_t aLength);
  template <typename U>
  void internalMoveAppend(U* aBegin, size_t aLength);

 public:
  static const size_t sMaxInlineStorage = MinInlineCapacity;

  typedef T ElementType;

  explicit Vector(AllocPolicy);
  Vector() : Vector(AllocPolicy()) {}

  Vector(Vector&&);            /* Move constructor. */
  Vector& operator=(Vector&&); /* Move assignment. */
  ~Vector();

  /* accessors */

  const AllocPolicy& allocPolicy() const { return *this; }

  AllocPolicy& allocPolicy() { return *this; }

  enum { InlineLength = MinInlineCapacity };

  size_t length() const { return mLength; }

  bool empty() const { return mLength == 0; }

  size_t capacity() const { return mTail.mCapacity; }

  T* begin() {
    MOZ_ASSERT(!mEntered);
    return mBegin;
  }

  const T* begin() const {
    MOZ_ASSERT(!mEntered);
    return mBegin;
  }

  T* end() {
    MOZ_ASSERT(!mEntered);
    return mBegin + mLength;
  }

  const T* end() const {
    MOZ_ASSERT(!mEntered);
    return mBegin + mLength;
  }

  T& operator[](size_t aIndex) {
    MOZ_ASSERT(!mEntered);
    MOZ_ASSERT(aIndex < mLength);
    return begin()[aIndex];
  }

  const T& operator[](size_t aIndex) const {
    MOZ_ASSERT(!mEntered);
    MOZ_ASSERT(aIndex < mLength);
    return begin()[aIndex];
  }

  T& back() {
    MOZ_ASSERT(!mEntered);
    MOZ_ASSERT(!empty());
    return *(end() - 1);
  }

  const T& back() const {
    MOZ_ASSERT(!mEntered);
    MOZ_ASSERT(!empty());
    return *(end() - 1);
  }

  operator mozilla::Span<const T>() const {
    // Explicitly specify template argument here to avoid instantiating Span<T>
    // first and then implicitly converting to Span<const T>
    return mozilla::Span<const T>{mBegin, mLength};
  }

  operator mozilla::Span<T>() { return mozilla::Span{mBegin, mLength}; }

  class Range {
    friend class Vector;
    T* mCur;
    T* mEnd;
    Range(T* aCur, T* aEnd) : mCur(aCur), mEnd(aEnd) {
      MOZ_ASSERT(aCur <= aEnd);
    }

   public:
    bool empty() const { return mCur == mEnd; }
    size_t remain() const { return PointerRangeSize(mCur, mEnd); }
    T& front() const {
      MOZ_ASSERT(!empty());
      return *mCur;
    }
    void popFront() {
      MOZ_ASSERT(!empty());
      ++mCur;
    }
    T popCopyFront() {
      MOZ_ASSERT(!empty());
      return *mCur++;
    }
  };

  class ConstRange {
    friend class Vector;
    const T* mCur;
    const T* mEnd;
    ConstRange(const T* aCur, const T* aEnd) : mCur(aCur), mEnd(aEnd) {
      MOZ_ASSERT(aCur <= aEnd);
    }

   public:
    bool empty() const { return mCur == mEnd; }
    size_t remain() const { return PointerRangeSize(mCur, mEnd); }
    const T& front() const {
      MOZ_ASSERT(!empty());
      return *mCur;
    }
    void popFront() {
      MOZ_ASSERT(!empty());
      ++mCur;
    }
    T popCopyFront() {
      MOZ_ASSERT(!empty());
      return *mCur++;
    }
  };

  Range all() { return Range(begin(), end()); }
  ConstRange all() const { return ConstRange(begin(), end()); }

  /* mutators */

  /**
   * Reverse the order of the elements in the vector in place.
   */
  void reverse();

  /**
   * Given that the vector is empty, grow the internal capacity to |aRequest|,
   * keeping the length 0.
   */
  [[nodiscard]] bool initCapacity(size_t aRequest);

  /**
   * Given that the vector is empty, grow the internal capacity and length to
   * |aRequest| leaving the elements' memory completely uninitialized (with all
   * the associated hazards and caveats). This avoids the usual allocation-size
   * rounding that happens in resize and overhead of initialization for elements
   * that are about to be overwritten.
   */
  [[nodiscard]] bool initLengthUninitialized(size_t aRequest);

  /**
   * If reserve(aRequest) succeeds and |aRequest >= length()|, then appending
   * |aRequest - length()| elements, in any sequence of append/appendAll calls,
   * is guaranteed to succeed.
   *
   * A request to reserve an amount less than the current length does not affect
   * reserved space.
   */
  [[nodiscard]] bool reserve(size_t aRequest);

  /**
   * Destroy elements in the range [end() - aIncr, end()). Does not deallocate
   * or unreserve storage for those elements.
   */
  void shrinkBy(size_t aIncr);

  /**
   * Destroy elements in the range [aNewLength, end()). Does not deallocate
   * or unreserve storage for those elements.
   */
  void shrinkTo(size_t aNewLength);

  /** Grow the vector by aIncr elements. */
  [[nodiscard]] bool growBy(size_t aIncr);

  /** Call shrinkBy or growBy based on whether newSize > length(). */
  [[nodiscard]] bool resize(size_t aNewLength);

  /**
   * Increase the length of the vector, but don't initialize the new elements
   * -- leave them as uninitialized memory.
   */
  [[nodiscard]] bool growByUninitialized(size_t aIncr);
  void infallibleGrowByUninitialized(size_t aIncr);
  [[nodiscard]] bool resizeUninitialized(size_t aNewLength);

  /** Shorthand for shrinkBy(length()). */
  void clear();

  /** Clears and releases any heap-allocated storage. */
  void clearAndFree();

  /**
   * Shrinks the storage to drop excess capacity if possible.
   *
   * The return value indicates whether the operation succeeded, otherwise, it
   * represents an OOM. The bool can be safely ignored unless you want to
   * provide the guarantee that `length() == capacity()`.
   *
   * For PODs, it calls the AllocPolicy's pod_realloc. For non-PODs, it moves
   * the elements into the new storage.
   */
  bool shrinkStorageToFit();

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
  template <typename U>
  [[nodiscard]] bool append(U&& aU);

  /**
   * Construct a T in-place as a new entry at the end of this vector.
   */
  template <typename... Args>
  [[nodiscard]] bool emplaceBack(Args&&... aArgs) {
    if (!growByUninitialized(1)) return false;
    Impl::new_(&back(), std::forward<Args>(aArgs)...);
    return true;
  }

  template <typename U, size_t O, class BP>
  [[nodiscard]] bool appendAll(const Vector<U, O, BP>& aU);
  template <typename U, size_t O, class BP>
  [[nodiscard]] bool appendAll(Vector<U, O, BP>&& aU);
  [[nodiscard]] bool appendN(const T& aT, size_t aN);
  template <typename U>
  [[nodiscard]] bool append(const U* aBegin, const U* aEnd);
  template <typename U>
  [[nodiscard]] bool append(const U* aBegin, size_t aLength);
  template <typename U>
  [[nodiscard]] bool moveAppend(U* aBegin, U* aEnd);

  /*
   * Guaranteed-infallible append operations for use upon vectors whose
   * memory has been pre-reserved.  Don't use this if you haven't reserved the
   * memory!
   */
  template <typename U>
  void infallibleAppend(U&& aU) {
    internalAppend(std::forward<U>(aU));
  }
  void infallibleAppendN(const T& aT, size_t aN) { internalAppendN(aT, aN); }
  template <typename U>
  void infallibleAppend(const U* aBegin, const U* aEnd) {
    internalAppend(aBegin, PointerRangeSize(aBegin, aEnd));
  }
  template <typename U>
  void infallibleAppend(const U* aBegin, size_t aLength) {
    internalAppend(aBegin, aLength);
  }
  template <typename... Args>
  void infallibleEmplaceBack(Args&&... aArgs) {
    infallibleGrowByUninitialized(1);
    Impl::new_(&back(), std::forward<Args>(aArgs)...);
  }

  void popBack();

  T popCopy();

  /**
   * If elements are stored in-place, return nullptr and leave this vector
   * unmodified.
   *
   * Otherwise return this vector's elements buffer, and clear this vector as if
   * by clearAndFree(). The caller now owns the buffer and is responsible for
   * deallocating it consistent with this vector's AllocPolicy.
   *
   * N.B. Although a T*, only the range [0, length()) is constructed.
   */
  [[nodiscard]] T* extractRawBuffer();

  /**
   * If elements are stored in-place, allocate a new buffer, move this vector's
   * elements into it, and return that buffer.
   *
   * Otherwise return this vector's elements buffer. The caller now owns the
   * buffer and is responsible for deallocating it consistent with this vector's
   * AllocPolicy.
   *
   * This vector is cleared, as if by clearAndFree(), when this method
   * succeeds. This method fails and returns nullptr only if new elements buffer
   * allocation fails.
   *
   * N.B. Only the range [0, length()) of the returned buffer is constructed.
   * If any of these elements are uninitialized (as growByUninitialized
   * enables), behavior is undefined.
   */
  [[nodiscard]] T* extractOrCopyRawBuffer();

  /**
   * Transfer ownership of an array of objects into the vector.  The caller
   * must have allocated the array in accordance with this vector's
   * AllocPolicy.
   *
   * N.B. This call assumes that there are no uninitialized elements in the
   *      passed range [aP, aP + aLength). The range [aP + aLength, aP +
   *      aCapacity) must be allocated uninitialized memory.
   */
  void replaceRawBuffer(T* aP, size_t aLength, size_t aCapacity);

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
  template <typename U>
  [[nodiscard]] T* insert(T* aP, U&& aVal);

  /**
   * Removes the element |aT|, which must fall in the bounds [begin, end),
   * shifting existing elements from |aT + 1| onward one position lower.
   */
  void erase(T* aT);

  /**
   * Removes the elements [|aBegin|, |aEnd|), which must fall in the bounds
   * [begin, end), shifting existing elements from |aEnd| onward to aBegin's old
   * position.
   */
  void erase(T* aBegin, T* aEnd);

  /**
   * Removes all elements that satisfy the predicate, shifting existing elements
   * lower to fill erased gaps.
   */
  template <typename Pred>
  void eraseIf(Pred aPred);

  /**
   * Removes all elements that compare equal to |aU|, shifting existing elements
   * lower to fill erased gaps.
   */
  template <typename U>
  void eraseIfEqual(const U& aU);

  /**
   * Measure the size of the vector's heap-allocated storage.
   */
  size_t sizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const;

  /**
   * Like sizeOfExcludingThis, but also measures the size of the vector
   * object (which must be heap-allocated) itself.
   */
  size_t sizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const;

  void swap(Vector& aOther);

 private:
  Vector(const Vector&) = delete;
  void operator=(const Vector&) = delete;
};

/* This does the re-entrancy check plus several other sanity checks. */
#define MOZ_REENTRANCY_GUARD_ET_AL                                         \
  ReentrancyGuard g(*this);                                                \
  MOZ_ASSERT_IF(usingInlineStorage(), mTail.mCapacity == kInlineCapacity); \
  MOZ_ASSERT(reserved() <= mTail.mCapacity);                               \
  MOZ_ASSERT(mLength <= reserved());                                       \
  MOZ_ASSERT(mLength <= mTail.mCapacity)

/* Vector Implementation */

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE Vector<T, N, AP>::Vector(AP aAP)
    : AP(std::move(aAP)),
      mLength(0),
      mTail(kInlineCapacity, 0)
#ifdef DEBUG
      ,
      mEntered(false)
#endif
{
  mBegin = inlineStorage();
}

/* Move constructor. */
template <typename T, size_t N, class AllocPolicy>
MOZ_ALWAYS_INLINE Vector<T, N, AllocPolicy>::Vector(Vector&& aRhs)
    : AllocPolicy(std::move(aRhs))
#ifdef DEBUG
      ,
      mEntered(false)
#endif
{
  mLength = aRhs.mLength;
  mTail.mCapacity = aRhs.mTail.mCapacity;
#ifdef DEBUG
  mTail.mReserved = aRhs.mTail.mReserved;
#endif

  if (aRhs.usingInlineStorage()) {
    /* We can't move the buffer over in this case, so copy elements. */
    mBegin = inlineStorage();
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
    aRhs.mBegin = aRhs.inlineStorage();
    aRhs.mTail.mCapacity = kInlineCapacity;
    aRhs.mLength = 0;
#ifdef DEBUG
    aRhs.mTail.mReserved = 0;
#endif
  }
}

/* Move assignment. */
template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE Vector<T, N, AP>& Vector<T, N, AP>::operator=(Vector&& aRhs) {
  MOZ_ASSERT(this != &aRhs, "self-move assignment is prohibited");
  this->~Vector();
  new (KnownNotNull, this) Vector(std::move(aRhs));
  return *this;
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE Vector<T, N, AP>::~Vector() {
  MOZ_REENTRANCY_GUARD_ET_AL;
  Impl::destroy(beginNoCheck(), endNoCheck());
  if (!usingInlineStorage()) {
    this->free_(beginNoCheck(), mTail.mCapacity);
  }
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE void Vector<T, N, AP>::reverse() {
  MOZ_REENTRANCY_GUARD_ET_AL;
  T* elems = mBegin;
  size_t len = mLength;
  size_t mid = len / 2;
  for (size_t i = 0; i < mid; i++) {
    std::swap(elems[i], elems[len - i - 1]);
  }
}

/*
 * This function will create a new heap buffer with capacity aNewCap,
 * move all elements in the inline buffer to this new buffer,
 * and fail on OOM.
 */
template <typename T, size_t N, class AP>
inline bool Vector<T, N, AP>::convertToHeapStorage(size_t aNewCap) {
  MOZ_ASSERT(usingInlineStorage());

  /* Allocate buffer. */
  MOZ_ASSERT(!detail::CapacityHasExcessSpace<sizeof(T)>(aNewCap));
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
  mTail.mCapacity = aNewCap;
  return true;
}

template <typename T, size_t N, class AP>
MOZ_NEVER_INLINE bool Vector<T, N, AP>::growStorageBy(size_t aIncr) {
  MOZ_ASSERT(mLength + aIncr > mTail.mCapacity);

  size_t newCap;

  if (aIncr == 1 && usingInlineStorage()) {
    /* This case occurs in ~70--80% of the calls to this function. */
    constexpr size_t newSize =
        tl::RoundUpPow2<(kInlineCapacity + 1) * sizeof(T)>::value;
    static_assert(newSize / sizeof(T) > 0,
                  "overflow when exceeding inline Vector storage");
    newCap = newSize / sizeof(T);
  } else {
    newCap = detail::ComputeGrowth<AP, sizeof(T)>(mLength, aIncr, true);
    if (MOZ_UNLIKELY(newCap == 0)) {
      this->reportAllocOverflow();
      return false;
    }
  }

  if (usingInlineStorage()) {
    return convertToHeapStorage(newCap);
  }

  return Impl::growTo(*this, newCap);
}

template <typename T, size_t N, class AP>
inline bool Vector<T, N, AP>::initCapacity(size_t aRequest) {
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
  mTail.mCapacity = aRequest;
#ifdef DEBUG
  mTail.mReserved = aRequest;
#endif
  return true;
}

template <typename T, size_t N, class AP>
inline bool Vector<T, N, AP>::initLengthUninitialized(size_t aRequest) {
  if (!initCapacity(aRequest)) {
    return false;
  }
  infallibleGrowByUninitialized(aRequest);
  return true;
}

template <typename T, size_t N, class AP>
inline bool Vector<T, N, AP>::maybeCheckSimulatedOOM(size_t aRequestedSize) {
  if (aRequestedSize <= N) {
    return true;
  }

#ifdef DEBUG
  if (aRequestedSize <= mTail.mReserved) {
    return true;
  }
#endif

  return allocPolicy().checkSimulatedOOM();
}

template <typename T, size_t N, class AP>
inline bool Vector<T, N, AP>::reserve(size_t aRequest) {
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (aRequest > mTail.mCapacity) {
    if (MOZ_UNLIKELY(!growStorageBy(aRequest - mLength))) {
      return false;
    }
  } else if (!maybeCheckSimulatedOOM(aRequest)) {
    return false;
  }
#ifdef DEBUG
  if (aRequest > mTail.mReserved) {
    mTail.mReserved = aRequest;
  }
  MOZ_ASSERT(mLength <= mTail.mReserved);
  MOZ_ASSERT(mTail.mReserved <= mTail.mCapacity);
#endif
  return true;
}

template <typename T, size_t N, class AP>
inline void Vector<T, N, AP>::shrinkBy(size_t aIncr) {
  MOZ_REENTRANCY_GUARD_ET_AL;
  MOZ_ASSERT(aIncr <= mLength);
  Impl::destroy(endNoCheck() - aIncr, endNoCheck());
  mLength -= aIncr;
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE void Vector<T, N, AP>::shrinkTo(size_t aNewLength) {
  MOZ_ASSERT(aNewLength <= mLength);
  shrinkBy(mLength - aNewLength);
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::growBy(size_t aIncr) {
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (aIncr > mTail.mCapacity - mLength) {
    if (MOZ_UNLIKELY(!growStorageBy(aIncr))) {
      return false;
    }
  } else if (!maybeCheckSimulatedOOM(mLength + aIncr)) {
    return false;
  }
  MOZ_ASSERT(mLength + aIncr <= mTail.mCapacity);
  T* newend = endNoCheck() + aIncr;
  Impl::initialize(endNoCheck(), newend);
  mLength += aIncr;
#ifdef DEBUG
  if (mLength > mTail.mReserved) {
    mTail.mReserved = mLength;
  }
#endif
  return true;
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::growByUninitialized(size_t aIncr) {
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (aIncr > mTail.mCapacity - mLength) {
    if (MOZ_UNLIKELY(!growStorageBy(aIncr))) {
      return false;
    }
  } else if (!maybeCheckSimulatedOOM(mLength + aIncr)) {
    return false;
  }
#ifdef DEBUG
  if (mLength + aIncr > mTail.mReserved) {
    mTail.mReserved = mLength + aIncr;
  }
#endif
  infallibleGrowByUninitialized(aIncr);
  return true;
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE void Vector<T, N, AP>::infallibleGrowByUninitialized(
    size_t aIncr) {
  MOZ_ASSERT(mLength + aIncr <= reserved());
  mLength += aIncr;
}

template <typename T, size_t N, class AP>
inline bool Vector<T, N, AP>::resize(size_t aNewLength) {
  size_t curLength = mLength;
  if (aNewLength > curLength) {
    return growBy(aNewLength - curLength);
  }
  shrinkBy(curLength - aNewLength);
  return true;
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::resizeUninitialized(
    size_t aNewLength) {
  size_t curLength = mLength;
  if (aNewLength > curLength) {
    return growByUninitialized(aNewLength - curLength);
  }
  shrinkBy(curLength - aNewLength);
  return true;
}

template <typename T, size_t N, class AP>
inline void Vector<T, N, AP>::clear() {
  MOZ_REENTRANCY_GUARD_ET_AL;
  Impl::destroy(beginNoCheck(), endNoCheck());
  mLength = 0;
}

template <typename T, size_t N, class AP>
inline void Vector<T, N, AP>::clearAndFree() {
  clear();

  if (usingInlineStorage()) {
    return;
  }
  this->free_(beginNoCheck(), mTail.mCapacity);
  mBegin = inlineStorage();
  mTail.mCapacity = kInlineCapacity;
#ifdef DEBUG
  mTail.mReserved = 0;
#endif
}

template <typename T, size_t N, class AP>
inline bool Vector<T, N, AP>::shrinkStorageToFit() {
  MOZ_REENTRANCY_GUARD_ET_AL;

  const auto length = this->length();
  if (usingInlineStorage() || length == capacity()) {
    return true;
  }

  if (!length) {
    this->free_(beginNoCheck(), mTail.mCapacity);
    mBegin = inlineStorage();
    mTail.mCapacity = kInlineCapacity;
#ifdef DEBUG
    mTail.mReserved = 0;
#endif
    return true;
  }

  T* newBuf;
  size_t newCap;
  if (length <= kInlineCapacity) {
    newBuf = inlineStorage();
    newCap = kInlineCapacity;
  } else {
    if (kElemIsPod) {
      newBuf = this->template pod_realloc<T>(beginNoCheck(), mTail.mCapacity,
                                             length);
    } else {
      newBuf = this->template pod_malloc<T>(length);
    }
    if (MOZ_UNLIKELY(!newBuf)) {
      return false;
    }
    newCap = length;
  }
  if (!kElemIsPod || newBuf == inlineStorage()) {
    Impl::moveConstruct(newBuf, beginNoCheck(), endNoCheck());
    Impl::destroy(beginNoCheck(), endNoCheck());
  }
  if (!kElemIsPod) {
    this->free_(beginNoCheck(), mTail.mCapacity);
  }
  mBegin = newBuf;
  mTail.mCapacity = newCap;
#ifdef DEBUG
  mTail.mReserved = length;
#endif
  return true;
}

template <typename T, size_t N, class AP>
inline bool Vector<T, N, AP>::canAppendWithoutRealloc(size_t aNeeded) const {
  return mLength + aNeeded <= mTail.mCapacity;
}

template <typename T, size_t N, class AP>
template <typename U, size_t O, class BP>
MOZ_ALWAYS_INLINE void Vector<T, N, AP>::internalAppendAll(
    const Vector<U, O, BP>& aOther) {
  internalAppend(aOther.begin(), aOther.length());
}

template <typename T, size_t N, class AP>
template <typename U>
MOZ_ALWAYS_INLINE void Vector<T, N, AP>::internalAppend(U&& aU) {
  MOZ_ASSERT(mLength + 1 <= mTail.mReserved);
  MOZ_ASSERT(mTail.mReserved <= mTail.mCapacity);
  Impl::new_(endNoCheck(), std::forward<U>(aU));
  ++mLength;
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::appendN(const T& aT, size_t aNeeded) {
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (mLength + aNeeded > mTail.mCapacity) {
    if (MOZ_UNLIKELY(!growStorageBy(aNeeded))) {
      return false;
    }
  } else if (!maybeCheckSimulatedOOM(mLength + aNeeded)) {
    return false;
  }
#ifdef DEBUG
  if (mLength + aNeeded > mTail.mReserved) {
    mTail.mReserved = mLength + aNeeded;
  }
#endif
  internalAppendN(aT, aNeeded);
  return true;
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE void Vector<T, N, AP>::internalAppendN(const T& aT,
                                                         size_t aNeeded) {
  MOZ_ASSERT(mLength + aNeeded <= mTail.mReserved);
  MOZ_ASSERT(mTail.mReserved <= mTail.mCapacity);
  Impl::copyConstructN(endNoCheck(), aNeeded, aT);
  mLength += aNeeded;
}

template <typename T, size_t N, class AP>
template <typename U>
inline T* Vector<T, N, AP>::insert(T* aP, U&& aVal) {
  MOZ_ASSERT(begin() <= aP);
  MOZ_ASSERT(aP <= end());
  size_t pos = aP - begin();
  MOZ_ASSERT(pos <= mLength);
  size_t oldLength = mLength;
  if (pos == oldLength) {
    if (!append(std::forward<U>(aVal))) {
      return nullptr;
    }
  } else {
    T oldBack = std::move(back());
    if (!append(std::move(oldBack))) {
      return nullptr;
    }
    for (size_t i = oldLength - 1; i > pos; --i) {
      (*this)[i] = std::move((*this)[i - 1]);
    }
    (*this)[pos] = std::forward<U>(aVal);
  }
  return begin() + pos;
}

template <typename T, size_t N, class AP>
inline void Vector<T, N, AP>::erase(T* aIt) {
  MOZ_ASSERT(begin() <= aIt);
  MOZ_ASSERT(aIt < end());
  while (aIt + 1 < end()) {
    *aIt = std::move(*(aIt + 1));
    ++aIt;
  }
  popBack();
}

template <typename T, size_t N, class AP>
inline void Vector<T, N, AP>::erase(T* aBegin, T* aEnd) {
  MOZ_ASSERT(begin() <= aBegin);
  MOZ_ASSERT(aBegin <= aEnd);
  MOZ_ASSERT(aEnd <= end());
  while (aEnd < end()) {
    *aBegin++ = std::move(*aEnd++);
  }
  shrinkBy(aEnd - aBegin);
}

template <typename T, size_t N, class AP>
template <typename Pred>
void Vector<T, N, AP>::eraseIf(Pred aPred) {
  // remove_if finds the first element to be erased, and then efficiently move-
  // assigns elements to effectively overwrite elements that satisfy the
  // predicate. It returns the new end pointer, after which there are only
  // moved-from elements ready to be destroyed, so we just need to shrink the
  // vector accordingly.
  T* newEnd = std::remove_if(begin(), end(),
                             [&aPred](const T& aT) { return aPred(aT); });
  MOZ_ASSERT(newEnd <= end());
  shrinkBy(end() - newEnd);
}

template <typename T, size_t N, class AP>
template <typename U>
void Vector<T, N, AP>::eraseIfEqual(const U& aU) {
  return eraseIf([&aU](const T& aT) { return aT == aU; });
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::internalEnsureCapacity(
    size_t aNeeded) {
  if (mLength + aNeeded > mTail.mCapacity) {
    if (MOZ_UNLIKELY(!growStorageBy(aNeeded))) {
      return false;
    }
  } else if (!maybeCheckSimulatedOOM(mLength + aNeeded)) {
    return false;
  }
#ifdef DEBUG
  if (mLength + aNeeded > mTail.mReserved) {
    mTail.mReserved = mLength + aNeeded;
  }
#endif
  return true;
}

template <typename T, size_t N, class AP>
template <typename U>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::append(const U* aInsBegin,
                                                const U* aInsEnd) {
  MOZ_REENTRANCY_GUARD_ET_AL;
  const size_t needed = PointerRangeSize(aInsBegin, aInsEnd);
  if (!internalEnsureCapacity(needed)) {
    return false;
  }
  internalAppend(aInsBegin, needed);
  return true;
}

template <typename T, size_t N, class AP>
template <typename U>
MOZ_ALWAYS_INLINE void Vector<T, N, AP>::internalAppend(const U* aInsBegin,
                                                        size_t aInsLength) {
  MOZ_ASSERT(mLength + aInsLength <= mTail.mReserved);
  MOZ_ASSERT(mTail.mReserved <= mTail.mCapacity);
  Impl::copyConstruct(endNoCheck(), aInsBegin, aInsBegin + aInsLength);
  mLength += aInsLength;
}

template <typename T, size_t N, class AP>
template <typename U>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::moveAppend(U* aInsBegin, U* aInsEnd) {
  MOZ_REENTRANCY_GUARD_ET_AL;
  const size_t needed = PointerRangeSize(aInsBegin, aInsEnd);
  if (!internalEnsureCapacity(needed)) {
    return false;
  }
  internalMoveAppend(aInsBegin, needed);
  return true;
}

template <typename T, size_t N, class AP>
template <typename U>
MOZ_ALWAYS_INLINE void Vector<T, N, AP>::internalMoveAppend(U* aInsBegin,
                                                            size_t aInsLength) {
  MOZ_ASSERT(mLength + aInsLength <= mTail.mReserved);
  MOZ_ASSERT(mTail.mReserved <= mTail.mCapacity);
  Impl::moveConstruct(endNoCheck(), aInsBegin, aInsBegin + aInsLength);
  mLength += aInsLength;
}

template <typename T, size_t N, class AP>
template <typename U>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::append(U&& aU) {
  MOZ_REENTRANCY_GUARD_ET_AL;
  if (mLength == mTail.mCapacity) {
    if (MOZ_UNLIKELY(!growStorageBy(1))) {
      return false;
    }
  } else if (!maybeCheckSimulatedOOM(mLength + 1)) {
    return false;
  }
#ifdef DEBUG
  if (mLength + 1 > mTail.mReserved) {
    mTail.mReserved = mLength + 1;
  }
#endif
  internalAppend(std::forward<U>(aU));
  return true;
}

template <typename T, size_t N, class AP>
template <typename U, size_t O, class BP>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::appendAll(
    const Vector<U, O, BP>& aOther) {
  return append(aOther.begin(), aOther.length());
}

template <typename T, size_t N, class AP>
template <typename U, size_t O, class BP>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::appendAll(Vector<U, O, BP>&& aOther) {
  if (empty() && capacity() < aOther.length()) {
    *this = std::move(aOther);
    return true;
  }

  if (moveAppend(aOther.begin(), aOther.end())) {
    aOther.clearAndFree();
    return true;
  }

  return false;
}

template <typename T, size_t N, class AP>
template <class U>
MOZ_ALWAYS_INLINE bool Vector<T, N, AP>::append(const U* aInsBegin,
                                                size_t aInsLength) {
  return append(aInsBegin, aInsBegin + aInsLength);
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE void Vector<T, N, AP>::popBack() {
  MOZ_REENTRANCY_GUARD_ET_AL;
  MOZ_ASSERT(!empty());
  --mLength;
  endNoCheck()->~T();
}

template <typename T, size_t N, class AP>
MOZ_ALWAYS_INLINE T Vector<T, N, AP>::popCopy() {
  T ret = back();
  popBack();
  return ret;
}

template <typename T, size_t N, class AP>
inline T* Vector<T, N, AP>::extractRawBuffer() {
  MOZ_REENTRANCY_GUARD_ET_AL;

  if (usingInlineStorage()) {
    return nullptr;
  }

  T* ret = mBegin;
  mBegin = inlineStorage();
  mLength = 0;
  mTail.mCapacity = kInlineCapacity;
#ifdef DEBUG
  mTail.mReserved = 0;
#endif
  return ret;
}

template <typename T, size_t N, class AP>
inline T* Vector<T, N, AP>::extractOrCopyRawBuffer() {
  if (T* ret = extractRawBuffer()) {
    return ret;
  }

  MOZ_REENTRANCY_GUARD_ET_AL;

  T* copy = this->template pod_malloc<T>(mLength);
  if (!copy) {
    return nullptr;
  }

  Impl::moveConstruct(copy, beginNoCheck(), endNoCheck());
  Impl::destroy(beginNoCheck(), endNoCheck());
  mBegin = inlineStorage();
  mLength = 0;
  mTail.mCapacity = kInlineCapacity;
#ifdef DEBUG
  mTail.mReserved = 0;
#endif
  return copy;
}

template <typename T, size_t N, class AP>
inline void Vector<T, N, AP>::replaceRawBuffer(T* aP, size_t aLength,
                                               size_t aCapacity) {
  MOZ_REENTRANCY_GUARD_ET_AL;

  /* Destroy what we have. */
  Impl::destroy(beginNoCheck(), endNoCheck());
  if (!usingInlineStorage()) {
    this->free_(beginNoCheck(), mTail.mCapacity);
  }

  /* Take in the new buffer. */
  if (aCapacity <= kInlineCapacity) {
    /*
     * We convert to inline storage if possible, even though aP might
     * otherwise be acceptable.  Maybe this behaviour should be
     * specifiable with an argument to this function.
     */
    mBegin = inlineStorage();
    mLength = aLength;
    mTail.mCapacity = kInlineCapacity;
    Impl::moveConstruct(mBegin, aP, aP + aLength);
    Impl::destroy(aP, aP + aLength);
    this->free_(aP, aCapacity);
  } else {
    mBegin = aP;
    mLength = aLength;
    mTail.mCapacity = aCapacity;
  }
#ifdef DEBUG
  mTail.mReserved = aCapacity;
#endif
}

template <typename T, size_t N, class AP>
inline void Vector<T, N, AP>::replaceRawBuffer(T* aP, size_t aLength) {
  replaceRawBuffer(aP, aLength, aLength);
}

template <typename T, size_t N, class AP>
inline size_t Vector<T, N, AP>::sizeOfExcludingThis(
    MallocSizeOf aMallocSizeOf) const {
  return usingInlineStorage() ? 0 : aMallocSizeOf(beginNoCheck());
}

template <typename T, size_t N, class AP>
inline size_t Vector<T, N, AP>::sizeOfIncludingThis(
    MallocSizeOf aMallocSizeOf) const {
  return aMallocSizeOf(this) + sizeOfExcludingThis(aMallocSizeOf);
}

template <typename T, size_t N, class AP>
inline void Vector<T, N, AP>::swap(Vector& aOther) {
  static_assert(N == 0, "still need to implement this for N != 0");

  // This only works when inline storage is always empty.
  if (!usingInlineStorage() && aOther.usingInlineStorage()) {
    aOther.mBegin = mBegin;
    mBegin = inlineStorage();
  } else if (usingInlineStorage() && !aOther.usingInlineStorage()) {
    mBegin = aOther.mBegin;
    aOther.mBegin = aOther.inlineStorage();
  } else if (!usingInlineStorage() && !aOther.usingInlineStorage()) {
    std::swap(mBegin, aOther.mBegin);
  } else {
    // This case is a no-op, since we'd set both to use their inline storage.
  }

  std::swap(mLength, aOther.mLength);
  std::swap(mTail.mCapacity, aOther.mTail.mCapacity);
#ifdef DEBUG
  std::swap(mTail.mReserved, aOther.mTail.mReserved);
#endif
}

}  // namespace mozilla

#endif /* mozilla_Vector_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// A simple segmented vector class.
//
// This class should be used in preference to mozilla::Vector or nsTArray when
// you are simply gathering items in order to later iterate over them.
//
// - In the case where you don't know the final size in advance, using
//   SegmentedVector avoids the need to repeatedly allocate increasingly large
//   buffers and copy the data into them.
//
// - In the case where you know the final size in advance and so can set the
//   capacity appropriately, using SegmentedVector still avoids the need for
//   large allocations (which can trigger OOMs).

#ifndef mozilla_SegmentedVector_h
#define mozilla_SegmentedVector_h

#include "mozilla/Alignment.h"
#include "mozilla/AllocPolicy.h"
#include "mozilla/Array.h"
#include "mozilla/LinkedList.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"
#include "mozilla/TypeTraits.h"

#include <new>  // for placement new

namespace mozilla {

// |IdealSegmentSize| specifies how big each segment will be in bytes (or as
// close as is possible). Use the following guidelines to choose a size.
//
// - It should be a power-of-two, to avoid slop.
//
// - It should not be too small, so that segment allocations are infrequent,
//   and so that per-segment bookkeeping overhead is low. Typically each
//   segment should be able to hold hundreds of elements, at least.
//
// - It should not be too large, so that OOMs are unlikely when allocating
//   segments, and so that not too much space is wasted when the final segment
//   is not full.
//
// The ideal size depends on how the SegmentedVector is used and the size of
// |T|, but reasonable sizes include 1024, 4096 (the default), 8192, and 16384.
//
template<typename T,
         size_t IdealSegmentSize = 4096,
         typename AllocPolicy = MallocAllocPolicy>
class SegmentedVector : private AllocPolicy
{
  template<size_t SegmentCapacity>
  struct SegmentImpl
    : public mozilla::LinkedListElement<SegmentImpl<SegmentCapacity>>
  {
    SegmentImpl() : mLength(0) {}

    ~SegmentImpl()
    {
      for (uint32_t i = 0; i < mLength; i++) {
        (*this)[i].~T();
      }
    }

    uint32_t Length() const { return mLength; }

    T* Elems() { return reinterpret_cast<T*>(&mStorage.mBuf); }

    T& operator[](size_t aIndex)
    {
      MOZ_ASSERT(aIndex < mLength);
      return Elems()[aIndex];
    }

    const T& operator[](size_t aIndex) const
    {
      MOZ_ASSERT(aIndex < mLength);
      return Elems()[aIndex];
    }

    template<typename U>
    void Append(U&& aU)
    {
      MOZ_ASSERT(mLength < SegmentCapacity);
      // Pre-increment mLength so that the bounds-check in operator[] passes.
      mLength++;
      T* elem = &(*this)[mLength - 1];
      new (elem) T(mozilla::Forward<U>(aU));
    }

    void PopLast()
    {
      MOZ_ASSERT(mLength > 0);
      (*this)[mLength - 1].~T();
      mLength--;
    }

    uint32_t mLength;

    // The union ensures that the elements are appropriately aligned.
    union Storage
    {
      char mBuf[sizeof(T) * SegmentCapacity];
      mozilla::AlignedElem<MOZ_ALIGNOF(T)> mAlign;
    } mStorage;

    static_assert(MOZ_ALIGNOF(T) == MOZ_ALIGNOF(Storage),
                  "SegmentedVector provides incorrect alignment");
  };

  // See how many we elements we can fit in a segment of IdealSegmentSize. If
  // IdealSegmentSize is too small, it'll be just one. The +1 is because
  // kSingleElementSegmentSize already accounts for one element.
  static const size_t kSingleElementSegmentSize = sizeof(SegmentImpl<1>);
  static const size_t kSegmentCapacity =
    kSingleElementSegmentSize <= IdealSegmentSize
    ? (IdealSegmentSize - kSingleElementSegmentSize) / sizeof(T) + 1
    : 1;

public:
  typedef SegmentImpl<kSegmentCapacity> Segment;

  // The |aIdealSegmentSize| is only for sanity checking. If it's specified, we
  // check that the actual segment size is as close as possible to it. This
  // serves as a sanity check for SegmentedVectorCapacity's capacity
  // computation.
  explicit SegmentedVector(size_t aIdealSegmentSize = 0)
  {
    // The difference between the actual segment size and the ideal segment
    // size should be less than the size of a single element... unless the
    // ideal size was too small, in which case the capacity should be one.
    MOZ_ASSERT_IF(
      aIdealSegmentSize != 0,
      (sizeof(Segment) > aIdealSegmentSize && kSegmentCapacity == 1) ||
      aIdealSegmentSize - sizeof(Segment) < sizeof(T));
  }

  SegmentedVector(SegmentedVector&& aOther)
    : mSegments(mozilla::Move(aOther.mSegments))
  {
  }

  ~SegmentedVector() { Clear(); }

  bool IsEmpty() const { return !mSegments.getFirst(); }

  // Note that this is O(n) rather than O(1), but the constant factor is very
  // small because it only has to do one addition per segment.
  size_t Length() const
  {
    size_t n = 0;
    for (auto segment = mSegments.getFirst();
         segment;
         segment = segment->getNext()) {
      n += segment->Length();
    }
    return n;
  }

  // Returns false if the allocation failed. (If you are using an infallible
  // allocation policy, use InfallibleAppend() instead.)
  template<typename U>
  MOZ_MUST_USE bool Append(U&& aU)
  {
    Segment* last = mSegments.getLast();
    if (!last || last->Length() == kSegmentCapacity) {
      last = this->template pod_malloc<Segment>(1);
      if (!last) {
        return false;
      }
      new (last) Segment();
      mSegments.insertBack(last);
    }
    last->Append(mozilla::Forward<U>(aU));
    return true;
  }

  // You should probably only use this instead of Append() if you are using an
  // infallible allocation policy. It will crash if the allocation fails.
  template<typename U>
  void InfallibleAppend(U&& aU)
  {
    bool ok = Append(mozilla::Forward<U>(aU));
    MOZ_RELEASE_ASSERT(ok);
  }

  void Clear()
  {
    Segment* segment;
    while ((segment = mSegments.popFirst())) {
      segment->~Segment();
      this->free_(segment);
    }
  }

  T& GetLast()
  {
    MOZ_ASSERT(!IsEmpty());
    Segment* last = mSegments.getLast();
    return (*last)[last->Length() - 1];
  }

  const T& GetLast() const
  {
    MOZ_ASSERT(!IsEmpty());
    Segment* last = mSegments.getLast();
    return (*last)[last->Length() - 1];
  }

  void PopLast()
  {
    MOZ_ASSERT(!IsEmpty());
    Segment* last = mSegments.getLast();
    last->PopLast();
    if (!last->Length()) {
      mSegments.popLast();
      last->~Segment();
      this->free_(last);
    }
  }

  // Equivalent to calling |PopLast| |aNumElements| times, but potentially
  // more efficient.
  void PopLastN(uint32_t aNumElements)
  {
    MOZ_ASSERT(aNumElements <= Length());

    Segment* last;

    // Pop full segments for as long as we can.  Note that this loop
    // cleanly handles the case when the initial last segment is not
    // full and we are popping more elements than said segment contains.
    do {
      last = mSegments.getLast();

      // The list is empty.  We're all done.
      if (!last) {
        return;
      }

      // Check to see if the list contains too many elements.  Handle
      // that in the epilogue.
      uint32_t segmentLen = last->Length();
      if (segmentLen > aNumElements) {
        break;
      }

      // Destroying the segment destroys all elements contained therein.
      mSegments.popLast();
      last->~Segment();
      this->free_(last);

      MOZ_ASSERT(aNumElements >= segmentLen);
      aNumElements -= segmentLen;
      if (aNumElements == 0) {
        return;
      }
    } while (true);

    // Handle the case where the last segment contains more elements
    // than we want to pop.
    MOZ_ASSERT(last);
    MOZ_ASSERT(last == mSegments.getLast());
    MOZ_ASSERT(aNumElements < last->Length());
    for (uint32_t i = 0; i < aNumElements; ++i) {
      last->PopLast();
    }
    MOZ_ASSERT(last->Length() != 0);
  }

  // Use this class to iterate over a SegmentedVector, like so:
  //
  //  for (auto iter = v.Iter(); !iter.Done(); iter.Next()) {
  //    MyElem& elem = iter.Get();
  //    f(elem);
  //  }
  //
  // Note, adding new entries to the SegmentedVector while using iterators
  // is supported, but removing is not!
  // If an iterator has entered Done() state, adding more entries to the
  // vector doesn't affect it.
  class IterImpl
  {
    friend class SegmentedVector;

    Segment* mSegment;
    size_t mIndex;

    explicit IterImpl(SegmentedVector* aVector, bool aFromFirst)
      : mSegment(aFromFirst ? aVector->mSegments.getFirst() :
                              aVector->mSegments.getLast())
      , mIndex(aFromFirst ? 0 :
                            (mSegment ? mSegment->Length() - 1 : 0))
    {
      MOZ_ASSERT_IF(mSegment, mSegment->Length() > 0);
    }

  public:
    bool Done() const { return !mSegment; }

    T& Get()
    {
      MOZ_ASSERT(!Done());
      return (*mSegment)[mIndex];
    }

    const T& Get() const
    {
      MOZ_ASSERT(!Done());
      return (*mSegment)[mIndex];
    }

    void Next()
    {
      MOZ_ASSERT(!Done());
      mIndex++;
      if (mIndex == mSegment->Length()) {
        mSegment = mSegment->getNext();
        mIndex = 0;
      }
    }

    void Prev()
    {
      MOZ_ASSERT(!Done());
      if (mIndex == 0) {
        mSegment = mSegment->getPrevious();
        if (mSegment) {
          mIndex = mSegment->Length() - 1;
        }
      } else {
        --mIndex;
      }
    }
  };

  IterImpl Iter() { return IterImpl(this, true); }
  IterImpl IterFromLast() { return IterImpl(this, false); }

  // Measure the memory consumption of the vector excluding |this|. Note that
  // it only measures the vector itself. If the vector elements contain
  // pointers to other memory blocks, those blocks must be measured separately
  // during a subsequent iteration over the vector.
  size_t SizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
  {
    return mSegments.sizeOfExcludingThis(aMallocSizeOf);
  }

  // Like sizeOfExcludingThis(), but measures |this| as well.
  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const
  {
    return aMallocSizeOf(this) + SizeOfExcludingThis(aMallocSizeOf);
  }

private:
  mozilla::LinkedList<Segment> mSegments;
};

} // namespace mozilla

#endif /* mozilla_SegmentedVector_h */

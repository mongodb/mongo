/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_BufferList_h
#define mozilla_BufferList_h

#include <algorithm>
#include "mozilla/AllocPolicy.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Types.h"
#include "mozilla/TypeTraits.h"
#include "mozilla/Vector.h"
#include <string.h>

// BufferList represents a sequence of buffers of data. A BufferList can choose
// to own its buffers or not. The class handles writing to the buffers,
// iterating over them, and reading data out. Unlike SegmentedVector, the
// buffers may be of unequal size. Like SegmentedVector, BufferList is a nice
// way to avoid large contiguous allocations (which can trigger OOMs).

class InfallibleAllocPolicy;

namespace mozilla {

template<typename AllocPolicy>
class BufferList : private AllocPolicy
{
  // Each buffer in a BufferList has a size and a capacity. The first mSize
  // bytes are initialized and the remaining |mCapacity - mSize| bytes are free.
  struct Segment
  {
    char* mData;
    size_t mSize;
    size_t mCapacity;

    Segment(char* aData, size_t aSize, size_t aCapacity)
     : mData(aData),
       mSize(aSize),
       mCapacity(aCapacity)
    {
    }

    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    Segment(Segment&&) = default;
    Segment& operator=(Segment&&) = default;

    char* Start() const { return mData; }
    char* End() const { return mData + mSize; }
  };

  template<typename OtherAllocPolicy>
  friend class BufferList;

 public:
  // For the convenience of callers, all segments are required to be a multiple
  // of 8 bytes in capacity. Also, every buffer except the last one is required
  // to be full (i.e., size == capacity). Therefore, a byte at offset N within
  // the BufferList and stored in memory at an address A will satisfy
  // (N % Align == A % Align) if Align == 2, 4, or 8.
  static const size_t kSegmentAlignment = 8;

  // Allocate a BufferList. The BufferList will free all its buffers when it is
  // destroyed. If an infallible allocator is used, an initial buffer of size
  // aInitialSize and capacity aInitialCapacity is allocated automatically. This
  // data will be contiguous and can be accessed via |Start()|. If a fallible
  // alloc policy is used, aInitialSize must be 0, and the fallible |Init()|
  // method may be called instead. Subsequent buffers will be allocated with
  // capacity aStandardCapacity.
  BufferList(size_t aInitialSize,
             size_t aInitialCapacity,
             size_t aStandardCapacity,
             AllocPolicy aAP = AllocPolicy())
   : AllocPolicy(aAP),
     mOwning(true),
     mSegments(aAP),
     mSize(0),
     mStandardCapacity(aStandardCapacity)
  {
    MOZ_ASSERT(aInitialCapacity % kSegmentAlignment == 0);
    MOZ_ASSERT(aStandardCapacity % kSegmentAlignment == 0);

    if (aInitialCapacity) {
      MOZ_ASSERT((aInitialSize == 0 || IsSame<AllocPolicy, InfallibleAllocPolicy>::value),
                 "BufferList may only be constructed with an initial size when "
                 "using an infallible alloc policy");

      AllocateSegment(aInitialSize, aInitialCapacity);
    }
  }

  BufferList(const BufferList& aOther) = delete;

  BufferList(BufferList&& aOther)
   : mOwning(aOther.mOwning),
     mSegments(Move(aOther.mSegments)),
     mSize(aOther.mSize),
     mStandardCapacity(aOther.mStandardCapacity)
  {
    aOther.mSegments.clear();
    aOther.mSize = 0;
  }

  BufferList& operator=(const BufferList& aOther) = delete;

  BufferList& operator=(BufferList&& aOther)
  {
    Clear();

    mOwning = aOther.mOwning;
    mSegments = Move(aOther.mSegments);
    mSize = aOther.mSize;
    aOther.mSegments.clear();
    aOther.mSize = 0;
    return *this;
  }

  ~BufferList() { Clear(); }

  // Initializes the BufferList with a segment of the given size and capacity.
  // May only be called once, before any segments have been allocated.
  bool Init(size_t aInitialSize, size_t aInitialCapacity)
  {
    MOZ_ASSERT(mSegments.empty());
    MOZ_ASSERT(aInitialCapacity != 0);
    MOZ_ASSERT(aInitialCapacity % kSegmentAlignment == 0);

    return AllocateSegment(aInitialSize, aInitialCapacity);
  }

  // Returns the sum of the sizes of all the buffers.
  size_t Size() const { return mSize; }

  size_t SizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf)
  {
    size_t size = mSegments.sizeOfExcludingThis(aMallocSizeOf);
    for (Segment& segment : mSegments) {
      size += aMallocSizeOf(segment.Start());
    }
    return size;
  }

  void Clear()
  {
    if (mOwning) {
      for (Segment& segment : mSegments) {
        this->free_(segment.mData);
      }
    }
    mSegments.clear();

    mSize = 0;
  }

  // Iterates over bytes in the segments. You can advance it by as many bytes as
  // you choose.
  class IterImpl
  {
    // Invariants:
    //   (0) mSegment <= bufferList.mSegments.length()
    //   (1) mData <= mDataEnd
    //   (2) If mSegment is not the last segment, mData < mDataEnd
    uintptr_t mSegment;
    char* mData;
    char* mDataEnd;

    friend class BufferList;

  public:
    explicit IterImpl(const BufferList& aBuffers)
     : mSegment(0),
       mData(nullptr),
       mDataEnd(nullptr)
    {
      if (!aBuffers.mSegments.empty()) {
        mData = aBuffers.mSegments[0].Start();
        mDataEnd = aBuffers.mSegments[0].End();
      }
    }

    // Returns a pointer to the raw data. It is valid to access up to
    // RemainingInSegment bytes of this buffer.
    char* Data() const
    {
      MOZ_RELEASE_ASSERT(!Done());
      return mData;
    }

    // Returns true if the memory in the range [Data(), Data() + aBytes) is all
    // part of one contiguous buffer.
    bool HasRoomFor(size_t aBytes) const
    {
      MOZ_RELEASE_ASSERT(mData <= mDataEnd);
      return size_t(mDataEnd - mData) >= aBytes;
    }

    // Returns the maximum value aBytes for which HasRoomFor(aBytes) will be
    // true.
    size_t RemainingInSegment() const
    {
      MOZ_RELEASE_ASSERT(mData <= mDataEnd);
      return mDataEnd - mData;
    }

    // Advances the iterator by aBytes bytes. aBytes must be less than
    // RemainingInSegment(). If advancing by aBytes takes the iterator to the
    // end of a buffer, it will be moved to the beginning of the next buffer
    // unless it is the last buffer.
    void Advance(const BufferList& aBuffers, size_t aBytes)
    {
      const Segment& segment = aBuffers.mSegments[mSegment];
      MOZ_RELEASE_ASSERT(segment.Start() <= mData);
      MOZ_RELEASE_ASSERT(mData <= mDataEnd);
      MOZ_RELEASE_ASSERT(mDataEnd == segment.End());

      MOZ_RELEASE_ASSERT(HasRoomFor(aBytes));
      mData += aBytes;

      if (mData == mDataEnd && mSegment + 1 < aBuffers.mSegments.length()) {
        mSegment++;
        const Segment& nextSegment = aBuffers.mSegments[mSegment];
        mData = nextSegment.Start();
        mDataEnd = nextSegment.End();
        MOZ_RELEASE_ASSERT(mData < mDataEnd);
      }
    }

    // Advance the iterator by aBytes, possibly crossing segments. This function
    // returns false if it runs out of buffers to advance through. Otherwise it
    // returns true.
    bool AdvanceAcrossSegments(const BufferList& aBuffers, size_t aBytes)
    {
      size_t bytes = aBytes;
      while (bytes) {
        size_t toAdvance = std::min(bytes, RemainingInSegment());
        if (!toAdvance) {
          return false;
        }
        Advance(aBuffers, toAdvance);
        bytes -= toAdvance;
      }
      return true;
    }

    // Returns true when the iterator reaches the end of the BufferList.
    bool Done() const
    {
      return mData == mDataEnd;
    }

   private:

    // Count the bytes we would need to advance in order to reach aTarget.
    size_t BytesUntil(const BufferList& aBuffers, const IterImpl& aTarget) const {
      size_t offset = 0;

      MOZ_ASSERT(aTarget.IsIn(aBuffers));

      char* data = mData;
      for (uintptr_t segment = mSegment; segment < aTarget.mSegment; segment++) {
        offset += aBuffers.mSegments[segment].End() - data;
        data = aBuffers.mSegments[segment].mData;
      }

      MOZ_RELEASE_ASSERT(IsIn(aBuffers));
      MOZ_RELEASE_ASSERT(aTarget.mData >= data);

      offset += aTarget.mData - data;
      return offset;
    }

    bool IsIn(const BufferList& aBuffers) const {
      return mSegment < aBuffers.mSegments.length() &&
             mData >= aBuffers.mSegments[mSegment].mData &&
             mData < aBuffers.mSegments[mSegment].End();
    }
  };

  // Special convenience method that returns Iter().Data().
  char* Start()
  {
    MOZ_RELEASE_ASSERT(!mSegments.empty());
    return mSegments[0].mData;
  }
  const char* Start() const { return mSegments[0].mData; }

  IterImpl Iter() const { return IterImpl(*this); }

  // Copies aSize bytes from aData into the BufferList. The storage for these
  // bytes may be split across multiple buffers. Size() is increased by aSize.
  inline bool WriteBytes(const char* aData, size_t aSize);

  // Allocates a buffer of at most |aMaxBytes| bytes and, if successful, returns
  // that buffer, and places its size in |aSize|. If unsuccessful, returns null
  // and leaves |aSize| undefined.
  inline char* AllocateBytes(size_t aMaxSize, size_t* aSize);

  // Copies possibly non-contiguous byte range starting at aIter into
  // aData. aIter is advanced by aSize bytes. Returns false if it runs out of
  // data before aSize.
  inline bool ReadBytes(IterImpl& aIter, char* aData, size_t aSize) const;

  // Return a new BufferList that shares storage with this BufferList. The new
  // BufferList is read-only. It allows iteration over aSize bytes starting at
  // aIter. Borrow can fail, in which case *aSuccess will be false upon
  // return. The borrowed BufferList can use a different AllocPolicy than the
  // original one. However, it is not responsible for freeing buffers, so the
  // AllocPolicy is only used for the buffer vector.
  template<typename BorrowingAllocPolicy>
  BufferList<BorrowingAllocPolicy> Borrow(IterImpl& aIter, size_t aSize, bool* aSuccess,
                                          BorrowingAllocPolicy aAP = BorrowingAllocPolicy()) const;

  // Return a new BufferList and move storage from this BufferList to it. The
  // new BufferList owns the buffers. Move can fail, in which case *aSuccess
  // will be false upon return. The new BufferList can use a different
  // AllocPolicy than the original one. The new OtherAllocPolicy is responsible
  // for freeing buffers, so the OtherAllocPolicy must use freeing method
  // compatible to the original one.
  template<typename OtherAllocPolicy>
  BufferList<OtherAllocPolicy> MoveFallible(bool* aSuccess, OtherAllocPolicy aAP = OtherAllocPolicy());

  // Return a new BufferList that adopts the byte range starting at Iter so that
  // range [aIter, aIter + aSize) is transplanted to the returned BufferList.
  // Contents of the buffer before aIter + aSize is left undefined.
  // Extract can fail, in which case *aSuccess will be false upon return. The
  // moved buffers are erased from the original BufferList. In case of extract
  // fails, the original BufferList is intact.  All other iterators except aIter
  // are invalidated.
  // This method requires aIter and aSize to be 8-byte aligned.
  BufferList Extract(IterImpl& aIter, size_t aSize, bool* aSuccess);

  // Return the number of bytes from 'start' to 'end', two iterators within
  // this BufferList.
  size_t RangeLength(const IterImpl& start, const IterImpl& end) const {
    MOZ_ASSERT(start.IsIn(*this) && end.IsIn(*this));
    return start.BytesUntil(*this, end);
  }

  // This takes ownership of the data
  void* WriteBytesZeroCopy(char *aData, size_t aSize, size_t aCapacity)
  {
    MOZ_ASSERT(aCapacity != 0);
    MOZ_ASSERT(aSize <= aCapacity);
    MOZ_ASSERT(mOwning);

    if (!mSegments.append(Segment(aData, aSize, aCapacity))) {
      this->free_(aData);
      return nullptr;
    }
    mSize += aSize;
    return aData;
  }

private:
  explicit BufferList(AllocPolicy aAP)
   : AllocPolicy(aAP),
     mOwning(false),
     mSize(0),
     mStandardCapacity(0)
  {
  }

  char* AllocateSegment(size_t aSize, size_t aCapacity)
  {
    MOZ_RELEASE_ASSERT(mOwning);
    MOZ_ASSERT(aCapacity != 0);
    MOZ_ASSERT(aSize <= aCapacity);

    char* data = this->template pod_malloc<char>(aCapacity);
    if (!data) {
      return nullptr;
    }
    if (!mSegments.append(Segment(data, aSize, aCapacity))) {
      this->free_(data);
      return nullptr;
    }
    mSize += aSize;
    return data;
  }

  bool mOwning;
  Vector<Segment, 1, AllocPolicy> mSegments;
  size_t mSize;
  size_t mStandardCapacity;
};

template<typename AllocPolicy>
bool
BufferList<AllocPolicy>::WriteBytes(const char* aData, size_t aSize)
{
  MOZ_RELEASE_ASSERT(mOwning);
  MOZ_RELEASE_ASSERT(mStandardCapacity);

  size_t copied = 0;
  while (copied < aSize) {
    size_t toCopy;
    char* data = AllocateBytes(aSize - copied, &toCopy);
    if (!data) {
      return false;
    }
    memcpy(data, aData + copied, toCopy);
    copied += toCopy;
  }

  return true;
}

template<typename AllocPolicy>
char*
BufferList<AllocPolicy>::AllocateBytes(size_t aMaxSize, size_t* aSize)
{
  MOZ_RELEASE_ASSERT(mOwning);
  MOZ_RELEASE_ASSERT(mStandardCapacity);

  if (!mSegments.empty()) {
    Segment& lastSegment = mSegments.back();

    size_t capacity = lastSegment.mCapacity - lastSegment.mSize;
    if (capacity) {
      size_t size = std::min(aMaxSize, capacity);
      char* data = lastSegment.mData + lastSegment.mSize;

      lastSegment.mSize += size;
      mSize += size;

      *aSize = size;
      return data;
    }
  }

  size_t size = std::min(aMaxSize, mStandardCapacity);
  char* data = AllocateSegment(size, mStandardCapacity);
  if (data) {
    *aSize = size;
  }
  return data;
}

template<typename AllocPolicy>
bool
BufferList<AllocPolicy>::ReadBytes(IterImpl& aIter, char* aData, size_t aSize) const
{
  size_t copied = 0;
  size_t remaining = aSize;
  while (remaining) {
    size_t toCopy = std::min(aIter.RemainingInSegment(), remaining);
    if (!toCopy) {
      // We've run out of data in the last segment.
      return false;
    }
    memcpy(aData + copied, aIter.Data(), toCopy);
    copied += toCopy;
    remaining -= toCopy;

    aIter.Advance(*this, toCopy);
  }

  return true;
}

template<typename AllocPolicy> template<typename BorrowingAllocPolicy>
BufferList<BorrowingAllocPolicy>
BufferList<AllocPolicy>::Borrow(IterImpl& aIter, size_t aSize, bool* aSuccess,
                                BorrowingAllocPolicy aAP) const
{
  BufferList<BorrowingAllocPolicy> result(aAP);

  size_t size = aSize;
  while (size) {
    size_t toAdvance = std::min(size, aIter.RemainingInSegment());

    if (!toAdvance || !result.mSegments.append(typename BufferList<BorrowingAllocPolicy>::Segment(aIter.mData, toAdvance, toAdvance))) {
      *aSuccess = false;
      return result;
    }
    aIter.Advance(*this, toAdvance);
    size -= toAdvance;
  }

  result.mSize = aSize;
  *aSuccess = true;
  return result;
}

template<typename AllocPolicy> template<typename OtherAllocPolicy>
BufferList<OtherAllocPolicy>
BufferList<AllocPolicy>::MoveFallible(bool* aSuccess, OtherAllocPolicy aAP)
{
  BufferList<OtherAllocPolicy> result(0, 0, mStandardCapacity, aAP);

  IterImpl iter = Iter();
  while (!iter.Done()) {
    size_t toAdvance = iter.RemainingInSegment();

    if (!toAdvance || !result.mSegments.append(typename BufferList<OtherAllocPolicy>::Segment(iter.mData, toAdvance, toAdvance))) {
      *aSuccess = false;
      result.mSegments.clear();
      return result;
    }
    iter.Advance(*this, toAdvance);
  }

  result.mSize = mSize;
  mSegments.clear();
  mSize = 0;
  *aSuccess = true;
  return result;
}

template<typename AllocPolicy>
BufferList<AllocPolicy>
BufferList<AllocPolicy>::Extract(IterImpl& aIter, size_t aSize, bool* aSuccess)
{
  MOZ_RELEASE_ASSERT(aSize);
  MOZ_RELEASE_ASSERT(mOwning);
  MOZ_ASSERT(aSize % kSegmentAlignment == 0);
  MOZ_ASSERT(intptr_t(aIter.mData) % kSegmentAlignment == 0);

  auto failure = [this, aSuccess]() {
    *aSuccess = false;
    return BufferList(0, 0, mStandardCapacity);
  };

  // Number of segments we'll need to copy data from to satisfy the request.
  size_t segmentsNeeded = 0;
  // If this is None then the last segment is a full segment, otherwise we need
  // to copy this many bytes.
  Maybe<size_t> lastSegmentSize;
  {
    // Copy of the iterator to walk the BufferList and see how many segments we
    // need to copy.
    IterImpl iter = aIter;
    size_t remaining = aSize;
    while (!iter.Done() && remaining &&
           remaining >= iter.RemainingInSegment()) {
      remaining -= iter.RemainingInSegment();
      iter.Advance(*this, iter.RemainingInSegment());
      segmentsNeeded++;
    }

    if (remaining) {
      if (iter.Done()) {
        // We reached the end of the BufferList and there wasn't enough data to
        // satisfy the request.
        return failure();
      }
      lastSegmentSize.emplace(remaining);
      // The last block also counts as a segment. This makes the conditionals
      // on segmentsNeeded work in the rest of the function.
      segmentsNeeded++;
    }
  }

  BufferList result(0, 0, mStandardCapacity);
  if (!result.mSegments.reserve(segmentsNeeded + lastSegmentSize.isSome())) {
    return failure();
  }

  // Copy the first segment, it's special because we can't just steal the
  // entire Segment struct from this->mSegments.
  size_t firstSegmentSize = std::min(aSize, aIter.RemainingInSegment());
  if (!result.WriteBytes(aIter.Data(), firstSegmentSize)) {
    return failure();
  }
  aIter.Advance(*this, firstSegmentSize);
  segmentsNeeded--;

  // The entirety of the request wasn't in the first segment, now copy the
  // rest.
  if (segmentsNeeded) {
    char* finalSegment = nullptr;
    // Pre-allocate the final segment so that if this fails, we return before
    // we delete the elements from |this->mSegments|.
    if (lastSegmentSize.isSome()) {
      MOZ_RELEASE_ASSERT(mStandardCapacity >= *lastSegmentSize);
      finalSegment = this->template pod_malloc<char>(mStandardCapacity);
      if (!finalSegment) {
        return failure();
      }
    }

    size_t copyStart = aIter.mSegment;
    // Copy segments from this over to the result and remove them from our
    // storage. Not needed if the only segment we need to copy is the last
    // partial one.
    size_t segmentsToCopy = segmentsNeeded - lastSegmentSize.isSome();
    for (size_t i = 0; i < segmentsToCopy; ++i) {
      result.mSegments.infallibleAppend(
        Segment(mSegments[aIter.mSegment].mData,
                mSegments[aIter.mSegment].mSize,
                mSegments[aIter.mSegment].mCapacity));
      aIter.Advance(*this, aIter.RemainingInSegment());
    }
    // Due to the way IterImpl works, there are two cases here: (1) if we've
    // consumed the entirety of the BufferList, then the iterator is pointed at
    // the end of the final segment, (2) otherwise it is pointed at the start
    // of the next segment. We want to verify that we really consumed all
    // |segmentsToCopy| segments.
    MOZ_RELEASE_ASSERT(
      (aIter.mSegment == copyStart + segmentsToCopy) ||
      (aIter.Done() && aIter.mSegment == copyStart + segmentsToCopy - 1));
    mSegments.erase(mSegments.begin() + copyStart,
                    mSegments.begin() + copyStart + segmentsToCopy);

    // Reset the iter's position for what we just deleted.
    aIter.mSegment -= segmentsToCopy;

    if (lastSegmentSize.isSome()) {
      // We called reserve() on result.mSegments so infallibleAppend is safe.
      result.mSegments.infallibleAppend(
        Segment(finalSegment, 0, mStandardCapacity));
      bool r = result.WriteBytes(aIter.Data(), *lastSegmentSize);
      MOZ_RELEASE_ASSERT(r);
      aIter.Advance(*this, *lastSegmentSize);
    }
  }

  mSize -= aSize;
  result.mSize = aSize;

  *aSuccess = true;
  return result;
}

} // namespace mozilla

#endif /* mozilla_BufferList_h */

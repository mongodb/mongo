/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_BufferList_h
#define mozilla_BufferList_h

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Vector.h"

// BufferList represents a sequence of buffers of data. A BufferList can choose
// to own its buffers or not. The class handles writing to the buffers,
// iterating over them, and reading data out. Unlike SegmentedVector, the
// buffers may be of unequal size. Like SegmentedVector, BufferList is a nice
// way to avoid large contiguous allocations (which can trigger OOMs).

class InfallibleAllocPolicy;

namespace mozilla {

template <typename AllocPolicy>
class BufferList : private AllocPolicy {
  // Each buffer in a BufferList has a size and a capacity. The first mSize
  // bytes are initialized and the remaining |mCapacity - mSize| bytes are free.
  struct Segment {
    char* mData;
    size_t mSize;
    size_t mCapacity;

    Segment(char* aData, size_t aSize, size_t aCapacity)
        : mData(aData), mSize(aSize), mCapacity(aCapacity) {}

    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    Segment(Segment&&) = default;
    Segment& operator=(Segment&&) = default;

    char* Start() const { return mData; }
    char* End() const { return mData + mSize; }
  };

  template <typename OtherAllocPolicy>
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
  BufferList(size_t aInitialSize, size_t aInitialCapacity,
             size_t aStandardCapacity, AllocPolicy aAP = AllocPolicy())
      : AllocPolicy(aAP),
        mOwning(true),
        mSegments(aAP),
        mSize(0),
        mStandardCapacity(aStandardCapacity) {
    MOZ_ASSERT(aInitialCapacity % kSegmentAlignment == 0);
    MOZ_ASSERT(aStandardCapacity % kSegmentAlignment == 0);

    if (aInitialCapacity) {
      MOZ_ASSERT((aInitialSize == 0 ||
                  std::is_same_v<AllocPolicy, InfallibleAllocPolicy>),
                 "BufferList may only be constructed with an initial size when "
                 "using an infallible alloc policy");

      AllocateSegment(aInitialSize, aInitialCapacity);
    }
  }

  BufferList(const BufferList& aOther) = delete;

  BufferList(BufferList&& aOther)
      : mOwning(aOther.mOwning),
        mSegments(std::move(aOther.mSegments)),
        mSize(aOther.mSize),
        mStandardCapacity(aOther.mStandardCapacity) {
    aOther.mSegments.clear();
    aOther.mSize = 0;
  }

  BufferList& operator=(const BufferList& aOther) = delete;

  BufferList& operator=(BufferList&& aOther) {
    Clear();

    mOwning = aOther.mOwning;
    mSegments = std::move(aOther.mSegments);
    mSize = aOther.mSize;
    aOther.mSegments.clear();
    aOther.mSize = 0;
    return *this;
  }

  ~BufferList() { Clear(); }

  // Initializes the BufferList with a segment of the given size and capacity.
  // May only be called once, before any segments have been allocated.
  bool Init(size_t aInitialSize, size_t aInitialCapacity) {
    MOZ_ASSERT(mSegments.empty());
    MOZ_ASSERT(aInitialCapacity != 0);
    MOZ_ASSERT(aInitialCapacity % kSegmentAlignment == 0);

    return AllocateSegment(aInitialSize, aInitialCapacity);
  }

  bool CopyFrom(const BufferList& aOther) {
    MOZ_ASSERT(mOwning);

    Clear();

    // We don't make an exact copy of aOther. Instead, create a single segment
    // with enough space to hold all data in aOther.
    if (!Init(aOther.mSize, (aOther.mSize + kSegmentAlignment - 1) &
                                ~(kSegmentAlignment - 1))) {
      return false;
    }

    size_t offset = 0;
    for (const Segment& segment : aOther.mSegments) {
      memcpy(Start() + offset, segment.mData, segment.mSize);
      offset += segment.mSize;
    }
    MOZ_ASSERT(offset == mSize);

    return true;
  }

  // Returns the sum of the sizes of all the buffers.
  size_t Size() const { return mSize; }

  size_t SizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf) {
    size_t size = mSegments.sizeOfExcludingThis(aMallocSizeOf);
    for (Segment& segment : mSegments) {
      size += aMallocSizeOf(segment.Start());
    }
    return size;
  }

  void Clear() {
    if (mOwning) {
      for (Segment& segment : mSegments) {
        this->free_(segment.mData, segment.mCapacity);
      }
    }
    mSegments.clear();

    mSize = 0;
  }

  // Iterates over bytes in the segments. You can advance it by as many bytes as
  // you choose.
  class IterImpl {
    // Invariants:
    //   (0) mSegment <= bufferList.mSegments.length()
    //   (1) mData <= mDataEnd
    //   (2) If mSegment is not the last segment, mData < mDataEnd
    uintptr_t mSegment{0};
    char* mData{nullptr};
    char* mDataEnd{nullptr};
    size_t mAbsoluteOffset{0};

    friend class BufferList;

   public:
    explicit IterImpl(const BufferList& aBuffers) {
      if (!aBuffers.mSegments.empty()) {
        mData = aBuffers.mSegments[0].Start();
        mDataEnd = aBuffers.mSegments[0].End();
      }
    }

    // Returns a pointer to the raw data. It is valid to access up to
    // RemainingInSegment bytes of this buffer.
    char* Data() const {
      MOZ_RELEASE_ASSERT(!Done());
      return mData;
    }

    bool operator==(const IterImpl& other) const {
      return mAbsoluteOffset == other.mAbsoluteOffset;
    }
    bool operator!=(const IterImpl& other) const { return !(*this == other); }

    // Returns true if the memory in the range [Data(), Data() + aBytes) is all
    // part of one contiguous buffer.
    bool HasRoomFor(size_t aBytes) const {
      return RemainingInSegment() >= aBytes;
    }

    // Returns the largest value aBytes for which HasRoomFor(aBytes) will be
    // true.
    size_t RemainingInSegment() const {
      MOZ_RELEASE_ASSERT(mData <= mDataEnd);
      return mDataEnd - mData;
    }

    // Returns true if there are at least aBytes entries remaining in the
    // BufferList after this iterator.
    bool HasBytesAvailable(const BufferList& aBuffers, size_t aBytes) const {
      return TotalBytesAvailable(aBuffers) >= aBytes;
    }

    // Returns the largest value `aBytes` for which HasBytesAvailable(aBytes)
    // will be true.
    size_t TotalBytesAvailable(const BufferList& aBuffers) const {
      return aBuffers.mSize - mAbsoluteOffset;
    }

    // Advances the iterator by aBytes bytes. aBytes must be less than
    // RemainingInSegment(). If advancing by aBytes takes the iterator to the
    // end of a buffer, it will be moved to the beginning of the next buffer
    // unless it is the last buffer.
    void Advance(const BufferList& aBuffers, size_t aBytes) {
      const Segment& segment = aBuffers.mSegments[mSegment];
      MOZ_RELEASE_ASSERT(segment.Start() <= mData);
      MOZ_RELEASE_ASSERT(mData <= mDataEnd);
      MOZ_RELEASE_ASSERT(mDataEnd == segment.End());

      MOZ_RELEASE_ASSERT(HasRoomFor(aBytes));
      mData += aBytes;
      mAbsoluteOffset += aBytes;

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
    bool AdvanceAcrossSegments(const BufferList& aBuffers, size_t aBytes) {
      // If we don't need to cross segments, we can directly use `Advance` to
      // get to our destination.
      if (MOZ_LIKELY(aBytes <= RemainingInSegment())) {
        Advance(aBuffers, aBytes);
        return true;
      }

      // Check if we have enough bytes to scan this far forward.
      if (!HasBytesAvailable(aBuffers, aBytes)) {
        return false;
      }

      // Compare the distance to our target offset from the end of the
      // BufferList to the distance from the start of our next segment.
      // Depending on which is closer, we'll advance either forwards or
      // backwards.
      size_t targetOffset = mAbsoluteOffset + aBytes;
      size_t fromEnd = aBuffers.mSize - targetOffset;
      if (aBytes - RemainingInSegment() < fromEnd) {
        // Advance through the buffer list until we reach the desired absolute
        // offset.
        while (mAbsoluteOffset < targetOffset) {
          Advance(aBuffers, std::min(targetOffset - mAbsoluteOffset,
                                     RemainingInSegment()));
        }
        MOZ_ASSERT(mAbsoluteOffset == targetOffset);
        return true;
      }

      // Scanning starting from the end of the BufferList. We advance
      // backwards from the final segment until we find the segment to end in.
      //
      // If we end on a segment boundary, make sure to place the cursor at the
      // beginning of the next segment.
      mSegment = aBuffers.mSegments.length() - 1;
      while (fromEnd > aBuffers.mSegments[mSegment].mSize) {
        fromEnd -= aBuffers.mSegments[mSegment].mSize;
        mSegment--;
      }
      mDataEnd = aBuffers.mSegments[mSegment].End();
      mData = mDataEnd - fromEnd;
      mAbsoluteOffset = targetOffset;
      MOZ_ASSERT_IF(Done(), mSegment == aBuffers.mSegments.length() - 1);
      MOZ_ASSERT_IF(Done(), mAbsoluteOffset == aBuffers.mSize);
      return true;
    }

    // Returns true when the iterator reaches the end of the BufferList.
    bool Done() const { return mData == mDataEnd; }

    // The absolute offset of this iterator within the BufferList.
    size_t AbsoluteOffset() const { return mAbsoluteOffset; }

   private:
    bool IsIn(const BufferList& aBuffers) const {
      return mSegment < aBuffers.mSegments.length() &&
             mData >= aBuffers.mSegments[mSegment].mData &&
             mData < aBuffers.mSegments[mSegment].End();
    }
  };

  // Special convenience method that returns Iter().Data().
  char* Start() {
    MOZ_RELEASE_ASSERT(!mSegments.empty());
    return mSegments[0].mData;
  }
  const char* Start() const { return mSegments[0].mData; }

  IterImpl Iter() const { return IterImpl(*this); }

  // Copies aSize bytes from aData into the BufferList. The storage for these
  // bytes may be split across multiple buffers. Size() is increased by aSize.
  [[nodiscard]] inline bool WriteBytes(const char* aData, size_t aSize);

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
  template <typename BorrowingAllocPolicy>
  BufferList<BorrowingAllocPolicy> Borrow(
      IterImpl& aIter, size_t aSize, bool* aSuccess,
      BorrowingAllocPolicy aAP = BorrowingAllocPolicy()) const;

  // Return a new BufferList and move storage from this BufferList to it. The
  // new BufferList owns the buffers. Move can fail, in which case *aSuccess
  // will be false upon return. The new BufferList can use a different
  // AllocPolicy than the original one. The new OtherAllocPolicy is responsible
  // for freeing buffers, so the OtherAllocPolicy must use freeing method
  // compatible to the original one.
  template <typename OtherAllocPolicy>
  BufferList<OtherAllocPolicy> MoveFallible(
      bool* aSuccess, OtherAllocPolicy aAP = OtherAllocPolicy());

  // Return the number of bytes from 'start' to 'end', two iterators within
  // this BufferList.
  size_t RangeLength(const IterImpl& start, const IterImpl& end) const {
    MOZ_ASSERT(start.IsIn(*this) && end.IsIn(*this));
    return end.mAbsoluteOffset - start.mAbsoluteOffset;
  }

  // This takes ownership of the data
  [[nodiscard]] bool WriteBytesZeroCopy(char* aData, size_t aSize,
                                        size_t aCapacity) {
    MOZ_ASSERT(mOwning);
    MOZ_ASSERT(aSize <= aCapacity);

    // Don't create zero-length segments; that can cause problems for
    // consumers of the data (bug 1595453).
    if (aSize == 0) {
      this->free_(aData, aCapacity);
      return true;
    }

    if (!mSegments.append(Segment(aData, aSize, aCapacity))) {
      this->free_(aData, aCapacity);
      return false;
    }
    mSize += aSize;
    return true;
  }

  // Truncate this BufferList at the given iterator location, discarding all
  // data after this point. After this call, all other iterators will be
  // invalidated, and the passed-in iterator will be "Done".
  //
  // Returns the number of bytes discarded by this truncation.
  size_t Truncate(IterImpl& aIter);

 private:
  explicit BufferList(AllocPolicy aAP)
      : AllocPolicy(aAP), mOwning(false), mSize(0), mStandardCapacity(0) {}

  char* AllocateSegment(size_t aSize, size_t aCapacity) {
    MOZ_RELEASE_ASSERT(mOwning);
    MOZ_ASSERT(aCapacity != 0);
    MOZ_ASSERT(aSize <= aCapacity);

    char* data = this->template pod_malloc<char>(aCapacity);
    if (!data) {
      return nullptr;
    }
    if (!mSegments.append(Segment(data, aSize, aCapacity))) {
      this->free_(data, aCapacity);
      return nullptr;
    }
    mSize += aSize;
    return data;
  }

  void AssertConsistentSize() const {
#ifdef DEBUG
    size_t realSize = 0;
    for (const auto& segment : mSegments) {
      realSize += segment.mSize;
    }
    MOZ_ASSERT(realSize == mSize, "cached size value is inconsistent!");
#endif
  }

  bool mOwning;
  Vector<Segment, 1, AllocPolicy> mSegments;
  size_t mSize;
  size_t mStandardCapacity;
};

template <typename AllocPolicy>
[[nodiscard]] bool BufferList<AllocPolicy>::WriteBytes(const char* aData,
                                                       size_t aSize) {
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

template <typename AllocPolicy>
char* BufferList<AllocPolicy>::AllocateBytes(size_t aMaxSize, size_t* aSize) {
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

template <typename AllocPolicy>
bool BufferList<AllocPolicy>::ReadBytes(IterImpl& aIter, char* aData,
                                        size_t aSize) const {
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

template <typename AllocPolicy>
template <typename BorrowingAllocPolicy>
BufferList<BorrowingAllocPolicy> BufferList<AllocPolicy>::Borrow(
    IterImpl& aIter, size_t aSize, bool* aSuccess,
    BorrowingAllocPolicy aAP) const {
  BufferList<BorrowingAllocPolicy> result(aAP);

  size_t size = aSize;
  while (size) {
    size_t toAdvance = std::min(size, aIter.RemainingInSegment());

    if (!toAdvance || !result.mSegments.append(
                          typename BufferList<BorrowingAllocPolicy>::Segment(
                              aIter.mData, toAdvance, toAdvance))) {
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

template <typename AllocPolicy>
template <typename OtherAllocPolicy>
BufferList<OtherAllocPolicy> BufferList<AllocPolicy>::MoveFallible(
    bool* aSuccess, OtherAllocPolicy aAP) {
  BufferList<OtherAllocPolicy> result(0, 0, mStandardCapacity, aAP);

  IterImpl iter = Iter();
  while (!iter.Done()) {
    size_t toAdvance = iter.RemainingInSegment();

    if (!toAdvance ||
        !result.mSegments.append(typename BufferList<OtherAllocPolicy>::Segment(
            iter.mData, toAdvance, toAdvance))) {
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

template <typename AllocPolicy>
size_t BufferList<AllocPolicy>::Truncate(IterImpl& aIter) {
  MOZ_ASSERT(aIter.IsIn(*this) || aIter.Done());
  if (aIter.Done()) {
    return 0;
  }

  size_t prevSize = mSize;

  // Remove any segments after the iterator's current segment.
  while (mSegments.length() > aIter.mSegment + 1) {
    Segment& toFree = mSegments.back();
    mSize -= toFree.mSize;
    if (mOwning) {
      this->free_(toFree.mData, toFree.mCapacity);
    }
    mSegments.popBack();
  }

  // The last segment is now aIter's current segment. Truncate or remove it.
  Segment& seg = mSegments.back();
  MOZ_ASSERT(aIter.mDataEnd == seg.End());
  mSize -= aIter.RemainingInSegment();
  seg.mSize -= aIter.RemainingInSegment();
  if (!seg.mSize) {
    if (mOwning) {
      this->free_(seg.mData, seg.mCapacity);
    }
    mSegments.popBack();
  }

  // Correct `aIter` to point to the new end of the BufferList.
  if (mSegments.empty()) {
    MOZ_ASSERT(mSize == 0);
    aIter.mSegment = 0;
    aIter.mData = aIter.mDataEnd = nullptr;
  } else {
    aIter.mSegment = mSegments.length() - 1;
    aIter.mData = aIter.mDataEnd = mSegments.back().End();
  }
  MOZ_ASSERT(aIter.Done());

  AssertConsistentSize();
  return prevSize - mSize;
}

}  // namespace mozilla

#endif /* mozilla_BufferList_h */

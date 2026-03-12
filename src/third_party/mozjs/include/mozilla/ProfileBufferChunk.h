/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileBufferChunk_h
#define ProfileBufferChunk_h

#include "mozilla/MemoryReporting.h"
#include "mozilla/ProfileBufferIndex.h"
#include "mozilla/Span.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/UniquePtr.h"

#if defined(MOZ_MEMORY)
#  include "mozmemory.h"
#endif

#include <algorithm>
#include <limits>
#include <type_traits>

#ifdef DEBUG
#  include <cstdio>
#endif

namespace mozilla {

// Represents a single chunk of memory, with a link to the next chunk (or null).
//
// A chunk is made of an internal header (which contains a public part) followed
// by user-accessible bytes.
//
// +---------------+---------+----------------------------------------------+
// | public Header | private |         memory containing user blocks        |
// +---------------+---------+----------------------------------------------+
//                           <---------------BufferBytes()------------------>
// <------------------------------ChunkBytes()------------------------------>
//
// The chunk can reserve "blocks", but doesn't know the internal contents of
// each block, it only knows where the first one starts, and where the last one
// ends (which is where the next one will begin, if not already out of range).
// It is up to the user to add structure to each block so that they can be
// distinguished when later read.
//
// +---------------+---------+----------------------------------------------+
// | public Header | private |      [1st block]...[last full block]         |
// +---------------+---------+----------------------------------------------+
//  ChunkHeader().mOffsetFirstBlock ^                             ^
//                           ChunkHeader().mOffsetPastLastBlock --'
//
// It is possible to attempt to reserve more than the remaining space, in which
// case only what is available is returned. The caller is responsible for using
// another chunk, reserving a block "tail" in it, and using both parts to
// constitute a full block. (This initial tail may be empty in some chunks.)
//
// +---------------+---------+----------------------------------------------+
// | public Header | private | tail][1st block]...[last full block][head... |
// +---------------+---------+----------------------------------------------+
//  ChunkHeader().mOffsetFirstBlock ^                                       ^
//                                     ChunkHeader().mOffsetPastLastBlock --'
//
// Each Chunk has an internal state (checked in DEBUG builds) that directs how
// to use it during creation, initialization, use, end of life, recycling, and
// destruction. See `State` below for details.
// In particular:
// - `ReserveInitialBlockAsTail()` must be called before the first `Reserve()`
//   after construction or recycling, even with a size of 0 (no actual tail),
// - `MarkDone()` and `MarkRecycled()` must be called as appropriate.
class ProfileBufferChunk {
 public:
  using Byte = uint8_t;
  using Length = uint32_t;

  using SpanOfBytes = Span<Byte>;

  // Hint about the size of the metadata (public and private headers).
  // `Create()` below takes the minimum *buffer* size, so the minimum total
  // Chunk size is at least `SizeofChunkMetadata() + aMinBufferBytes`.
  [[nodiscard]] static constexpr Length SizeofChunkMetadata() {
    return static_cast<Length>(sizeof(InternalHeader));
  }

  // Allocate space for a chunk with a given minimum size, and construct it.
  // The actual size may be higher, to match the actual space taken in the
  // memory pool.
  [[nodiscard]] static UniquePtr<ProfileBufferChunk> Create(
      Length aMinBufferBytes) {
    // We need at least one byte, to cover the always-present `mBuffer` byte.
    aMinBufferBytes = std::max(aMinBufferBytes, Length(1));
    // Trivial struct with the same alignment as `ProfileBufferChunk`, and size
    // equal to that alignment, because typically the sizeof of an object is
    // a multiple of its alignment.
    struct alignas(alignof(InternalHeader)) ChunkStruct {
      Byte c[alignof(InternalHeader)];
    };
    static_assert(std::is_trivial_v<ChunkStruct>,
                  "ChunkStruct must be trivial to avoid any construction");
    // Allocate an array of that struct, enough to contain the expected
    // `ProfileBufferChunk` (with its header+buffer).
    size_t count = (sizeof(InternalHeader) + aMinBufferBytes +
                    (alignof(InternalHeader) - 1)) /
                   alignof(InternalHeader);
#if defined(MOZ_MEMORY)
    // Potentially expand the array to use more of the effective allocation.
    count = (malloc_good_size(count * sizeof(ChunkStruct)) +
             (sizeof(ChunkStruct) - 1)) /
            sizeof(ChunkStruct);
#endif
    auto chunkStorage = MakeUnique<ChunkStruct[]>(count);
    MOZ_ASSERT(reinterpret_cast<uintptr_t>(chunkStorage.get()) %
                   alignof(InternalHeader) ==
               0);
    // After the allocation, compute the actual chunk size (including header).
    const size_t chunkBytes = count * sizeof(ChunkStruct);
    MOZ_ASSERT(chunkBytes >= sizeof(ProfileBufferChunk),
               "Not enough space to construct a ProfileBufferChunk");
    MOZ_ASSERT(chunkBytes <=
               static_cast<size_t>(std::numeric_limits<Length>::max()));
    // Compute the size of the user-accessible buffer inside the chunk.
    const Length bufferBytes =
        static_cast<Length>(chunkBytes - sizeof(InternalHeader));
    MOZ_ASSERT(bufferBytes >= aMinBufferBytes,
               "Not enough space for minimum buffer size");
    // Construct the header at the beginning of the allocated array, with the
    // known buffer size.
    new (chunkStorage.get()) ProfileBufferChunk(bufferBytes);
    // We now have a proper `ProfileBufferChunk` object, create the appropriate
    // UniquePtr for it.
    UniquePtr<ProfileBufferChunk> chunk{
        reinterpret_cast<ProfileBufferChunk*>(chunkStorage.release())};
    MOZ_ASSERT(
        size_t(reinterpret_cast<const char*>(
                   &chunk.get()->BufferSpan()[bufferBytes - 1]) -
               reinterpret_cast<const char*>(chunk.get())) == chunkBytes - 1,
        "Buffer span spills out of chunk allocation");
    return chunk;
  }

#ifdef DEBUG
  ~ProfileBufferChunk() {
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::InUse);
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Full);
    MOZ_ASSERT(mInternalHeader.mState == InternalHeader::State::Created ||
               mInternalHeader.mState == InternalHeader::State::Done ||
               mInternalHeader.mState == InternalHeader::State::Recycled);
  }
#endif

  // Must be called with the first block tail (may be empty), which will be
  // skipped if the reader starts with this ProfileBufferChunk.
  [[nodiscard]] SpanOfBytes ReserveInitialBlockAsTail(Length aTailSize) {
#ifdef DEBUG
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::InUse);
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Full);
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Done);
    MOZ_ASSERT(mInternalHeader.mState == InternalHeader::State::Created ||
               mInternalHeader.mState == InternalHeader::State::Recycled);
    mInternalHeader.mState = InternalHeader::State::InUse;
#endif
    mInternalHeader.mHeader.mOffsetFirstBlock = aTailSize;
    mInternalHeader.mHeader.mOffsetPastLastBlock = aTailSize;
    mInternalHeader.mHeader.mStartTimeStamp = TimeStamp::Now();
    return SpanOfBytes(&mBuffer, aTailSize);
  }

  struct ReserveReturn {
    SpanOfBytes mSpan;
    ProfileBufferBlockIndex mBlockRangeIndex;
  };

  // Reserve a block of up to `aBlockSize` bytes, and return a Span to it, and
  // its starting index. The actual size may be smaller, if the block cannot fit
  // in the remaining space.
  [[nodiscard]] ReserveReturn ReserveBlock(Length aBlockSize) {
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Created);
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Full);
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Done);
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Recycled);
    MOZ_ASSERT(mInternalHeader.mState == InternalHeader::State::InUse);
    MOZ_ASSERT(RangeStart() != 0,
               "Expected valid range start before first Reserve()");
    const Length blockOffset = mInternalHeader.mHeader.mOffsetPastLastBlock;
    Length reservedSize = aBlockSize;
    if (MOZ_UNLIKELY(aBlockSize >= RemainingBytes())) {
      reservedSize = RemainingBytes();
#ifdef DEBUG
      mInternalHeader.mState = InternalHeader::State::Full;
#endif
    }
    mInternalHeader.mHeader.mOffsetPastLastBlock += reservedSize;
    mInternalHeader.mHeader.mBlockCount += 1;
    return {SpanOfBytes(&mBuffer + blockOffset, reservedSize),
            ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
                mInternalHeader.mHeader.mRangeStart + blockOffset)};
  }

  // When a chunk will not be used to store more blocks (because it is full, or
  // because the profiler will not add more data), it should be marked "done".
  // Access to its content is still allowed.
  void MarkDone() {
#ifdef DEBUG
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Created);
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Done);
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Recycled);
    MOZ_ASSERT(mInternalHeader.mState == InternalHeader::State::InUse ||
               mInternalHeader.mState == InternalHeader::State::Full);
    mInternalHeader.mState = InternalHeader::State::Done;
#endif
    mInternalHeader.mHeader.mDoneTimeStamp = TimeStamp::Now();
  }

  // A "Done" chunk may be recycled, to avoid allocating a new one.
  void MarkRecycled() {
#ifdef DEBUG
    // We also allow Created and already-Recycled chunks to be recycled, this
    // way it's easier to recycle chunks when their state is not easily
    // trackable.
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::InUse);
    MOZ_ASSERT(mInternalHeader.mState != InternalHeader::State::Full);
    MOZ_ASSERT(mInternalHeader.mState == InternalHeader::State::Created ||
               mInternalHeader.mState == InternalHeader::State::Done ||
               mInternalHeader.mState == InternalHeader::State::Recycled);
    mInternalHeader.mState = InternalHeader::State::Recycled;
#endif
    // Reset all header fields, in case this recycled chunk gets read.
    mInternalHeader.mHeader.Reset();
  }

  // Public header, meant to uniquely identify a chunk, it may be shared with
  // other processes to coordinate global memory handling.
  struct Header {
    explicit Header(Length aBufferBytes) : mBufferBytes(aBufferBytes) {}

    // Reset all members to their as-new values (apart from the buffer size,
    // which cannot change), ready for re-use.
    void Reset() {
      mOffsetFirstBlock = 0;
      mOffsetPastLastBlock = 0;
      mStartTimeStamp = TimeStamp{};
      mDoneTimeStamp = TimeStamp{};
      mBlockCount = 0;
      mRangeStart = 0;
      mProcessId = 0;
    }

    // Note: Part of the ordering of members below is to avoid unnecessary
    // padding.

    // Members managed by the ProfileBufferChunk.

    // Offset of the first block (past the initial tail block, which may be 0).
    Length mOffsetFirstBlock = 0;
    // Offset past the last byte of the last reserved block
    // It may be past mBufferBytes when last block continues in the next
    // ProfileBufferChunk. It may be before mBufferBytes if ProfileBufferChunk
    // is marked "Done" before the end is reached.
    Length mOffsetPastLastBlock = 0;
    // Timestamp when the buffer becomes in-use, ready to record data.
    TimeStamp mStartTimeStamp;
    // Timestamp when the buffer is "Done" (which happens when the last block is
    // written). This will be used to find and discard the oldest
    // ProfileBufferChunk.
    TimeStamp mDoneTimeStamp;
    // Number of bytes in the buffer, set once at construction time.
    const Length mBufferBytes;
    // Number of reserved blocks (including final one even if partial, but
    // excluding initial tail).
    Length mBlockCount = 0;

    // Meta-data set by the user.

    // Index of the first byte of this ProfileBufferChunk, relative to all
    // Chunks for this process. Index 0 is reserved as nullptr-like index,
    // mRangeStart should be set to a non-0 value before the first `Reserve()`.
    ProfileBufferIndex mRangeStart = 0;
    // Process writing to this ProfileBufferChunk.
    int mProcessId = 0;

    // A bit of spare space (necessary here because of the alignment due to
    // other members), may be later repurposed for extra data.
    const int mPADDING = 0;
  };

  [[nodiscard]] const Header& ChunkHeader() const {
    return mInternalHeader.mHeader;
  }

  [[nodiscard]] Length BufferBytes() const {
    return ChunkHeader().mBufferBytes;
  }

  // Total size of the chunk (buffer + header).
  [[nodiscard]] Length ChunkBytes() const {
    return static_cast<Length>(sizeof(InternalHeader)) + BufferBytes();
  }

  // Size of external resources, in this case all the following chunks.
  [[nodiscard]] size_t SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const {
    const ProfileBufferChunk* const next = GetNext();
    return next ? next->SizeOfIncludingThis(aMallocSizeOf) : 0;
  }

  // Size of this chunk and all following ones.
  [[nodiscard]] size_t SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const {
    // Just in case `aMallocSizeOf` falls back on just `sizeof`, make sure we
    // account for at least the actual Chunk requested allocation size.
    return std::max<size_t>(aMallocSizeOf(this), ChunkBytes()) +
           SizeOfExcludingThis(aMallocSizeOf);
  }

  [[nodiscard]] Length RemainingBytes() const {
    return BufferBytes() - OffsetPastLastBlock();
  }

  [[nodiscard]] Length OffsetFirstBlock() const {
    return ChunkHeader().mOffsetFirstBlock;
  }

  [[nodiscard]] Length OffsetPastLastBlock() const {
    return ChunkHeader().mOffsetPastLastBlock;
  }

  [[nodiscard]] Length BlockCount() const { return ChunkHeader().mBlockCount; }

  [[nodiscard]] int ProcessId() const { return ChunkHeader().mProcessId; }

  void SetProcessId(int aProcessId) {
    mInternalHeader.mHeader.mProcessId = aProcessId;
  }

  // Global range index at the start of this Chunk.
  [[nodiscard]] ProfileBufferIndex RangeStart() const {
    return ChunkHeader().mRangeStart;
  }

  void SetRangeStart(ProfileBufferIndex aRangeStart) {
    mInternalHeader.mHeader.mRangeStart = aRangeStart;
  }

  // Get a read-only Span to the buffer. It is up to the caller to decypher the
  // contents, based on known offsets and the internal block structure.
  [[nodiscard]] Span<const Byte> BufferSpan() const {
    return Span<const Byte>(&mBuffer, BufferBytes());
  }

  [[nodiscard]] Byte ByteAt(Length aOffset) const {
    MOZ_ASSERT(aOffset < OffsetPastLastBlock());
    return *(&mBuffer + aOffset);
  }

  [[nodiscard]] ProfileBufferChunk* GetNext() {
    return mInternalHeader.mNext.get();
  }
  [[nodiscard]] const ProfileBufferChunk* GetNext() const {
    return mInternalHeader.mNext.get();
  }

  [[nodiscard]] UniquePtr<ProfileBufferChunk> ReleaseNext() {
    return std::move(mInternalHeader.mNext);
  }

  void InsertNext(UniquePtr<ProfileBufferChunk>&& aChunk) {
    if (!aChunk) {
      return;
    }
    aChunk->SetLast(ReleaseNext());
    mInternalHeader.mNext = std::move(aChunk);
  }

  // Find the last chunk in this chain (it may be `this`).
  [[nodiscard]] ProfileBufferChunk* Last() {
    ProfileBufferChunk* chunk = this;
    for (;;) {
      ProfileBufferChunk* next = chunk->GetNext();
      if (!next) {
        return chunk;
      }
      chunk = next;
    }
  }
  [[nodiscard]] const ProfileBufferChunk* Last() const {
    const ProfileBufferChunk* chunk = this;
    for (;;) {
      const ProfileBufferChunk* next = chunk->GetNext();
      if (!next) {
        return chunk;
      }
      chunk = next;
    }
  }

  void SetLast(UniquePtr<ProfileBufferChunk>&& aChunk) {
    if (!aChunk) {
      return;
    }
    Last()->mInternalHeader.mNext = std::move(aChunk);
  }

  // Join two possibly-null chunk lists.
  [[nodiscard]] static UniquePtr<ProfileBufferChunk> Join(
      UniquePtr<ProfileBufferChunk>&& aFirst,
      UniquePtr<ProfileBufferChunk>&& aLast) {
    if (aFirst) {
      aFirst->SetLast(std::move(aLast));
      return std::move(aFirst);
    }
    return std::move(aLast);
  }

#ifdef DEBUG
  void Dump(std::FILE* aFile = stdout) const {
    fprintf(aFile,
            "Chunk[%p] chunkSize=%u bufferSize=%u state=%s rangeStart=%u "
            "firstBlockOffset=%u offsetPastLastBlock=%u blockCount=%u",
            this, unsigned(ChunkBytes()), unsigned(BufferBytes()),
            mInternalHeader.StateString(), unsigned(RangeStart()),
            unsigned(OffsetFirstBlock()), unsigned(OffsetPastLastBlock()),
            unsigned(BlockCount()));
    const auto len = OffsetPastLastBlock();
    constexpr unsigned columns = 16;
    unsigned char ascii[columns + 1];
    ascii[columns] = '\0';
    for (Length i = 0; i < len; ++i) {
      if (i % columns == 0) {
        fprintf(aFile, "\n  %4u=0x%03x:", unsigned(i), unsigned(i));
        for (unsigned a = 0; a < columns; ++a) {
          ascii[a] = ' ';
        }
      }
      unsigned char sep = ' ';
      if (i == OffsetFirstBlock()) {
        if (i == OffsetPastLastBlock()) {
          sep = '#';
        } else {
          sep = '[';
        }
      } else if (i == OffsetPastLastBlock()) {
        sep = ']';
      }
      unsigned char c = *(&mBuffer + i);
      fprintf(aFile, "%c%02x", sep, c);

      if (i == len - 1) {
        if (i + 1 == OffsetPastLastBlock()) {
          // Special case when last block ends right at the end.
          fprintf(aFile, "]");
        } else {
          fprintf(aFile, " ");
        }
      } else if (i % columns == columns - 1) {
        fprintf(aFile, " ");
      }

      ascii[i % columns] = (c >= ' ' && c <= '~') ? c : '.';

      if (i % columns == columns - 1) {
        fprintf(aFile, " %s", ascii);
      }
    }

    if (len % columns < columns - 1) {
      for (Length i = len % columns; i < columns; ++i) {
        fprintf(aFile, "   ");
      }
      fprintf(aFile, " %s", ascii);
    }

    fprintf(aFile, "\n");
  }
#endif  // DEBUG

 private:
  // ProfileBufferChunk constructor. Use static `Create()` to allocate and
  // construct a ProfileBufferChunk.
  explicit ProfileBufferChunk(Length aBufferBytes)
      : mInternalHeader(aBufferBytes) {}

  // This internal header starts with the public `Header`, and adds some data
  // only necessary for local handling.
  // This encapsulation is also necessary to perform placement-new in
  // `Create()`.
  struct InternalHeader {
    explicit InternalHeader(Length aBufferBytes) : mHeader(aBufferBytes) {}

    Header mHeader;
    UniquePtr<ProfileBufferChunk> mNext;

#ifdef DEBUG
    enum class State {
      Created,  // Self-set. Just constructed, waiting for initial block tail.
      InUse,    // Ready to accept blocks.
      Full,     // Self-set. Blocks reach the end (or further).
      Done,     // Blocks won't be added anymore.
      Recycled  // Still full of data, but expecting an initial block tail.
    };

    State mState = State::Created;
    // Transition table: (X=unexpected)
    // Method          \  State   Created  InUse    Full     Done     Recycled
    // ReserveInitialBlockAsTail   InUse     X       X        X        InUse
    // Reserve                       X   InUse/Full  X        X          X
    // MarkDone                      X     Done     Done      X          X
    // MarkRecycled                  X       X       X      Recycled     X
    // destructor                    ok      X       X        ok         ok

    const char* StateString() const {
      switch (mState) {
        case State::Created:
          return "Created";
        case State::InUse:
          return "InUse";
        case State::Full:
          return "Full";
        case State::Done:
          return "Done";
        case State::Recycled:
          return "Recycled";
        default:
          return "?";
      }
    }
#else  // DEBUG
    const char* StateString() const { return "(non-DEBUG)"; }
#endif
  };

  InternalHeader mInternalHeader;

  // KEEP THIS LAST!
  // First byte of the buffer. Note that ProfileBufferChunk::Create allocates a
  // bigger block, such that `mBuffer` is the first of `mBufferBytes` available
  // bytes.
  // The initialization is not strictly needed, because bytes should only be
  // read after they have been written and `mOffsetPastLastBlock` has been
  // updated. However:
  // - Reviewbot complains that it's not initialized.
  // - It's cheap to initialize one byte.
  // - In the worst case (reading does happen), zero is not a valid entry size
  //   and should get caught in entry readers.
  Byte mBuffer = '\0';
};

}  // namespace mozilla

#endif  // ProfileBufferChunk_h

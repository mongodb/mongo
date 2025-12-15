/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileChunkedBufferDetail_h
#define ProfileChunkedBufferDetail_h

#include "mozilla/Assertions.h"
#include "mozilla/Likely.h"
#include "mozilla/ProfileBufferChunk.h"
#include "mozilla/ProfileBufferEntrySerialization.h"

namespace mozilla::profiler::detail {

// Internal accessor pointing at a position inside a chunk.
// It can handle two groups of chunks (typically the extant chunks stored in
// the store manager, and the current chunk).
// The main operations are:
// - ReadEntrySize() to read an entry size, 0 means failure.
// - operator+=(Length) to skip a number of bytes.
// - EntryReader() creates an entry reader at the current position for a given
//   size (it may fail with an empty reader), and skips the entry.
// Note that there is no "past-the-end" position -- as soon as InChunkPointer
// reaches the end, it becomes effectively null.
class InChunkPointer {
 public:
  using Byte = ProfileBufferChunk::Byte;
  using Length = ProfileBufferChunk::Length;

  // Nullptr-like InChunkPointer, may be used as end iterator.
  InChunkPointer()
      : mChunk(nullptr), mNextChunkGroup(nullptr), mOffsetInChunk(0) {}

  // InChunkPointer over one or two chunk groups, pointing at the given
  // block index (if still in range).
  // This constructor should only be used with *trusted* block index values!
  InChunkPointer(const ProfileBufferChunk* aChunk,
                 const ProfileBufferChunk* aNextChunkGroup,
                 ProfileBufferBlockIndex aBlockIndex)
      : mChunk(aChunk), mNextChunkGroup(aNextChunkGroup) {
    if (mChunk) {
      mOffsetInChunk = mChunk->OffsetFirstBlock();
      Adjust();
    } else if (mNextChunkGroup) {
      mChunk = mNextChunkGroup;
      mNextChunkGroup = nullptr;
      mOffsetInChunk = mChunk->OffsetFirstBlock();
      Adjust();
    } else {
      mOffsetInChunk = 0;
    }

    // Try to advance to given position.
    if (!AdvanceToGlobalRangePosition(aBlockIndex)) {
      // Block does not exist anymore (or block doesn't look valid), reset the
      // in-chunk pointer.
      mChunk = nullptr;
      mNextChunkGroup = nullptr;
    }
  }

  // InChunkPointer over one or two chunk groups, will start at the first
  // block (if any). This may be slow, so avoid using it too much.
  InChunkPointer(const ProfileBufferChunk* aChunk,
                 const ProfileBufferChunk* aNextChunkGroup,
                 ProfileBufferIndex aIndex = ProfileBufferIndex(0))
      : mChunk(aChunk), mNextChunkGroup(aNextChunkGroup) {
    if (mChunk) {
      mOffsetInChunk = mChunk->OffsetFirstBlock();
      Adjust();
    } else if (mNextChunkGroup) {
      mChunk = mNextChunkGroup;
      mNextChunkGroup = nullptr;
      mOffsetInChunk = mChunk->OffsetFirstBlock();
      Adjust();
    } else {
      mOffsetInChunk = 0;
    }

    // Try to advance to given position.
    if (!AdvanceToGlobalRangePosition(aIndex)) {
      // Block does not exist anymore, reset the in-chunk pointer.
      mChunk = nullptr;
      mNextChunkGroup = nullptr;
    }
  }

  // Compute the current position in the global range.
  // 0 if null (including if we're reached the end).
  [[nodiscard]] ProfileBufferIndex GlobalRangePosition() const {
    if (IsNull()) {
      return 0;
    }
    return mChunk->RangeStart() + mOffsetInChunk;
  }

  // Move InChunkPointer forward to the block at the given global block
  // position, which is assumed to be valid exactly -- but it may be obsolete.
  // 0 stays where it is (if valid already).
  // MOZ_ASSERTs if the index is invalid.
  [[nodiscard]] bool AdvanceToGlobalRangePosition(
      ProfileBufferBlockIndex aBlockIndex) {
    if (IsNull()) {
      // Pointer is null already. (Not asserting because it's acceptable.)
      return false;
    }
    if (!aBlockIndex) {
      // Special null position, just stay where we are.
      return ShouldPointAtValidBlock();
    }
    if (aBlockIndex.ConvertToProfileBufferIndex() < GlobalRangePosition()) {
      // Past the requested position, stay where we are (assuming the current
      // position was valid).
      return ShouldPointAtValidBlock();
    }
    for (;;) {
      if (aBlockIndex.ConvertToProfileBufferIndex() <
          mChunk->RangeStart() + mChunk->OffsetPastLastBlock()) {
        // Target position is in this chunk's written space, move to it.
        mOffsetInChunk =
            aBlockIndex.ConvertToProfileBufferIndex() - mChunk->RangeStart();
        return ShouldPointAtValidBlock();
      }
      // Position is after this chunk, try next chunk.
      GoToNextChunk();
      if (IsNull()) {
        return false;
      }
      // Skip whatever block tail there is, we don't allow pointing in the
      // middle of a block.
      mOffsetInChunk = mChunk->OffsetFirstBlock();
      if (aBlockIndex.ConvertToProfileBufferIndex() < GlobalRangePosition()) {
        // Past the requested position, meaning that the given position was in-
        // between blocks -> Failure.
        MOZ_ASSERT(false, "AdvanceToGlobalRangePosition - In-between blocks");
        return false;
      }
    }
  }

  // Move InChunkPointer forward to the block at or after the given global
  // range position.
  // 0 stays where it is (if valid already).
  [[nodiscard]] bool AdvanceToGlobalRangePosition(
      ProfileBufferIndex aPosition) {
    if (aPosition == 0) {
      // Special position '0', just stay where we are.
      // Success if this position is already valid.
      return !IsNull();
    }
    for (;;) {
      ProfileBufferIndex currentPosition = GlobalRangePosition();
      if (currentPosition == 0) {
        // Pointer is null.
        return false;
      }
      if (aPosition <= currentPosition) {
        // At or past the requested position, stay where we are.
        return true;
      }
      if (aPosition < mChunk->RangeStart() + mChunk->OffsetPastLastBlock()) {
        // Target position is in this chunk's written space, move to it.
        for (;;) {
          // Skip the current block.
          mOffsetInChunk += ReadEntrySize();
          if (mOffsetInChunk >= mChunk->OffsetPastLastBlock()) {
            // Reached the end of the chunk, this can happen for the last
            // block, let's just continue to the next chunk.
            break;
          }
          if (aPosition <= mChunk->RangeStart() + mOffsetInChunk) {
            // We're at or after the position, return at this block position.
            return true;
          }
        }
      }
      // Position is after this chunk, try next chunk.
      GoToNextChunk();
      if (IsNull()) {
        return false;
      }
      // Skip whatever block tail there is, we don't allow pointing in the
      // middle of a block.
      mOffsetInChunk = mChunk->OffsetFirstBlock();
    }
  }

  [[nodiscard]] Byte ReadByte() {
    MOZ_ASSERT(!IsNull());
    MOZ_ASSERT(mOffsetInChunk < mChunk->OffsetPastLastBlock());
    Byte byte = mChunk->ByteAt(mOffsetInChunk);
    if (MOZ_UNLIKELY(++mOffsetInChunk == mChunk->OffsetPastLastBlock())) {
      Adjust();
    }
    return byte;
  }

  // Read and skip a ULEB128-encoded size.
  // 0 means failure (0-byte entries are not allowed.)
  // Note that this doesn't guarantee that there are actually that many bytes
  // available to read! (EntryReader() below may gracefully fail.)
  [[nodiscard]] Length ReadEntrySize() {
    ULEB128Reader<Length> reader;
    if (IsNull()) {
      return 0;
    }
    for (;;) {
      const bool isComplete = reader.FeedByteIsComplete(ReadByte());
      if (MOZ_UNLIKELY(IsNull())) {
        // End of chunks, so there's no actual entry after this anyway.
        return 0;
      }
      if (MOZ_LIKELY(isComplete)) {
        if (MOZ_UNLIKELY(reader.Value() > mChunk->BufferBytes())) {
          // Don't allow entries larger than a chunk.
          return 0;
        }
        return reader.Value();
      }
    }
  }

  InChunkPointer& operator+=(Length aLength) {
    MOZ_ASSERT(!IsNull());
    mOffsetInChunk += aLength;
    Adjust();
    return *this;
  }

  [[nodiscard]] ProfileBufferEntryReader EntryReader(Length aLength) {
    if (IsNull() || aLength == 0) {
      return ProfileBufferEntryReader();
    }

    MOZ_ASSERT(mOffsetInChunk < mChunk->OffsetPastLastBlock());

    // We should be pointing at the entry, past the entry size.
    const ProfileBufferIndex entryIndex = GlobalRangePosition();
    // Verify that there's enough space before for the size (starting at index
    // 1 at least).
    MOZ_ASSERT(entryIndex >= 1u + ULEB128Size(aLength));

    const Length remaining = mChunk->OffsetPastLastBlock() - mOffsetInChunk;
    Span<const Byte> mem0 = mChunk->BufferSpan();
    mem0 = mem0.From(mOffsetInChunk);
    if (aLength <= remaining) {
      // Move to the end of this block, which could make this null if we have
      // reached the end of all buffers.
      *this += aLength;
      return ProfileBufferEntryReader(
          mem0.To(aLength),
          // Block starts before the entry size.
          ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
              entryIndex - ULEB128Size(aLength)),
          // Block ends right after the entry (could be null for last entry).
          ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
              GlobalRangePosition()));
    }

    // We need to go to the next chunk for the 2nd part of this block.
    GoToNextChunk();
    if (IsNull()) {
      return ProfileBufferEntryReader();
    }

    Span<const Byte> mem1 = mChunk->BufferSpan();
    const Length tail = aLength - remaining;
    MOZ_ASSERT(tail <= mChunk->BufferBytes());
    MOZ_ASSERT(tail == mChunk->OffsetFirstBlock());
    // We are in the correct chunk, move the offset to the end of the block.
    mOffsetInChunk = tail;
    // And adjust as needed, which could make this null if we have reached the
    // end of all buffers.
    Adjust();
    return ProfileBufferEntryReader(
        mem0, mem1.To(tail),
        // Block starts before the entry size.
        ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
            entryIndex - ULEB128Size(aLength)),
        // Block ends right after the entry (could be null for last entry).
        ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
            GlobalRangePosition()));
  }

  [[nodiscard]] bool IsNull() const { return !mChunk; }

  [[nodiscard]] bool operator==(const InChunkPointer& aOther) const {
    if (IsNull() || aOther.IsNull()) {
      return IsNull() && aOther.IsNull();
    }
    return mChunk == aOther.mChunk && mOffsetInChunk == aOther.mOffsetInChunk;
  }

  [[nodiscard]] bool operator!=(const InChunkPointer& aOther) const {
    return !(*this == aOther);
  }

  [[nodiscard]] Byte operator*() const {
    MOZ_ASSERT(!IsNull());
    MOZ_ASSERT(mOffsetInChunk < mChunk->OffsetPastLastBlock());
    return mChunk->ByteAt(mOffsetInChunk);
  }

  InChunkPointer& operator++() {
    MOZ_ASSERT(!IsNull());
    MOZ_ASSERT(mOffsetInChunk < mChunk->OffsetPastLastBlock());
    if (MOZ_UNLIKELY(++mOffsetInChunk == mChunk->OffsetPastLastBlock())) {
      mOffsetInChunk = 0;
      GoToNextChunk();
      Adjust();
    }
    return *this;
  }

 private:
  void GoToNextChunk() {
    MOZ_ASSERT(!IsNull());
    const ProfileBufferIndex expectedNextRangeStart =
        mChunk->RangeStart() + mChunk->BufferBytes();

    mChunk = mChunk->GetNext();
    if (!mChunk) {
      // Reached the end of the current chunk group, try the next one (which
      // may be null too, especially on the 2nd try).
      mChunk = mNextChunkGroup;
      mNextChunkGroup = nullptr;
    }

    if (mChunk && mChunk->RangeStart() == 0) {
      // Reached a chunk without a valid (non-null) range start, assume there
      // are only unused chunks from here on.
      mChunk = nullptr;
    }

    MOZ_ASSERT(!mChunk || mChunk->RangeStart() == expectedNextRangeStart,
               "We don't handle discontinuous buffers (yet)");
    // Non-DEBUG fallback: Stop reading past discontinuities.
    // (They should be rare, only happening on temporary OOMs.)
    // TODO: Handle discontinuities (by skipping over incomplete blocks).
    if (mChunk && mChunk->RangeStart() != expectedNextRangeStart) {
      mChunk = nullptr;
    }
  }

  // We want `InChunkPointer` to always point at a valid byte (or be null).
  // After some operations, `mOffsetInChunk` may point past the end of the
  // current `mChunk`, in which case we need to adjust our position to be inside
  // the appropriate chunk. E.g., if we're 10 bytes after the end of the current
  // chunk, we should end up at offset 10 in the next chunk.
  // Note that we may "fall off" the last chunk and make this `InChunkPointer`
  // effectively null.
  void Adjust() {
    while (mChunk && mOffsetInChunk >= mChunk->OffsetPastLastBlock()) {
      // TODO: Try to adjust offset between chunks relative to mRangeStart
      // differences. But we don't handle discontinuities yet.
      if (mOffsetInChunk < mChunk->BufferBytes()) {
        mOffsetInChunk -= mChunk->BufferBytes();
      } else {
        mOffsetInChunk -= mChunk->OffsetPastLastBlock();
      }
      GoToNextChunk();
    }
  }

  // Check if the current position is likely to point at a valid block.
  // (Size should be reasonable, and block should fully fit inside buffer.)
  // MOZ_ASSERTs on failure, to catch incorrect uses of block indices (which
  // should only point at valid blocks if still in range). Non-asserting build
  // fallback should still be handled.
  [[nodiscard]] bool ShouldPointAtValidBlock() const {
    if (IsNull()) {
      // Pointer is null, no blocks here.
      MOZ_ASSERT(false, "ShouldPointAtValidBlock - null pointer");
      return false;
    }
    // Use a copy, so we don't modify `*this`.
    InChunkPointer pointer = *this;
    // Try to read the entry size.
    Length entrySize = pointer.ReadEntrySize();
    if (entrySize == 0) {
      // Entry size of zero means we read 0 or a way-too-big value.
      MOZ_ASSERT(false, "ShouldPointAtValidBlock - invalid size");
      return false;
    }
    // See if the last byte of the entry is still inside the buffer.
    pointer += entrySize - 1;
    MOZ_ASSERT(!pointer.IsNull(),
               "ShouldPointAtValidBlock - past end of buffer");
    return !pointer.IsNull();
  }

  const ProfileBufferChunk* mChunk;
  const ProfileBufferChunk* mNextChunkGroup;
  Length mOffsetInChunk;
};

}  // namespace mozilla::profiler::detail

#endif  // ProfileChunkedBufferDetail_h

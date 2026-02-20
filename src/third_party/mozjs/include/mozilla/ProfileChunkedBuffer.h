/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileChunkedBuffer_h
#define ProfileChunkedBuffer_h

#include "mozilla/Attributes.h"
#include "mozilla/BaseProfilerDetail.h"
#include "mozilla/NotNull.h"
#include "mozilla/ProfileBufferChunkManager.h"
#include "mozilla/ProfileBufferChunkManagerSingle.h"
#include "mozilla/ProfileBufferEntrySerialization.h"
#include "mozilla/ProfileChunkedBufferDetail.h"
#include "mozilla/RefPtr.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Unused.h"

#include <utility>

#ifdef DEBUG
#  include <cstdio>
#endif

namespace mozilla {

// Thread-safe buffer that can store blocks of different sizes during defined
// sessions, using Chunks (from a ChunkManager) as storage.
//
// Each *block* contains an *entry* and the entry size:
// [ entry_size | entry ] [ entry_size | entry ] ...
//
// *In-session* is a period of time during which `ProfileChunkedBuffer` allows
// reading and writing.
// *Out-of-session*, the `ProfileChunkedBuffer` object is still valid, but
// contains no data, and gracefully denies accesses.
//
// To write an entry, the buffer reserves a block of sufficient size (to contain
// user data of predetermined size), writes the entry size, and lets the caller
// fill the entry contents using a ProfileBufferEntryWriter. E.g.:
// ```
// ProfileChunkedBuffer cb(...);
// cb.ReserveAndPut([]() { return sizeof(123); },
//                  [&](Maybe<ProfileBufferEntryWriter>& aEW) {
//                    if (aEW) { aEW->WriteObject(123); }
//                  });
// ```
// Other `Put...` functions may be used as shortcuts for simple entries.
// The objects given to the caller's callbacks should only be used inside the
// callbacks and not stored elsewhere, because they keep their own references to
// chunk memory and therefore should not live longer.
// Different type of objects may be serialized into an entry, see
// `ProfileBufferEntryWriter::Serializer` for more information.
//
// When reading data, the buffer iterates over blocks (it knows how to read the
// entry size, and therefore move to the next block), and lets the caller read
// the entry inside of each block. E.g.:
// ```
// cb.ReadEach([](ProfileBufferEntryReader& aER) {
//   /* Use ProfileBufferEntryReader functions to read serialized objects. */
//   int n = aER.ReadObject<int>();
// });
// ```
// Different type of objects may be deserialized from an entry, see
// `ProfileBufferEntryReader::Deserializer` for more information.
//
// Writers may retrieve the block index corresponding to an entry
// (`ProfileBufferBlockIndex` is an opaque type preventing the user from easily
// modifying it). That index may later be used with `ReadAt` to get back to the
// entry in that particular block -- if it still exists.
class ProfileChunkedBuffer {
 public:
  using Byte = ProfileBufferChunk::Byte;
  using Length = ProfileBufferChunk::Length;

  enum class ThreadSafety { WithoutMutex, WithMutex };

  // Default constructor starts out-of-session (nothing to read or write).
  explicit ProfileChunkedBuffer(ThreadSafety aThreadSafety)
      : mMutex(aThreadSafety != ThreadSafety::WithoutMutex) {}

  // Start in-session with external chunk manager.
  ProfileChunkedBuffer(ThreadSafety aThreadSafety,
                       ProfileBufferChunkManager& aChunkManager)
      : mMutex(aThreadSafety != ThreadSafety::WithoutMutex) {
    SetChunkManager(aChunkManager);
  }

  // Start in-session with owned chunk manager.
  ProfileChunkedBuffer(ThreadSafety aThreadSafety,
                       UniquePtr<ProfileBufferChunkManager>&& aChunkManager)
      : mMutex(aThreadSafety != ThreadSafety::WithoutMutex) {
    SetChunkManager(std::move(aChunkManager));
  }

  ~ProfileChunkedBuffer() {
    // Do proper clean-up by resetting the chunk manager.
    ResetChunkManager();
  }

  // This cannot change during the lifetime of this buffer, so there's no need
  // to lock.
  [[nodiscard]] bool IsThreadSafe() const { return mMutex.IsActivated(); }

  [[nodiscard]] bool IsInSession() const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    return !!mChunkManager;
  }

  // Stop using the current chunk manager.
  // If we own the current chunk manager, it will be destroyed.
  // This will always clear currently-held chunks, if any.
  void ResetChunkManager() {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    Unused << ResetChunkManager(lock);
  }

  // Set the current chunk manager.
  // The caller is responsible for keeping the chunk manager alive as along as
  // it's used here (until the next (Re)SetChunkManager, or
  // ~ProfileChunkedBuffer).
  void SetChunkManager(ProfileBufferChunkManager& aChunkManager) {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    Unused << ResetChunkManager(lock);
    SetChunkManager(aChunkManager, lock);
  }

  // Set the current chunk manager, and keep ownership of it.
  void SetChunkManager(UniquePtr<ProfileBufferChunkManager>&& aChunkManager) {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    Unused << ResetChunkManager(lock);
    mOwnedChunkManager = std::move(aChunkManager);
    if (mOwnedChunkManager) {
      SetChunkManager(*mOwnedChunkManager, lock);
    }
  }

  // Set the current chunk manager, except if it's already the one provided.
  // The caller is responsible for keeping the chunk manager alive as along as
  // it's used here (until the next (Re)SetChunkManager, or
  // ~ProfileChunkedBuffer).
  void SetChunkManagerIfDifferent(ProfileBufferChunkManager& aChunkManager) {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    if (!mChunkManager || mChunkManager != &aChunkManager) {
      Unused << ResetChunkManager(lock);
      SetChunkManager(aChunkManager, lock);
    }
  }

  // Clear the contents of this buffer, ready to receive new chunks.
  // Note that memory is not freed: No chunks are destroyed, they are all
  // receycled.
  // Also the range doesn't reset, instead it continues at some point after the
  // previous range. This may be useful if the caller may be keeping indexes
  // into old chunks that have now been cleared, using these indexes will fail
  // gracefully (instead of potentially pointing into new data).
  void Clear() {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    if (MOZ_UNLIKELY(!mChunkManager)) {
      // Out-of-session.
      return;
    }

    mRangeStart = mRangeEnd = mNextChunkRangeStart;
    mPushedBlockCount = 0;
    mClearedBlockCount = 0;
    mFailedPutBytes = 0;

    // Recycle all released chunks as "next" chunks. This will reduce the number
    // of future allocations. Also, when using ProfileBufferChunkManagerSingle,
    // this retrieves the one chunk if it was released.
    UniquePtr<ProfileBufferChunk> releasedChunks =
        mChunkManager->GetExtantReleasedChunks();
    if (releasedChunks) {
      // Released chunks should be in the "Done" state, they need to be marked
      // "recycled" before they can be reused.
      for (ProfileBufferChunk* chunk = releasedChunks.get(); chunk;
           chunk = chunk->GetNext()) {
        chunk->MarkRecycled();
      }
      mNextChunks = ProfileBufferChunk::Join(std::move(mNextChunks),
                                             std::move(releasedChunks));
    }

    if (mCurrentChunk) {
      // We already have a current chunk (empty or in-use), mark it "done" and
      // then "recycled", ready to be reused.
      mCurrentChunk->MarkDone();
      mCurrentChunk->MarkRecycled();
    } else {
      if (!mNextChunks) {
        // No current chunk, and no next chunks to recycle, nothing more to do.
        // The next "Put" operation will try to allocate a chunk as needed.
        return;
      }

      // No current chunk, take a next chunk.
      mCurrentChunk = std::exchange(mNextChunks, mNextChunks->ReleaseNext());
    }

    // Here, there was already a current chunk, or one has just been taken.
    // Make sure it's ready to receive new entries.
    InitializeCurrentChunk(lock);
  }

  // Buffer maximum length in bytes.
  Maybe<size_t> BufferLength() const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    if (!mChunkManager) {
      return Nothing{};
    }
    return Some(mChunkManager->MaxTotalSize());
  }

  [[nodiscard]] size_t SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    return SizeOfExcludingThis(aMallocSizeOf, lock);
  }

  [[nodiscard]] size_t SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    return aMallocSizeOf(this) + SizeOfExcludingThis(aMallocSizeOf, lock);
  }

  // Snapshot of the buffer state.
  struct State {
    // Index to/before the first block.
    ProfileBufferIndex mRangeStart = 1;

    // Index past the last block. Equals mRangeStart if empty.
    ProfileBufferIndex mRangeEnd = 1;

    // Number of blocks that have been pushed into this buffer.
    uint64_t mPushedBlockCount = 0;

    // Number of blocks that have been removed from this buffer.
    // Note: Live entries = pushed - cleared.
    uint64_t mClearedBlockCount = 0;

    // Number of bytes that could not be put into this buffer.
    uint64_t mFailedPutBytes = 0;
  };

  // Get a snapshot of the current state.
  // When out-of-session, mFirstReadIndex==mNextWriteIndex, and
  // mPushedBlockCount==mClearedBlockCount==0.
  // Note that these may change right after this thread-safe call, so they
  // should only be used for statistical purposes.
  [[nodiscard]] State GetState() const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    return {mRangeStart, mRangeEnd, mPushedBlockCount, mClearedBlockCount,
            mFailedPutBytes};
  }

  // In in-session, return the start TimeStamp of the earliest chunk.
  // If out-of-session, return a null TimeStamp.
  [[nodiscard]] TimeStamp GetEarliestChunkStartTimeStamp() const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    if (MOZ_UNLIKELY(!mChunkManager)) {
      // Out-of-session.
      return {};
    }
    return mChunkManager->PeekExtantReleasedChunks(
        [&](const ProfileBufferChunk* aOldestChunk) -> TimeStamp {
          if (aOldestChunk) {
            return aOldestChunk->ChunkHeader().mStartTimeStamp;
          }
          if (mCurrentChunk) {
            return mCurrentChunk->ChunkHeader().mStartTimeStamp;
          }
          return {};
        });
  }

  [[nodiscard]] bool IsEmpty() const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    return mRangeStart == mRangeEnd;
  }

  // True if this buffer is already locked on this thread.
  // This should be used if some functions may call an already-locked buffer,
  // e.g.: Put -> memory hook -> profiler_add_native_allocation_marker -> Put.
  [[nodiscard]] bool IsThreadSafeAndLockedOnCurrentThread() const {
    return mMutex.IsActivatedAndLockedOnCurrentThread();
  }

  // Lock the buffer mutex and run the provided callback.
  // This can be useful when the caller needs to explicitly lock down this
  // buffer, but not do anything else with it.
  template <typename Callback>
  auto LockAndRun(Callback&& aCallback) const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    return std::forward<Callback>(aCallback)();
  }

  // Reserve a block that can hold an entry of the given `aCallbackEntryBytes()`
  // size, write the entry size (ULEB128-encoded), and invoke and return
  // `aCallback(Maybe<ProfileBufferEntryWriter>&)`.
  // Note: `aCallbackEntryBytes` is a callback instead of a simple value, to
  // delay this potentially-expensive computation until after we're checked that
  // we're in-session; use `Put(Length, Callback)` below if you know the size
  // already.
  template <typename CallbackEntryBytes, typename Callback>
  auto ReserveAndPut(CallbackEntryBytes&& aCallbackEntryBytes,
                     Callback&& aCallback)
      -> decltype(std::forward<Callback>(aCallback)(
          std::declval<Maybe<ProfileBufferEntryWriter>&>())) {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);

    // This can only be read in the 2nd lambda below after it has been written
    // by the first lambda.
    Length entryBytes;

    return ReserveAndPutRaw(
        [&]() {
          entryBytes = std::forward<CallbackEntryBytes>(aCallbackEntryBytes)();
          MOZ_ASSERT(entryBytes != 0, "Empty entries are not allowed");
          return ULEB128Size(entryBytes) + entryBytes;
        },
        [&](Maybe<ProfileBufferEntryWriter>& aMaybeEntryWriter) {
          if (aMaybeEntryWriter.isSome()) {
            aMaybeEntryWriter->WriteULEB128(entryBytes);
            MOZ_ASSERT(aMaybeEntryWriter->RemainingBytes() == entryBytes);
          }
          return std::forward<Callback>(aCallback)(aMaybeEntryWriter);
        },
        lock);
  }

  template <typename Callback>
  auto Put(Length aEntryBytes, Callback&& aCallback) {
    return ReserveAndPut([aEntryBytes]() { return aEntryBytes; },
                         std::forward<Callback>(aCallback));
  }

  // Add a new entry copied from the given buffer, return block index.
  ProfileBufferBlockIndex PutFrom(const void* aSrc, Length aBytes) {
    return ReserveAndPut(
        [aBytes]() { return aBytes; },
        [aSrc, aBytes](Maybe<ProfileBufferEntryWriter>& aMaybeEntryWriter) {
          if (aMaybeEntryWriter.isNothing()) {
            return ProfileBufferBlockIndex{};
          }
          aMaybeEntryWriter->WriteBytes(aSrc, aBytes);
          return aMaybeEntryWriter->CurrentBlockIndex();
        });
  }

  // Add a new single entry with *all* given object (using a Serializer for
  // each), return block index.
  template <typename... Ts>
  ProfileBufferBlockIndex PutObjects(const Ts&... aTs) {
    static_assert(sizeof...(Ts) > 0,
                  "PutObjects must be given at least one object.");
    return ReserveAndPut(
        [&]() { return ProfileBufferEntryWriter::SumBytes(aTs...); },
        [&](Maybe<ProfileBufferEntryWriter>& aMaybeEntryWriter) {
          if (aMaybeEntryWriter.isNothing()) {
            return ProfileBufferBlockIndex{};
          }
          aMaybeEntryWriter->WriteObjects(aTs...);
          return aMaybeEntryWriter->CurrentBlockIndex();
        });
  }

  // Add a new entry copied from the given object, return block index.
  template <typename T>
  ProfileBufferBlockIndex PutObject(const T& aOb) {
    return PutObjects(aOb);
  }

  // Get *all* chunks related to this buffer, including extant chunks in its
  // ChunkManager, and yet-unused new/recycled chunks.
  // We don't expect this buffer to be used again, though it's still possible
  // and will allocate the first buffer when needed.
  [[nodiscard]] UniquePtr<ProfileBufferChunk> GetAllChunks() {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    if (MOZ_UNLIKELY(!mChunkManager)) {
      // Out-of-session.
      return nullptr;
    }
    UniquePtr<ProfileBufferChunk> chunks =
        mChunkManager->GetExtantReleasedChunks();
    Unused << HandleRequestedChunk_IsPending(lock);
    if (MOZ_LIKELY(!!mCurrentChunk)) {
      mCurrentChunk->MarkDone();
      chunks =
          ProfileBufferChunk::Join(std::move(chunks), std::move(mCurrentChunk));
    }
    chunks =
        ProfileBufferChunk::Join(std::move(chunks), std::move(mNextChunks));
    mChunkManager->ForgetUnreleasedChunks();
    mRangeStart = mRangeEnd = mNextChunkRangeStart;
    return chunks;
  }

  // True if the given index points inside the current chunk (up to the last
  // written byte).
  // This could be used to check if an index written now would have a good
  // chance of referring to a previous block that has not been destroyed yet.
  // But use with extreme care: This information may become incorrect right
  // after this function returns, because new writes could start a new chunk.
  [[nodiscard]] bool IsIndexInCurrentChunk(ProfileBufferIndex aIndex) const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    if (MOZ_UNLIKELY(!mChunkManager || !mCurrentChunk)) {
      // Out-of-session, or no current chunk.
      return false;
    }
    return (mCurrentChunk->RangeStart() <= aIndex) &&
           (aIndex < (mCurrentChunk->RangeStart() +
                      mCurrentChunk->OffsetPastLastBlock()));
  }

  class Reader;

  // Class that can iterate through blocks and provide
  // `ProfileBufferEntryReader`s.
  // Created through `Reader`, lives within a lock guard lifetime.
  class BlockIterator {
   public:
#ifdef DEBUG
    ~BlockIterator() {
      // No BlockIterator should live outside of a mutexed call.
      mBuffer->mMutex.AssertCurrentThreadOwns();
    }
#endif  // DEBUG

    // Comparison with other iterator, mostly used in range-for loops.
    [[nodiscard]] bool operator==(const BlockIterator& aRhs) const {
      MOZ_ASSERT(mBuffer == aRhs.mBuffer);
      return mCurrentBlockIndex == aRhs.mCurrentBlockIndex;
    }
    [[nodiscard]] bool operator!=(const BlockIterator& aRhs) const {
      MOZ_ASSERT(mBuffer == aRhs.mBuffer);
      return mCurrentBlockIndex != aRhs.mCurrentBlockIndex;
    }

    // Advance to next BlockIterator.
    BlockIterator& operator++() {
      mBuffer->mMutex.AssertCurrentThreadOwns();
      mCurrentBlockIndex =
          ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
              mNextBlockPointer.GlobalRangePosition());
      mCurrentEntry =
          mNextBlockPointer.EntryReader(mNextBlockPointer.ReadEntrySize());
      return *this;
    }

    // Dereferencing creates a `ProfileBufferEntryReader` object for the entry
    // inside this block.
    // (Note: It would be possible to return a `const
    // ProfileBufferEntryReader&`, but not useful in practice, because in most
    // case the user will want to read, which is non-const.)
    [[nodiscard]] ProfileBufferEntryReader operator*() const {
      return mCurrentEntry;
    }

    // True if this iterator is just past the last entry.
    [[nodiscard]] bool IsAtEnd() const {
      return mCurrentEntry.RemainingBytes() == 0;
    }

    // Can be used as reference to come back to this entry with `GetEntryAt()`.
    [[nodiscard]] ProfileBufferBlockIndex CurrentBlockIndex() const {
      return mCurrentBlockIndex;
    }

    // Index past the end of this block, which is the start of the next block.
    [[nodiscard]] ProfileBufferBlockIndex NextBlockIndex() const {
      MOZ_ASSERT(!IsAtEnd());
      return ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
          mNextBlockPointer.GlobalRangePosition());
    }

    // Index of the first block in the whole buffer.
    [[nodiscard]] ProfileBufferBlockIndex BufferRangeStart() const {
      mBuffer->mMutex.AssertCurrentThreadOwns();
      return ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
          mBuffer->mRangeStart);
    }

    // Index past the last block in the whole buffer.
    [[nodiscard]] ProfileBufferBlockIndex BufferRangeEnd() const {
      mBuffer->mMutex.AssertCurrentThreadOwns();
      return ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
          mBuffer->mRangeEnd);
    }

   private:
    // Only a Reader can instantiate a BlockIterator.
    friend class Reader;

    BlockIterator(const ProfileChunkedBuffer& aBuffer,
                  const ProfileBufferChunk* aChunks0,
                  const ProfileBufferChunk* aChunks1,
                  ProfileBufferBlockIndex aBlockIndex)
        : mNextBlockPointer(aChunks0, aChunks1, aBlockIndex),
          mCurrentBlockIndex(
              ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
                  mNextBlockPointer.GlobalRangePosition())),
          mCurrentEntry(
              mNextBlockPointer.EntryReader(mNextBlockPointer.ReadEntrySize())),
          mBuffer(WrapNotNull(&aBuffer)) {
      // No BlockIterator should live outside of a mutexed call.
      mBuffer->mMutex.AssertCurrentThreadOwns();
    }

    profiler::detail::InChunkPointer mNextBlockPointer;

    ProfileBufferBlockIndex mCurrentBlockIndex;

    ProfileBufferEntryReader mCurrentEntry;

    // Using a non-null pointer instead of a reference, to allow copying.
    // This BlockIterator should only live inside one of the thread-safe
    // ProfileChunkedBuffer functions, for this reference to stay valid.
    NotNull<const ProfileChunkedBuffer*> mBuffer;
  };

  // Class that can create `BlockIterator`s (e.g., for range-for), or just
  // iterate through entries; lives within a lock guard lifetime.
  class MOZ_RAII Reader {
   public:
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) = delete;
    Reader& operator=(Reader&&) = delete;

#ifdef DEBUG
    ~Reader() {
      // No Reader should live outside of a mutexed call.
      mBuffer.mMutex.AssertCurrentThreadOwns();
    }
#endif  // DEBUG

    // Index of the first block in the whole buffer.
    [[nodiscard]] ProfileBufferBlockIndex BufferRangeStart() const {
      mBuffer.mMutex.AssertCurrentThreadOwns();
      return ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
          mBuffer.mRangeStart);
    }

    // Index past the last block in the whole buffer.
    [[nodiscard]] ProfileBufferBlockIndex BufferRangeEnd() const {
      mBuffer.mMutex.AssertCurrentThreadOwns();
      return ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
          mBuffer.mRangeEnd);
    }

    // Iterators to the first and past-the-last blocks.
    // Compatible with range-for (see `ForEach` below as example).
    [[nodiscard]] BlockIterator begin() const {
      return BlockIterator(mBuffer, mChunks0, mChunks1, nullptr);
    }
    // Note that a `BlockIterator` at the `end()` should not be dereferenced, as
    // there is no actual block there!
    [[nodiscard]] BlockIterator end() const {
      return BlockIterator(mBuffer, nullptr, nullptr, nullptr);
    }

    // Get a `BlockIterator` at the given `ProfileBufferBlockIndex`, clamped to
    // the stored range. Note that a `BlockIterator` at the `end()` should not
    // be dereferenced, as there is no actual block there!
    [[nodiscard]] BlockIterator At(ProfileBufferBlockIndex aBlockIndex) const {
      if (aBlockIndex < BufferRangeStart()) {
        // Anything before the range (including null ProfileBufferBlockIndex) is
        // clamped at the beginning.
        return begin();
      }
      // Otherwise we at least expect the index to be valid (pointing exactly at
      // a live block, or just past the end.)
      return BlockIterator(mBuffer, mChunks0, mChunks1, aBlockIndex);
    }

    // Run `aCallback(ProfileBufferEntryReader&)` on each entry from first to
    // last. Callback should not store `ProfileBufferEntryReader`, as it may
    // become invalid after this thread-safe call.
    template <typename Callback>
    void ForEach(Callback&& aCallback) const {
      for (ProfileBufferEntryReader reader : *this) {
        aCallback(reader);
      }
    }

    // If this reader only points at one chunk with some data, this data will be
    // exposed as a single entry.
    [[nodiscard]] ProfileBufferEntryReader SingleChunkDataAsEntry() {
      const ProfileBufferChunk* onlyNonEmptyChunk = nullptr;
      for (const ProfileBufferChunk* chunkList : {mChunks0, mChunks1}) {
        for (const ProfileBufferChunk* chunk = chunkList; chunk;
             chunk = chunk->GetNext()) {
          if (chunk->OffsetFirstBlock() != chunk->OffsetPastLastBlock()) {
            if (onlyNonEmptyChunk) {
              // More than one non-empty chunk.
              return ProfileBufferEntryReader();
            }
            onlyNonEmptyChunk = chunk;
          }
        }
      }
      if (!onlyNonEmptyChunk) {
        // No non-empty chunks.
        return ProfileBufferEntryReader();
      }
      // Here, we have found one chunk that had some data.
      return ProfileBufferEntryReader(
          onlyNonEmptyChunk->BufferSpan().FromTo(
              onlyNonEmptyChunk->OffsetFirstBlock(),
              onlyNonEmptyChunk->OffsetPastLastBlock()),
          ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
              onlyNonEmptyChunk->RangeStart()),
          ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
              onlyNonEmptyChunk->RangeStart() +
              (onlyNonEmptyChunk->OffsetPastLastBlock() -
               onlyNonEmptyChunk->OffsetFirstBlock())));
    }

   private:
    friend class ProfileChunkedBuffer;

    explicit Reader(const ProfileChunkedBuffer& aBuffer,
                    const ProfileBufferChunk* aChunks0,
                    const ProfileBufferChunk* aChunks1)
        : mBuffer(aBuffer), mChunks0(aChunks0), mChunks1(aChunks1) {
      // No Reader should live outside of a mutexed call.
      mBuffer.mMutex.AssertCurrentThreadOwns();
    }

    // This Reader should only live inside one of the thread-safe
    // ProfileChunkedBuffer functions, for this reference to stay valid.
    const ProfileChunkedBuffer& mBuffer;
    const ProfileBufferChunk* mChunks0;
    const ProfileBufferChunk* mChunks1;
  };

  // In in-session, call `aCallback(ProfileChunkedBuffer::Reader&)` and return
  // true. Callback should not store `Reader`, because it may become invalid
  // after this call.
  // If out-of-session, return false (callback is not invoked).
  template <typename Callback>
  [[nodiscard]] auto Read(Callback&& aCallback) const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    if (MOZ_UNLIKELY(!mChunkManager)) {
      // Out-of-session.
      return std::forward<Callback>(aCallback)(static_cast<Reader*>(nullptr));
    }
    return mChunkManager->PeekExtantReleasedChunks(
        [&](const ProfileBufferChunk* aOldestChunk) {
          Reader reader(*this, aOldestChunk, mCurrentChunk.get());
          return std::forward<Callback>(aCallback)(&reader);
        });
  }

  // Invoke `aCallback(ProfileBufferEntryReader& [, ProfileBufferBlockIndex])`
  // on each entry, it must read or at least skip everything. Either/both chunk
  // pointers may be null.
  template <typename Callback>
  static void ReadEach(const ProfileBufferChunk* aChunks0,
                       const ProfileBufferChunk* aChunks1,
                       Callback&& aCallback) {
    static_assert(std::is_invocable_v<Callback, ProfileBufferEntryReader&> ||
                      std::is_invocable_v<Callback, ProfileBufferEntryReader&,
                                          ProfileBufferBlockIndex>,
                  "ReadEach callback must take ProfileBufferEntryReader& and "
                  "optionally a ProfileBufferBlockIndex");
    profiler::detail::InChunkPointer p{aChunks0, aChunks1};
    while (!p.IsNull()) {
      // The position right before an entry size *is* a block index.
      const ProfileBufferBlockIndex blockIndex =
          ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
              p.GlobalRangePosition());
      Length entrySize = p.ReadEntrySize();
      if (entrySize == 0) {
        return;
      }
      ProfileBufferEntryReader entryReader = p.EntryReader(entrySize);
      if (entryReader.RemainingBytes() == 0) {
        return;
      }
      MOZ_ASSERT(entryReader.RemainingBytes() == entrySize);
      if constexpr (std::is_invocable_v<Callback, ProfileBufferEntryReader&,
                                        ProfileBufferBlockIndex>) {
        aCallback(entryReader, blockIndex);
      } else {
        Unused << blockIndex;
        aCallback(entryReader);
      }
      MOZ_ASSERT(entryReader.RemainingBytes() == 0);
    }
  }

  // Invoke `aCallback(ProfileBufferEntryReader& [, ProfileBufferBlockIndex])`
  // on each entry, it must read or at least skip everything.
  template <typename Callback>
  void ReadEach(Callback&& aCallback) const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    if (MOZ_UNLIKELY(!mChunkManager)) {
      // Out-of-session.
      return;
    }
    mChunkManager->PeekExtantReleasedChunks(
        [&](const ProfileBufferChunk* aOldestChunk) {
          ReadEach(aOldestChunk, mCurrentChunk.get(),
                   std::forward<Callback>(aCallback));
        });
  }

  // Call `aCallback(Maybe<ProfileBufferEntryReader>&&)` on the entry at
  // the given ProfileBufferBlockIndex; The `Maybe` will be `Nothing` if
  // out-of-session, or if that entry doesn't exist anymore, or if we've reached
  // just past the last entry. Return whatever `aCallback` returns. Callback
  // should not store `ProfileBufferEntryReader`, because it may become invalid
  // after this call.
  // Either/both chunk pointers may be null.
  template <typename Callback>
  [[nodiscard]] static auto ReadAt(ProfileBufferBlockIndex aMinimumBlockIndex,
                                   const ProfileBufferChunk* aChunks0,
                                   const ProfileBufferChunk* aChunks1,
                                   Callback&& aCallback) {
    static_assert(
        std::is_invocable_v<Callback, Maybe<ProfileBufferEntryReader>&&>,
        "ReadAt callback must take a Maybe<ProfileBufferEntryReader>&&");
    Maybe<ProfileBufferEntryReader> maybeEntryReader;
    if (profiler::detail::InChunkPointer p{aChunks0, aChunks1}; !p.IsNull()) {
      // If the pointer position is before the given position, try to advance.
      if (p.GlobalRangePosition() >=
              aMinimumBlockIndex.ConvertToProfileBufferIndex() ||
          p.AdvanceToGlobalRangePosition(
              aMinimumBlockIndex.ConvertToProfileBufferIndex())) {
        MOZ_ASSERT(p.GlobalRangePosition() >=
                   aMinimumBlockIndex.ConvertToProfileBufferIndex());
        // Here we're pointing at the start of a block, try to read the entry
        // size. (Entries cannot be empty, so 0 means failure.)
        if (Length entrySize = p.ReadEntrySize(); entrySize != 0) {
          maybeEntryReader.emplace(p.EntryReader(entrySize));
          if (maybeEntryReader->RemainingBytes() == 0) {
            // An empty entry reader means there was no complete block at the
            // given index.
            maybeEntryReader.reset();
          } else {
            MOZ_ASSERT(maybeEntryReader->RemainingBytes() == entrySize);
          }
        }
      }
    }
#ifdef DEBUG
    auto assertAllRead = MakeScopeExit([&]() {
      MOZ_ASSERT(!maybeEntryReader || maybeEntryReader->RemainingBytes() == 0);
    });
#endif  // DEBUG
    return std::forward<Callback>(aCallback)(std::move(maybeEntryReader));
  }

  // Call `aCallback(Maybe<ProfileBufferEntryReader>&&)` on the entry at
  // the given ProfileBufferBlockIndex; The `Maybe` will be `Nothing` if
  // out-of-session, or if that entry doesn't exist anymore, or if we've reached
  // just past the last entry. Return whatever `aCallback` returns. Callback
  // should not store `ProfileBufferEntryReader`, because it may become invalid
  // after this call.
  template <typename Callback>
  [[nodiscard]] auto ReadAt(ProfileBufferBlockIndex aBlockIndex,
                            Callback&& aCallback) const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    if (MOZ_UNLIKELY(!mChunkManager)) {
      // Out-of-session.
      return std::forward<Callback>(aCallback)(Nothing{});
    }
    return mChunkManager->PeekExtantReleasedChunks(
        [&](const ProfileBufferChunk* aOldestChunk) {
          return ReadAt(aBlockIndex, aOldestChunk, mCurrentChunk.get(),
                        std::forward<Callback>(aCallback));
        });
  }

  // Append the contents of another ProfileChunkedBuffer to this one.
  ProfileBufferBlockIndex AppendContents(const ProfileChunkedBuffer& aSrc) {
    ProfileBufferBlockIndex firstBlockIndex;
    // If we start failing, we'll stop writing.
    bool failed = false;
    aSrc.ReadEach([&](ProfileBufferEntryReader& aER) {
      if (failed) {
        return;
      }
      failed =
          !Put(aER.RemainingBytes(), [&](Maybe<ProfileBufferEntryWriter>& aEW) {
            if (aEW.isNothing()) {
              return false;
            }
            if (!firstBlockIndex) {
              firstBlockIndex = aEW->CurrentBlockIndex();
            }
            aEW->WriteFromReader(aER, aER.RemainingBytes());
            return true;
          });
    });
    return failed ? nullptr : firstBlockIndex;
  }

#ifdef DEBUG
  void Dump(std::FILE* aFile = stdout) const {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    fprintf(aFile,
            "ProfileChunkedBuffer[%p] State: range %u-%u pushed=%u cleared=%u "
            "(live=%u) failed-puts=%u bytes",
            this, unsigned(mRangeStart), unsigned(mRangeEnd),
            unsigned(mPushedBlockCount), unsigned(mClearedBlockCount),
            unsigned(mPushedBlockCount) - unsigned(mClearedBlockCount),
            unsigned(mFailedPutBytes));
    if (MOZ_UNLIKELY(!mChunkManager)) {
      fprintf(aFile, " - Out-of-session\n");
      return;
    }
    fprintf(aFile, " - chunks:\n");
    bool hasChunks = false;
    mChunkManager->PeekExtantReleasedChunks(
        [&](const ProfileBufferChunk* aOldestChunk) {
          for (const ProfileBufferChunk* chunk = aOldestChunk; chunk;
               chunk = chunk->GetNext()) {
            fprintf(aFile, "R ");
            chunk->Dump(aFile);
            hasChunks = true;
          }
        });
    if (mCurrentChunk) {
      fprintf(aFile, "C ");
      mCurrentChunk->Dump(aFile);
      hasChunks = true;
    }
    for (const ProfileBufferChunk* chunk = mNextChunks.get(); chunk;
         chunk = chunk->GetNext()) {
      fprintf(aFile, "N ");
      chunk->Dump(aFile);
      hasChunks = true;
    }
    switch (mRequestedChunkHolder->GetState()) {
      case RequestedChunkRefCountedHolder::State::Unused:
        fprintf(aFile, " - No request pending.\n");
        break;
      case RequestedChunkRefCountedHolder::State::Requested:
        fprintf(aFile, " - Request pending.\n");
        break;
      case RequestedChunkRefCountedHolder::State::Fulfilled:
        fprintf(aFile, " - Request fulfilled.\n");
        break;
    }
    if (!hasChunks) {
      fprintf(aFile, " No chunks.\n");
    }
  }
#endif  // DEBUG

 private:
  // Used to de/serialize a ProfileChunkedBuffer (e.g., containing a backtrace).
  friend ProfileBufferEntryWriter::Serializer<ProfileChunkedBuffer>;
  friend ProfileBufferEntryReader::Deserializer<ProfileChunkedBuffer>;
  friend ProfileBufferEntryWriter::Serializer<UniquePtr<ProfileChunkedBuffer>>;
  friend ProfileBufferEntryReader::Deserializer<
      UniquePtr<ProfileChunkedBuffer>>;

  [[nodiscard]] UniquePtr<ProfileBufferChunkManager> ResetChunkManager(
      const baseprofiler::detail::BaseProfilerMaybeAutoLock&) {
    UniquePtr<ProfileBufferChunkManager> chunkManager;
    if (mChunkManager) {
      mRequestedChunkHolder = nullptr;
      mChunkManager->ForgetUnreleasedChunks();
#ifdef DEBUG
      mChunkManager->DeregisteredFrom(this);
#endif
      mChunkManager = nullptr;
      chunkManager = std::move(mOwnedChunkManager);
      if (mCurrentChunk) {
        mCurrentChunk->MarkDone();
        mCurrentChunk = nullptr;
      }
      mNextChunks = nullptr;
      mNextChunkRangeStart = mRangeEnd;
      mRangeStart = mRangeEnd;
      mPushedBlockCount = 0;
      mClearedBlockCount = 0;
      mFailedPutBytes = 0;
    }
    return chunkManager;
  }

  void SetChunkManager(
      ProfileBufferChunkManager& aChunkManager,
      const baseprofiler::detail::BaseProfilerMaybeAutoLock& aLock) {
    MOZ_ASSERT(!mChunkManager);
    mChunkManager = &aChunkManager;
#ifdef DEBUG
    mChunkManager->RegisteredWith(this);
#endif

    mChunkManager->SetChunkDestroyedCallback(
        [this](const ProfileBufferChunk& aChunk) {
          for (;;) {
            ProfileBufferIndex rangeStart = mRangeStart;
            if (MOZ_LIKELY(rangeStart <= aChunk.RangeStart())) {
              if (MOZ_LIKELY(mRangeStart.compareExchange(
                      rangeStart,
                      aChunk.RangeStart() + aChunk.BufferBytes()))) {
                break;
              }
            }
          }
          mClearedBlockCount += aChunk.BlockCount();
        });

    // We start with one chunk right away, and request a following one now
    // so it should be available before the current chunk is full.
    SetAndInitializeCurrentChunk(mChunkManager->GetChunk(), aLock);
    mRequestedChunkHolder = MakeRefPtr<RequestedChunkRefCountedHolder>();
    RequestChunk(aLock);
  }

  [[nodiscard]] size_t SizeOfExcludingThis(
      MallocSizeOf aMallocSizeOf,
      const baseprofiler::detail::BaseProfilerMaybeAutoLock&) const {
    if (MOZ_UNLIKELY(!mChunkManager)) {
      // Out-of-session.
      return 0;
    }
    size_t size = mChunkManager->SizeOfIncludingThis(aMallocSizeOf);
    if (mCurrentChunk) {
      size += mCurrentChunk->SizeOfIncludingThis(aMallocSizeOf);
    }
    if (mNextChunks) {
      size += mNextChunks->SizeOfIncludingThis(aMallocSizeOf);
    }
    return size;
  }

  void InitializeCurrentChunk(
      const baseprofiler::detail::BaseProfilerMaybeAutoLock&) {
    MOZ_ASSERT(!!mCurrentChunk);
    mCurrentChunk->SetRangeStart(mNextChunkRangeStart);
    mNextChunkRangeStart += mCurrentChunk->BufferBytes();
    Unused << mCurrentChunk->ReserveInitialBlockAsTail(0);
  }

  void SetAndInitializeCurrentChunk(
      UniquePtr<ProfileBufferChunk>&& aChunk,
      const baseprofiler::detail::BaseProfilerMaybeAutoLock& aLock) {
    mCurrentChunk = std::move(aChunk);
    if (mCurrentChunk) {
      InitializeCurrentChunk(aLock);
    }
  }

  void RequestChunk(
      const baseprofiler::detail::BaseProfilerMaybeAutoLock& aLock) {
    if (HandleRequestedChunk_IsPending(aLock)) {
      // There is already a pending request, don't start a new one.
      return;
    }

    // Ensure the `RequestedChunkHolder` knows we're starting a request.
    mRequestedChunkHolder->StartRequest();

    // Request a chunk, the callback carries a `RefPtr` of the
    // `RequestedChunkHolder`, so it's guaranteed to live until it's invoked,
    // even if this `ProfileChunkedBuffer` changes its `ChunkManager` or is
    // destroyed.
    mChunkManager->RequestChunk(
        [requestedChunkHolder = RefPtr<RequestedChunkRefCountedHolder>(
             mRequestedChunkHolder)](UniquePtr<ProfileBufferChunk> aChunk) {
          requestedChunkHolder->AddRequestedChunk(std::move(aChunk));
        });
  }

  [[nodiscard]] bool HandleRequestedChunk_IsPending(
      const baseprofiler::detail::BaseProfilerMaybeAutoLock& aLock) {
    MOZ_ASSERT(!!mChunkManager);
    MOZ_ASSERT(!!mRequestedChunkHolder);

    if (mRequestedChunkHolder->GetState() ==
        RequestedChunkRefCountedHolder::State::Unused) {
      return false;
    }

    // A request is either in-flight or fulfilled.
    Maybe<UniquePtr<ProfileBufferChunk>> maybeChunk =
        mRequestedChunkHolder->GetChunkIfFulfilled();
    if (maybeChunk.isNothing()) {
      // Request is still pending.
      return true;
    }

    // Since we extracted the provided chunk, the holder should now be unused.
    MOZ_ASSERT(mRequestedChunkHolder->GetState() ==
               RequestedChunkRefCountedHolder::State::Unused);

    // Request has been fulfilled.
    UniquePtr<ProfileBufferChunk>& chunk = *maybeChunk;
    if (chunk) {
      // Try to use as current chunk if needed.
      if (!mCurrentChunk) {
        SetAndInitializeCurrentChunk(std::move(chunk), aLock);
        // We've just received a chunk and made it current, request a next chunk
        // for later.
        MOZ_ASSERT(!mNextChunks);
        RequestChunk(aLock);
        return true;
      }

      if (!mNextChunks) {
        mNextChunks = std::move(chunk);
      } else {
        mNextChunks->InsertNext(std::move(chunk));
      }
    }

    return false;
  }

  // Get a pointer to the next chunk available
  [[nodiscard]] ProfileBufferChunk* GetOrCreateCurrentChunk(
      const baseprofiler::detail::BaseProfilerMaybeAutoLock& aLock) {
    ProfileBufferChunk* current = mCurrentChunk.get();
    if (MOZ_UNLIKELY(!current)) {
      // No current chunk ready.
      MOZ_ASSERT(!mNextChunks,
                 "There shouldn't be next chunks when there is no current one");
      // See if a request has recently been fulfilled, ignore pending status.
      Unused << HandleRequestedChunk_IsPending(aLock);
      current = mCurrentChunk.get();
      if (MOZ_UNLIKELY(!current)) {
        // There was no pending chunk, try to get one right now.
        // This may still fail, but we can't do anything else about it, the
        // caller must handle the nullptr case.
        // Attempt a request for later.
        SetAndInitializeCurrentChunk(mChunkManager->GetChunk(), aLock);
        current = mCurrentChunk.get();
      }
    }
    return current;
  }

  // Get a pointer to the next chunk available
  [[nodiscard]] ProfileBufferChunk* GetOrCreateNextChunk(
      const baseprofiler::detail::BaseProfilerMaybeAutoLock& aLock) {
    MOZ_ASSERT(!!mCurrentChunk,
               "Why ask for a next chunk when there isn't even a current one?");
    ProfileBufferChunk* next = mNextChunks.get();
    if (MOZ_UNLIKELY(!next)) {
      // No next chunk ready, see if a request has recently been fulfilled,
      // ignore pending status.
      Unused << HandleRequestedChunk_IsPending(aLock);
      next = mNextChunks.get();
      if (MOZ_UNLIKELY(!next)) {
        // There was no pending chunk, try to get one right now.
        mNextChunks = mChunkManager->GetChunk();
        next = mNextChunks.get();
        // This may still fail, but we can't do anything else about it, the
        // caller must handle the nullptr case.
        if (MOZ_UNLIKELY(!next)) {
          // Attempt a request for later.
          RequestChunk(aLock);
        }
      }
    }
    return next;
  }

  // Reserve a block of `aCallbackBlockBytes()` size, and invoke and return
  // `aCallback(Maybe<ProfileBufferEntryWriter>&)`. Note that this is the "raw"
  // version that doesn't write the entry size at the beginning of the block.
  // Note: `aCallbackBlockBytes` is a callback instead of a simple value, to
  // delay this potentially-expensive computation until after we're checked that
  // we're in-session; use `Put(Length, Callback)` below if you know the size
  // already.
  template <typename CallbackBlockBytes, typename Callback>
  auto ReserveAndPutRaw(CallbackBlockBytes&& aCallbackBlockBytes,
                        Callback&& aCallback,
                        baseprofiler::detail::BaseProfilerMaybeAutoLock& aLock,
                        uint64_t aBlockCount = 1) {
    // The entry writer that will point into one or two chunks to write
    // into, empty by default (failure).
    Maybe<ProfileBufferEntryWriter> maybeEntryWriter;

    // The current chunk will be filled if we need to write more than its
    // remaining space.
    bool currentChunkFilled = false;

    // If the current chunk gets filled, we may or may not initialize the next
    // chunk!
    bool nextChunkInitialized = false;

    if (MOZ_LIKELY(mChunkManager)) {
      // In-session.

      const Length blockBytes =
          std::forward<CallbackBlockBytes>(aCallbackBlockBytes)();

      if (ProfileBufferChunk* current = GetOrCreateCurrentChunk(aLock);
          MOZ_LIKELY(current)) {
        if (blockBytes <= current->RemainingBytes()) {
          // Block fits in current chunk with only one span.
          currentChunkFilled = blockBytes == current->RemainingBytes();
          const auto [mem0, blockIndex] = current->ReserveBlock(blockBytes);
          MOZ_ASSERT(mem0.LengthBytes() == blockBytes);
          maybeEntryWriter.emplace(
              mem0, blockIndex,
              ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
                  blockIndex.ConvertToProfileBufferIndex() + blockBytes));
          MOZ_ASSERT(maybeEntryWriter->RemainingBytes() == blockBytes);
          mRangeEnd += blockBytes;
          mPushedBlockCount += aBlockCount;
        } else if (blockBytes >= current->BufferBytes()) {
          // Currently only two buffer chunks are held at a time and it is not
          // possible to write an object that takes up more space than this. In
          // this scenario, silently discard this block of data if it is unable
          // to fit into the two reserved profiler chunks.
          mFailedPutBytes += blockBytes;
        } else {
          // Block doesn't fit fully in current chunk, it needs to overflow into
          // the next one.
          // Whether or not we can write this entry, the current chunk is now
          // considered full, so it will be released. (Otherwise we could refuse
          // this entry, but later accept a smaller entry into this chunk, which
          // would be somewhat inconsistent.)
          currentChunkFilled = true;
          // Make sure the next chunk is available (from a previous request),
          // otherwise create one on the spot.
          if (ProfileBufferChunk* next = GetOrCreateNextChunk(aLock);
              MOZ_LIKELY(next)) {
            // Here, we know we have a current and a next chunk.
            // Reserve head of block at the end of the current chunk.
            const auto [mem0, blockIndex] =
                current->ReserveBlock(current->RemainingBytes());
            MOZ_ASSERT(mem0.LengthBytes() < blockBytes);
            MOZ_ASSERT(current->RemainingBytes() == 0);
            // Set the next chunk range, and reserve the needed space for the
            // tail of the block.
            next->SetRangeStart(mNextChunkRangeStart);
            mNextChunkRangeStart += next->BufferBytes();
            const auto mem1 = next->ReserveInitialBlockAsTail(
                blockBytes - mem0.LengthBytes());
            MOZ_ASSERT(next->RemainingBytes() != 0);
            nextChunkInitialized = true;
            // Block is split in two spans.
            maybeEntryWriter.emplace(
                mem0, mem1, blockIndex,
                ProfileBufferBlockIndex::CreateFromProfileBufferIndex(
                    blockIndex.ConvertToProfileBufferIndex() + blockBytes));
            MOZ_ASSERT(maybeEntryWriter->RemainingBytes() == blockBytes);
            mRangeEnd += blockBytes;
            mPushedBlockCount += aBlockCount;
          } else {
            // Cannot get a new chunk. Record put failure.
            mFailedPutBytes += blockBytes;
          }
        }
      } else {
        // Cannot get a current chunk. Record put failure.
        mFailedPutBytes += blockBytes;
      }
    }  // end of `if (MOZ_LIKELY(mChunkManager))`

    // Here, we either have a `Nothing` (failure), or a non-empty entry writer
    // pointing at the start of the block.

    // After we invoke the callback and return, we may need to handle the
    // current chunk being filled.
    auto handleFilledChunk = MakeScopeExit([&]() {
      // If the entry writer was not already empty, the callback *must* have
      // filled the full entry.
      MOZ_ASSERT(!maybeEntryWriter || maybeEntryWriter->RemainingBytes() == 0);

      if (currentChunkFilled) {
        // Extract current (now filled) chunk.
        UniquePtr<ProfileBufferChunk> filled = std::move(mCurrentChunk);

        if (mNextChunks) {
          // Cycle to the next chunk.
          mCurrentChunk =
              std::exchange(mNextChunks, mNextChunks->ReleaseNext());

          // Make sure it is initialized (it is now the current chunk).
          if (!nextChunkInitialized) {
            InitializeCurrentChunk(aLock);
          }
        }

        // And finally mark filled chunk done and release it.
        filled->MarkDone();
        mChunkManager->ReleaseChunk(std::move(filled));

        // Request another chunk if needed.
        // In most cases, here we should have one current chunk and no next
        // chunk, so we want to do a request so there hopefully will be a next
        // chunk available when the current one gets filled.
        // But we also for a request if we don't even have a current chunk (if
        // it's too late, it's ok because the next `ReserveAndPutRaw` wil just
        // allocate one on the spot.)
        // And if we already have a next chunk, there's no need for more now.
        if (!mCurrentChunk || !mNextChunks) {
          RequestChunk(aLock);
        }
      }
    });

    return std::forward<Callback>(aCallback)(maybeEntryWriter);
  }

  // Reserve a block of `aBlockBytes` size, and invoke and return
  // `aCallback(Maybe<ProfileBufferEntryWriter>&)`. Note that this is the "raw"
  // version that doesn't write the entry size at the beginning of the block.
  template <typename Callback>
  auto ReserveAndPutRaw(Length aBlockBytes, Callback&& aCallback,
                        uint64_t aBlockCount) {
    baseprofiler::detail::BaseProfilerMaybeAutoLock lock(mMutex);
    return ReserveAndPutRaw([aBlockBytes]() { return aBlockBytes; },
                            std::forward<Callback>(aCallback), lock,
                            aBlockCount);
  }

  // Mutex guarding the following members.
  mutable baseprofiler::detail::BaseProfilerMaybeMutex mMutex;

  // Pointer to the current Chunk Manager (or null when out-of-session.)
  // It may be owned locally (see below) or externally.
  ProfileBufferChunkManager* mChunkManager = nullptr;

  // Only non-null when we own the current Chunk Manager.
  UniquePtr<ProfileBufferChunkManager> mOwnedChunkManager;

  UniquePtr<ProfileBufferChunk> mCurrentChunk;

  UniquePtr<ProfileBufferChunk> mNextChunks;

  // Class used to transfer requested chunks from a `ChunkManager` to a
  // `ProfileChunkedBuffer`.
  // It needs to be ref-counted because the request may be fulfilled
  // asynchronously, and either side may be destroyed during the request.
  // It cannot use the `ProfileChunkedBuffer` mutex, because that buffer and its
  // mutex could be destroyed during the request.
  class RequestedChunkRefCountedHolder {
   public:
    enum class State { Unused, Requested, Fulfilled };

    // Get the current state. Note that it may change after the function
    // returns, so it should be used carefully, e.g., `ProfileChunkedBuffer` can
    // see if a request is pending or fulfilled, to avoid starting another
    // request.
    [[nodiscard]] State GetState() const {
      baseprofiler::detail::BaseProfilerAutoLock lock(mRequestMutex);
      return mState;
    }

    // Must be called by `ProfileChunkedBuffer` when it requests a chunk.
    // There cannot be more than one request in-flight.
    void StartRequest() {
      baseprofiler::detail::BaseProfilerAutoLock lock(mRequestMutex);
      MOZ_ASSERT(mState == State::Unused, "Already requested or fulfilled");
      mState = State::Requested;
    }

    // Must be called by the `ChunkManager` with a chunk.
    // If the `ChunkManager` cannot provide a chunk (because of memory limits,
    // or it gets destroyed), it must call this anyway with a nullptr.
    void AddRequestedChunk(UniquePtr<ProfileBufferChunk>&& aChunk) {
      baseprofiler::detail::BaseProfilerAutoLock lock(mRequestMutex);
      MOZ_ASSERT(mState == State::Requested);
      mState = State::Fulfilled;
      mRequestedChunk = std::move(aChunk);
    }

    // The `ProfileChunkedBuffer` can try to extract the provided chunk after a
    // request:
    // - Nothing -> Request is not fulfilled yet.
    // - Some(nullptr) -> The `ChunkManager` was not able to provide a chunk.
    // - Some(chunk) -> Requested chunk.
    [[nodiscard]] Maybe<UniquePtr<ProfileBufferChunk>> GetChunkIfFulfilled() {
      Maybe<UniquePtr<ProfileBufferChunk>> maybeChunk;
      baseprofiler::detail::BaseProfilerAutoLock lock(mRequestMutex);
      MOZ_ASSERT(mState == State::Requested || mState == State::Fulfilled);
      if (mState == State::Fulfilled) {
        mState = State::Unused;
        maybeChunk.emplace(std::move(mRequestedChunk));
      }
      return maybeChunk;
    }

    // Ref-counting implementation. Hand-rolled, because mozilla::RefCounted
    // logs AddRefs and Releases in xpcom, but this object could be AddRef'd
    // by the Base Profiler before xpcom starts, then Release'd by the Gecko
    // Profiler in xpcom, leading to apparent negative leaks.

    void AddRef() {
      baseprofiler::detail::BaseProfilerAutoLock lock(mRequestMutex);
      ++mRefCount;
    }

    void Release() {
      {
        baseprofiler::detail::BaseProfilerAutoLock lock(mRequestMutex);
        if (--mRefCount > 0) {
          return;
        }
      }
      delete this;
    }

   private:
    ~RequestedChunkRefCountedHolder() = default;

    // Mutex guarding the following members.
    mutable baseprofiler::detail::BaseProfilerMutex mRequestMutex;
    int mRefCount = 0;
    State mState = State::Unused;
    UniquePtr<ProfileBufferChunk> mRequestedChunk;
  };

  // Requested-chunk holder, kept alive when in-session, but may also live
  // longer if a request is in-flight.
  RefPtr<RequestedChunkRefCountedHolder> mRequestedChunkHolder;

  // Range start of the next chunk to become current. Starting at 1 because
  // 0 is a reserved index similar to nullptr.
  ProfileBufferIndex mNextChunkRangeStart = 1;

  // Index to the first block.
  // Atomic because it may be increased when a Chunk is destroyed, and the
  // callback may be invoked from anywhere, including from inside one of our
  // locked section, so we cannot protect it with a mutex.
  Atomic<ProfileBufferIndex, MemoryOrdering::ReleaseAcquire> mRangeStart{1};

  // Index past the last block. Equals mRangeStart if empty.
  ProfileBufferIndex mRangeEnd = 1;

  // Number of blocks that have been pushed into this buffer.
  uint64_t mPushedBlockCount = 0;

  // Number of blocks that have been removed from this buffer.
  // Note: Live entries = pushed - cleared.
  // Atomic because it may be updated when a Chunk is destroyed, and the
  // callback may be invoked from anywhere, including from inside one of our
  // locked section, so we cannot protect it with a mutex.
  Atomic<uint64_t, MemoryOrdering::ReleaseAcquire> mClearedBlockCount{0};

  // Number of bytes that could not be put into this buffer.
  uint64_t mFailedPutBytes = 0;
};

// ----------------------------------------------------------------------------
// ProfileChunkedBuffer serialization

// A ProfileChunkedBuffer can hide another one!
// This will be used to store marker backtraces; They can be read back into a
// UniquePtr<ProfileChunkedBuffer>.
// Format: len (ULEB128) | start | end | buffer (len bytes) | pushed | cleared
// len==0 marks an out-of-session buffer, or empty buffer.
template <>
struct ProfileBufferEntryWriter::Serializer<ProfileChunkedBuffer> {
  static Length Bytes(const ProfileChunkedBuffer& aBuffer) {
    return aBuffer.Read([&](ProfileChunkedBuffer::Reader* aReader) {
      if (!aReader) {
        // Out-of-session, we only need 1 byte to store a length of 0.
        return ULEB128Size<Length>(0);
      }
      ProfileBufferEntryReader reader = aReader->SingleChunkDataAsEntry();
      const ProfileBufferIndex start =
          reader.CurrentBlockIndex().ConvertToProfileBufferIndex();
      const ProfileBufferIndex end =
          reader.NextBlockIndex().ConvertToProfileBufferIndex();
      MOZ_ASSERT(end - start <= std::numeric_limits<Length>::max());
      const Length len = static_cast<Length>(end - start);
      if (len == 0) {
        // In-session but empty, also store a length of 0.
        return ULEB128Size<Length>(0);
      }
      // In-session.
      return static_cast<Length>(ULEB128Size(len) + sizeof(start) + len +
                                 sizeof(aBuffer.mPushedBlockCount) +
                                 sizeof(aBuffer.mClearedBlockCount));
    });
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const ProfileChunkedBuffer& aBuffer) {
    aBuffer.Read([&](ProfileChunkedBuffer::Reader* aReader) {
      if (!aReader) {
        // Out-of-session, only store a length of 0.
        aEW.WriteULEB128<Length>(0);
        return;
      }
      ProfileBufferEntryReader reader = aReader->SingleChunkDataAsEntry();
      const ProfileBufferIndex start =
          reader.CurrentBlockIndex().ConvertToProfileBufferIndex();
      const ProfileBufferIndex end =
          reader.NextBlockIndex().ConvertToProfileBufferIndex();
      MOZ_ASSERT(end - start <= std::numeric_limits<Length>::max());
      const Length len = static_cast<Length>(end - start);
      MOZ_ASSERT(len <= aEW.RemainingBytes());
      if (len == 0) {
        // In-session but empty, only store a length of 0.
        aEW.WriteULEB128<Length>(0);
        return;
      }
      // In-session.
      // Store buffer length, and start index.
      aEW.WriteULEB128(len);
      aEW.WriteObject(start);
      // Write all the bytes.
      aEW.WriteFromReader(reader, reader.RemainingBytes());
      // And write stats.
      aEW.WriteObject(static_cast<uint64_t>(aBuffer.mPushedBlockCount));
      aEW.WriteObject(static_cast<uint64_t>(aBuffer.mClearedBlockCount));
      // Note: Failed pushes are not important to serialize.
    });
  }
};

// A serialized ProfileChunkedBuffer can be read into an empty buffer (either
// out-of-session, or in-session with enough room).
template <>
struct ProfileBufferEntryReader::Deserializer<ProfileChunkedBuffer> {
  static void ReadInto(ProfileBufferEntryReader& aER,
                       ProfileChunkedBuffer& aBuffer) {
    // Expect an empty buffer, as we're going to overwrite it.
    MOZ_ASSERT(aBuffer.GetState().mRangeStart == aBuffer.GetState().mRangeEnd);
    // Read the stored buffer length.
    const auto len = aER.ReadULEB128<ProfileChunkedBuffer::Length>();
    if (len == 0) {
      // 0-length means an "uninteresting" buffer, just return now.
      return;
    }
    // We have a non-empty buffer to read.

    // Read start and end indices.
    const auto start = aER.ReadObject<ProfileBufferIndex>();
    aBuffer.mRangeStart = start;
    // For now, set the end to be the start (the buffer is still empty). It will
    // be updated in `ReserveAndPutRaw()` below.
    aBuffer.mRangeEnd = start;

    if (aBuffer.IsInSession()) {
      // Output buffer is in-session (i.e., it already has a memory buffer
      // attached). Make sure the caller allocated enough space.
      MOZ_RELEASE_ASSERT(aBuffer.BufferLength().value() >= len);
    } else {
      // Output buffer is out-of-session, set a new chunk manager that will
      // provide a single chunk of just the right size.
      aBuffer.SetChunkManager(MakeUnique<ProfileBufferChunkManagerSingle>(len));
      MOZ_ASSERT(aBuffer.BufferLength().value() >= len);
    }

    // Copy bytes into the buffer.
    aBuffer.ReserveAndPutRaw(
        len,
        [&](Maybe<ProfileBufferEntryWriter>& aEW) {
          MOZ_RELEASE_ASSERT(aEW.isSome());
          aEW->WriteFromReader(aER, len);
        },
        0);
    // Finally copy stats.
    aBuffer.mPushedBlockCount = aER.ReadObject<uint64_t>();
    aBuffer.mClearedBlockCount = aER.ReadObject<uint64_t>();
    // Failed puts are not important to keep.
    aBuffer.mFailedPutBytes = 0;
  }

  // We cannot output a ProfileChunkedBuffer object (not copyable), use
  // `ReadInto()` or `aER.ReadObject<UniquePtr<BlocksRinbBuffer>>()` instead.
  static ProfileChunkedBuffer Read(ProfileBufferEntryReader& aER) = delete;
};

// A ProfileChunkedBuffer is usually refererenced through a UniquePtr, for
// convenience we support (de)serializing that UniquePtr directly.
// This is compatible with the non-UniquePtr serialization above, with a null
// pointer being treated like an out-of-session or empty buffer; and any of
// these would be deserialized into a null pointer.
template <>
struct ProfileBufferEntryWriter::Serializer<UniquePtr<ProfileChunkedBuffer>> {
  static Length Bytes(const UniquePtr<ProfileChunkedBuffer>& aBufferUPtr) {
    if (!aBufferUPtr) {
      // Null pointer, treat it like an empty buffer, i.e., write length of 0.
      return ULEB128Size<Length>(0);
    }
    // Otherwise write the pointed-at ProfileChunkedBuffer (which could be
    // out-of-session or empty.)
    return SumBytes(*aBufferUPtr);
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const UniquePtr<ProfileChunkedBuffer>& aBufferUPtr) {
    if (!aBufferUPtr) {
      // Null pointer, treat it like an empty buffer, i.e., write length of 0.
      aEW.WriteULEB128<Length>(0);
      return;
    }
    // Otherwise write the pointed-at ProfileChunkedBuffer (which could be
    // out-of-session or empty.)
    aEW.WriteObject(*aBufferUPtr);
  }
};

// Serialization of a raw pointer to ProfileChunkedBuffer.
// Use Deserializer<UniquePtr<ProfileChunkedBuffer>> to read it back.
template <>
struct ProfileBufferEntryWriter::Serializer<ProfileChunkedBuffer*> {
  static Length Bytes(ProfileChunkedBuffer* aBufferUPtr) {
    if (!aBufferUPtr) {
      // Null pointer, treat it like an empty buffer, i.e., write length of 0.
      return ULEB128Size<Length>(0);
    }
    // Otherwise write the pointed-at ProfileChunkedBuffer (which could be
    // out-of-session or empty.)
    return SumBytes(*aBufferUPtr);
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    ProfileChunkedBuffer* aBufferUPtr) {
    if (!aBufferUPtr) {
      // Null pointer, treat it like an empty buffer, i.e., write length of 0.
      aEW.WriteULEB128<Length>(0);
      return;
    }
    // Otherwise write the pointed-at ProfileChunkedBuffer (which could be
    // out-of-session or empty.)
    aEW.WriteObject(*aBufferUPtr);
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<UniquePtr<ProfileChunkedBuffer>> {
  static void ReadInto(ProfileBufferEntryReader& aER,
                       UniquePtr<ProfileChunkedBuffer>& aBuffer) {
    aBuffer = Read(aER);
  }

  static UniquePtr<ProfileChunkedBuffer> Read(ProfileBufferEntryReader& aER) {
    UniquePtr<ProfileChunkedBuffer> bufferUPtr;
    // Keep a copy of the reader before reading the length, so we can restart
    // from here below.
    ProfileBufferEntryReader readerBeforeLen = aER;
    // Read the stored buffer length.
    const auto len = aER.ReadULEB128<ProfileChunkedBuffer::Length>();
    if (len == 0) {
      // 0-length means an "uninteresting" buffer, just return nullptr.
      return bufferUPtr;
    }
    // We have a non-empty buffer.
    // allocate an empty ProfileChunkedBuffer without mutex.
    bufferUPtr = MakeUnique<ProfileChunkedBuffer>(
        ProfileChunkedBuffer::ThreadSafety::WithoutMutex);
    // Rewind the reader before the length and deserialize the contents, using
    // the non-UniquePtr Deserializer.
    aER = readerBeforeLen;
    aER.ReadIntoObject(*bufferUPtr);
    return bufferUPtr;
  }
};

}  // namespace mozilla

#endif  // ProfileChunkedBuffer_h

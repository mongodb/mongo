/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileBufferChunkManagerWithLocalLimit_h
#define ProfileBufferChunkManagerWithLocalLimit_h

#include "BaseProfiler.h"
#include "mozilla/BaseProfilerDetail.h"
#include "mozilla/ProfileBufferChunkManager.h"
#include "mozilla/ProfileBufferControlledChunkManager.h"
#include "mozilla/mozalloc.h"

#include <utility>

namespace mozilla {

// Manages the Chunks for this process in a thread-safe manner, with a maximum
// size per process.
//
// "Unreleased" chunks are not owned here, only "released" chunks can be
// destroyed or recycled when reaching the memory limit, so it is theoretically
// possible to break that limit, if:
// - The user of this class doesn't release their chunks, AND/OR
// - The limit is too small (e.g., smaller than 2 or 3 chunks, which should be
//   the usual number of unreleased chunks in flight).
// In this case, it just means that we will use more memory than allowed,
// potentially risking OOMs. Hopefully this shouldn't happen in real code,
// assuming that the user is doing the right thing and releasing chunks ASAP,
// and that the memory limit is reasonably large.
class ProfileBufferChunkManagerWithLocalLimit final
    : public ProfileBufferChunkManager,
      public ProfileBufferControlledChunkManager {
 public:
  using Length = ProfileBufferChunk::Length;

  // MaxTotalBytes: Maximum number of bytes allocated in all local Chunks.
  // ChunkMinBufferBytes: Minimum number of user-available bytes in each Chunk.
  // Note that Chunks use a bit more memory for their header.
  explicit ProfileBufferChunkManagerWithLocalLimit(size_t aMaxTotalBytes,
                                                   Length aChunkMinBufferBytes)
      : mMaxTotalBytes(aMaxTotalBytes),
        mChunkMinBufferBytes(aChunkMinBufferBytes) {}

  ~ProfileBufferChunkManagerWithLocalLimit() {
    if (mUpdateCallback) {
      // Signal the end of this callback.
      std::move(mUpdateCallback)(Update(nullptr));
    }
  }

  [[nodiscard]] size_t MaxTotalSize() const final {
    // `mMaxTotalBytes` is `const` so there is no need to lock the mutex.
    return mMaxTotalBytes;
  }

  [[nodiscard]] size_t TotalSize() const { return mTotalBytes; }

  [[nodiscard]] UniquePtr<ProfileBufferChunk> GetChunk() final {
    AUTO_PROFILER_STATS(Local_GetChunk);

    ChunkAndUpdate chunkAndUpdate = [&]() {
      baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
      return GetChunk(lock);
    }();

    baseprofiler::detail::BaseProfilerAutoLock lock(mUpdateCallbackMutex);
    if (mUpdateCallback && !chunkAndUpdate.second.IsNotUpdate()) {
      mUpdateCallback(std::move(chunkAndUpdate.second));
    }

    return std::move(chunkAndUpdate.first);
  }

  void RequestChunk(std::function<void(UniquePtr<ProfileBufferChunk>)>&&
                        aChunkReceiver) final {
    AUTO_PROFILER_STATS(Local_RequestChunk);
    baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
    if (mChunkReceiver) {
      // We already have a chunk receiver, meaning a request is pending.
      return;
    }
    // Store the chunk receiver. This indicates that a request is pending, and
    // it will be handled in the next `FulfillChunkRequests()` call.
    mChunkReceiver = std::move(aChunkReceiver);
  }

  void FulfillChunkRequests() final {
    AUTO_PROFILER_STATS(Local_FulfillChunkRequests);
    std::function<void(UniquePtr<ProfileBufferChunk>)> chunkReceiver;
    ChunkAndUpdate chunkAndUpdate = [&]() -> ChunkAndUpdate {
      baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
      if (!mChunkReceiver) {
        // No receiver means no pending request, we're done.
        return {};
      }
      // Otherwise there is a request, extract the receiver to call below.
      std::swap(chunkReceiver, mChunkReceiver);
      MOZ_ASSERT(!mChunkReceiver, "mChunkReceiver should have been emptied");
      // And allocate the requested chunk. This may fail, it's fine, we're
      // letting the receiver know about it.
      AUTO_PROFILER_STATS(Local_FulfillChunkRequests_GetChunk);
      return GetChunk(lock);
    }();

    if (chunkReceiver) {
      {
        baseprofiler::detail::BaseProfilerAutoLock lock(mUpdateCallbackMutex);
        if (mUpdateCallback && !chunkAndUpdate.second.IsNotUpdate()) {
          mUpdateCallback(std::move(chunkAndUpdate.second));
        }
      }

      // Invoke callback outside of lock, so that it can use other chunk manager
      // functions if needed.
      // Note that this means there could be a race, where another request
      // happens now and even gets fulfilled before this one is! It should be
      // rare, and shouldn't be a problem anyway, the user will still get their
      // requested chunks, new/recycled chunks look the same so their order
      // doesn't matter.
      std::move(chunkReceiver)(std::move(chunkAndUpdate.first));
    }
  }

  void ReleaseChunk(UniquePtr<ProfileBufferChunk> aChunk) final {
    if (!aChunk) {
      return;
    }

    MOZ_RELEASE_ASSERT(!aChunk->GetNext(), "ReleaseChunk only accepts 1 chunk");
    MOZ_RELEASE_ASSERT(!aChunk->ChunkHeader().mDoneTimeStamp.IsNull(),
                       "Released chunk should have a 'Done' timestamp");

    Update update = [&]() {
      baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
      MOZ_ASSERT(mUser, "Not registered yet");
      // Keep a pointer to the first newly-released chunk, so we can use it to
      // prepare an update (after `aChunk` is moved-from).
      const ProfileBufferChunk* const newlyReleasedChunk = aChunk.get();
      // Transfer the chunk size from the unreleased bucket to the released one.
      mUnreleasedBufferBytes -= aChunk->BufferBytes();
      mReleasedBufferBytes += aChunk->BufferBytes();
      if (!mReleasedChunks) {
        // No other released chunks at the moment, we're starting the list.
        MOZ_ASSERT(mReleasedBufferBytes == aChunk->BufferBytes());
        mReleasedChunks = std::move(aChunk);
      } else {
        // Insert aChunk in mReleasedChunks to keep done-timestamp order.
        const TimeStamp& releasedChunkDoneTimeStamp =
            aChunk->ChunkHeader().mDoneTimeStamp;
        if (releasedChunkDoneTimeStamp <
            mReleasedChunks->ChunkHeader().mDoneTimeStamp) {
          // aChunk is the oldest -> Insert at the beginning.
          aChunk->SetLast(std::move(mReleasedChunks));
          mReleasedChunks = std::move(aChunk);
        } else {
          // Go through the already-released chunk list, and insert aChunk
          // before the first younger released chunk, or at the end.
          ProfileBufferChunk* chunk = mReleasedChunks.get();
          for (;;) {
            ProfileBufferChunk* const nextChunk = chunk->GetNext();
            if (!nextChunk || releasedChunkDoneTimeStamp <
                                  nextChunk->ChunkHeader().mDoneTimeStamp) {
              // Either we're at the last released chunk, or the next released
              // chunk is younger -> Insert right after this released chunk.
              chunk->InsertNext(std::move(aChunk));
              break;
            }
            chunk = nextChunk;
          }
        }
      }

      return Update(mUnreleasedBufferBytes, mReleasedBufferBytes,
                    mReleasedChunks.get(), newlyReleasedChunk);
    }();

    baseprofiler::detail::BaseProfilerAutoLock lock(mUpdateCallbackMutex);
    if (mUpdateCallback && !update.IsNotUpdate()) {
      mUpdateCallback(std::move(update));
    }
  }

  void SetChunkDestroyedCallback(
      std::function<void(const ProfileBufferChunk&)>&& aChunkDestroyedCallback)
      final {
    baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
    MOZ_ASSERT(mUser, "Not registered yet");
    mChunkDestroyedCallback = std::move(aChunkDestroyedCallback);
  }

  [[nodiscard]] UniquePtr<ProfileBufferChunk> GetExtantReleasedChunks() final {
    UniquePtr<ProfileBufferChunk> chunks;
    size_t unreleasedBufferBytes = [&]() {
      baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
      MOZ_ASSERT(mUser, "Not registered yet");
      mReleasedBufferBytes = 0;
      chunks = std::move(mReleasedChunks);
      return mUnreleasedBufferBytes;
    }();

    baseprofiler::detail::BaseProfilerAutoLock lock(mUpdateCallbackMutex);
    if (mUpdateCallback) {
      mUpdateCallback(Update(unreleasedBufferBytes, 0, nullptr, nullptr));
    }

    return chunks;
  }

  void ForgetUnreleasedChunks() final {
    Update update = [&]() {
      baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
      MOZ_ASSERT(mUser, "Not registered yet");
      mUnreleasedBufferBytes = 0;
      return Update(0, mReleasedBufferBytes, mReleasedChunks.get(), nullptr);
    }();
    baseprofiler::detail::BaseProfilerAutoLock lock(mUpdateCallbackMutex);
    if (mUpdateCallback) {
      mUpdateCallback(std::move(update));
    }
  }

  [[nodiscard]] size_t SizeOfExcludingThis(
      MallocSizeOf aMallocSizeOf) const final {
    baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
    return SizeOfExcludingThis(aMallocSizeOf, lock);
  }

  [[nodiscard]] size_t SizeOfIncludingThis(
      MallocSizeOf aMallocSizeOf) const final {
    baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
    MOZ_ASSERT(mUser, "Not registered yet");
    return aMallocSizeOf(this) + SizeOfExcludingThis(aMallocSizeOf, lock);
  }

  void SetUpdateCallback(UpdateCallback&& aUpdateCallback) final {
    {
      baseprofiler::detail::BaseProfilerAutoLock lock(mUpdateCallbackMutex);
      if (mUpdateCallback) {
        // Signal the end of the previous callback.
        std::move(mUpdateCallback)(Update(nullptr));
        mUpdateCallback = nullptr;
      }
    }

    if (aUpdateCallback) {
      Update initialUpdate = [&]() {
        baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
        return Update(mUnreleasedBufferBytes, mReleasedBufferBytes,
                      mReleasedChunks.get(), nullptr);
      }();

      baseprofiler::detail::BaseProfilerAutoLock lock(mUpdateCallbackMutex);
      MOZ_ASSERT(!mUpdateCallback, "Only one update callback allowed");
      mUpdateCallback = std::move(aUpdateCallback);
      mUpdateCallback(std::move(initialUpdate));
    }
  }

  void DestroyChunksAtOrBefore(TimeStamp aDoneTimeStamp) final {
    MOZ_ASSERT(!aDoneTimeStamp.IsNull());
    baseprofiler::detail::BaseProfilerAutoLock lock(mMutex);
    for (;;) {
      if (!mReleasedChunks) {
        // We don't own any released chunks (anymore), we're done.
        break;
      }
      if (mReleasedChunks->ChunkHeader().mDoneTimeStamp > aDoneTimeStamp) {
        // The current chunk is strictly after the given timestamp, we're done.
        break;
      }
      // We've found a chunk at or before the timestamp, discard it.
      DiscardOldestReleasedChunk(lock);
    }
  }

 protected:
  const ProfileBufferChunk* PeekExtantReleasedChunksAndLock() final
      MOZ_CAPABILITY_ACQUIRE(mMutex) {
    mMutex.Lock();
    MOZ_ASSERT(mUser, "Not registered yet");
    return mReleasedChunks.get();
  }
  void UnlockAfterPeekExtantReleasedChunks() final
      MOZ_CAPABILITY_RELEASE(mMutex) {
    mMutex.Unlock();
  }

 private:
  size_t MaybeRecycleChunkAndGetDeallocatedSize(
      UniquePtr<ProfileBufferChunk>&& chunk,
      const baseprofiler::detail::BaseProfilerAutoLock& aLock) {
    // Try to recycle big-enough chunks. (All chunks should have the same size,
    // but it's a cheap test and may allow future adjustments based on actual
    // data rate.)
    if (chunk->BufferBytes() >= mChunkMinBufferBytes) {
      // We keep up to two recycled chunks at any time.
      if (!mRecycledChunks) {
        mRecycledChunks = std::move(chunk);
        return 0;
      } else if (!mRecycledChunks->GetNext()) {
        mRecycledChunks->InsertNext(std::move(chunk));
        return 0;
      }
    }
    return moz_malloc_usable_size(chunk.get());
  }

  UniquePtr<ProfileBufferChunk> TakeRecycledChunk(
      const baseprofiler::detail::BaseProfilerAutoLock& aLock) {
    UniquePtr<ProfileBufferChunk> recycled;
    if (mRecycledChunks) {
      recycled = std::exchange(mRecycledChunks, mRecycledChunks->ReleaseNext());
      recycled->MarkRecycled();
    }
    return recycled;
  }

  void DiscardOldestReleasedChunk(
      const baseprofiler::detail::BaseProfilerAutoLock& aLock) {
    MOZ_ASSERT(!!mReleasedChunks);
    UniquePtr<ProfileBufferChunk> oldest =
        std::exchange(mReleasedChunks, mReleasedChunks->ReleaseNext());
    mReleasedBufferBytes -= oldest->BufferBytes();
    if (mChunkDestroyedCallback) {
      // Inform the user that we're going to destroy this chunk.
      mChunkDestroyedCallback(*oldest);
    }

    mTotalBytes -=
        MaybeRecycleChunkAndGetDeallocatedSize(std::move(oldest), aLock);
  }

  using ChunkAndUpdate = std::pair<UniquePtr<ProfileBufferChunk>, Update>;
  [[nodiscard]] ChunkAndUpdate GetChunk(
      const baseprofiler::detail::BaseProfilerAutoLock& aLock) {
    MOZ_ASSERT(mUser, "Not registered yet");
    // After this function, the total memory consumption will be the sum of:
    // - Bytes from released (i.e., full) chunks,
    // - Bytes from unreleased (still in use) chunks,
    // - Bytes from the chunk we want to create/recycle. (Note that we don't
    //   count the extra bytes of chunk header, and of extra allocation ability,
    //   for the new chunk, as it's assumed to be negligible compared to the
    //   total memory limit.)
    // If this total is higher than the local limit, we'll want to destroy
    // the oldest released chunks until we're under the limit; if any, we may
    // recycle one of them to avoid a deallocation followed by an allocation.
    while (mReleasedBufferBytes + mUnreleasedBufferBytes +
                   mChunkMinBufferBytes >=
               mMaxTotalBytes &&
           !!mReleasedChunks) {
      // We have reached the local limit, discard the oldest released chunk.
      DiscardOldestReleasedChunk(aLock);
    }

    // Extract the recycled chunk, if any.
    ChunkAndUpdate chunkAndUpdate{TakeRecycledChunk(aLock), Update()};
    UniquePtr<ProfileBufferChunk>& chunk = chunkAndUpdate.first;

    if (!chunk) {
      // No recycled chunk -> Create a chunk now. (This could still fail.)
      chunk = ProfileBufferChunk::Create(mChunkMinBufferBytes);
      mTotalBytes += moz_malloc_usable_size(chunk.get());
    }

    if (chunk) {
      // We do have a chunk (recycled or new), record its size as "unreleased".
      mUnreleasedBufferBytes += chunk->BufferBytes();

      chunkAndUpdate.second =
          Update(mUnreleasedBufferBytes, mReleasedBufferBytes,
                 mReleasedChunks.get(), nullptr);
    }

    return chunkAndUpdate;
  }

  [[nodiscard]] size_t SizeOfExcludingThis(
      MallocSizeOf aMallocSizeOf,
      const baseprofiler::detail::BaseProfilerAutoLock&) const {
    MOZ_ASSERT(mUser, "Not registered yet");
    size_t size = 0;
    if (mReleasedChunks) {
      size += mReleasedChunks->SizeOfIncludingThis(aMallocSizeOf);
    }
    if (mRecycledChunks) {
      size += mRecycledChunks->SizeOfIncludingThis(aMallocSizeOf);
    }
    // Note: Missing size of std::function external resources (if any).
    return size;
  }

  // Maxumum number of bytes that should be used by all unreleased and released
  // chunks. Note that only released chunks can be destroyed here, so it is the
  // responsibility of the user to properly release their chunks when possible.
  const size_t mMaxTotalBytes;

  // Minimum number of bytes that new chunks should be able to store.
  // Used when calling `ProfileBufferChunk::Create()`.
  const Length mChunkMinBufferBytes;

  // Mutex guarding the following members.
  mutable baseprofiler::detail::BaseProfilerMutex mMutex;

  // Number of bytes currently held in chunks that have been given away (through
  // `GetChunk` or `RequestChunk`) and not released yet.
  size_t mUnreleasedBufferBytes = 0;

  // Number of bytes currently held in chunks that have been released and stored
  // in `mReleasedChunks` below.
  size_t mReleasedBufferBytes = 0;

  // Total allocated size (used to substract it from memory counters).
  size_t mTotalBytes = 0;

  // List of all released chunks. The oldest one should be at the start of the
  // list, and may be destroyed or recycled when the memory limit is reached.
  UniquePtr<ProfileBufferChunk> mReleasedChunks;

  // This may hold chunks that were released then slated for destruction, they
  // will be reused next time an allocation would have been needed.
  UniquePtr<ProfileBufferChunk> mRecycledChunks;

  // Optional callback used to notify the user when a chunk is about to be
  // destroyed or recycled. (The data content is always destroyed, but the chunk
  // container may be reused.)
  std::function<void(const ProfileBufferChunk&)> mChunkDestroyedCallback;

  // Callback set from `RequestChunk()`, until it is serviced in
  // `FulfillChunkRequests()`. There can only be one request in flight.
  std::function<void(UniquePtr<ProfileBufferChunk>)> mChunkReceiver;

  // Separate mutex guarding mUpdateCallback, so that it may be invoked outside
  // of the main buffer `mMutex`.
  mutable baseprofiler::detail::BaseProfilerMutex mUpdateCallbackMutex;

  UpdateCallback mUpdateCallback;
};

}  // namespace mozilla

#endif  // ProfileBufferChunkManagerWithLocalLimit_h

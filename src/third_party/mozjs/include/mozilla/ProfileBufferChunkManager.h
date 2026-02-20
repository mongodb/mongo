/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileBufferChunkManager_h
#define ProfileBufferChunkManager_h

#include "mozilla/ProfileBufferChunk.h"
#include "mozilla/ScopeExit.h"

#include <functional>

namespace mozilla {

// Manages the ProfileBufferChunks for this process.
// The main user of this class is the buffer that needs chunks to store its
// data.
// The main ProfileBufferChunks responsibilities are:
// - It can create new chunks, they are called "unreleased".
// - Later these chunks are returned here, and become "released".
// - The manager is free to destroy or recycle the oldest released chunks
//   (usually to reclaim memory), and will inform the user through a provided
//   callback.
// - The user may access still-alive released chunks.
class ProfileBufferChunkManager {
 public:
  virtual ~ProfileBufferChunkManager()
#ifdef DEBUG
  {
    MOZ_ASSERT(!mUser, "Still registered when being destroyed");
  }
#else
      = default;
#endif

  // Expected maximum size needed to store one stack sample.
  // Most ChunkManager sub-classes will require chunk sizes, this can serve as
  // a minimum recommendation to hold most backtraces.
  constexpr static ProfileBufferChunk::Length scExpectedMaximumStackSize =
      128 * 1024;

  // Estimated maximum buffer size.
  [[nodiscard]] virtual size_t MaxTotalSize() const = 0;

  // Create or recycle a chunk right now. May return null in case of allocation
  // failure.
  // Note that the chunk-destroyed callback may be invoked during this call;
  // user should be careful with reentrancy issues.
  [[nodiscard]] virtual UniquePtr<ProfileBufferChunk> GetChunk() = 0;

  // `aChunkReceiver` may be called with a new or recycled chunk, or nullptr.
  // (See `FulfillChunkRequests()` regarding when the callback may happen.)
  virtual void RequestChunk(
      std::function<void(UniquePtr<ProfileBufferChunk>)>&& aChunkReceiver) = 0;

  // This method may be invoked at any time on any thread (and not necessarily
  // by the main user of this class), to do the work necessary to respond to a
  // previous `RequestChunk()`.
  // It is optional: If it is never called, or called too late, the user is
  // responsible for directly calling `GetChunk()` when a chunk is really
  // needed (or it should at least fail gracefully).
  // The idea is to fulfill chunk request on a separate thread, and most
  // importantly outside of profiler calls, to avoid doing expensive memory
  // allocations during these calls.
  virtual void FulfillChunkRequests() = 0;

  // One chunk is released by the user, the ProfileBufferChunkManager should
  // keep it as long as possible (depending on local or global memory/time
  // limits). Note that the chunk-destroyed callback may be invoked during this
  // call; user should be careful with reentrancy issues.
  virtual void ReleaseChunk(UniquePtr<ProfileBufferChunk> aChunk) = 0;

  // `aChunkDestroyedCallback` will be called whenever the contents of a
  // previously-released chunk is about to be destroyed or recycled.
  // Note that it may be called during other functions above, or at other times
  // from the same or other threads; user should be careful with reentrancy
  // issues.
  virtual void SetChunkDestroyedCallback(
      std::function<void(const ProfileBufferChunk&)>&&
          aChunkDestroyedCallback) = 0;

  // Give away all released chunks that have not yet been destroyed.
  [[nodiscard]] virtual UniquePtr<ProfileBufferChunk>
  GetExtantReleasedChunks() = 0;

  // Let a callback see all released chunks that have not yet been destroyed, if
  // any. Return whatever the callback returns.
  template <typename Callback>
  [[nodiscard]] auto PeekExtantReleasedChunks(Callback&& aCallback) {
    const ProfileBufferChunk* chunks = PeekExtantReleasedChunksAndLock();
    auto unlock =
        MakeScopeExit([&]() { UnlockAfterPeekExtantReleasedChunks(); });
    return std::forward<Callback>(aCallback)(chunks);
  }

  // Chunks that were still unreleased will never be released.
  virtual void ForgetUnreleasedChunks() = 0;

  [[nodiscard]] virtual size_t SizeOfExcludingThis(
      MallocSizeOf aMallocSizeOf) const = 0;
  [[nodiscard]] virtual size_t SizeOfIncludingThis(
      MallocSizeOf aMallocSizeOf) const = 0;

 protected:
  // Derived classes to implement `PeekExtantReleasedChunks` through these:
  virtual const ProfileBufferChunk* PeekExtantReleasedChunksAndLock() = 0;
  virtual void UnlockAfterPeekExtantReleasedChunks() = 0;

#ifdef DEBUG
 public:
  // DEBUG checks ensuring that this manager and its users avoid UAFs.
  // Derived classes should assert that mUser is not null in their functions.

  void RegisteredWith(const void* aUser) {
    MOZ_ASSERT(!mUser);
    MOZ_ASSERT(aUser);
    mUser = aUser;
  }

  void DeregisteredFrom(const void* aUser) {
    MOZ_ASSERT(mUser == aUser);
    mUser = nullptr;
  }

 protected:
  const void* mUser = nullptr;
#endif  // DEBUG
};

}  // namespace mozilla

#endif  // ProfileBufferChunkManager_h

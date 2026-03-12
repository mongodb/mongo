/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileBufferChunkManagerSingle_h
#define ProfileBufferChunkManagerSingle_h

#include "mozilla/ProfileBufferChunkManager.h"

#ifdef DEBUG
#  include "mozilla/Atomics.h"
#endif  // DEBUG

namespace mozilla {

// Manages only one Chunk.
// The first call to `Get`/`RequestChunk()` will retrieve the one chunk, and all
// subsequent calls will return nullptr. That chunk may still be released, but
// it will never be destroyed or recycled.
// Unlike others, this manager may be `Reset()`, to allow another round of
// small-data gathering.
// The main use is with short-lived ProfileChunkedBuffers that collect little
// data that can fit in one chunk, e.g., capturing one stack.
// It is not thread-safe.
class ProfileBufferChunkManagerSingle final : public ProfileBufferChunkManager {
 public:
  using Length = ProfileBufferChunk::Length;

  // Use a preallocated chunk. (Accepting null to gracefully handle OOM.)
  explicit ProfileBufferChunkManagerSingle(UniquePtr<ProfileBufferChunk> aChunk)
      : mInitialChunk(std::move(aChunk)),
        mBufferBytes(mInitialChunk ? mInitialChunk->BufferBytes() : 0) {
    MOZ_ASSERT(!mInitialChunk || !mInitialChunk->GetNext(),
               "Expected at most one chunk");
  }

  // ChunkMinBufferBytes: Minimum number of user-available bytes in the Chunk.
  // Note that Chunks use a bit more memory for their header.
  explicit ProfileBufferChunkManagerSingle(Length aChunkMinBufferBytes)
      : mInitialChunk(ProfileBufferChunk::Create(aChunkMinBufferBytes)),
        mBufferBytes(mInitialChunk ? mInitialChunk->BufferBytes() : 0) {}

#ifdef DEBUG
  ~ProfileBufferChunkManagerSingle() { MOZ_ASSERT(mVirtuallyLocked == false); }
#endif  // DEBUG

  // Reset this manager, using the provided chunk (probably coming from the
  // ProfileChunkedBuffer that just used it); if null, fallback on current or
  // released chunk.
  void Reset(UniquePtr<ProfileBufferChunk> aPossibleChunk) {
    if (aPossibleChunk) {
      mInitialChunk = std::move(aPossibleChunk);
      mReleasedChunk = nullptr;
    } else if (!mInitialChunk) {
      MOZ_ASSERT(!!mReleasedChunk, "Can't reset properly!");
      mInitialChunk = std::move(mReleasedChunk);
    }

    if (mInitialChunk) {
      mInitialChunk->MarkRecycled();
      mBufferBytes = mInitialChunk->BufferBytes();
    } else {
      mBufferBytes = 0;
    }
  }

  [[nodiscard]] size_t MaxTotalSize() const final { return mBufferBytes; }

  // One of `GetChunk` and `RequestChunk` will only work the very first time (if
  // there's even a chunk).
  [[nodiscard]] UniquePtr<ProfileBufferChunk> GetChunk() final {
    MOZ_ASSERT(mUser, "Not registered yet");
    return std::move(mInitialChunk);
  }

  void RequestChunk(std::function<void(UniquePtr<ProfileBufferChunk>)>&&
                        aChunkReceiver) final {
    MOZ_ASSERT(mUser, "Not registered yet");
    // Simple retrieval.
    std::move(aChunkReceiver)(GetChunk());
  }

  void FulfillChunkRequests() final {
    // Nothing to do here.
  }

  void ReleaseChunk(UniquePtr<ProfileBufferChunk> aChunk) final {
    MOZ_ASSERT(mUser, "Not registered yet");
    if (!aChunk) {
      return;
    }
    MOZ_ASSERT(!mReleasedChunk, "Unexpected 2nd released chunk");
    MOZ_ASSERT(!aChunk->GetNext(), "Only expected one released chunk");
    mReleasedChunk = std::move(aChunk);
  }

  void SetChunkDestroyedCallback(
      std::function<void(const ProfileBufferChunk&)>&& aChunkDestroyedCallback)
      final {
    MOZ_ASSERT(mUser, "Not registered yet");
    // The chunk-destroyed callback will never actually be called, but we keep
    // the callback here in case the caller expects it to live as long as this
    // manager.
    mChunkDestroyedCallback = std::move(aChunkDestroyedCallback);
  }

  [[nodiscard]] UniquePtr<ProfileBufferChunk> GetExtantReleasedChunks() final {
    MOZ_ASSERT(mUser, "Not registered yet");
    return std::move(mReleasedChunk);
  }

  void ForgetUnreleasedChunks() final {
    MOZ_ASSERT(mUser, "Not registered yet");
  }

  [[nodiscard]] size_t SizeOfExcludingThis(
      MallocSizeOf aMallocSizeOf) const final {
    MOZ_ASSERT(mUser, "Not registered yet");
    size_t size = 0;
    if (mInitialChunk) {
      size += mInitialChunk->SizeOfIncludingThis(aMallocSizeOf);
    }
    if (mReleasedChunk) {
      size += mReleasedChunk->SizeOfIncludingThis(aMallocSizeOf);
    }
    // Note: Missing size of std::function external resources (if any).
    return size;
  }

  [[nodiscard]] size_t SizeOfIncludingThis(
      MallocSizeOf aMallocSizeOf) const final {
    MOZ_ASSERT(mUser, "Not registered yet");
    return aMallocSizeOf(this) + SizeOfExcludingThis(aMallocSizeOf);
  }

 protected:
  // This manager is not thread-safe, so there's not actual locking needed.
  const ProfileBufferChunk* PeekExtantReleasedChunksAndLock() final {
    MOZ_ASSERT(mVirtuallyLocked.compareExchange(false, true));
    MOZ_ASSERT(mUser, "Not registered yet");
    return mReleasedChunk.get();
  }
  void UnlockAfterPeekExtantReleasedChunks() final {
    MOZ_ASSERT(mVirtuallyLocked.compareExchange(true, false));
  }

 private:
  // Initial chunk created with this manager, given away at first Get/Request.
  UniquePtr<ProfileBufferChunk> mInitialChunk;

  // Storage for the released chunk (which should probably not happen, as it
  // means the chunk is full).
  UniquePtr<ProfileBufferChunk> mReleasedChunk;

  // Size of the one chunk we're managing. Stored here, because the chunk may
  // be moved out and inaccessible from here.
  Length mBufferBytes;

  // The chunk-destroyed callback will never actually be called, but we keep it
  // here in case the caller expects it to live as long as this manager.
  std::function<void(const ProfileBufferChunk&)> mChunkDestroyedCallback;

#ifdef DEBUG
  mutable Atomic<bool> mVirtuallyLocked{false};
#endif  // DEBUG
};

}  // namespace mozilla

#endif  // ProfileBufferChunkManagerSingle_h

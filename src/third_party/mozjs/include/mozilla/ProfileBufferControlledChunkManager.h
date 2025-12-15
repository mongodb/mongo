/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileBufferControlledChunkManager_h
#define ProfileBufferControlledChunkManager_h

#include "mozilla/ProfileBufferChunk.h"

#include <functional>
#include <vector>

namespace mozilla {

// A "Controlled" chunk manager will provide updates about chunks that it
// creates, releases, and destroys; and it can destroy released chunks as
// requested.
class ProfileBufferControlledChunkManager {
 public:
  using Length = ProfileBufferChunk::Length;

  virtual ~ProfileBufferControlledChunkManager() = default;

  // Minimum amount of chunk metadata to be transferred between processes.
  struct ChunkMetadata {
    // Timestamp when chunk was marked "done", which is used to:
    // - determine its age, so the oldest one will be destroyed first,
    // - uniquely identify this chunk in this process. (The parent process is
    //   responsible for associating this timestamp to its process id.)
    TimeStamp mDoneTimeStamp;
    // Size of this chunk's buffer.
    Length mBufferBytes;

    ChunkMetadata(TimeStamp aDoneTimeStamp, Length aBufferBytes)
        : mDoneTimeStamp(aDoneTimeStamp), mBufferBytes(aBufferBytes) {}
  };

  // Class collecting all information necessary to describe updates that
  // happened in a chunk manager.
  // An update can be folded into a previous update.
  class Update {
   public:
    // Construct a "not-an-Update" object, which should only be used after a
    // real update is folded into it.
    Update() = default;

    // Construct a "final" Update, which marks the end of all updates from a
    // chunk manager.
    explicit Update(decltype(nullptr)) : mUnreleasedBytes(FINAL) {}

    // Construct an Update from the given data and released chunks.
    // The chunk pointers may be null, and it doesn't matter if
    // `aNewlyReleasedChunks` is already linked to `aExistingReleasedChunks` or
    // not.
    Update(size_t aUnreleasedBytes, size_t aReleasedBytes,
           const ProfileBufferChunk* aExistingReleasedChunks,
           const ProfileBufferChunk* aNewlyReleasedChunks)
        : mUnreleasedBytes(aUnreleasedBytes),
          mReleasedBytes(aReleasedBytes),
          mOldestDoneTimeStamp(
              aExistingReleasedChunks
                  ? aExistingReleasedChunks->ChunkHeader().mDoneTimeStamp
                  : TimeStamp{}) {
      MOZ_RELEASE_ASSERT(
          !IsNotUpdate(),
          "Empty update should only be constructed with default constructor");
      MOZ_RELEASE_ASSERT(
          !IsFinal(),
          "Final update should only be constructed with nullptr constructor");
      for (const ProfileBufferChunk* chunk = aNewlyReleasedChunks; chunk;
           chunk = chunk->GetNext()) {
        mNewlyReleasedChunks.emplace_back(ChunkMetadata{
            chunk->ChunkHeader().mDoneTimeStamp, chunk->BufferBytes()});
      }
    }

    // Construct an Update from raw data.
    // This may be used to re-construct an Update that was previously
    // serialized.
    Update(size_t aUnreleasedBytes, size_t aReleasedBytes,
           TimeStamp aOldestDoneTimeStamp,
           std::vector<ChunkMetadata>&& aNewlyReleasedChunks)
        : mUnreleasedBytes(aUnreleasedBytes),
          mReleasedBytes(aReleasedBytes),
          mOldestDoneTimeStamp(aOldestDoneTimeStamp),
          mNewlyReleasedChunks(std::move(aNewlyReleasedChunks)) {}

    // Clear the Update completely and return it to a "not-an-Update" state.
    void Clear() {
      mUnreleasedBytes = NO_UPDATE;
      mReleasedBytes = 0;
      mOldestDoneTimeStamp = TimeStamp{};
      mNewlyReleasedChunks.clear();
    }

    bool IsNotUpdate() const { return mUnreleasedBytes == NO_UPDATE; }

    bool IsFinal() const { return mUnreleasedBytes == FINAL; }

    size_t UnreleasedBytes() const {
      MOZ_RELEASE_ASSERT(!IsNotUpdate(),
                         "Cannot access UnreleasedBytes from empty update");
      MOZ_RELEASE_ASSERT(!IsFinal(),
                         "Cannot access UnreleasedBytes from final update");
      return mUnreleasedBytes;
    }

    size_t ReleasedBytes() const {
      MOZ_RELEASE_ASSERT(!IsNotUpdate(),
                         "Cannot access ReleasedBytes from empty update");
      MOZ_RELEASE_ASSERT(!IsFinal(),
                         "Cannot access ReleasedBytes from final update");
      return mReleasedBytes;
    }

    TimeStamp OldestDoneTimeStamp() const {
      MOZ_RELEASE_ASSERT(!IsNotUpdate(),
                         "Cannot access OldestDoneTimeStamp from empty update");
      MOZ_RELEASE_ASSERT(!IsFinal(),
                         "Cannot access OldestDoneTimeStamp from final update");
      return mOldestDoneTimeStamp;
    }

    const std::vector<ChunkMetadata>& NewlyReleasedChunksRef() const {
      MOZ_RELEASE_ASSERT(
          !IsNotUpdate(),
          "Cannot access NewlyReleasedChunksRef from empty update");
      MOZ_RELEASE_ASSERT(
          !IsFinal(), "Cannot access NewlyReleasedChunksRef from final update");
      return mNewlyReleasedChunks;
    }

    // Fold a later update into this one.
    void Fold(Update&& aNewUpdate) {
      MOZ_ASSERT(
          !IsFinal() || aNewUpdate.IsFinal(),
          "There shouldn't be another non-final update after the final update");

      if (IsNotUpdate() || aNewUpdate.IsFinal()) {
        // We were empty, or the new update is the final update, we just switch
        // to that new update.
        *this = std::move(aNewUpdate);
        return;
      }

      mUnreleasedBytes = aNewUpdate.mUnreleasedBytes;
      mReleasedBytes = aNewUpdate.mReleasedBytes;
      if (!aNewUpdate.mOldestDoneTimeStamp.IsNull()) {
        MOZ_ASSERT(mOldestDoneTimeStamp.IsNull() ||
                   mOldestDoneTimeStamp <= aNewUpdate.mOldestDoneTimeStamp);
        mOldestDoneTimeStamp = aNewUpdate.mOldestDoneTimeStamp;
        auto it = mNewlyReleasedChunks.begin();
        while (it != mNewlyReleasedChunks.end() &&
               it->mDoneTimeStamp < mOldestDoneTimeStamp) {
          it = mNewlyReleasedChunks.erase(it);
        }
      }
      if (!aNewUpdate.mNewlyReleasedChunks.empty()) {
        mNewlyReleasedChunks.reserve(mNewlyReleasedChunks.size() +
                                     aNewUpdate.mNewlyReleasedChunks.size());
        mNewlyReleasedChunks.insert(mNewlyReleasedChunks.end(),
                                    aNewUpdate.mNewlyReleasedChunks.begin(),
                                    aNewUpdate.mNewlyReleasedChunks.end());
      }
    }

   private:
    static const size_t NO_UPDATE = size_t(-1);
    static const size_t FINAL = size_t(-2);

    size_t mUnreleasedBytes = NO_UPDATE;
    size_t mReleasedBytes = 0;
    TimeStamp mOldestDoneTimeStamp;
    std::vector<ChunkMetadata> mNewlyReleasedChunks;
  };

  using UpdateCallback = std::function<void(Update&&)>;

  // This *may* be set (or reset) by an object that needs to know about all
  // chunk updates that happen in this manager. The main use will be to
  // coordinate the global memory usage of Firefox.
  // If a non-empty callback is given, it will be immediately invoked with the
  // current state.
  // When the callback is about to be destroyed (by overwriting it here, or in
  // the class destructor), it will be invoked one last time with an empty
  // update.
  // Note that the callback (even the first current-state callback) will be
  // invoked from inside a locked scope, so it should *not* call other functions
  // of the chunk manager. A side benefit of this locking is that it guarantees
  // that no two invocations can overlap.
  virtual void SetUpdateCallback(UpdateCallback&& aUpdateCallback) = 0;

  // This is a request to destroy all chunks before the given timestamp.
  // This timestamp should be one that was given in a previous UpdateCallback
  // call. Obviously, only released chunks can be destroyed.
  virtual void DestroyChunksAtOrBefore(TimeStamp aDoneTimeStamp) = 0;
};

}  // namespace mozilla

#endif  // ProfileBufferControlledChunkManager_h

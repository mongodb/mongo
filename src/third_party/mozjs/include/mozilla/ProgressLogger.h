/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProgressLogger_h
#define ProgressLogger_h

#include "mozilla/Assertions.h"
#include "mozilla/ProportionValue.h"
#include "mozilla/RefCounted.h"
#include "mozilla/RefPtr.h"

#include <atomic>

// Uncomment to printf ProcessLogger updates.
// #define DEBUG_PROCESSLOGGER

#ifdef DEBUG_PROCESSLOGGER
#  include "mozilla/BaseProfilerUtils.h"
#  include <cstdio>
#endif  // DEBUG_PROCESSLOGGER

namespace mozilla {

// A `ProgressLogger` is used to update a referenced atomic `ProportionValue`,
// and can recursively create a sub-logger corresponding to a subset of their
// own range, but that sub-logger's updates are done in its local 0%-100% range.
// The typical usage is for multi-level tasks, where each level can estimate its
// own work and the work delegated to a next-level function, without knowing how
// this local work relates to the higher-level total work. See
// `CreateSubLoggerFromTo` for details.
// Note that this implementation is single-threaded, it does not support logging
// progress from multiple threads at the same time.
class ProgressLogger {
 public:
  // An RefPtr'd object of this class is used as the target of all
  // ProgressLogger updates, and it may be shared to make these updates visible
  // from other code in any thread.
  class SharedProgress : public external::AtomicRefCounted<SharedProgress> {
   public:
    MOZ_DECLARE_REFCOUNTED_TYPENAME(SharedProgress)

    SharedProgress() = default;

    SharedProgress(const SharedProgress&) = delete;
    SharedProgress& operator=(const SharedProgress&) = delete;

    // This constant is used to indicate that an update may change the progress
    // value, but should not modify the previously-recorded location.
    static constexpr const char* NO_LOCATION_UPDATE = nullptr;

    // Set the current progress and location, but the previous location is not
    // overwritten if the new one is null or empty.
    // The location and then the progress are atomically "released", so that all
    // preceding writes on this thread will be visible to other threads reading
    // these values; most importantly when reaching 100% progress, the reader
    // can be confident that the location is final and the operation being
    // watched has completed.
    void SetProgress(
        ProportionValue aProgress,
        const char* aLocationOrNullEmptyToIgnore = NO_LOCATION_UPDATE) {
      if (aLocationOrNullEmptyToIgnore &&
          *aLocationOrNullEmptyToIgnore != '\0') {
        mLastLocation.store(aLocationOrNullEmptyToIgnore,
                            std::memory_order_release);
      }
      mProgress.store(aProgress, std::memory_order_release);
    }

    // Read the current progress value. Atomically "acquired", so that writes
    // from the thread that stored this value are all visible to the reader
    // here; most importantly when reaching 100%, we can be confident that the
    // location is final and the operation being watched has completed.
    [[nodiscard]] ProportionValue Progress() const {
      return mProgress.load(std::memory_order_acquire);
    }

    // Read the current progress value. Atomically "acquired".
    [[nodiscard]] const char* LastLocation() const {
      return mLastLocation.load(std::memory_order_acquire);
    }

   private:
    friend mozilla::detail::RefCounted<SharedProgress,
                                       mozilla::detail::AtomicRefCount>;
    ~SharedProgress() = default;

    // Progress and last-known location.
    // Beware that these two values are not strongly tied: Reading one then the
    // other may give mismatched information; but it should be fine for
    // informational usage.
    // They are stored using atomic acquire-release ordering, to guarantee that
    // when read, all writes preceding these values are visible.
    std::atomic<ProportionValue> mProgress = ProportionValue{0.0};
    std::atomic<const char*> mLastLocation = nullptr;
  };

  static constexpr const char* NO_LOCATION_UPDATE =
      SharedProgress::NO_LOCATION_UPDATE;

  ProgressLogger() = default;

  // Construct a top-level logger, starting at 0% and expected to end at 100%.
  explicit ProgressLogger(
      RefPtr<SharedProgress> aGlobalProgressOrNull,
      const char* aLocationOrNullEmptyToIgnoreAtStart = NO_LOCATION_UPDATE,
      const char* aLocationOrNullEmptyToIgnoreAtEnd = NO_LOCATION_UPDATE)
      : ProgressLogger{std::move(aGlobalProgressOrNull),
                       /* Start */ ProportionValue{0.0},
                       /* Multiplier */ ProportionValue{1.0},
                       aLocationOrNullEmptyToIgnoreAtStart,
                       aLocationOrNullEmptyToIgnoreAtEnd} {}

  // Don't make copies, it would be confusing!
  // TODO: Copies could one day be allowed to track multi-threaded work, but it
  // is outside the scope of this implementation; Please update if needed.
  ProgressLogger(const ProgressLogger&) = delete;
  ProgressLogger& operator&(const ProgressLogger&) = delete;

  // Move-construct is allowed, to return from CreateSubLoggerFromTo, and
  // forward straight into a function. Note that moved-from ProgressLoggers must
  // not be used anymore! Use `CreateSubLoggerFromTo` to pass a sub-logger to
  // functions.
  ProgressLogger(ProgressLogger&& aOther)
      : mGlobalProgressOrNull(std::move(aOther.mGlobalProgressOrNull)),
        mLocalStartInGlobalSpace(aOther.mLocalStartInGlobalSpace),
        mLocalToGlobalMultiplier(aOther.mLocalToGlobalMultiplier),
        mLocationAtDestruction(aOther.mLocationAtDestruction) {
    aOther.MarkMovedFrom();
#ifdef DEBUG_PROCESSLOGGER
    if (mGlobalProgressOrNull) {
      printf("[%d] Moved (staying globally at %.2f in [%.2f, %.2f])\n",
             int(baseprofiler::profiler_current_process_id().ToNumber()),
             GetGlobalProgress().ToDouble() * 100.0,
             mLocalStartInGlobalSpace.ToDouble() * 100.0,
             (mLocalStartInGlobalSpace + mLocalToGlobalMultiplier).ToDouble() *
                 100.0);
    }
#endif  // DEBUG_PROCESSLOGGER
  }

  // Move-assign. This may be useful when starting with a default (empty) logger
  // and later assigning it a progress value to start updating.
  ProgressLogger& operator=(ProgressLogger&& aOther) {
    mGlobalProgressOrNull = std::move(aOther.mGlobalProgressOrNull);
    mLocalStartInGlobalSpace = aOther.mLocalStartInGlobalSpace;
    mLocalToGlobalMultiplier = aOther.mLocalToGlobalMultiplier;
    mLocationAtDestruction = aOther.mLocationAtDestruction;
    aOther.MarkMovedFrom();
#ifdef DEBUG_PROCESSLOGGER
    if (mGlobalProgressOrNull) {
      printf("[%d] Re-assigned (globally at %.2f in [%.2f, %.2f])\n",
             int(baseprofiler::profiler_current_process_id().ToNumber()),
             GetGlobalProgress().ToDouble() * 100.0,
             mLocalStartInGlobalSpace.ToDouble() * 100.0,
             (mLocalStartInGlobalSpace + mLocalToGlobalMultiplier).ToDouble() *
                 100.0);
    }
#endif  // DEBUG_PROCESSLOGGER
    return *this;
  }

  // Destruction sets the local update value to 100% unless empty or moved-from.
  ~ProgressLogger() {
    if (!IsMovedFrom()) {
#ifdef DEBUG_PROCESSLOGGER
      if (mGlobalProgressOrNull) {
        printf("[%d] Destruction:\n",
               int(baseprofiler::profiler_current_process_id().ToNumber()));
      }
#endif  // DEBUG_PROCESSLOGGER
      SetLocalProgress(ProportionValue{1.0}, mLocationAtDestruction);
    }
  }

  // Retrieve the current progress in the global space. May be invalid.
  [[nodiscard]] ProportionValue GetGlobalProgress() const {
    return mGlobalProgressOrNull ? mGlobalProgressOrNull->Progress()
                                 : ProportionValue::MakeInvalid();
  }

  // Retrieve the last known global location. May be null.
  [[nodiscard]] const char* GetLastGlobalLocation() const {
    return mGlobalProgressOrNull ? mGlobalProgressOrNull->LastLocation()
                                 : nullptr;
  }

  // Set the current progress in the local space.
  void SetLocalProgress(ProportionValue aLocalProgress,
                        const char* aLocationOrNullEmptyToIgnore) {
    MOZ_ASSERT(!IsMovedFrom());
    if (mGlobalProgressOrNull && !mLocalToGlobalMultiplier.IsExactlyZero()) {
      mGlobalProgressOrNull->SetProgress(LocalToGlobal(aLocalProgress),
                                         aLocationOrNullEmptyToIgnore);
#ifdef DEBUG_PROCESSLOGGER
      printf("[%d] - local %.0f%% ~ global %.2f%% \"%s\"\n",
             int(baseprofiler::profiler_current_process_id().ToNumber()),
             aLocalProgress.ToDouble() * 100.0,
             LocalToGlobal(aLocalProgress).ToDouble() * 100.0,
             aLocationOrNullEmptyToIgnore ? aLocationOrNullEmptyToIgnore
                                          : "<null>");
#endif  // DEBUG_PROCESSLOGGER
    }
  }

  // Create a sub-logger that will record progress in the given local range.
  // E.g.: `f(pl.CreateSubLoggerFromTo(0.2, "f...", 0.4, "f done"));` expects
  // that `f` will produce work in the local range 0.2 (when starting) to 0.4
  // (when returning); `f` itself will update this provided logger from 0.0
  // to 1.0 (local to that `f` function), which will effectively be converted to
  // 0.2-0.4 (local to the calling function).
  // This can cascade multiple levels, each deeper level affecting a smaller and
  // smaller range in the global output.
  [[nodiscard]] ProgressLogger CreateSubLoggerFromTo(
      ProportionValue aSubStartInLocalSpace,
      const char* aLocationOrNullEmptyToIgnoreAtStart,
      ProportionValue aSubEndInLocalSpace,
      const char* aLocationOrNullEmptyToIgnoreAtEnd = NO_LOCATION_UPDATE) {
    MOZ_ASSERT(!IsMovedFrom());
    if (!mGlobalProgressOrNull) {
      return ProgressLogger{};
    }
    const ProportionValue subStartInGlobalSpace =
        LocalToGlobal(aSubStartInLocalSpace);
    const ProportionValue subEndInGlobalSpace =
        LocalToGlobal(aSubEndInLocalSpace);
    if (subStartInGlobalSpace.IsInvalid() || subEndInGlobalSpace.IsInvalid()) {
      return ProgressLogger{mGlobalProgressOrNull,
                            /* Start */ ProportionValue::MakeInvalid(),
                            /* Multiplier */ ProportionValue{0.0},
                            aLocationOrNullEmptyToIgnoreAtStart,
                            aLocationOrNullEmptyToIgnoreAtEnd};
    }
#ifdef DEBUG_PROCESSLOGGER
    if (mGlobalProgressOrNull) {
      printf("[%d] * Sub: local [%.0f%%, %.0f%%] ~ global [%.2f%%, %.2f%%]\n",
             int(baseprofiler::profiler_current_process_id().ToNumber()),
             aSubStartInLocalSpace.ToDouble() * 100.0,
             aSubEndInLocalSpace.ToDouble() * 100.0,
             subStartInGlobalSpace.ToDouble() * 100.0,
             subEndInGlobalSpace.ToDouble() * 100.0);
    }
#endif  // DEBUG_PROCESSLOGGER
    return ProgressLogger{
        mGlobalProgressOrNull,
        /* Start */ subStartInGlobalSpace,
        /* Multipler */ subEndInGlobalSpace - subStartInGlobalSpace,
        aLocationOrNullEmptyToIgnoreAtStart, aLocationOrNullEmptyToIgnoreAtEnd};
  }

  // Helper with no start location.
  [[nodiscard]] ProgressLogger CreateSubLoggerFromTo(
      ProportionValue aSubStartInLocalSpace,
      ProportionValue aSubEndInLocalSpace,
      const char* aLocationOrNullEmptyToIgnoreAtEnd = NO_LOCATION_UPDATE) {
    return CreateSubLoggerFromTo(aSubStartInLocalSpace, NO_LOCATION_UPDATE,
                                 aSubEndInLocalSpace,
                                 aLocationOrNullEmptyToIgnoreAtEnd);
  }

  // Helper using the current progress as start.
  [[nodiscard]] ProgressLogger CreateSubLoggerTo(
      const char* aLocationOrNullEmptyToIgnoreAtStart,
      ProportionValue aSubEndInLocalSpace,
      const char* aLocationOrNullEmptyToIgnoreAtEnd = NO_LOCATION_UPDATE) {
    MOZ_ASSERT(!IsMovedFrom());
    if (!mGlobalProgressOrNull) {
      return ProgressLogger{};
    }
    const ProportionValue subStartInGlobalSpace = GetGlobalProgress();
    const ProportionValue subEndInGlobalSpace =
        LocalToGlobal(aSubEndInLocalSpace);
    if (subStartInGlobalSpace.IsInvalid() || subEndInGlobalSpace.IsInvalid()) {
      return ProgressLogger{mGlobalProgressOrNull,
                            /* Start */ ProportionValue::MakeInvalid(),
                            /* Multiplier */ ProportionValue{0.0},
                            aLocationOrNullEmptyToIgnoreAtStart,
                            aLocationOrNullEmptyToIgnoreAtEnd};
    }
#ifdef DEBUG_PROCESSLOGGER
    if (mGlobalProgressOrNull) {
      printf("[%d] * Sub: local [(here), %.0f%%] ~ global [%.2f%%, %.2f%%]\n",
             int(baseprofiler::profiler_current_process_id().ToNumber()),
             aSubEndInLocalSpace.ToDouble() * 100.0,
             subStartInGlobalSpace.ToDouble() * 100.0,
             subEndInGlobalSpace.ToDouble() * 100.0);
    }
#endif  // DEBUG_PROCESSLOGGER
    return ProgressLogger{
        mGlobalProgressOrNull,
        /* Start */ subStartInGlobalSpace,
        /* Multiplier */ subEndInGlobalSpace - subStartInGlobalSpace,
        aLocationOrNullEmptyToIgnoreAtStart, aLocationOrNullEmptyToIgnoreAtEnd};
  }

  // Helper using the current progress as start, no start location.
  [[nodiscard]] ProgressLogger CreateSubLoggerTo(
      ProportionValue aSubEndInLocalSpace,
      const char* aLocationOrNullEmptyToIgnoreAtEnd = NO_LOCATION_UPDATE) {
    return CreateSubLoggerTo(NO_LOCATION_UPDATE, aSubEndInLocalSpace,
                             aLocationOrNullEmptyToIgnoreAtEnd);
  }

  class IndexAndProgressLoggerRange;

  [[nodiscard]] inline IndexAndProgressLoggerRange CreateLoopSubLoggersFromTo(
      ProportionValue aLoopStartInLocalSpace,
      ProportionValue aLoopEndInLocalSpace, uint32_t aLoopCount,
      const char* aLocationOrNullEmptyToIgnoreAtEdges =
          ProgressLogger::NO_LOCATION_UPDATE);
  [[nodiscard]] inline IndexAndProgressLoggerRange CreateLoopSubLoggersTo(
      ProportionValue aLoopEndInLocalSpace, uint32_t aLoopCount,
      const char* aLocationOrNullEmptyToIgnoreAtEdges =
          ProgressLogger::NO_LOCATION_UPDATE);

 private:
  // All constructions start at the local 0%.
  ProgressLogger(RefPtr<SharedProgress> aGlobalProgressOrNull,
                 ProportionValue aLocalStartInGlobalSpace,
                 ProportionValue aLocalToGlobalMultiplier,
                 const char* aLocationOrNullEmptyToIgnoreAtConstruction,
                 const char* aLocationOrNullEmptyToIgnoreAtDestruction)
      : mGlobalProgressOrNull(std::move(aGlobalProgressOrNull)),
        mLocalStartInGlobalSpace(aLocalStartInGlobalSpace),
        mLocalToGlobalMultiplier(aLocalToGlobalMultiplier),
        mLocationAtDestruction(aLocationOrNullEmptyToIgnoreAtDestruction) {
    MOZ_ASSERT(!IsMovedFrom(), "Don't construct a moved-from object!");
    SetLocalProgress(ProportionValue{0.0},
                     aLocationOrNullEmptyToIgnoreAtConstruction);
  }

  void MarkMovedFrom() {
    mLocalToGlobalMultiplier = ProportionValue::MakeInvalid();
  }
  [[nodiscard]] bool IsMovedFrom() const {
    return mLocalToGlobalMultiplier.IsInvalid();
  }

  [[nodiscard]] ProportionValue LocalToGlobal(
      ProportionValue aLocalProgress) const {
    return aLocalProgress * mLocalToGlobalMultiplier + mLocalStartInGlobalSpace;
  }

  // Global progress value to update from local changes.
  RefPtr<SharedProgress> mGlobalProgressOrNull;

  // How much to multiply and add to a local [0, 100%] value, to get the
  // corresponding value in the global space.
  // If mLocalToGlobalMultiplier is invalid, this ProgressLogger is moved-from,
  // functions should not be used, and destructor won't update progress.
  ProportionValue mLocalStartInGlobalSpace;
  ProportionValue mLocalToGlobalMultiplier;

  const char* mLocationAtDestruction = nullptr;
};

// Helper class for range-for loop, e.g., with `aProgressLogger`:
//   for (auto [index, loopProgressLogger] :
//        IndexAndProgressLoggerRange{aProgressLogger, 30_pc, 50_pc, 10,
//                                    "looping..."}) {
//     // This will loop 10 times.
//     // `index` is the loop index, from 0 to 9.
//     // The overall loop will start at 30% and end at 50% of aProgressLogger.
//     // `loopProgressLogger` is the progress logger for each iteration,
//     // covering 1/10th of the range, therefore: [30%,32%], then [32%,34%],
//     // etc. until [48%,50%].
//     // Progress is automatically updated before/after each loop.
//   }
// Note that this implementation is single-threaded, it does not support logging
// progress from parallel loops.
class ProgressLogger::IndexAndProgressLoggerRange {
 public:
  struct IndexAndProgressLogger {
    uint32_t index;
    ProgressLogger progressLogger;
  };

  class IndexAndProgressLoggerEndIterator {
   public:
    explicit IndexAndProgressLoggerEndIterator(uint32_t aIndex)
        : mIndex(aIndex) {}

    [[nodiscard]] uint32_t Index() const { return mIndex; }

   private:
    uint32_t mIndex;
  };

  class IndexAndProgressLoggerIterator {
   public:
    IndexAndProgressLoggerIterator(
        RefPtr<ProgressLogger::SharedProgress> aGlobalProgressOrNull,
        ProportionValue aLoopStartInGlobalSpace,
        ProportionValue aLoopIncrementInGlobalSpace,
        const char* aLocationOrNullEmptyToIgnoreAtEdges)
        : mGlobalProgressOrNull(aGlobalProgressOrNull),
          mLoopStartInGlobalSpace(aLoopStartInGlobalSpace),
          mLoopIncrementInGlobalSpace(aLoopIncrementInGlobalSpace),
          mIndex(0u),
          mLocationOrNullEmptyToIgnoreAtEdges(
              aLocationOrNullEmptyToIgnoreAtEdges) {
      if (mGlobalProgressOrNull) {
        mGlobalProgressOrNull->SetProgress(mLoopStartInGlobalSpace,
                                           mLocationOrNullEmptyToIgnoreAtEdges);
      }
    }

    [[nodiscard]] IndexAndProgressLogger operator*() {
      return IndexAndProgressLogger{
          mIndex,
          mGlobalProgressOrNull
              ? ProgressLogger{mGlobalProgressOrNull, mLoopStartInGlobalSpace,
                               mLoopIncrementInGlobalSpace,
                               ProgressLogger::NO_LOCATION_UPDATE,
                               ProgressLogger::NO_LOCATION_UPDATE}
              : ProgressLogger{}};
    }

    [[nodiscard]] bool operator!=(
        const IndexAndProgressLoggerEndIterator& aEnd) const {
      return mIndex != aEnd.Index();
    }

    IndexAndProgressLoggerIterator& operator++() {
      ++mIndex;
      mLoopStartInGlobalSpace =
          mLoopStartInGlobalSpace + mLoopIncrementInGlobalSpace;
      if (mGlobalProgressOrNull) {
        mGlobalProgressOrNull->SetProgress(mLoopStartInGlobalSpace,
                                           mLocationOrNullEmptyToIgnoreAtEdges);
      }
      return *this;
    }

   private:
    RefPtr<ProgressLogger::SharedProgress> mGlobalProgressOrNull;
    ProportionValue mLoopStartInGlobalSpace;
    ProportionValue mLoopIncrementInGlobalSpace;
    uint32_t mIndex;
    const char* mLocationOrNullEmptyToIgnoreAtEdges;
  };

  [[nodiscard]] IndexAndProgressLoggerIterator begin() {
    return IndexAndProgressLoggerIterator{
        mGlobalProgressOrNull, mLoopStartInGlobalSpace,
        mLoopIncrementInGlobalSpace, mLocationOrNullEmptyToIgnoreAtEdges};
  }

  [[nodiscard]] IndexAndProgressLoggerEndIterator end() {
    return IndexAndProgressLoggerEndIterator{mLoopCount};
  }

 private:
  friend class ProgressLogger;
  IndexAndProgressLoggerRange(ProgressLogger& aProgressLogger,
                              ProportionValue aLoopStartInGlobalSpace,
                              ProportionValue aLoopEndInGlobalSpace,
                              uint32_t aLoopCount,
                              const char* aLocationOrNullEmptyToIgnoreAtEdges =
                                  ProgressLogger::NO_LOCATION_UPDATE)
      : mGlobalProgressOrNull(aProgressLogger.mGlobalProgressOrNull),
        mLoopStartInGlobalSpace(aLoopStartInGlobalSpace),
        mLoopIncrementInGlobalSpace(
            (aLoopEndInGlobalSpace - aLoopStartInGlobalSpace) / aLoopCount),
        mLoopCount(aLoopCount),
        mLocationOrNullEmptyToIgnoreAtEdges(
            aLocationOrNullEmptyToIgnoreAtEdges) {}

  RefPtr<ProgressLogger::SharedProgress> mGlobalProgressOrNull;
  ProportionValue mLoopStartInGlobalSpace;
  ProportionValue mLoopIncrementInGlobalSpace;
  uint32_t mLoopCount;
  const char* mLocationOrNullEmptyToIgnoreAtEdges;
};

[[nodiscard]] ProgressLogger::IndexAndProgressLoggerRange
ProgressLogger::CreateLoopSubLoggersFromTo(
    ProportionValue aLoopStartInLocalSpace,
    ProportionValue aLoopEndInLocalSpace, uint32_t aLoopCount,
    const char* aLocationOrNullEmptyToIgnoreAtEdges) {
  return IndexAndProgressLoggerRange{
      *this, LocalToGlobal(aLoopStartInLocalSpace),
      LocalToGlobal(aLoopEndInLocalSpace), aLoopCount,
      aLocationOrNullEmptyToIgnoreAtEdges};
}

[[nodiscard]] ProgressLogger::IndexAndProgressLoggerRange
ProgressLogger::CreateLoopSubLoggersTo(
    ProportionValue aLoopEndInLocalSpace, uint32_t aLoopCount,
    const char* aLocationOrNullEmptyToIgnoreAtEdges) {
  return IndexAndProgressLoggerRange{
      *this, GetGlobalProgress(), LocalToGlobal(aLoopEndInLocalSpace),
      aLoopCount, aLocationOrNullEmptyToIgnoreAtEdges};
}

}  // namespace mozilla

#endif  // ProgressLogger_h

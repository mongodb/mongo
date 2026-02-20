/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The Gecko Profiler is an always-on profiler that takes fast and low overhead
// samples of the program execution using only userspace functionality for
// portability. The goal of this module is to provide performance data in a
// generic cross-platform way without requiring custom tools or kernel support.
//
// Samples are collected to form a timeline with optional timeline event
// (markers) used for filtering. The samples include both native stacks and
// platform-independent "label stack" frames.

#ifndef BaseProfilerState_h
#define BaseProfilerState_h

// This header contains most functions that give information about the Base
// Profiler: Whether it is active or not, paused, the selected features, and
// some generic process and thread information.
// It is safe to include unconditionally, but uses of structs and functions must
// be guarded by `#ifdef MOZ_GECKO_PROFILER`.

#include "mozilla/BaseProfilerUtils.h"

#ifndef MOZ_GECKO_PROFILER

#  define AUTO_PROFILER_STATS(name)

namespace mozilla::baseprofiler {

[[nodiscard]] inline bool profiler_is_active() { return false; }
[[nodiscard]] inline bool profiler_is_active_and_unpaused() { return false; }

}  // namespace mozilla::baseprofiler

#else  // !MOZ_GECKO_PROFILER

#  include "mozilla/Atomics.h"
#  include "mozilla/Maybe.h"

#  include <stdint.h>
#  include <string>

// Uncomment the following line to display profiler runtime statistics at
// shutdown.
// #  define PROFILER_RUNTIME_STATS

#  ifdef PROFILER_RUNTIME_STATS
#    include "mozilla/TimeStamp.h"
#  endif

namespace mozilla::baseprofiler {

#  ifdef PROFILER_RUNTIME_STATS
// This class gathers durations and displays some basic stats when destroyed.
// It is intended to be used as a static variable (see `AUTO_PROFILER_STATS`
// below), to display stats at the end of the program.
class StaticBaseProfilerStats {
 public:
  explicit StaticBaseProfilerStats(const char* aName) : mName(aName) {}

  ~StaticBaseProfilerStats() {
    // Using unsigned long long for computations and printfs.
    using ULL = unsigned long long;
    ULL n = static_cast<ULL>(mNumberDurations);
    if (n != 0) {
      ULL sumNs = static_cast<ULL>(mSumDurationsNs);
      printf(
          "[%d] Profiler stats `%s`: %llu ns / %llu = %llu ns, max %llu ns\n",
          int(profiler_current_process_id().ToNumber()), mName, sumNs, n,
          sumNs / n, static_cast<ULL>(mLongestDurationNs));
    } else {
      printf("[%d] Profiler stats `%s`: (nothing)\n",
             int(profiler_current_process_id().ToNumber()), mName);
    }
  }

  void AddDurationFrom(TimeStamp aStart) {
    DurationNs duration = static_cast<DurationNs>(
        (TimeStamp::Now() - aStart).ToMicroseconds() * 1000 + 0.5);
    mSumDurationsNs += duration;
    ++mNumberDurations;
    // Update mLongestDurationNs if this one is longer.
    for (;;) {
      DurationNs longest = mLongestDurationNs;
      if (MOZ_LIKELY(longest >= duration)) {
        // This duration is not the longest, nothing to do.
        break;
      }
      if (MOZ_LIKELY(mLongestDurationNs.compareExchange(longest, duration))) {
        // Successfully updated `mLongestDurationNs` with the new value.
        break;
      }
      // Otherwise someone else just updated `mLongestDurationNs`, we need to
      // try again by looping.
    }
  }

 private:
  using DurationNs = uint64_t;
  using Count = uint32_t;

  Atomic<DurationNs> mSumDurationsNs{0};
  Atomic<DurationNs> mLongestDurationNs{0};
  Atomic<Count> mNumberDurations{0};
  const char* mName;
};

// RAII object that measure its scoped lifetime duration and reports it to a
// `StaticBaseProfilerStats`.
class MOZ_RAII AutoProfilerStats {
 public:
  explicit AutoProfilerStats(StaticBaseProfilerStats& aStats)
      : mStats(aStats), mStart(TimeStamp::Now()) {}

  ~AutoProfilerStats() { mStats.AddDurationFrom(mStart); }

 private:
  StaticBaseProfilerStats& mStats;
  TimeStamp mStart;
};

// Macro that should be used to collect basic statistics from measurements of
// block durations, from where this macro is, until the end of its enclosing
// scope. The name is used in the static variable name and when displaying stats
// at the end of the program; Another location could use the same name but their
// stats will not be combined, so use different name if these locations should
// be distinguished.
#    define AUTO_PROFILER_STATS(name)                                      \
      static ::mozilla::baseprofiler::StaticBaseProfilerStats sStat##name( \
          #name);                                                          \
      ::mozilla::baseprofiler::AutoProfilerStats autoStat##name(sStat##name);

#  else  // PROFILER_RUNTIME_STATS

#    define AUTO_PROFILER_STATS(name)

#  endif  // PROFILER_RUNTIME_STATS else

//---------------------------------------------------------------------------
// Profiler features
//---------------------------------------------------------------------------

#  if defined(__APPLE__) && defined(__aarch64__)
#    define POWER_HELP "Sample per process power use"
#  elif defined(__APPLE__) && defined(__x86_64__)
#    define POWER_HELP \
      "Record the power used by the entire system with each sample."
#  elif defined(__linux__) && defined(__x86_64__)
#    define POWER_HELP                                                \
      "Record the power used by the entire system with each sample. " \
      "Only available with Intel CPUs and requires setting "          \
      "the sysctl kernel.perf_event_paranoid to 0."
#  elif defined(_MSC_VER)
#    define POWER_HELP                                                       \
      "Record the value of every energy meter available on the system with " \
      "each sample. Only available on Windows 11 with Intel CPUs."
#  else
#    define POWER_HELP "Not supported on this platform."
#  endif

// Higher-order macro containing all the feature info in one place. Define
// |MACRO| appropriately to extract the relevant parts. Note that the number
// values are used internally only and so can be changed without consequence.
// Any changes to this list should also be applied to the feature list in
// toolkit/components/extensions/schemas/geckoProfiler.json.
// *** Synchronize with lists in ProfilerState.h and geckoProfiler.json ***
#  define BASE_PROFILER_FOR_EACH_FEATURE(MACRO)                              \
    MACRO(0, "java", Java, "Profile Java code, Android only")                \
                                                                             \
    MACRO(1, "js", JS,                                                       \
          "Get the JS engine to expose the JS stack to the profiler")        \
                                                                             \
    MACRO(2, "mainthreadio", MainThreadIO, "Add main thread file I/O")       \
                                                                             \
    MACRO(3, "fileio", FileIO,                                               \
          "Add file I/O from all profiled threads, implies mainthreadio")    \
                                                                             \
    MACRO(4, "fileioall", FileIOAll,                                         \
          "Add file I/O from all threads, implies fileio")                   \
                                                                             \
    MACRO(5, "nomarkerstacks", NoMarkerStacks,                               \
          "Markers do not capture stacks, to reduce overhead")               \
                                                                             \
    MACRO(6, "screenshots", Screenshots,                                     \
          "Take a snapshot of the window on every composition")              \
                                                                             \
    MACRO(7, "seqstyle", SequentialStyle,                                    \
          "Disable parallel traversal in styling")                           \
                                                                             \
    MACRO(8, "stackwalk", StackWalk,                                         \
          "Walk the C++ stack, not available on all platforms")              \
                                                                             \
    MACRO(9, "jsallocations", JSAllocations,                                 \
          "Have the JavaScript engine track allocations")                    \
                                                                             \
    MACRO(10, "nostacksampling", NoStackSampling,                            \
          "Disable all stack sampling: Cancels \"js\", \"stackwalk\" and "   \
          "labels")                                                          \
                                                                             \
    MACRO(11, "nativeallocations", NativeAllocations,                        \
          "Collect the stacks from a smaller subset of all native "          \
          "allocations, biasing towards collecting larger allocations")      \
                                                                             \
    MACRO(12, "ipcmessages", IPCMessages,                                    \
          "Have the IPC layer track cross-process messages")                 \
                                                                             \
    MACRO(13, "audiocallbacktracing", AudioCallbackTracing,                  \
          "Audio callback tracing")                                          \
                                                                             \
    MACRO(14, "cpu", CPUUtilization, "CPU utilization")                      \
                                                                             \
    MACRO(15, "notimerresolutionchange", NoTimerResolutionChange,            \
          "Do not adjust the timer resolution for fast sampling, so that "   \
          "other Firefox timers do not get affected")                        \
                                                                             \
    MACRO(16, "cpuallthreads", CPUAllThreads,                                \
          "Sample the CPU utilization of all registered threads")            \
                                                                             \
    MACRO(17, "samplingallthreads", SamplingAllThreads,                      \
          "Sample the stacks of all registered threads")                     \
                                                                             \
    MACRO(18, "markersallthreads", MarkersAllThreads,                        \
          "Record markers from all registered threads")                      \
                                                                             \
    MACRO(19, "unregisteredthreads", UnregisteredThreads,                    \
          "Discover and profile unregistered threads -- beware: expensive!") \
                                                                             \
    MACRO(20, "processcpu", ProcessCPU,                                      \
          "Sample the CPU utilization of each process")                      \
                                                                             \
    MACRO(21, "power", Power, POWER_HELP)                                    \
                                                                             \
    MACRO(22, "cpufreq", CPUFrequency,                                       \
          "Record the clock frequency of "                                   \
          "every CPU core for every profiler sample.")                       \
                                                                             \
    MACRO(23, "bandwidth", Bandwidth,                                        \
          "Record the network bandwidth used for every profiler sample.")    \
                                                                             \
    MACRO(24, "memory", Memory,                                              \
          "Track the memory allocations and deallocations per process over " \
          "time.")                                                           \
                                                                             \
    MACRO(25, "tracing", Tracing,                                            \
          "Instead of sampling periodically, captures information about "    \
          "every function executed for the duration (JS only)")              \
                                                                             \
    MACRO(26, "sandbox", Sandbox,                                            \
          "Report sandbox syscalls and logs in the "                         \
          "profiler.")                                                       \
                                                                             \
    MACRO(27, "flows", Flows,                                                \
          "Include all flow-related markers. These markers show the program" \
          "better but can cause more overhead in some places than normal.")

// *** Synchronize with lists in ProfilerState.h and geckoProfiler.json ***

struct ProfilerFeature {
#  define DECLARE(n_, str_, Name_, desc_)                                \
    static constexpr uint32_t Name_ = (1u << n_);                        \
    [[nodiscard]] static constexpr bool Has##Name_(uint32_t aFeatures) { \
      return aFeatures & Name_;                                          \
    }                                                                    \
    static constexpr void Set##Name_(uint32_t& aFeatures) {              \
      aFeatures |= Name_;                                                \
    }                                                                    \
    static constexpr void Clear##Name_(uint32_t& aFeatures) {            \
      aFeatures &= ~Name_;                                               \
    }

  // Define a bitfield constant, a getter, and two setters for each feature.
  BASE_PROFILER_FOR_EACH_FEATURE(DECLARE)

#  undef DECLARE
};

namespace detail {

// RacyFeatures is only defined in this header file so that its methods can
// be inlined into profiler_is_active(). Please do not use anything from the
// detail namespace outside the profiler.

// Within the profiler's code, the preferred way to check profiler activeness
// and features is via ActivePS(). However, that requires locking gPSMutex.
// There are some hot operations where absolute precision isn't required, so we
// duplicate the activeness/feature state in a lock-free manner in this class.
class RacyFeatures {
 public:
  MFBT_API static void SetActive(uint32_t aFeatures);

  MFBT_API static void SetInactive();

  MFBT_API static void SetPaused();

  MFBT_API static void SetUnpaused();

  MFBT_API static void SetSamplingPaused();

  MFBT_API static void SetSamplingUnpaused();

  [[nodiscard]] MFBT_API static mozilla::Maybe<uint32_t> FeaturesIfActive() {
    if (uint32_t af = sActiveAndFeatures; af & Active) {
      // Active, remove the Active&Paused bits to get all features.
      return Some(af & ~(Active | Paused | SamplingPaused));
    }
    return Nothing();
  }

  [[nodiscard]] MFBT_API static bool IsActive();

  [[nodiscard]] MFBT_API static bool IsActiveWithFeature(uint32_t aFeature);

  [[nodiscard]] MFBT_API static bool IsActiveWithoutFeature(uint32_t aFeature);

  // True if profiler is active, and not fully paused.
  // Note that periodic sampling *could* be paused!
  [[nodiscard]] MFBT_API static bool IsActiveAndUnpaused();

  // True if profiler is active, and sampling is not paused (though generic
  // `SetPaused()` or specific `SetSamplingPaused()`).
  [[nodiscard]] MFBT_API static bool IsActiveAndSamplingUnpaused();

 private:
  static constexpr uint32_t Active = 1u << 31;
  static constexpr uint32_t Paused = 1u << 30;
  static constexpr uint32_t SamplingPaused = 1u << 29;

// Ensure Active/Paused don't overlap with any of the feature bits.
#  define NO_OVERLAP(n_, str_, Name_, desc_)                \
    static_assert(ProfilerFeature::Name_ != SamplingPaused, \
                  "bad feature value");

  BASE_PROFILER_FOR_EACH_FEATURE(NO_OVERLAP);

#  undef NO_OVERLAP

  // We combine the active bit with the feature bits so they can be read or
  // written in a single atomic operation.
  // TODO: Could this be MFBT_DATA for better inlining optimization?
  MFBT_DATA static Atomic<uint32_t, MemoryOrdering::Relaxed> sActiveAndFeatures;
};

MFBT_API bool IsThreadBeingProfiled();

}  // namespace detail

//---------------------------------------------------------------------------
// Get information from the profiler
//---------------------------------------------------------------------------

// Is the profiler active? Note: the return value of this function can become
// immediately out-of-date. E.g. the profile might be active but then
// profiler_stop() is called immediately afterward. One common and reasonable
// pattern of usage is the following:
//
//   if (profiler_is_active()) {
//     ExpensiveData expensiveData = CreateExpensiveData();
//     PROFILER_OPERATION(expensiveData);
//   }
//
// where PROFILER_OPERATION is a no-op if the profiler is inactive. In this
// case the profiler_is_active() check is just an optimization -- it prevents
// us calling CreateExpensiveData() unnecessarily in most cases, but the
// expensive data will end up being created but not used if another thread
// stops the profiler between the CreateExpensiveData() and PROFILER_OPERATION
// calls.
[[nodiscard]] inline bool profiler_is_active() {
  return baseprofiler::detail::RacyFeatures::IsActive();
}

// Same as profiler_is_active(), but also checks if the profiler is not paused.
[[nodiscard]] inline bool profiler_is_active_and_unpaused() {
  return baseprofiler::detail::RacyFeatures::IsActiveAndUnpaused();
}

// Is the profiler active and unpaused, and is the current thread being
// profiled? (Same caveats and recommented usage as profiler_is_active().)
[[nodiscard]] inline bool profiler_thread_is_being_profiled() {
  return baseprofiler::detail::RacyFeatures::IsActiveAndUnpaused() &&
         baseprofiler::detail::IsThreadBeingProfiled();
}

// Is the profiler active and paused? Returns false if the profiler is inactive.
[[nodiscard]] MFBT_API bool profiler_is_paused();

// Is the profiler active and sampling is paused? Returns false if the profiler
// is inactive.
[[nodiscard]] MFBT_API bool profiler_is_sampling_paused();

// Is the current thread sleeping?
[[nodiscard]] MFBT_API bool profiler_thread_is_sleeping();

// Get all the features supported by the profiler that are accepted by
// profiler_start(). The result is the same whether the profiler is active or
// not.
[[nodiscard]] MFBT_API uint32_t profiler_get_available_features();

// Returns the full feature set if the profiler is active.
// Note: the return value can become immediately out-of-date, much like the
// return value of profiler_is_active().
[[nodiscard]] inline mozilla::Maybe<uint32_t> profiler_features_if_active() {
  return baseprofiler::detail::RacyFeatures::FeaturesIfActive();
}

// Check if a profiler feature (specified via the ProfilerFeature type) is
// active. Returns false if the profiler is inactive. Note: the return value
// can become immediately out-of-date, much like the return value of
// profiler_is_active().
[[nodiscard]] MFBT_API bool profiler_feature_active(uint32_t aFeature);

// Check if the profiler is active without a feature (specified via the
// ProfilerFeature type). Note: the return value can become immediately
// out-of-date, much like the return value of profiler_is_active().
[[nodiscard]] MFBT_API bool profiler_active_without_feature(uint32_t aFeature);

// Returns true if any of the profiler mutexes are currently locked *on the
// current thread*. This may be used by re-entrant code that may call profiler
// functions while the same of a different profiler mutex is locked, which could
// deadlock.
[[nodiscard]] bool profiler_is_locked_on_current_thread();

}  // namespace mozilla::baseprofiler

#endif  // !MOZ_GECKO_PROFILER

#endif  // BaseProfilerState_h

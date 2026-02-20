/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BaseProfilerCounts_h
#define BaseProfilerCounts_h

#ifndef MOZ_GECKO_PROFILER

#  define BASE_PROFILER_DEFINE_COUNT_TOTAL(label, category, description)
#  define BASE_PROFILER_DEFINE_COUNT(label, category, description)
#  define BASE_PROFILER_DEFINE_STATIC_COUNT_TOTAL(label, category, description)
#  define AUTO_BASE_PROFILER_COUNT_TOTAL(label, count)
#  define AUTO_BASE_PROFILER_COUNT(label)
#  define AUTO_BASE_PROFILER_STATIC_COUNT(label, count)
#  define AUTO_BASE_PROFILER_FORCE_ALLOCATION(label)

#else

#  include "mozilla/Assertions.h"
#  include "mozilla/Atomics.h"

namespace mozilla {
namespace baseprofiler {

class BaseProfilerCount;
MFBT_API void profiler_add_sampled_counter(BaseProfilerCount* aCounter);
MFBT_API void profiler_remove_sampled_counter(BaseProfilerCount* aCounter);

typedef Atomic<int64_t, MemoryOrdering::Relaxed> ProfilerAtomicSigned;
typedef Atomic<uint64_t, MemoryOrdering::Relaxed> ProfilerAtomicUnsigned;

// Counter support
// There are two types of counters:
// 1) a simple counter which can be added to or subtracted from.  This could
// track the number of objects of a type, the number of calls to something
// (reflow, JIT, etc).
// 2) a combined counter which has the above, plus a number-of-calls counter
// that is incremented by 1 for each call to modify the count.  This provides
// an optional source for a 'heatmap' of access.  This can be used (for
// example) to track the amount of memory allocated, and provide a heatmap of
// memory operations (allocs/frees).
//
// Counters are sampled by the profiler once per sample-period.  At this time,
// all counters are global to the process.  In the future, there might be more
// versions with per-thread or other discriminators.
//
// Typical usage:
// There are two ways to use counters: With heap-created counter objects,
// or using macros.  Note: the macros use statics, and will be slightly
// faster/smaller, and you need to care about creating them before using
// them.  They're similar to the use-pattern for the other AUTO_PROFILER*
// macros, but they do need the PROFILER_DEFINE* to be use to instantiate
// the statics.
//
// PROFILER_DEFINE_COUNT(mything, "JIT", "Some JIT byte count")
// ...
// void foo() { ... AUTO_PROFILER_COUNT(mything, number_of_bytes_used); ... }
//
// or (to also get a heatmap)
//
// PROFILER_DEFINE_COUNT_TOTAL(mything, "JIT", "Some JIT byte count")
// ...
// void foo() {
//   ...
//   AUTO_PROFILER_COUNT_TOTAL(mything, number_of_bytes_generated);
//   ...
// }
//
// To use without statics/macros:
//
// UniquePtr<ProfilerCounter> myCounter;
// ...
// myCounter =
//   MakeUnique<ProfilerCounter>("mything", "JIT", "Some JIT byte count"));
// ...
// void foo() { ... myCounter->Add(number_of_bytes_generated0; ... }

class BaseProfilerCount {
 public:
  BaseProfilerCount(const char* aLabel, ProfilerAtomicSigned* aCounter,
                    ProfilerAtomicUnsigned* aNumber, const char* aCategory,
                    const char* aDescription)
      : mLabel(aLabel),
        mCategory(aCategory),
        mDescription(aDescription),
        mCounter(aCounter),
        mNumber(aNumber) {
#  define COUNTER_CANARY 0xDEADBEEF
#  ifdef DEBUG
    mCanary = COUNTER_CANARY;
    mPrevNumber = 0;
#  endif
    // Can't call profiler_* here since this may be non-xul-library
  }
#  ifdef DEBUG
  ~BaseProfilerCount() { mCanary = 0; }
#  endif

  void Sample(int64_t& aCounter, uint64_t& aNumber) {
    MOZ_ASSERT(mCanary == COUNTER_CANARY);

    aCounter = *mCounter;
    aNumber = mNumber ? *mNumber : 0;
#  ifdef DEBUG
    MOZ_ASSERT(aNumber >= mPrevNumber);
    mPrevNumber = aNumber;
#  endif
  }

  // We don't define ++ and Add() here, since the static defines directly
  // increment the atomic counters, and the subclasses implement ++ and
  // Add() directly.

  // These typically are static strings (for example if you use the macros
  // below)
  const char* mLabel;
  const char* mCategory;
  const char* mDescription;
  // We're ok with these being un-ordered in race conditions.  These are
  // pointers because we want to be able to use statics and increment them
  // directly.  Otherwise we could just have them inline, and not need the
  // constructor args.
  // These can be static globals (using the macros below), though they
  // don't have to be - their lifetime must be longer than the use of them
  // by the profiler (see profiler_add/remove_sampled_counter()).  If you're
  // using a lot of these, they probably should be allocated at runtime (see
  // class ProfilerCountOnly below).
  ProfilerAtomicSigned* mCounter;
  ProfilerAtomicUnsigned* mNumber;  // may be null

#  ifdef DEBUG
  uint32_t mCanary;
  uint64_t mPrevNumber;  // value of number from the last Sample()
#  endif
};

// Designed to be allocated dynamically, and simply incremented with obj++
// or obj->Add(n)
class ProfilerCounter final : public BaseProfilerCount {
 public:
  ProfilerCounter(const char* aLabel, const char* aCategory,
                  const char* aDescription)
      : BaseProfilerCount(aLabel, &mCounter, nullptr, aCategory, aDescription) {
    // Assume we're in libxul
    profiler_add_sampled_counter(this);
  }

  ~ProfilerCounter() { profiler_remove_sampled_counter(this); }

  BaseProfilerCount& operator++() {
    Add(1);
    return *this;
  }

  void Add(int64_t aNumber) { mCounter += aNumber; }

  ProfilerAtomicSigned mCounter;
};

// Also keeps a heatmap (number of calls to ++/Add())
class ProfilerCounterTotal final : public BaseProfilerCount {
 public:
  ProfilerCounterTotal(const char* aLabel, const char* aCategory,
                       const char* aDescription)
      : BaseProfilerCount(aLabel, &mCounter, &mNumber, aCategory,
                          aDescription) {
    // Assume we're in libxul
    profiler_add_sampled_counter(this);
  }

  ~ProfilerCounterTotal() { profiler_remove_sampled_counter(this); }

  BaseProfilerCount& operator++() {
    Add(1);
    return *this;
  }

  void Add(int64_t aNumber) {
    mCounter += aNumber;
    mNumber++;
  }

  ProfilerAtomicSigned mCounter;
  ProfilerAtomicUnsigned mNumber;
};

// Defines a counter that is sampled on each profiler tick, with a running
// count (signed), and number-of-instances. Note that because these are two
// independent Atomics, there is a possiblity that count will not include
// the last call, but number of uses will.  I think this is not worth
// worrying about
#  define BASE_PROFILER_DEFINE_COUNT_TOTAL(label, category, description) \
    ProfilerAtomicSigned profiler_count_##label(0);                      \
    ProfilerAtomicUnsigned profiler_number_##label(0);                   \
    const char profiler_category_##label[] = category;                   \
    const char profiler_description_##label[] = description;             \
    UniquePtr<::mozilla::baseprofiler::BaseProfilerCount> AutoCount_##label;

// This counts, but doesn't keep track of the number of calls to
// AUTO_PROFILER_COUNT()
#  define BASE_PROFILER_DEFINE_COUNT(label, category, description) \
    ProfilerAtomicSigned profiler_count_##label(0);                \
    const char profiler_category_##label[] = category;             \
    const char profiler_description_##label[] = description;       \
    UniquePtr<::mozilla::baseprofiler::BaseProfilerCount> AutoCount_##label;

// This will create a static initializer if used, but avoids a possible
// allocation.
#  define BASE_PROFILER_DEFINE_STATIC_COUNT_TOTAL(label, category,           \
                                                  description)               \
    ProfilerAtomicSigned profiler_count_##label(0);                          \
    ProfilerAtomicUnsigned profiler_number_##label(0);                       \
    ::mozilla::baseprofiler::BaseProfilerCount AutoCount_##label(            \
        #label, &profiler_count_##label, &profiler_number_##label, category, \
        description);

// If we didn't care about static initializers, we could avoid the need for
// a ptr to the BaseProfilerCount object.

// XXX It would be better to do this without the if() and without the
// theoretical race to set the UniquePtr (i.e. possible leak).
#  define AUTO_BASE_PROFILER_COUNT_TOTAL(label, count)                      \
    do {                                                                    \
      profiler_number_##label++; /* do this first*/                         \
      profiler_count_##label += count;                                      \
      if (!AutoCount_##label) {                                             \
        /* Ignore that we could call this twice in theory, and that we leak \
         * them                                                             \
         */                                                                 \
        AutoCount_##label.reset(new BaseProfilerCount(                      \
            #label, &profiler_count_##label, &profiler_number_##label,      \
            profiler_category_##label, profiler_description_##label));      \
        ::mozilla::baseprofiler::profiler_add_sampled_counter(              \
            AutoCount_##label.get());                                       \
      }                                                                     \
    } while (0)

#  define AUTO_BASE_PROFILER_COUNT(label, count)                            \
    do {                                                                    \
      profiler_count_##label += count; /* do this first*/                   \
      if (!AutoCount_##label) {                                             \
        /* Ignore that we could call this twice in theory, and that we leak \
         * them                                                             \
         */                                                                 \
        AutoCount_##label.reset(new BaseProfilerCount(                      \
            #label, nullptr, &profiler_number_##label,                      \
            profiler_category_##label, profiler_description_##label));      \
        ::mozilla::baseprofiler::profiler_add_sampled_counter(              \
            AutoCount_##label.get());                                       \
      }                                                                     \
    } while (0)

#  define AUTO_BASE_PROFILER_STATIC_COUNT(label, count) \
    do {                                                \
      profiler_number_##label++; /* do this first*/     \
      profiler_count_##label += count;                  \
    } while (0)

// if we need to force the allocation
#  define AUTO_BASE_PROFILER_FORCE_ALLOCATION(label)                        \
    do {                                                                    \
      if (!AutoCount_##label) {                                             \
        /* Ignore that we could call this twice in theory, and that we leak \
         * them                                                             \
         */                                                                 \
        AutoCount_##label.reset(                                            \
            new ::mozilla::baseprofiler::BaseProfilerCount(                 \
                #label, &profiler_count_##label, &profiler_number_##label,  \
                profiler_category_##label, profiler_description_##label));  \
      }                                                                     \
    } while (0)

}  // namespace baseprofiler
}  // namespace mozilla

#endif  // !MOZ_GECKO_PROFILER

#endif  // BaseProfilerCounts_h

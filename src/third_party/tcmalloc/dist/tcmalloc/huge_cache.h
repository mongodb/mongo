// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Wrapping interface for HugeAllocator that handles backing and
// unbacking, including a hot cache of backed single hugepages.
#ifndef TCMALLOC_HUGE_CACHE_H_
#define TCMALLOC_HUGE_CACHE_H_
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <limits>

#include "absl/base/attributes.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/time/time.h"
#include "tcmalloc/huge_address_map.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/timeseries_tracker.h"
#include "tcmalloc/metadata_allocator.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class MemoryModifyFunction {
 public:
  virtual ~MemoryModifyFunction() = default;

  ABSL_MUST_USE_RESULT virtual bool operator()(void* start, size_t len) = 0;
};

// Track the extreme values of a HugeLength value over the past
// kWindow (time ranges approximate.)
template <size_t kEpochs = 16>
class MinMaxTracker {
 public:
  explicit constexpr MinMaxTracker(Clock clock, absl::Duration w)
      : kEpochLength(w / kEpochs), timeseries_(clock, w) {}

  void Report(HugeLength val);
  void Print(Printer* out) const;
  void PrintInPbtxt(PbtxtRegion* hpaa) const;

  // If t < kEpochLength, these functions return statistics for last epoch. The
  // granularity is kEpochLength (rounded up).
  HugeLength MaxOverTime(absl::Duration t) const;
  HugeLength MinOverTime(absl::Duration t) const;

 private:
  const absl::Duration kEpochLength;

  static constexpr HugeLength kMaxVal =
      NHugePages(std::numeric_limits<size_t>::max());
  struct Extrema {
    HugeLength min, max;

    static Extrema Nil() {
      Extrema e;
      e.max = NHugePages(0);
      e.min = kMaxVal;
      return e;
    }

    void Report(HugeLength n) {
      max = std::max(max, n);
      min = std::min(min, n);
    }

    bool empty() const { return (*this == Nil()); }

    bool operator==(const Extrema& other) const;
  };

  TimeSeriesTracker<Extrema, HugeLength, kEpochs> timeseries_;
};

// Explicit instantiations are defined in huge_cache.cc.
extern template class MinMaxTracker<>;
extern template class MinMaxTracker<600>;

template <size_t kEpochs>
constexpr HugeLength MinMaxTracker<kEpochs>::kMaxVal;

class HugeCache {
 public:
  // For use in production
  HugeCache(HugeAllocator* allocator,
            MetadataAllocator& meta_allocate ABSL_ATTRIBUTE_LIFETIME_BOUND,
            MemoryModifyFunction& unback ABSL_ATTRIBUTE_LIFETIME_BOUND)
      : HugeCache(allocator, meta_allocate, unback,
                  Clock{.now = absl::base_internal::CycleClock::Now,
                        .freq = absl::base_internal::CycleClock::Frequency}) {}

  // For testing with mock clock.
  //
  // 2s (kCacheTime * 2) looks like an arbitrary window; it mostly is.
  //
  // Suffice to say that the below code (see MaybeGrowCacheLimit)
  // tries to make sure the cache is sized to protect a working set
  // that ebbs for 1 second, as a reasonable heuristic. This means it
  // needs 1s of historical data to examine.
  //
  // Why 2s duration, then? Two reasons:
  //
  // - (minor) granularity of epoch boundaries make me want to err towards
  //   keeping a bit too much data over a bit too little.
  //
  // - (major) hysteresis: in ReleaseCachedPages we try to detect
  //   mistaken cache expansion and reverse it. I hope that using a
  //   longer timescale than our expansion will increase stability
  //   here: I will take some caches staying a bit too big over caches
  //   oscillating back and forth between two size estimates, so we
  //   require stronger evidence (longer time) to reverse an expansion
  //   than to make it.
  //
  // We also tried other algorithms, but this one is simple and suffices to
  // capture the empirical dynamics we've seen.  See "Beyond Malloc
  // Efficiency..." (https://research.google/pubs/pub50370/) for more
  // information.
  HugeCache(HugeAllocator* allocator,
            MetadataAllocator& meta_allocate ABSL_ATTRIBUTE_LIFETIME_BOUND,
            MemoryModifyFunction& unback ABSL_ATTRIBUTE_LIFETIME_BOUND,
            Clock clock)
      : allocator_(allocator),
        cache_(meta_allocate),
        clock_(clock),
        cache_time_ticks_(clock_.freq() * absl::ToDoubleSeconds(kCacheTime)),
        nanoseconds_per_tick_(absl::ToInt64Nanoseconds(absl::Seconds(1)) /
                              clock_.freq()),
        last_limit_change_(clock.now()),
        last_regret_update_(clock.now()),
        detailed_tracker_(clock, absl::Minutes(10)),
        usage_tracker_(clock, kCacheTime * 2),
        off_peak_tracker_(clock, kCacheTime * 2),
        size_tracker_(clock, kCacheTime * 2),
        unback_(unback) {}
  // Allocate a usable set of <n> contiguous hugepages.  Try to give out
  // memory that's currently backed from the kernel if we have it available.
  // *from_released is set to false if the return range is already backed;
  // otherwise, it is set to true (and the caller should back it.)
  HugeRange Get(HugeLength n, bool* from_released);

  // Deallocate <r> (assumed to be backed by the kernel.)
  void Release(HugeRange r);
  // As Release, but the range is assumed to _not_ be backed.
  void ReleaseUnbacked(HugeRange r);

  // Release to the system up to <n> hugepages of cache contents; returns
  // the number of hugepages released.
  HugeLength ReleaseCachedPages(HugeLength n);

  // Backed memory available.
  HugeLength size() const { return size_; }
  // Total memory cached (in HugeLength * nanoseconds)
  uint64_t regret() const { return regret_ * nanoseconds_per_tick_; }
  // Current limit for how much backed memory we'll cache.
  HugeLength limit() const { return limit_; }
  // Sum total of unreleased requests.
  HugeLength usage() const { return usage_; }

  void AddSpanStats(SmallSpanStats* small, LargeSpanStats* large) const;

  BackingStats stats() const {
    BackingStats s;
    s.system_bytes = (usage() + size()).in_bytes();
    s.free_bytes = size().in_bytes();
    s.unmapped_bytes = 0;
    return s;
  }

  void Print(Printer* out);
  void PrintInPbtxt(PbtxtRegion* hpaa);

 private:
  HugeAllocator* allocator_;

  // We just cache-missed a request for <missed> pages;
  // should we grow?
  void MaybeGrowCacheLimit(HugeLength missed);
  // Check if the cache seems consistently too big.  Returns the
  // number of pages *evicted* (not the change in limit).
  HugeLength MaybeShrinkCacheLimit();

  // Ensure the cache contains at most <target> hugepages,
  // returning the number removed.
  HugeLength ShrinkCache(HugeLength target);

  HugeRange DoGet(HugeLength n, bool* from_released);

  HugeAddressMap::Node* Find(HugeLength n);

  HugeAddressMap cache_;
  HugeLength size_{NHugePages(0)};

  HugeLength limit_{NHugePages(10)};
  const absl::Duration kCacheTime = absl::Seconds(1);

  size_t hits_{0};
  size_t misses_{0};
  size_t fills_{0};
  size_t overflows_{0};
  uint64_t weighted_hits_{0};
  uint64_t weighted_misses_{0};

  // Sum(size of Gets) - Sum(size of Releases), i.e. amount of backed
  // hugepages our user currently wants to have.
  void IncUsage(HugeLength n);
  void DecUsage(HugeLength n);
  HugeLength usage_{NHugePages(0)};

  // This is CycleClock, except overridable for tests.
  Clock clock_;
  const int64_t cache_time_ticks_;
  const double nanoseconds_per_tick_;

  int64_t last_limit_change_;

  // 10 hugepages is a good baseline for our cache--easily wiped away
  // by periodic release, and not that much memory on any real server.
  // However, we can go below it if we haven't used that much for 30 seconds.
  HugeLength MinCacheLimit() const { return NHugePages(10); }

  uint64_t regret_{0};  // overflows if we cache 585 hugepages for 1 year
  int64_t last_regret_update_;
  void UpdateSize(HugeLength size);

  MinMaxTracker<600> detailed_tracker_;

  MinMaxTracker<> usage_tracker_;
  MinMaxTracker<> off_peak_tracker_;
  MinMaxTracker<> size_tracker_;

  HugeLength total_fast_unbacked_{NHugePages(0)};
  HugeLength total_periodic_unbacked_{NHugePages(0)};

  MemoryModifyFunction& unback_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_CACHE_H_

// Copyright 2021 The TCMalloc Authors
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

#ifndef TCMALLOC_LIFETIME_BASED_ALLOCATOR_H_
#define TCMALLOC_LIFETIME_BASED_ALLOCATOR_H_

#include "absl/base/thread_annotations.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/lifetime_predictions.h"
#include "tcmalloc/internal/lifetime_tracker.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Configures lifetime-based allocation mechanisms.
class LifetimePredictionOptions {
 public:
  enum class Mode {
    // If enabled, predict lifetimes of allocations that lead to filler
    // donations and put donated pages of short-lived objects (i.e., lifetime
    // below a threshold) into a separate region.
    kEnabled,
    // When disabled, behavior does not change.
    kDisabled,
    // Predict lifetimes but only calculate the correctness and the region
    // memory without changing the actual behavior of the filler.
    kCounterfactual,
  };

  // This option determines the policy that the lifetime-based allocator (if
  // switched on) should use to handle allocations.
  enum class Strategy {
    // Predict everything to be short-lived and allocate into regions.
    kAlwaysShortLivedRegions,
    // Predict lifetime and put short-lived into regions. Lifetimes are
    // predicted based on the stack trace; if a significantly larger number of
    // objects emanating from the same stack trace are short-lived than long-
    // lived, the allocation will be predicted short-lived. The default
    // prediction is long-lived (i.e., no special behavior).
    kPredictedLifetimeRegions,
  };

  LifetimePredictionOptions(Mode mode, Strategy strategy,
                            absl::Duration short_lived_threshold)
      : mode_(mode),
        strategy_(strategy),
        short_lived_threshold_(short_lived_threshold) {}

  // Returns true iff we perform any lifetime-related operations, whether or
  // not it changes allocation behavior.
  bool active() const { return mode_ != Mode::kDisabled; }

  // Returns true iff we always predict allocations as short-lived.
  bool always_predict_short_lived() const {
    return active() && strategy_ == Strategy::kAlwaysShortLivedRegions;
  }

  // Returns true iff we actually change allocation behavior.
  bool enabled() const { return (mode_ == Mode::kEnabled); }

  // Returns true iff we perform a counterfactual analysis.
  bool counterfactual() const { return (mode_ == Mode::kCounterfactual); }

  LifetimeStats::Certainty certainty() const {
    // TODO(mmaas): Consider making this configurable.
    return LifetimeStats::Certainty::kHighCertainty;
  }

  absl::Duration threshold() const { return short_lived_threshold_; }

  Strategy strategy() const { return strategy_; }

  Mode mode() const { return mode_; }

  bool error() const { return contains_parse_error_; }

  void Print(Printer *out) const;

  // Expects flag to have format "mode[;strategy][;threshold_in_ms]" with
  // mode being one of ["enabled", "counterfactual", "disabled"] and strategy
  // being one of ["predict_region", "always_region"].
  static LifetimePredictionOptions FromFlag(absl::string_view flag);

  static LifetimePredictionOptions Default() {
    return LifetimePredictionOptions(Mode::kDisabled,
                                     Strategy::kPredictedLifetimeRegions,
                                     absl::Milliseconds(500));
  }

 private:
  static LifetimePredictionOptions DefaultWithParseError() {
    LifetimePredictionOptions opts = Default();
    opts.contains_parse_error_ = true;
    return opts;
  }

  Mode mode_;
  Strategy strategy_;
  absl::Duration short_lived_threshold_;
  bool contains_parse_error_ = false;
};

// Represents a region in memory used for lifetime-based allocation. Everything
// predicted to be short-lived is placed into this region instead of the filler.
// Can be used in a counterfactual setup to perform a what-if analysis without
// actually allocating memory. Interface is similar to HugeRegionSet.
class LifetimeBasedRegion {
 public:
  static constexpr Length kPagesPerBlock = kPagesPerHugePage / 2;
  static constexpr int kNumBlocks =
      HugeRegion::size().in_pages() / kPagesPerBlock;

  struct Stats {
    int64_t allocations = 0;
    int64_t deallocations = 0;
    int64_t allocated_pages = 0;
    int64_t deallocated_pages = 0;
  };

  // Since everything allocated into the lifetime-based region is at least half
  // a hugepage in size (i.e., 1 MiB), we store one LifetimeStats object per
  // MiB in the region. For a 1 GiB region, this metadata is 48 KiB.
  struct LifetimeMetaData : public TList<LifetimeMetaData>::Elem {
    // Matches the region this metadata belongs to.
    HugeRange location;

    // The actual metadata, one entry for each 1/2 HP block of the region.
    LifetimeTracker::Tracker stats[kNumBlocks];
  };

  explicit LifetimeBasedRegion(bool counterfactual)
      : counterfactual_(counterfactual) {}

  // Tries to allocate from the lifetime region. Fails if counterfactual or if
  // the maximum size of the lifetime region is reached.
  bool MaybeGet(Length n, PageId *page, bool *from_released,
                LifetimeTracker::Tracker **stats) {
    CHECK_CONDITION(n > kPagesPerHugePage / 2);

    if (!regions_.MaybeGet(n, page, from_released)) {
      return false;
    }
    ++stats_.allocations;
    stats_.allocated_pages += n.raw_num();
    if (stats != nullptr) {
      *stats = GetMetaData(*page);
    }
    return true;
  }

  // Returns an allocation to the lifetime region. Only succeeds if not running
  // in counterfactual mode and if the allocation is within this region.
  bool MaybePut(PageId p, Length n,
                LifetimeTracker::Tracker **stats = nullptr) {
    bool result = regions_.MaybePut(p, n);
    if (result) {
      if (stats != nullptr) {
        *stats = GetMetaData(p);
      }
      ++stats_.deallocations;
      stats_.deallocated_pages += n.raw_num();
    }
    return result;
  }

  // Adds region to the set. The metadata must have been allocated and
  // initialized externally and match the region.
  void Contribute(HugeRegion *region, LifetimeMetaData *metadata) {
    regions_.Contribute(region);
    if (!counterfactual_) {
      metadata_.append(metadata);
    }
  }

  Stats stats() const { return stats_; }

  // If not running in counterfactual mode, returns actual stats. Otherwise 0.
  BackingStats backing_stats() const {
    if (counterfactual_) {
      return BackingStats();
    } else {
      return regions_.stats();
    }
  }

  void AddSpanStats(SmallSpanStats *small, LargeSpanStats *large,
                    PageAgeHistograms *ages) const {
    if (!counterfactual_) {
      regions_.AddSpanStats(small, large, ages);
    }
  }

  // When allocating a new region, start here to ensure unique addresses. Only
  // used in counterfactual mode.
  HugePage NextRegionOffset() {
    ASSERT(counterfactual_);
    HugePage ret = HugePageContaining(reinterpret_cast<void *>(region_offset_));
    region_offset_ += HugeRegion::size().in_bytes();
    return ret;
  }

  // Switches counterfactual mode for reconfiguring LifetimeRegion at runtime.
  // This will only succeed if the region has not been used yet. Returns true
  // if and only if the switch was successful.
  bool SwitchCounterfactual(bool counterfactual) {
    if (stats_.allocations > 0) {
      return false;
    }
    counterfactual_ = counterfactual;
    return true;
  }

 private:
  // For an allocation in this region, returns the corresponding lifetime
  // metadata. This is only valid when not in counterfactual mode.
  LifetimeTracker::Tracker *GetMetaData(PageId page) {
    ASSERT(!counterfactual_);
    for (LifetimeMetaData *metadata : metadata_) {
      if (metadata->location.contains(page)) {
        Length region_offset = Length(
            page.index() - metadata->location.start().first_page().index());
        ASSERT(region_offset < kNumBlocks * kPagesPerHugePage);
        return &metadata->stats[region_offset / kPagesPerBlock];
      }
    }

    return nullptr;
  }

  HugeRegionSet<HugeRegion> regions_;
  TList<LifetimeMetaData> metadata_;
  bool counterfactual_;
  Stats stats_;
  uintptr_t region_offset_ = HugeRegion::size().in_bytes();
};

using HugeRegionAllocFunction = absl::FunctionRef<HugeRegion *()>;
using BackingMemoryAllocFunction = absl::FunctionRef<HugeRange(HugeLength)>;

// Determines whether an object is likely to be short-lived and places those
// objects into a LifetimeBasedRegion. Tracks object lifetimes both for
// counterfactual analysis and donated objects that are being profiled.
class LifetimeBasedAllocator {
 public:
  // The outcome of an allocation attempt.
  struct AllocationResult {
    // Was the object placed in short-lived region? (including counterfactual)
    bool allocated = false;

    // Is this a counterfactual allocation? (i.e., we still need to allocate
    // a "real" object somewhere else for it.)
    bool is_counterfactual = false;

    // Was this object predicted to be short-lived on allocation?
    bool predicted_short_lived = false;

    // Lifetime statistics associated with the allocation site that generated
    // this allocation. If no predicted lifetime is attached with this
    // allocation, the value is nullptr.
    LifetimeStats *lifetime = nullptr;

    // The location in the short-lived region this was allocated at.
    PageId page_id{};

    // Initialize a fresh tracker associated with this allocation.
    void InitTracker(LifetimeTracker::Tracker *tracker) {
      ASSERT(lifetime != nullptr);
      tracker->lifetime = lifetime;
      tracker->predicted_short_lived = predicted_short_lived;
      if (allocated && is_counterfactual) {
        tracker->counterfactual_ptr = page_id.start_addr();
      }
    }

    // Returns true if and only if a real object was allocated that can be
    // finalized and used. If so, sets page to this object.
    bool TryGetAllocation(PageId *page) {
      if (allocated && !is_counterfactual) {
        *page = page_id;
        return true;
      } else {
        return false;
      }
    }
  };

  struct Stats {
    LifetimePredictionOptions opts;
    LifetimeTracker::Stats tracker;
    LifetimeBasedRegion::Stats region;
    size_t database_size;
    size_t database_evictions;
  };

  // This interface needs to be implemented by the allocator that instantiates
  // the lifetime-based allocator. Since these functions are called very rarely,
  // the virtual function call overheads are acceptable.
  class RegionAlloc {
   public:
    virtual ~RegionAlloc() = default;

    // Allocates a new HugeRegion of size n. If `range` is valid, uses this
    // range to back the region. Otherwise, allocates the appropriate amount
    // of backing memory itself and writes the location of the allocated memory
    // to the variable pointed to by `range`.
    virtual HugeRegion *AllocRegion(HugeLength n, HugeRange *range) = 0;
  };

  LifetimeBasedAllocator(
      LifetimePredictionOptions lifetime_opts, RegionAlloc *region_allocator,
      Clock clock = {.now = absl::base_internal::CycleClock::Now,
                     .freq = absl::base_internal::CycleClock::Frequency});

  bool IsActive() const { return lifetime_opts_.active(); }

  bool IsCounterfactual() const { return lifetime_opts_.counterfactual(); }

  // Checks if the lifetime-based allocation policy applies, and if so, collects
  // the context of this allocation for later use in lifetime-based memory
  // management. Otherwise returns nullptr. This is called before the pageheap
  // lock is acquired.
  inline LifetimeStats *CollectLifetimeContext(Length n)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    // If lifetime allocation is disabled, don't do anything.
    if (!is_active_.load(std::memory_order_acquire)) {
      return nullptr;
    }

    // Ignore allocation requests that would not be handled by "large"
    // allocations, which are the only allocations handled by the lifetime-
    // aware allocator.
    if ((n <= kPagesPerHugePage / 2) || (n > HugeRegion::size().in_pages())) {
      return nullptr;
    }

    // TODO(b/207321377): Most allocations on this path are also sampled. An
    // optimization could reuse the stack trace instead of capturing it twice.
    return LookupOrAddLifetimeStats();
  }

  // Predicts whether an object is likely short-lived. If so, tries to allocate
  // in the short-lived region (either counterfactually or real).
  AllocationResult MaybeGet(Length n, bool *from_released, LifetimeStats *stats)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    // There is a rare corner case where the lifetime stats were reused by
    // another thread between the time we allocated them and reacquiring the
    // page heap lock. In that case, we act like this is not a lifetime
    // allocation at all.
    if (stats == nullptr) {
      return AllocationResult{.allocated = false, .lifetime = nullptr};
    }

    // All fields are pre-filled except page_id and allocated (which depend
    // on whether and where a lifetime-based allocation occurs).
    AllocationResult result{
        .allocated = false,
        .is_counterfactual = IsCounterfactual(),
        .predicted_short_lived = lifetime_opts_.always_predict_short_lived() ||
                                 (stats->Predict(lifetime_opts_.certainty()) ==
                                  LifetimeStats::Prediction::kShortLived),
        .lifetime = stats};

    // Put everything that is predicted to be short-lived into the short-lived
    // region (if counterfactual analysis is enabled, this will only track the
    // allocation there but not actually allocate backing memory).
    if (result.predicted_short_lived) {
      LifetimeTracker::Tracker *tracker;
      bool region_allocated =
          lifetime_region_.MaybeGet(n, &result.page_id, from_released,
                                    IsCounterfactual() ? nullptr : &tracker);
      if (!region_allocated) {
        // If unable to allocate from a lifetime region, try adding a new one.
        if (AddLifetimeRegion()) {
          region_allocated = lifetime_region_.MaybeGet(
              n, &result.page_id, from_released,
              IsCounterfactual() ? nullptr : &tracker);
        }
      }

      if (region_allocated) {
        result.allocated = true;
        if (lifetime_opts_.enabled()) {
          // Only track the lifetime of this allocation if we are not using
          // counterfactual analysis; if we are, the allocation is tracked
          // by the filler as usual.
          CHECK_CONDITION(tracker != nullptr);
          result.InitTracker(tracker);
          lifetime_tracker_.AddAllocation(tracker, tracker->lifetime,
                                          result.predicted_short_lived);
        }
      } else {
        // Fail gracefully by falling back on the regular allocator.
        return AllocationResult{.allocated = false, .lifetime = nullptr};
      }
    }

    return result;
  }

  // Registers a tracker for an object that is not in the short-lived region
  // but whose lifetime we still need to track.
  void MaybeAddTracker(AllocationResult allocation_result,
                       LifetimeTracker::Tracker *tracker) {
    if (allocation_result.lifetime == nullptr) {
      return;
    }

    CHECK_CONDITION(tracker != nullptr);
    allocation_result.InitTracker(tracker);
    CHECK_CONDITION(tracker->lifetime != nullptr);
    lifetime_tracker_.AddAllocation(tracker, tracker->lifetime,
                                    tracker->predicted_short_lived);
  }

  // Tries to return a previously registered tracker (both for counterfactual
  // analysis and tracking donated objects) and updates lifetime statistics.
  void MaybePutTracker(LifetimeTracker::Tracker *stats, Length n) {
    CHECK_CONDITION(stats != nullptr);
    if (stats->counterfactual_ptr != nullptr) {
      lifetime_region_.MaybePut(PageIdContaining(stats->counterfactual_ptr), n);

      // We need to set counterfactual_ptr to nullptr now since it is possible
      // that we are looking at a donated hugepage that is not empty yet. If
      // we do not update the counterfactual_ptr, we may try to release this
      // counterfactual object a second time when the page becomes free.
      stats->counterfactual_ptr = nullptr;
    }
    lifetime_tracker_.RemoveAllocation(stats);
  }

  // Returns an object that was allocated in the short-lived region.
  bool MaybePut(PageId p, Length n) {
    LifetimeTracker::Tracker *stats;
    if (lifetime_opts_.enabled() && lifetime_region_.MaybePut(p, n, &stats)) {
      ASSERT(stats != nullptr);
      CHECK_CONDITION(stats->counterfactual_ptr == nullptr);
      lifetime_tracker_.RemoveAllocation(stats);
      return true;
    }
    return false;
  }

  // If short-lived allocations are enabled, returns statistics of the short-
  // lived region (for mallocz reporting).
  absl::optional<BackingStats> GetRegionStats() const {
    if (!IsActive() || IsCounterfactual()) {
      return absl::nullopt;
    } else {
      return lifetime_region_.backing_stats();
    }
  }

  // Enables the lifetime-based allocator at runtime. This only succeeds when
  // switching from disabled to lifetime region-based allocation (either
  // counterfactual or non-counterfactual). Note that switching the mechanism
  // off again is not possible. Returns true if and only if the lifetime
  // allocator's settings upon return are identical to the supplied options.
  bool Enable(LifetimePredictionOptions options);

  Stats GetStats() const;

  void Print(Printer *out) const ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void PrintInPbtxt(PbtxtRegion *hpaa) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

 private:
  // Snapshots the current stack trace and looks the lifetime statistics
  // associated with this allocation stack trace. If no lifetime statistics
  // exist, allocates them and returns the freshly allocated statistics.
  LifetimeStats *LookupOrAddLifetimeStats() ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    // Collecting the stack trace can take a substantial amount of time, so
    // we do this without holding the pageheap lock.
    LifetimeDatabase::Key k = LifetimeDatabase::Key::RecordCurrentKey();
    LifetimeStats *lifetime = lifetime_database_.LookupOrAddLifetimeStats(&k);
    return lifetime;
  }

  bool AddLifetimeRegion() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  LifetimePredictionOptions lifetime_opts_;
  LifetimeDatabase lifetime_database_;
  LifetimeBasedRegion lifetime_region_;
  LifetimeTracker lifetime_tracker_;
  PageHeapAllocator<typename LifetimeBasedRegion::LifetimeMetaData>
      lifetime_stats_allocator_;

  RegionAlloc *region_alloc_;

  std::atomic<bool> is_active_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_LIFETIME_BASED_ALLOCATOR_H_

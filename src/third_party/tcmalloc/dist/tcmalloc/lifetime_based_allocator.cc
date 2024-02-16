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

#include "tcmalloc/lifetime_based_allocator.h"

#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

void LifetimePredictionOptions::Print(Printer *out) const {
  if (mode_ == Mode::kDisabled) {
    out->printf("disabled");
  } else {
    if (mode_ == Mode::kEnabled) {
      out->printf("enabled / ");
    } else if (mode_ == Mode::kCounterfactual) {
      out->printf("counterfactual / ");
    }
    if (strategy_ == Strategy::kAlwaysShortLivedRegions) {
      out->printf("always region");
    } else {
      out->printf("short-lived regions");
    }
  }

  if (contains_parse_error_) {
    out->printf(" (default due to flag parse error!)");
  }
}

LifetimePredictionOptions LifetimePredictionOptions::FromFlag(
    absl::string_view flag) {
  LifetimePredictionOptions opts = Default();

  if (flag == "disabled") {
    return opts;
  }

  int counter = 0;
  for (absl::string_view s : absl::StrSplit(flag, ';')) {
    switch (counter++) {
      case 0: {
        if (s == "enabled") {
          opts.mode_ = Mode::kEnabled;
        } else if (s == "counterfactual") {
          opts.mode_ = Mode::kCounterfactual;
        } else {
          return DefaultWithParseError();
        }
        break;
      }
      case 1: {
        if (s == "always_region") {
          opts.strategy_ = Strategy::kAlwaysShortLivedRegions;
        } else if (s == "predict_region") {
          opts.strategy_ = Strategy::kPredictedLifetimeRegions;
        } else {
          return DefaultWithParseError();
        }
        break;
      }
      case 2: {
        int short_lived_threshold_ms;
        if (!absl::SimpleAtoi(s, &short_lived_threshold_ms)) {
          return DefaultWithParseError();
        }
        opts.short_lived_threshold_ =
            absl::Milliseconds(short_lived_threshold_ms);
        break;
      }
      default: {
        return DefaultWithParseError();
      }
    }
  }

  return opts;
}

LifetimeBasedAllocator::LifetimeBasedAllocator(
    LifetimePredictionOptions lifetime_opts, RegionAlloc *region_alloc,
    Clock clock)
    : lifetime_opts_(lifetime_opts),
      lifetime_region_(lifetime_opts.counterfactual()),
      lifetime_tracker_(&lifetime_database_, lifetime_opts.threshold(), clock),
      region_alloc_(region_alloc),
      is_active_(lifetime_opts.active()) {
  lifetime_stats_allocator_.Init(&tc_globals.arena());
}

LifetimeBasedAllocator::Stats LifetimeBasedAllocator::GetStats() const {
  return Stats{.opts = lifetime_opts_,
               .tracker = lifetime_tracker_.stats(),
               .region = lifetime_region_.stats(),
               .database_size = lifetime_database_.size(),
               .database_evictions = lifetime_database_.evictions()};
}

bool LifetimeBasedAllocator::Enable(LifetimePredictionOptions options)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
  // We disallow switching lifetime-based allocation off since there may already
  // be lifetime-based allocations in flight.
  if (lifetime_opts_.active() && !options.active()) return false;

  // We disallow changing the lifetime threshold since the tracker may already
  // use it to track allocations that are in flight.
  // TODO(mmaas): This could be relaxed at a later point.
  if (lifetime_opts_.threshold() != options.threshold()) return false;

  // We only allow switching between counterfactual and enabled mode before the
  // first lifetime-based allocation.
  if (!lifetime_region_.SwitchCounterfactual(options.counterfactual())) {
    return false;
  }

  lifetime_opts_ = options;
  is_active_.store(options.active(), std::memory_order_release);
  return true;
}

void LifetimeBasedAllocator::Print(Printer *out) const {
  Stats stats = GetStats();

  if (stats.opts.active()) {
    out->printf("HugePageAware: *** Lifetime-based regions: ***\n");
    out->printf(
        "HugePageAware: Predictions: %zu short / %zu long lived "
        "(%zu expired, %zu overestimated)\n",
        stats.tracker.short_lived_predictions,
        stats.tracker.long_lived_predictions, stats.tracker.expired_lifetimes,
        stats.tracker.overestimated_lifetimes);
    out->printf("HugePageAware: Lifetime-based allocations (");
    stats.opts.Print(out);
    out->printf(
        "): Threshold = %.2fs, "
        "%zu stack traces (%zu evicted)\n",
        absl::ToDoubleSeconds(stats.opts.threshold()), stats.database_size,
        stats.database_evictions);
    out->printf(
        "LifetimeBasedRegion: %ld allocated (%ld pages), %ld freed (%ld "
        "pages) in lifetime region.\n",
        stats.region.allocations, stats.region.allocated_pages,
        stats.region.deallocations, stats.region.deallocated_pages);
    out->printf("\n");
  } else {
    out->printf("HugePageAware: Lifetime-based allocations disabled.\n");
  }
}

void LifetimeBasedAllocator::PrintInPbtxt(PbtxtRegion *hpaa) const {
  Stats stats = GetStats();
  PbtxtRegion region = hpaa->CreateSubRegion("lifetime_based_allocator_stats");
  region.PrintBool("enabled", stats.opts.active());

  if (stats.opts.active()) {
    region.PrintBool("counterfactual", stats.opts.counterfactual());
    region.PrintI64("threshold_ms",
                    absl::ToInt64Milliseconds(stats.opts.threshold()));
    region.PrintI64("num_predicted_short_lived",
                    stats.tracker.short_lived_predictions);
    region.PrintI64("num_predicted_long_lived",
                    stats.tracker.long_lived_predictions);
    region.PrintI64("num_expired", stats.tracker.expired_lifetimes);
    region.PrintI64("num_overestimated", stats.tracker.overestimated_lifetimes);
    region.PrintI64("database_size", stats.database_size);
    region.PrintI64("database_evicted_count", stats.database_evictions);
    region.PrintI64("lifetime_region_allocated", stats.region.allocations);
    region.PrintI64("lifetime_region_allocated_pages",
                    stats.region.allocated_pages);
    region.PrintI64("lifetime_region_freed", stats.region.deallocations);
    region.PrintI64("lifetime_region_freed_pages",
                    stats.region.deallocated_pages);
  }
}

bool LifetimeBasedAllocator::AddLifetimeRegion()
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
  HugeRange range = HugeRange::Nil();
  HugeRegion *region = nullptr;
  LifetimeBasedRegion::LifetimeMetaData *metadata = nullptr;

  if (lifetime_opts_.counterfactual()) {
    // A counterfactual region looks like a normal region but is not actually
    // backed by any memory. Since memory is only backed when an allocation is
    // finalized, we can use the same class but apply it to an unbacked range.
    range = HugeRange::Make(lifetime_region_.NextRegionOffset(),
                            HugeRegion::size());
  }

  region = region_alloc_->AllocRegion(HugeRegion::size(), &range);
  if (region == nullptr) {
    return false;
  }

  if (!lifetime_opts_.counterfactual()) {
    // When lifetime-based allocations are enabled, run with an actual region.
    metadata = lifetime_stats_allocator_.New();
    new (metadata) typename LifetimeBasedRegion::LifetimeMetaData();
    metadata->location = range;
  }

  lifetime_region_.Contribute(region, metadata);
  return true;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

// Copyright 2020 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_LIFETIME_TRACKER_H_
#define TCMALLOC_INTERNAL_LIFETIME_TRACKER_H_

#include "absl/base/internal/cycleclock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/lifetime_predictions.h"
#include "tcmalloc/internal/linked_list.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

template <typename LifetimeDatabaseT, typename LifetimeStatsT>
class LifetimeTrackerImpl {
 public:
  // A tracker is attached to an individual allocation and tracks its lifetime.
  // This allocation can either be in a region or in the filler. It contains
  // a pointer back to the LifetimeStats of the allocation site that generated
  // this allocation, so that statistics can be updated.
  struct Tracker : public TList<Tracker>::Elem {
    // The deadline after which the object is considered long-lived.
    uint64_t deadline = 0;

    // If the allocation is associated with a counterfactual, this contains
    // the hypothetical location in the short-lived region (null otherwise).
    void* counterfactual_ptr = nullptr;

    // Lifetime statistics associated with this allocation (will be updated when
    // the lifetime is known).
    LifetimeStatsT* lifetime = nullptr;

    // The allocation this stat belongs to was predicted short-lived.
    bool predicted_short_lived = false;

    // Is this element currently tracked by the lifetime tracker?
    bool is_tracked() { return deadline != 0; }

    // Reset the element (implies not tracked).
    void reset() { deadline = 0; }
  };

  struct Stats {
    uint64_t expired_lifetimes = 0;
    uint64_t overestimated_lifetimes = 0;
    uint64_t short_lived_predictions = 0;
    uint64_t long_lived_predictions = 0;
  };

  explicit LifetimeTrackerImpl(
      LifetimeDatabaseT* lifetime_database, absl::Duration timeout,
      Clock clock = Clock{.now = absl::base_internal::CycleClock::Now,
                          .freq = absl::base_internal::CycleClock::Frequency})
      : timeout_(absl::ToDoubleSeconds(timeout) * clock.freq()),
        lifetime_database_(*lifetime_database),
        clock_(clock) {}

  // Registers a donated allocation with the tracker.
  void AddAllocation(Tracker* tracker, LifetimeStatsT* lifetime,
                     bool predicted_short_lived) {
    CheckForLifetimeExpirations();

    if (predicted_short_lived) {
      stats_.short_lived_predictions++;
    } else {
      stats_.long_lived_predictions++;
    }

    ASSERT(tracker != nullptr);
    ASSERT(lifetime != nullptr);
    tracker->deadline = clock_.now() + timeout_;
    tracker->lifetime = lifetime;
    tracker->predicted_short_lived = predicted_short_lived;
    list_.append(tracker);
  }

  // Remove an allocation from the tracker. This will stop tracking the
  // allocation and record whether it was correctly predicted.
  void RemoveAllocation(Tracker* tracker) {
    CheckForLifetimeExpirations();

    // This is not tracked anymore.
    if (!tracker->is_tracked()) {
      return;
    }

    if (!tracker->predicted_short_lived) {
      stats_.overestimated_lifetimes++;
    }

    if (tracker->lifetime != nullptr) {
      tracker->lifetime->Update(LifetimeStatsT::Prediction::kShortLived);
      lifetime_database_.RemoveLifetimeStatsReference(tracker->lifetime);
    }

    tracker->reset();

    list_.remove(tracker);
  }

  // Check whether any lifetimes in the tracker have passed the threshold after
  // which they are not short-lived anymore.
  void CheckForLifetimeExpirations() {
    // TODO(mmaas): Expirations are fairly cheap, but there is a theoretical
    // possibility of having an arbitrary number of expirations at once, which
    // could affect tail latency. We may want to limit the number of pages we
    // let expire per unit time.
    uint64_t now = clock_.now();
    Tracker* tracker = TryGetExpired(now);
    while (tracker != nullptr) {
      ASSERT(tracker->is_tracked());

      // A page that was predicted short-lived was actually long-lived.
      if (tracker->predicted_short_lived) {
        stats_.expired_lifetimes++;
      }

      if (tracker->lifetime != nullptr) {
        tracker->lifetime->Update(LifetimeStatsT::Prediction::kLongLived);
        lifetime_database_.RemoveLifetimeStatsReference(tracker->lifetime);
      }

      tracker->reset();
      tracker = TryGetExpired(now);
    }
  }

  Stats stats() const { return stats_; }

 private:
  // Returns the earliest expiring entry, or nullptr if none expired.
  Tracker* TryGetExpired(uint64_t now) {
    if (!list_.empty() && list_.first()->deadline < now) {
      Tracker* s = list_.first();
      list_.remove(s);
      return s;
    }
    return nullptr;
  }

  const uint64_t timeout_;

  TList<Tracker> list_;
  Stats stats_;
  LifetimeDatabaseT& lifetime_database_;
  Clock clock_;
};

using LifetimeTracker = LifetimeTrackerImpl<LifetimeDatabase, LifetimeStats>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_LIFETIME_TRACKER_H_

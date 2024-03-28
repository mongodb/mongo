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

#ifndef TCMALLOC_HUGE_PAGE_SUBRELEASE_H_
#define TCMALLOC_HUGE_PAGE_SUBRELEASE_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <limits>

#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/hinted_tracker_lists.h"
#include "tcmalloc/huge_cache.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/range_tracker.h"
#include "tcmalloc/internal/timeseries_tracker.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// This and the following classes implement the adaptive hugepage subrelease
// mechanism and realized fragmentation metric described in "Adaptive Hugepage
// Subrelease for Non-moving Memory Allocators in Warehouse-Scale Computers"
// (ISMM 2021).

// Tracks correctness of skipped subrelease decisions over time.
template <size_t kEpochs = 16>
class SkippedSubreleaseCorrectnessTracker {
 public:
  struct SkippedSubreleaseDecision {
    Length pages;  // Number of pages we skipped subreleasing.
    size_t count;  // Number of times we skipped a subrelease.

    SkippedSubreleaseDecision() : pages(0), count(0) {}
    explicit SkippedSubreleaseDecision(Length pages) : pages(pages), count(1) {}
    explicit SkippedSubreleaseDecision(Length pages, size_t count)
        : pages(pages), count(count) {}

    SkippedSubreleaseDecision& operator+=(SkippedSubreleaseDecision rhs) {
      pages += rhs.pages;
      count += rhs.count;
      return *this;
    }

    static SkippedSubreleaseDecision Zero() {
      return SkippedSubreleaseDecision();
    }
  };

  explicit constexpr SkippedSubreleaseCorrectnessTracker(Clock clock,
                                                         absl::Duration w)
      : window_(w),
        epoch_length_(window_ / kEpochs),
        last_confirmed_peak_(0),
        tracker_(clock, w) {}

  // Not copyable or movable
  SkippedSubreleaseCorrectnessTracker(
      const SkippedSubreleaseCorrectnessTracker&) = delete;
  SkippedSubreleaseCorrectnessTracker& operator=(
      const SkippedSubreleaseCorrectnessTracker&) = delete;

  void ReportSkippedSubreleasePages(
      Length skipped_pages, Length peak_pages,
      absl::Duration expected_time_until_next_peak) {
    total_skipped_ += SkippedSubreleaseDecision(skipped_pages);
    pending_skipped_ += SkippedSubreleaseDecision(skipped_pages);

    SkippedSubreleaseUpdate update;
    update.decision = SkippedSubreleaseDecision(skipped_pages);
    update.num_pages_at_decision = peak_pages;
    update.correctness_interval_epochs =
        expected_time_until_next_peak / epoch_length_;
    tracker_.Report(update);
  }

  void ReportUpdatedPeak(Length current_peak) {
    // Record this peak for the current epoch (so we don't double-count correct
    // predictions later) and advance the tracker.
    SkippedSubreleaseUpdate update;
    update.confirmed_peak = current_peak;
    if (tracker_.Report(update)) {
      // Also keep track of the largest peak we have confirmed this epoch.
      last_confirmed_peak_ = Length(0);
    }

    // Recompute currently pending decisions.
    pending_skipped_ = SkippedSubreleaseDecision::Zero();

    Length largest_peak_already_confirmed = last_confirmed_peak_;

    tracker_.IterBackwards(
        [&](size_t offset, int64_t ts, const SkippedSubreleaseEntry& e) {
          // Do not clear any decisions in the current epoch.
          if (offset == 0) {
            return;
          }

          if (e.decisions.count > 0 &&
              e.max_num_pages_at_decision > largest_peak_already_confirmed &&
              offset <= e.correctness_interval_epochs) {
            if (e.max_num_pages_at_decision <= current_peak) {
              // We can confirm a subrelease decision as correct and it had not
              // been confirmed correct by an earlier peak yet.
              correctly_skipped_ += e.decisions;
            } else {
              pending_skipped_ += e.decisions;
            }
          }

          // Did we clear any earlier decisions based on a peak in this epoch?
          // Keep track of the peak, so we do not clear them again.
          largest_peak_already_confirmed =
              std::max(largest_peak_already_confirmed, e.max_confirmed_peak);
        },
        -1);

    last_confirmed_peak_ = std::max(last_confirmed_peak_, current_peak);
  }

  inline SkippedSubreleaseDecision total_skipped() const {
    return total_skipped_;
  }

  inline SkippedSubreleaseDecision correctly_skipped() const {
    return correctly_skipped_;
  }

  inline SkippedSubreleaseDecision pending_skipped() const {
    return pending_skipped_;
  }

 private:
  struct SkippedSubreleaseUpdate {
    // A subrelease decision that was made at this time step: How much did we
    // decide not to release?
    SkippedSubreleaseDecision decision;

    // What does our future demand have to be for this to be correct? If there
    // were multiple subrelease decisions in the same epoch, use the max.
    Length num_pages_at_decision;

    // How long from the time of the decision do we have before the decision
    // will be determined incorrect?
    int64_t correctness_interval_epochs = 0;

    // At this time step, we confirmed a demand peak at this level, which means
    // all subrelease decisions in earlier time steps that had peak_demand_pages
    // <= this confirmed_peak were confirmed correct and don't need to be
    // considered again in the future.
    Length confirmed_peak;
  };

  struct SkippedSubreleaseEntry {
    SkippedSubreleaseDecision decisions = SkippedSubreleaseDecision::Zero();
    Length max_num_pages_at_decision;
    int64_t correctness_interval_epochs = 0;
    Length max_confirmed_peak;

    static SkippedSubreleaseEntry Nil() { return SkippedSubreleaseEntry(); }

    void Report(SkippedSubreleaseUpdate e) {
      decisions += e.decision;
      correctness_interval_epochs =
          std::max(correctness_interval_epochs, e.correctness_interval_epochs);
      max_num_pages_at_decision =
          std::max(max_num_pages_at_decision, e.num_pages_at_decision);
      max_confirmed_peak = std::max(max_confirmed_peak, e.confirmed_peak);
    }
  };

  const absl::Duration window_;
  const absl::Duration epoch_length_;

  // The largest peak we processed this epoch. This is required to avoid us
  // double-counting correctly predicted decisions.
  Length last_confirmed_peak_;

  SkippedSubreleaseDecision total_skipped_;
  SkippedSubreleaseDecision correctly_skipped_;
  SkippedSubreleaseDecision pending_skipped_;

  TimeSeriesTracker<SkippedSubreleaseEntry, SkippedSubreleaseUpdate, kEpochs>
      tracker_;
};

struct SkipSubreleaseIntervals {
  // Interval that locates recent demand peak.
  absl::Duration peak_interval;
  // Interval that locates recent short-term demand fluctuation.
  absl::Duration short_interval;
  // Interval that locates recent long-term demand trend.
  absl::Duration long_interval;
  // Checks if the peak interval is set.
  bool IsPeakIntervalSet() const {
    return peak_interval != absl::ZeroDuration();
  }
  // Checks if the skip subrelease feature is enabled.
  bool SkipSubreleaseEnabled() const {
    if (peak_interval != absl::ZeroDuration() ||
        short_interval != absl::ZeroDuration() ||
        long_interval != absl::ZeroDuration()) {
      return true;
    }
    return false;
  }
};

struct SubreleaseStats {
  Length total_pages_subreleased;  // cumulative since startup
  Length total_partial_alloc_pages_subreleased;  // cumulative since startup
  Length num_pages_subreleased;
  Length num_partial_alloc_pages_subreleased;
  HugeLength total_hugepages_broken{NHugePages(0)};  // cumulative since startup
  HugeLength num_hugepages_broken{NHugePages(0)};

  bool is_limit_hit = false;
  // Keep these limit-related stats cumulative since startup only
  Length total_pages_subreleased_due_to_limit;
  HugeLength total_hugepages_broken_due_to_limit{NHugePages(0)};

  void reset() {
    total_pages_subreleased += num_pages_subreleased;
    total_partial_alloc_pages_subreleased +=
        num_partial_alloc_pages_subreleased;
    total_hugepages_broken += num_hugepages_broken;
    num_pages_subreleased = Length(0);
    num_partial_alloc_pages_subreleased = Length(0);
    num_hugepages_broken = NHugePages(0);
  }

  // Must be called at the beginning of each subrelease request
  void set_limit_hit(bool value) { is_limit_hit = value; }

  // This only has a well-defined meaning within ReleaseCandidates where
  // set_limit_hit() has been called earlier. Do not use anywhere else.
  bool limit_hit() { return is_limit_hit; }
};

// Track subrelease statistics over a time window.
template <size_t kEpochs = 16>
class SubreleaseStatsTracker {
 public:
  enum Type {
    kRegular,
    kSparse = kRegular,
    kDense,
    kDonated,
    kPartialReleased,
    kReleased,
    kNumTypes
  };

  struct SubreleaseStats {
    Length num_pages;
    Length free_pages;
    Length unmapped_pages;
    Length used_pages_in_subreleased_huge_pages;
    HugeLength huge_pages[kNumTypes];
    Length num_pages_subreleased;
    Length num_partial_alloc_pages_subreleased;
    HugeLength num_hugepages_broken = NHugePages(0);

    HugeLength total_huge_pages() const {
      HugeLength total_huge_pages;
      for (int i = 0; i < kNumTypes; i++) {
        total_huge_pages += huge_pages[i];
      }
      return total_huge_pages;
    }
  };

  struct NumberOfFreePages {
    Length free;
    Length free_backed;
  };

  explicit constexpr SubreleaseStatsTracker(Clock clock, absl::Duration w,
                                            absl::Duration summary_interval)
      : summary_interval_(summary_interval),
        window_(w),
        epoch_length_(window_ / kEpochs),
        tracker_(clock, w),
        skipped_subrelease_correctness_(clock, w) {
    // The summary_interval is used in two trackers: SubreleaseStatsTracker for
    // evaluating realized fragmentation, and
    // SkippedSubreleaseCorrectnessTracker for evaluating the correctness of
    // skipped subrelease. Here we check the length of the two trackers are
    // sufficient for the evaluation.
    TC_ASSERT_LE(summary_interval, w);
  }

  // Not copyable or movable
  SubreleaseStatsTracker(const SubreleaseStatsTracker&) = delete;
  SubreleaseStatsTracker& operator=(const SubreleaseStatsTracker&) = delete;

  void Report(const SubreleaseStats& stats) {
    if (ABSL_PREDICT_FALSE(tracker_.Report(stats))) {
      if (ABSL_PREDICT_FALSE(pending_skipped().count > 0)) {
        // Consider the peak within the just completed epoch to confirm the
        // correctness of any recent subrelease decisions.
        skipped_subrelease_correctness_.ReportUpdatedPeak(std::max(
            stats.num_pages,
            tracker_.GetEpochAtOffset(1).stats[kStatsAtMaxDemand].num_pages));
      }
    }
  }

  void Print(Printer* out, absl::string_view field) const;
  void PrintSubreleaseStatsInPbtxt(PbtxtRegion* hpaa,
                                   absl::string_view field) const;
  void PrintTimeseriesStatsInPbtxt(PbtxtRegion* hpaa,
                                   absl::string_view field) const;

  // Calculates recent peaks for skipping subrelease decisions. If our allocated
  // memory is below the demand peak within the last peak_interval, we stop
  // subreleasing. If our demand is going above that peak again within another
  // realized fragemenation interval, we report that we made the correct
  // decision.
  Length GetRecentPeak(absl::Duration peak_interval) {
    last_skip_subrelease_intervals_.peak_interval =
        std::min(peak_interval, epoch_length_ * kEpochs);
    Length max_demand_pages;

    int64_t num_epochs =
        std::min<int64_t>(peak_interval / epoch_length_, kEpochs);

    tracker_.IterBackwards(
        [&](size_t offset, int64_t ts, const SubreleaseStatsEntry& e) {
          if (!e.empty()) {
            // Identify the maximum number of demand pages we have seen within
            // the time interval.
            if (e.stats[kStatsAtMaxDemand].num_pages > max_demand_pages) {
              max_demand_pages = e.stats[kStatsAtMaxDemand].num_pages;
            }
          }
        },
        num_epochs);

    return max_demand_pages;
  }

  // Calculates demand requirements for skip subrelease: HugePageFiller would
  // not subrelease if it has less pages than (or equal to) the required
  // amount. We report that the skipping is correct if future demand is going to
  // be above the required amount within another realized fragemenation
  // interval. The demand requirement is the sum of short-term demand
  // fluctuation peak and long-term demand trend. The former is the largest max
  // and min demand difference within short_interval, and the latter is the
  // largest min demand within long_interval. When both set, short_interval
  // should be (significantly) shorter or equal to long_interval to avoid
  // realized fragmentation caused by non-recent (short-term) demand spikes.
  Length GetRecentDemand(absl::Duration short_interval,
                         absl::Duration long_interval) {
    if (short_interval != absl::ZeroDuration() &&
        long_interval != absl::ZeroDuration()) {
      short_interval = std::min(short_interval, long_interval);
    }
    last_skip_subrelease_intervals_.short_interval =
        std::min(short_interval, epoch_length_ * kEpochs);
    last_skip_subrelease_intervals_.long_interval =
        std::min(long_interval, epoch_length_ * kEpochs);
    Length short_term_fluctuation_pages, long_term_trend_pages;
    int short_epochs = std::min<int>(short_interval / epoch_length_, kEpochs);
    int long_epochs = std::min<int>(long_interval / epoch_length_, kEpochs);

    tracker_.IterBackwards(
        [&](size_t offset, int64_t ts, const SubreleaseStatsEntry& e) {
          if (!e.empty()) {
            Length demand_difference = e.stats[kStatsAtMaxDemand].num_pages -
                                       e.stats[kStatsAtMinDemand].num_pages;
            // Identifies the highest demand fluctuation (i.e., difference
            // between max_demand and min_demand) that we have seen within the
            // time interval.
            if (demand_difference > short_term_fluctuation_pages) {
              short_term_fluctuation_pages = demand_difference;
            }
          }
        },
        short_epochs);
    tracker_.IterBackwards(
        [&](size_t offset, int64_t ts, const SubreleaseStatsEntry& e) {
          if (!e.empty()) {
            // Identifies the long-term demand peak (i.e., largest minimum
            // demand) that we have seen within the time interval.
            if (e.stats[kStatsAtMinDemand].num_pages > long_term_trend_pages) {
              long_term_trend_pages = e.stats[kStatsAtMinDemand].num_pages;
            }
          }
        },
        long_epochs);

    // Since we are taking the sum of peaks, we can end up with a demand peak
    // that is larger than the largest peak encountered so far, which could
    // lead to OOMs. We adjust the peak in that case, by capping it to the
    // largest peak observed in our time series.
    Length demand_peak = Length(0);
    tracker_.IterBackwards(
        [&](size_t offset, int64_t ts, const SubreleaseStatsEntry& e) {
          if (!e.empty()) {
            if (e.stats[kStatsAtMaxDemand].num_pages > demand_peak) {
              demand_peak = e.stats[kStatsAtMaxDemand].num_pages;
            }
          }
        },
        -1);

    return std::min(demand_peak,
                    short_term_fluctuation_pages + long_term_trend_pages);
  }

  // Reports a skipped subrelease, which is evaluated by coming peaks within the
  // realized fragmentation interval. The purpose is these skipped pages would
  // only create realized fragmentation if peaks in that interval are
  // smaller than peak_pages.
  void ReportSkippedSubreleasePages(Length pages, Length peak_pages) {
    ReportSkippedSubreleasePages(pages, peak_pages, summary_interval_);
  }

  // Reports a skipped subrelease, which is evaluated by coming peaks within the
  // given time interval.
  void ReportSkippedSubreleasePages(Length pages, Length peak_pages,
                                    absl::Duration next_peak_interval) {
    if (pages == Length(0)) {
      return;
    }
    last_next_peak_interval_ = next_peak_interval;
    skipped_subrelease_correctness_.ReportSkippedSubreleasePages(
        pages, peak_pages, next_peak_interval);
  }

  inline typename SkippedSubreleaseCorrectnessTracker<
      kEpochs>::SkippedSubreleaseDecision
  total_skipped() const {
    return skipped_subrelease_correctness_.total_skipped();
  }

  inline typename SkippedSubreleaseCorrectnessTracker<
      kEpochs>::SkippedSubreleaseDecision
  correctly_skipped() const {
    return skipped_subrelease_correctness_.correctly_skipped();
  }

  inline typename SkippedSubreleaseCorrectnessTracker<
      kEpochs>::SkippedSubreleaseDecision
  pending_skipped() const {
    return skipped_subrelease_correctness_.pending_skipped();
  }

  // Returns the minimum number of free pages throughout the tracker period.
  // The first value of the pair is the number of all free pages, the second
  // value contains only the backed ones.
  NumberOfFreePages min_free_pages(absl::Duration w) const {
    NumberOfFreePages mins;
    mins.free = Length::max();
    mins.free_backed = Length::max();

    int64_t num_epochs = std::clamp(w / epoch_length_, int64_t{0},
                                    static_cast<int64_t>(kEpochs));

    tracker_.IterBackwards(
        [&](size_t offset, int64_t ts, const SubreleaseStatsEntry& e) {
          if (!e.empty()) {
            mins.free = std::min(mins.free, e.min_free_pages);
            mins.free_backed =
                std::min(mins.free_backed, e.min_free_backed_pages);
          }
        },
        num_epochs);
    mins.free = (mins.free == Length::max()) ? Length(0) : mins.free;
    mins.free_backed =
        (mins.free_backed == Length::max()) ? Length(0) : mins.free_backed;
    return mins;
  }

 private:
  // We collect subrelease statistics at four "interesting points" within each
  // time step: at min/max demand of pages and at min/max use of hugepages. This
  // allows us to approximate the envelope of the different metrics.
  enum StatsType {
    kStatsAtMinDemand,
    kStatsAtMaxDemand,
    kStatsAtMinHugePages,
    kStatsAtMaxHugePages,
    kNumStatsTypes
  };

  struct SubreleaseStatsEntry {
    // Collect stats at "interesting points" (minimum/maximum page demand
    // and at minimum/maximum usage of huge pages).
    SubreleaseStats stats[kNumStatsTypes] = {};
    static constexpr Length kDefaultValue = Length::max();
    Length min_free_pages = kDefaultValue;
    Length min_free_backed_pages = kDefaultValue;
    Length num_pages_subreleased;
    Length num_partial_alloc_pages_subreleased;
    HugeLength num_hugepages_broken = NHugePages(0);

    static SubreleaseStatsEntry Nil() { return SubreleaseStatsEntry(); }

    void Report(const SubreleaseStats& e) {
      if (empty()) {
        for (int i = 0; i < kNumStatsTypes; i++) {
          stats[i] = e;
        }
      }

      if (e.num_pages < stats[kStatsAtMinDemand].num_pages) {
        stats[kStatsAtMinDemand] = e;
      }

      if (e.num_pages > stats[kStatsAtMaxDemand].num_pages) {
        stats[kStatsAtMaxDemand] = e;
      }

      if (e.total_huge_pages() <
          stats[kStatsAtMinHugePages].total_huge_pages()) {
        stats[kStatsAtMinHugePages] = e;
      }

      if (e.total_huge_pages() >
          stats[kStatsAtMaxHugePages].total_huge_pages()) {
        stats[kStatsAtMaxHugePages] = e;
      }

      min_free_pages =
          std::min(min_free_pages, e.free_pages + e.unmapped_pages);
      min_free_backed_pages = std::min(min_free_backed_pages, e.free_pages);

      // Subrelease stats
      num_pages_subreleased += e.num_pages_subreleased;
      num_partial_alloc_pages_subreleased +=
          e.num_partial_alloc_pages_subreleased;
      num_hugepages_broken += e.num_hugepages_broken;
    }

    bool empty() const { return min_free_pages == kDefaultValue; }
  };

  // The tracker reports pages that have been free for at least this interval,
  // as well as peaks within this interval. The interval is also used for
  // deciding correctness of skipped subreleases by associating past skipping
  // decisions to peaks within this interval.
  const absl::Duration summary_interval_;

  const absl::Duration window_;
  const absl::Duration epoch_length_;

  TimeSeriesTracker<SubreleaseStatsEntry, SubreleaseStats, kEpochs> tracker_;
  SkippedSubreleaseCorrectnessTracker<kEpochs> skipped_subrelease_correctness_;

  // Records most recent intervals for skipping subreleases, plus expected next
  // peak_interval for evaluating skipped subreleases. All for reporting and
  // debugging only.
  SkipSubreleaseIntervals last_skip_subrelease_intervals_;
  absl::Duration last_next_peak_interval_;
};

// Evaluates a/b, avoiding division by zero.
inline double safe_div(Length a, Length b) {
  return safe_div(a.raw_num(), b.raw_num());
}

template <size_t kEpochs>
void SubreleaseStatsTracker<kEpochs>::Print(Printer* out,
                                            absl::string_view field) const {
  NumberOfFreePages free_pages = min_free_pages(summary_interval_);
  out->printf("%s: time series over %d min interval\n\n", field,
              absl::ToInt64Minutes(summary_interval_));

  // Realized fragmentation is equivalent to backed minimum free pages over a
  // 5-min interval. It is printed for convenience but not included in pbtxt.
  out->printf("%s: realized fragmentation: %.1f MiB\n", field,
              free_pages.free_backed.in_mib());
  out->printf("%s: minimum free pages: %zu (%zu backed)\n", field,
              free_pages.free.raw_num(), free_pages.free_backed.raw_num());

  SubreleaseStatsEntry at_peak_demand;
  SubreleaseStatsEntry at_peak_hps;

  tracker_.IterBackwards(
      [&](size_t offset, int64_t ts, const SubreleaseStatsEntry& e) {
        if (!e.empty()) {
          if (at_peak_demand.empty() ||
              at_peak_demand.stats[kStatsAtMaxDemand].num_pages <
                  e.stats[kStatsAtMaxDemand].num_pages) {
            at_peak_demand = e;
          }

          if (at_peak_hps.empty() ||
              at_peak_hps.stats[kStatsAtMaxHugePages].total_huge_pages() <
                  e.stats[kStatsAtMaxHugePages].total_huge_pages()) {
            at_peak_hps = e;
          }
        }
      },
      summary_interval_ / epoch_length_);

  out->printf(
      "%s: at peak demand: %zu pages (and %zu free, %zu unmapped)\n"
      "%s: at peak demand: %zu hps (%zu regular, %zu donated, "
      "%zu partial, %zu released)\n",
      field, at_peak_demand.stats[kStatsAtMaxDemand].num_pages.raw_num(),
      at_peak_demand.stats[kStatsAtMaxDemand].free_pages.raw_num(),
      at_peak_demand.stats[kStatsAtMaxDemand].unmapped_pages.raw_num(), field,
      at_peak_demand.stats[kStatsAtMaxDemand].total_huge_pages().raw_num(),
      at_peak_demand.stats[kStatsAtMaxDemand].huge_pages[kRegular].raw_num(),
      at_peak_demand.stats[kStatsAtMaxDemand].huge_pages[kDonated].raw_num(),
      at_peak_demand.stats[kStatsAtMaxDemand]
          .huge_pages[kPartialReleased]
          .raw_num(),
      at_peak_demand.stats[kStatsAtMaxDemand].huge_pages[kReleased].raw_num());

  out->printf(
      "%s: at peak hps: %zu pages (and %zu free, %zu unmapped)\n"
      "%s: at peak hps: %zu hps (%zu regular, %zu donated, "
      "%zu partial, %zu released)\n",
      field, at_peak_hps.stats[kStatsAtMaxDemand].num_pages.raw_num(),
      at_peak_hps.stats[kStatsAtMaxDemand].free_pages.raw_num(),
      at_peak_hps.stats[kStatsAtMaxDemand].unmapped_pages.raw_num(), field,
      at_peak_hps.stats[kStatsAtMaxDemand].total_huge_pages().raw_num(),
      at_peak_hps.stats[kStatsAtMaxDemand].huge_pages[kRegular].raw_num(),
      at_peak_hps.stats[kStatsAtMaxDemand].huge_pages[kDonated].raw_num(),
      at_peak_hps.stats[kStatsAtMaxDemand]
          .huge_pages[kPartialReleased]
          .raw_num(),
      at_peak_hps.stats[kStatsAtMaxDemand].huge_pages[kReleased].raw_num());

  out->printf(
      "\n%s: Since the start of the execution, %zu subreleases (%zu"
      " pages) were skipped due to either recent (%ds) peaks, or the sum of"
      " short-term (%ds) fluctuations and long-term (%ds) trends.\n",
      field, total_skipped().count, total_skipped().pages.raw_num(),
      absl::ToInt64Seconds(last_skip_subrelease_intervals_.peak_interval),
      absl::ToInt64Seconds(last_skip_subrelease_intervals_.short_interval),
      absl::ToInt64Seconds(last_skip_subrelease_intervals_.long_interval));

  Length skipped_pages = total_skipped().pages - pending_skipped().pages;
  double correctly_skipped_pages_percentage =
      safe_div(100.0 * correctly_skipped().pages, skipped_pages);

  size_t skipped_count = total_skipped().count - pending_skipped().count;
  double correctly_skipped_count_percentage =
      safe_div(100.0 * correctly_skipped().count, skipped_count);

  out->printf(
      "%s: %.4f%% of decisions confirmed correct, %zu "
      "pending (%.4f%% of pages, %zu pending), as per anticipated %ds realized "
      "fragmentation.\n",
      field, correctly_skipped_count_percentage, pending_skipped().count,
      correctly_skipped_pages_percentage, pending_skipped().pages.raw_num(),
      absl::ToInt64Seconds(last_next_peak_interval_));

  // Print subrelease stats
  Length total_subreleased;
  Length total_partial_alloc_pages_subreleased;
  HugeLength total_broken = NHugePages(0);
  tracker_.Iter(
      [&](size_t offset, int64_t ts, const SubreleaseStatsEntry& e) {
        total_subreleased += e.num_pages_subreleased;
        total_partial_alloc_pages_subreleased +=
            e.num_partial_alloc_pages_subreleased;
        total_broken += e.num_hugepages_broken;
      },
      tracker_.kSkipEmptyEntries);
  out->printf(
      "%s: Subrelease stats last %d min: total "
      "%zu pages subreleased (%zu pages from partial allocs), "
      "%zu hugepages broken\n",
      field, static_cast<int64_t>(absl::ToInt64Minutes(window_)),
      total_subreleased.raw_num(),
      total_partial_alloc_pages_subreleased.raw_num(), total_broken.raw_num());
}

template <size_t kEpochs>
void SubreleaseStatsTracker<kEpochs>::PrintSubreleaseStatsInPbtxt(
    PbtxtRegion* hpaa, absl::string_view field) const {
  PbtxtRegion region = hpaa->CreateSubRegion(field);
  region.PrintI64(
      "skipped_subrelease_interval_ms",
      absl::ToInt64Milliseconds(last_skip_subrelease_intervals_.peak_interval));
  region.PrintI64("skipped_subrelease_short_interval_ms",
                  absl::ToInt64Milliseconds(
                      last_skip_subrelease_intervals_.short_interval));
  region.PrintI64(
      "skipped_subrelease_long_interval_ms",
      absl::ToInt64Milliseconds(last_skip_subrelease_intervals_.long_interval));
  region.PrintI64("skipped_subrelease_pages", total_skipped().pages.raw_num());
  region.PrintI64("correctly_skipped_subrelease_pages",
                  correctly_skipped().pages.raw_num());
  region.PrintI64("pending_skipped_subrelease_pages",
                  pending_skipped().pages.raw_num());
  region.PrintI64("skipped_subrelease_count", total_skipped().count);
  region.PrintI64("correctly_skipped_subrelease_count",
                  correctly_skipped().count);
  region.PrintI64("pending_skipped_subrelease_count", pending_skipped().count);
  region.PrintI64("next_peak_interval_ms",
                  absl::ToInt64Milliseconds(last_next_peak_interval_));
}

template <size_t kEpochs>
void SubreleaseStatsTracker<kEpochs>::PrintTimeseriesStatsInPbtxt(
    PbtxtRegion* hpaa, absl::string_view field) const {
  PbtxtRegion region = hpaa->CreateSubRegion(field);
  region.PrintI64("window_ms", absl::ToInt64Milliseconds(epoch_length_));
  region.PrintI64("epochs", kEpochs);

  NumberOfFreePages free_pages = min_free_pages(summary_interval_);
  region.PrintI64("min_free_pages_interval_ms",
                  absl::ToInt64Milliseconds(summary_interval_));
  region.PrintI64("min_free_pages", free_pages.free.raw_num());
  region.PrintI64("min_free_backed_pages", free_pages.free_backed.raw_num());

  static const char* labels[kNumStatsTypes] = {
      "at_minimum_demand", "at_maximum_demand", "at_minimum_huge_pages",
      "at_maximum_huge_pages"};

  tracker_.Iter(
      [&](size_t offset, int64_t ts, const SubreleaseStatsEntry& e) {
        auto subregion = region.CreateSubRegion("measurements");
        subregion.PrintI64("epoch", offset);
        subregion.PrintI64("timestamp_ms",
                           absl::ToInt64Milliseconds(absl::Nanoseconds(ts)));
        subregion.PrintI64("min_free_pages", e.min_free_pages.raw_num());
        subregion.PrintI64("min_free_backed_pages",
                           e.min_free_backed_pages.raw_num());
        subregion.PrintI64("num_pages_subreleased",
                           e.num_pages_subreleased.raw_num());
        subregion.PrintI64("num_hugepages_broken",
                           e.num_hugepages_broken.raw_num());
        subregion.PrintI64("partial_alloc_pages_subreleased",
                           e.num_partial_alloc_pages_subreleased.raw_num());
        for (int i = 0; i < kNumStatsTypes; i++) {
          auto m = subregion.CreateSubRegion(labels[i]);
          SubreleaseStats stats = e.stats[i];
          m.PrintI64("num_pages", stats.num_pages.raw_num());
          m.PrintI64("regular_huge_pages",
                     stats.huge_pages[kRegular].raw_num());
          m.PrintI64("donated_huge_pages",
                     stats.huge_pages[kDonated].raw_num());
          m.PrintI64("partial_released_huge_pages",
                     stats.huge_pages[kPartialReleased].raw_num());
          m.PrintI64("released_huge_pages",
                     stats.huge_pages[kReleased].raw_num());
          m.PrintI64("used_pages_in_subreleased_huge_pages",
                     stats.used_pages_in_subreleased_huge_pages.raw_num());
        }
      },
      tracker_.kSkipEmptyEntries);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_PAGE_SUBRELEASE_H_

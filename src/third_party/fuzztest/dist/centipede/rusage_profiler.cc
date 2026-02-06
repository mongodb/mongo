// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/rusage_profiler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>  // NOLINT
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/nullability.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./centipede/periodic_action.h"
#include "./centipede/rusage_stats.h"

namespace fuzztest::internal {

//------------------------------------------------------------------------------
//                          RUsageProfiler::Snapshot
//------------------------------------------------------------------------------

std::string RUsageProfiler::Snapshot::WhereStr() const {
  return absl::StrFormat("%s:%d", location.file, location.line);
}

std::string RUsageProfiler::Snapshot::ShortWhereStr() const {
  return absl::StrFormat(  //
      "%s:%d", std::filesystem::path(location.file).filename(), location.line);
}

std::string RUsageProfiler::Snapshot::WhenStr() const {
  return absl::FormatTime("%E4Y-%m-%dT%H:%M:%E2S", time, absl::LocalTimeZone());
}

std::string RUsageProfiler::Snapshot::ShortWhenStr() const {
  return absl::FormatTime("%H:%M:%E2S", time, absl::LocalTimeZone());
}

std::string RUsageProfiler::Snapshot::FormattedMetricsStr() const {
  std::string s;
  absl::StrAppendFormat(                      //
      &s, "  [P.%d:S.%d] TIMING   | %s |\n",  //
      profiler_id, id, timing.FormattedStr());
  if (delta_timing != RUsageTiming::Zero()) {
    absl::StrAppendFormat(                      //
        &s, "  [P.%d:S.%d] TIMING Δ | %s |\n",  //
        profiler_id, id, delta_timing.FormattedStr());
  }
  absl::StrAppendFormat(                      //
      &s, "  [P.%d:S.%d] MEMORY   | %s |\n",  //
      profiler_id, id, memory.FormattedStr());
  if (delta_memory != RUsageMemory::Zero()) {
    absl::StrAppendFormat(                      //
        &s, "  [P.%d:S.%d] MEMORY Δ | %s |\n",  //
        profiler_id, id, delta_memory.FormattedStr());
  }
  return s;
}

std::string RUsageProfiler::Snapshot::ShortMetricsStr() const {
  std::string s;
  absl::StrAppendFormat(  //
      &s, "TIMING { %s } ", timing.ShortStr());
  if (delta_timing != RUsageTiming::Zero()) {
    absl::StrAppendFormat(  //
        &s, "TIMING Δ { %s } ", delta_timing.ShortStr());
  }
  absl::StrAppendFormat(  //
      &s, "MEMORY { %s } ", memory.ShortStr());
  if (delta_memory != RUsageMemory::Zero()) {
    absl::StrAppendFormat(  //
        &s, "MEMORY Δ { %s } ", delta_memory.ShortStr());
  }
  return s;
}

const RUsageProfiler::Snapshot& RUsageProfiler::Snapshot::Log() const {
  if (id >= 0) {
    LOG(INFO).AtLocation(location.file, location.line)
        << "PROFILER [P." << profiler_id << (profiler_desc.empty() ? "" : " ")
        << profiler_desc << "] SNAPSHOT [S." << id << (title.empty() ? "" : " ")
        << title << "]:\n"
        << FormattedMetricsStr();
  }
  return *this;
}

std::ostream& operator<<(std::ostream& os, const RUsageProfiler::Snapshot& ss) {
  return os << ss.title << ": " << ss.ShortWhereStr() << " @ "
            << ss.ShortWhenStr() << ": " << ss.ShortMetricsStr();
}

namespace {

//------------------------------------------------------------------------------
//                           ProfileReportGenerator
//
// A helper for RUsageProfiler::GenerateReport(): generates individual
// chronological charts of the tracked metrics and streams them to an ostream.
//------------------------------------------------------------------------------

class ProfileReportGenerator {
 public:
  ProfileReportGenerator(                                     //
      const std::deque<RUsageProfiler::Snapshot>& snapshots,  //
      RUsageProfiler::ReportSink* absl_nonnull report_sink)
      : snapshots_{snapshots}, report_sink_{report_sink} {
    for (const auto& snapshot : snapshots_) {
      timing_low_ = RUsageTiming::LowWater(  //
          timing_low_, snapshot.timing);
      timing_high_ = RUsageTiming::HighWater(  //
          timing_high_, snapshot.timing);
      delta_timing_low_ = RUsageTiming::LowWater(  //
          delta_timing_low_, snapshot.delta_timing);
      delta_timing_high_ = RUsageTiming::HighWater(  //
          delta_timing_high_, snapshot.delta_timing);

      memory_low_ = RUsageMemory::LowWater(  //
          memory_low_, snapshot.memory);
      memory_high_ = RUsageMemory::HighWater(  //
          memory_high_, snapshot.memory);
      delta_memory_low_ = RUsageMemory::LowWater(  //
          delta_memory_low_, snapshot.delta_memory);
      delta_memory_high_ = RUsageMemory::HighWater(  //
          delta_memory_high_, snapshot.delta_memory);

      max_where_len_ =  //
          std::max<int>(max_where_len_, snapshot.ShortWhereStr().length());
      max_when_len_ =  //
          std::max<int>(max_when_len_, snapshot.ShortWhenStr().length());
      max_title_len_ =  //
          std::max<int>(max_title_len_, snapshot.title.length());
    }
  }

  // GenChartImpl() wrappers for the 2 available "snap" metrics.
  template <typename MetricT>
  void GenChart(const MetricT RUsageTiming::*metric_field) {
    GenChartImpl(                                         //
        &RUsageProfiler::Snapshot::timing, metric_field,  //
        timing_low_, timing_high_, /*is_delta=*/false);
  }
  template <typename MetricT>
  void GenChart(const MetricT RUsageMemory::*metric_field) const {
    GenChartImpl(                                         //
        &RUsageProfiler::Snapshot::memory, metric_field,  //
        memory_low_, memory_high_, /*is_delta=*/false);
  }

  // GenChartImpl() wrappers for the 2 available delta metrics.
  template <typename MetricT>
  void GenDeltaChart(const MetricT RUsageTiming::*metric_field) {
    GenChartImpl(                                               //
        &RUsageProfiler::Snapshot::delta_timing, metric_field,  //
        delta_timing_low_, delta_timing_high_, /*is_delta=*/true);
  }
  template <typename MetricT>
  void GenDeltaChart(const MetricT RUsageMemory::*metric_field) const {
    GenChartImpl(                                               //
        &RUsageProfiler::Snapshot::delta_memory, metric_field,  //
        delta_memory_low_, delta_memory_high_, /*is_delta=*/true);
  }

 private:
  // The actual chart generator. For better understanding of the code: an
  // example of `metric_field` is `&RUsageProfiler::Snapshot::delta_timing`
  // which has type `RUsageTiming`; an example of a matching `submetric_field`
  // for that is `&RUsageTiming::wall_time`.
  template <typename MetricT, typename SubmetricT>
  void GenChartImpl(                                          //
      const MetricT RUsageProfiler::Snapshot::*metric_field,  //
      const SubmetricT MetricT::*submetric_field,             //
      MetricT metric_low_water,                               //
      MetricT metric_high_water,                              //
      bool is_delta) const {
    constexpr SubmetricT kZero{};  // works for both ints and absl::Duration
    const SubmetricT low_water = metric_low_water.*submetric_field;
    const SubmetricT high_water = metric_high_water.*submetric_field;
    // SubmetricT can be int64 or Duration: calculate a notch_size that is a
    // double or an unrounded Duration, respectively, so the below calculations
    // are exact.
    const auto notch_size =
        (high_water - low_water) / static_cast<double>(kBarNotches);
    // The position of the notch indicating 0 (used for delta metrics only).
    // clang-format off
    const int notch_zero =
        notch_size == kZero ? kBarNotches :
        low_water >= kZero ? 0 :
        std::floor(std::abs(low_water / notch_size));
    // clang-format on
    CHECK_GE(kBarNotches, notch_zero);
    // Print a zero mark only if a delta metric goes negative.
    std::string zero_mark = low_water < kZero ? "|" : "";

    for (const auto& snapshot : snapshots_) {
      const SubmetricT current = snapshot.*metric_field.*submetric_field;

      // Generate a bar of #'s as a graphical representation of the current
      // value of the metric relative to its full range [low_water, high_water]:
      // low_water is no #'s and all -'s, high_water is kBarNotches of #'s.
      const std::string metric_str = FormatInOptimalUnits(current, is_delta);
      std::string metric_bar;
      // clang-format off
      const int notches =
          notch_size == kZero
              ? kBarNotches : std::floor((current - low_water) / notch_size);
      // clang-format on
      CHECK_GE(kBarNotches, notches);

      if (!is_delta) {
        // Non-delta metrics can't go negative, so the bar always looks like
        // this:
        // ###############--------------------------
        const std::string filled(notches, '#');
        const std::string unfilled(kBarNotches - notches, '-');
        metric_bar = absl::StrCat(filled, unfilled);
      } else {
        // Delta metrics can go negative, so this become more complicated. In
        // general, print a zero mark '|' at the proper fixed position of every
        // bar for this metric's history, and grow the #'s away from the zero
        // mark, to the left for negative and to the right for positive deltas:
        // +Delta: --------|#######---------
        // -Delta: ########|----------------
        std::string pad_minus, minus, plus, pad_plus;
        // Notches range from 0 (for low_water) to kBarNotches (for high_water).
        if (notches < notch_zero) {
          pad_minus = std::string(notches, '-');
          minus = std::string(notch_zero - notches, '#');
          pad_plus = std::string(kBarNotches - notch_zero, '-');
        } else if (notches > notch_zero) {
          pad_minus = std::string(notch_zero, '-');
          plus = std::string(notches - notch_zero, '#');
          pad_plus = std::string(kBarNotches - notches, '-');
        } else {
          pad_minus = std::string(notch_zero, '-');
          pad_plus = std::string(kBarNotches - notch_zero, '-');
        }
        metric_bar = absl::StrCat(pad_minus, minus, zero_mark, plus, pad_plus);
      }

      // Finally print a full line for the current snapshot/metric, like on of:
      // source.cc:123 @ 21:08:27.61 [P.1:S.1 Snap  ]  493.78M [############---]
      // source.cc:123 @ 21:08:27.61 [P.1:S.2 +Delta] +138.15M [-----|#####----]
      // source.cc:123 @ 21:08:27.61 [P.1:S.3 -Delta]  -82.69M [--###|---------]
      *report_sink_ << absl::StrFormat(                 //
          "  %*s @ %*s [P.%d:S.%-2d %*s] %10s [%s]\n",  // '*' is custom width
          -max_where_len_, snapshot.ShortWhereStr(),    // ...passed here.
          -max_when_len_, snapshot.ShortWhenStr(),      // '-' left-justifies
          snapshot.profiler_id, snapshot.id,            //
          -max_title_len_, snapshot.title,              //
          metric_str, metric_bar);
    }
  }

  static constexpr int kBarNotches = 50;

  const std::deque<RUsageProfiler::Snapshot>& snapshots_;
  RUsageProfiler::ReportSink* report_sink_;

  RUsageMemory memory_low_ = RUsageMemory::Max();
  RUsageMemory memory_high_ = RUsageMemory::Min();
  RUsageMemory delta_memory_low_ = RUsageMemory::Max();
  RUsageMemory delta_memory_high_ = RUsageMemory::Min();
  RUsageTiming timing_low_ = RUsageTiming::Max();
  RUsageTiming timing_high_ = RUsageTiming::Min();
  RUsageTiming delta_timing_low_ = RUsageTiming::Max();
  RUsageTiming delta_timing_high_ = RUsageTiming::Min();

  // NOTE: The values are negated, so have to be signed.
  int max_where_len_ = 0;
  int max_when_len_ = 0;
  int max_title_len_ = 0;
};

}  // namespace

//------------------------------------------------------------------------------
//                              RUsageProfiler
//------------------------------------------------------------------------------

std::atomic<int> RUsageProfiler::next_id_;

RUsageProfiler::RUsageProfiler(    //
    RUsageScope scope,             //
    MetricsMask metrics,           //
    RaiiActionsMask raii_actions,  //
    SourceLocation location,       //
    std::string description)
    : scope_{std::move(scope)},
      metrics_{metrics},
      raii_actions_{raii_actions},
      ctor_loc_{location},
      description_{std::move(description)},
      id_{next_id_.fetch_add(1, std::memory_order_relaxed)} {
  if (metrics_ == kMetricsOff) return;

  if (raii_actions_ & kCtorSnapshot) {
    TakeSnapshot(ctor_loc_, "INITIAL").Log();
  }
}

RUsageProfiler::RUsageProfiler(         //
    RUsageScope scope,                  //
    MetricsMask metrics,                //
    absl::Duration timelapse_interval,  //
    bool also_log_timelapses,           //
    SourceLocation location,            //
    std::string description)
    : scope_{std::move(scope)},
      metrics_{metrics},
      raii_actions_{kDtorSnapshot | kDtorReport},
      ctor_loc_{location},
      description_{std::move(description)},
      id_{next_id_.fetch_add(1, std::memory_order_relaxed)} {
  if (metrics_ == kMetricsOff) return;

  if (timelapse_interval != absl::ZeroDuration() &&
      timelapse_interval != absl::InfiniteDuration()) {
    StartTimelapse(  //
        ctor_loc_, timelapse_interval, also_log_timelapses, "Timelapse");
  }
}

RUsageProfiler::~RUsageProfiler() {
  if (metrics_ == kMetricsOff) return;

  // In case the caller hasn't done this.
  if (timelapse_recorder_) {
    StopTimelapse();
  }
  if (raii_actions_ & kDtorSnapshot) {
    // NOTE: Can't pass the real location from callers, so use next best thing.
    TakeSnapshot(ctor_loc_, "FINAL").Log();
  }
  // If requested, also print a final report.
  if (raii_actions_ & kDtorReport) {
    const std::string title =
        absl::StrFormat("PROFILER [P.%d %s] FINAL REPORT:", id_, description_);
    PrintReport(ctor_loc_, title);
  }
}

const RUsageProfiler::Snapshot& RUsageProfiler::TakeSnapshot(  //
    SourceLocation loc, std::string title) {
  if (metrics_ == kMetricsOff) {
    static const Snapshot kEmpty{};
    return kEmpty;
  }

  absl::WriterMutexLock lock{&mutex_};

  RUsageTiming snap_timing = RUsageTiming::Zero();
  RUsageTiming delta_timing = RUsageTiming::Zero();
  RUsageMemory snap_memory = RUsageMemory::Zero();
  RUsageMemory delta_memory = RUsageMemory::Zero();

  if (metrics_ & kTiming) {
    const auto current = RUsageTiming::Snapshot(scope_, timer_);
    if (metrics_ & kSnapTiming) {
      snap_timing = current;
    }
    if (metrics_ & kDeltaTiming && !snapshots_.empty()) {
      const auto& previous = snapshots_.back().timing;
      delta_timing = current - previous;
    }
  }

  if (metrics_ & kMemory) {
    const auto current = RUsageMemory::Snapshot(scope_);
    if (metrics_ & kSnapMemory) {
      snap_memory = current;
    }
    if (metrics_ & kDeltaMemory && !snapshots_.empty()) {
      const auto& previous = snapshots_.back().memory;
      delta_memory = current - previous;
    }
  }

  Snapshot snapshot{/*id=*/static_cast<int64_t>(snapshots_.size()),
                    /*title=*/std::move(title),
                    /*location=*/loc,
                    /*time=*/absl::Now(),
                    /*profiler_id=*/id_,
                    /*profiler_desc=*/description_,
                    /*timing=*/snap_timing,
                    /*delta_timing=*/delta_timing,
                    /*memory=*/snap_memory,
                    /*delta_memory=*/delta_memory};

  return snapshots_.emplace_back(std::move(snapshot));
}

void RUsageProfiler::StartTimelapse(  //
    SourceLocation loc,               //
    absl::Duration interval,          //
    bool also_log,                    //
    std::string title) {
  absl::WriterMutexLock lock{&mutex_};
  CHECK(!timelapse_recorder_) << "StopTimelapse() wasn't called";
  timelapse_recorder_ = std::make_unique<PeriodicAction>(
      [this, loc = std::move(loc), title = std::move(title), also_log]() {
        const auto& s = TakeSnapshot(loc, title);
        if (also_log) s.Log();
      },
      PeriodicAction::ZeroDelayConstInterval(interval));
}

void RUsageProfiler::StopTimelapse() {
  absl::WriterMutexLock lock{&mutex_};
  CHECK(timelapse_recorder_) << "StartTimelapse() wasn't called";
  timelapse_recorder_.reset();
}

void RUsageProfiler::PrintReport(  //
    SourceLocation loc, const std::string& title) {
  if (metrics_ == kMetricsOff) return;

  // Logs streamed-in text to LOG(INFO), while dropping the usual log prefix
  // (date/time/thread/source). LOG()'s limit on the size of a single message
  // applies to one streamed text fragment only (if needed, this can be reduced
  // even further to a single line of text in a fragment): this is the main
  // purpose of this class, as profiling reports can get very long. especially
  // with automatic timelapse snapshotting.
  class ReportLogger final : public ReportSink {
   public:
    ReportLogger(SourceLocation loc) : loc_{loc} {}

    ~ReportLogger() override {
      if (!buffer_.empty()) {
        LOG(INFO).AtLocation(loc_.file, loc_.line).NoPrefix() << buffer_;
      }
    }

    ReportLogger& operator<<(std::string_view fragment) override {
      const auto last_newline = fragment.rfind('\n');
      if (last_newline == std::string_view::npos) {
        // Accumulate no-'\n' fragments: LOG() always wraps around.
        buffer_ += fragment;
      } else {
        // Now we can log, but save the last bit of text
        LOG(INFO).AtLocation(loc_.file, loc_.line).NoPrefix()
            << buffer_ << fragment.substr(0, last_newline);
        buffer_ = fragment.substr(last_newline + 1);
      }
      return *this;
    }

   private:
    const SourceLocation loc_;
    std::string buffer_;
  };

  LOG(INFO).AtLocation(loc.file, loc.line) << title << "\n";
  ReportLogger report_logger{loc};
  GenerateReport(&report_logger);
}

void RUsageProfiler::GenerateReport(
    ReportSink* absl_nonnull report_sink) const {
  absl::ReaderMutexLock lock{&mutex_};
  // Prevent interleaved reports from multiple concurrent RUsageProfilers.
  ABSL_CONST_INIT static absl::Mutex report_generation_mutex_{absl::kConstInit};
  absl::WriterMutexLock logging_lock{&report_generation_mutex_};

  ProfileReportGenerator gen{snapshots_, report_sink};

  const std::string desc = absl::StrFormat("[P.%d %s]", id_, description_);
  *report_sink << "SCOPE: " << scope_ << "\n";

  if (metrics_ & kSnapTiming) {
    *report_sink << "\n=== TIMING " << desc << " ===\n";
    *report_sink << "\nWALL TIME " << desc << ":\n";
    gen.GenChart(&RUsageTiming::wall_time);
    *report_sink << "\nUSER TIME " << desc << ":\n";
    gen.GenChart(&RUsageTiming::user_time);
    *report_sink << "\nSYSTEM TIME " << desc << ":\n";
    gen.GenChart(&RUsageTiming::sys_time);
    *report_sink << "\nCPU UTILIZATION " << desc << ":\n";
    gen.GenChart(&RUsageTiming::cpu_utilization);
    *report_sink << "\nAVERAGE CORES " << desc << ":\n";
    gen.GenChart(&RUsageTiming::cpu_hyper_cores);
  }
  if (metrics_ & kDeltaTiming) {
    *report_sink << "\n=== Δ TIMING " << desc << " ===\n";
    *report_sink << "\nΔ WALL TIME " << desc << ":\n";
    gen.GenDeltaChart(&RUsageTiming::wall_time);
    *report_sink << "\nΔ USER TIME " << desc << ":\n";
    gen.GenDeltaChart(&RUsageTiming::user_time);
    *report_sink << "\nΔ SYSTEM TIME " << desc << ":\n";
    gen.GenDeltaChart(&RUsageTiming::sys_time);
    *report_sink << "\nΔ CPU UTILIZATION " << desc << ":\n";
    gen.GenDeltaChart(&RUsageTiming::cpu_utilization);
    *report_sink << "\nΔ AVERAGE CORES " << desc << ":\n";
    gen.GenDeltaChart(&RUsageTiming::cpu_hyper_cores);
  }
  if (metrics_ & kSnapMemory) {
    *report_sink << "\n=== MEMORY USAGE " << desc << " ===\n";
    *report_sink << "\nRESIDENT SET SIZE " << desc << ":\n";
    gen.GenChart(&RUsageMemory::mem_rss);
    *report_sink << "\nVIRTUAL SIZE " << desc << ":\n";
    gen.GenChart(&RUsageMemory::mem_vsize);
    *report_sink << "\nVIRTUAL PEAK " << desc << ":\n";
    gen.GenChart(&RUsageMemory::mem_vpeak);
    *report_sink << "\nDATA SEGMENT " << desc << ":\n";
    gen.GenChart(&RUsageMemory::mem_data);
    *report_sink << "\nSHARED MEMORY " << desc << ":\n";
    gen.GenChart(&RUsageMemory::mem_shared);
  }
  if (metrics_ & kDeltaMemory) {
    *report_sink << "\n=== Δ MEMORY USAGE " << desc << " ===\n";
    *report_sink << "\nΔ RESIDENT SET SIZE " << desc << ":\n";
    gen.GenDeltaChart(&RUsageMemory::mem_rss);
    *report_sink << "\nΔ VIRTUAL SIZE " << desc << ":\n";
    gen.GenDeltaChart(&RUsageMemory::mem_vsize);
    *report_sink << "\nΔ VIRTUAL PEAK " << desc << ":\n";
    gen.GenDeltaChart(&RUsageMemory::mem_vpeak);
    *report_sink << "\nΔ DATA SEGMENT " << desc << ":\n";
    gen.GenDeltaChart(&RUsageMemory::mem_data);
    *report_sink << "\nΔ SHARED MEMORY " << desc << ":\n";
    gen.GenDeltaChart(&RUsageMemory::mem_shared);
  }
}

}  // namespace fuzztest::internal

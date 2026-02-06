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

#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./centipede/rusage_stats.h"

ABSL_FLAG(bool, verbose, false, "Print extra info for debugging");
ABSL_FLAG(bool, enable_system_load_sensitive_tests, false,
          "Enable tests that are sensitive to the overall execution "
          "environment on the current machine, e.g. the wall time accuracy or "
          "average CPU load");

// clang-format off
#define EXPECT_TIME_NEAR(x, y, e) \
  EXPECT_NEAR(absl::ToDoubleSeconds(x), absl::ToDoubleSeconds(y), e)
#define EXPECT_SYS_TIMING_NEAR(x, y) \
  EXPECT_TIME_NEAR((x).wall_time, (y).wall_time, 0.1); \
  EXPECT_TIME_NEAR((x).user_time, (y).user_time, 0.01); \
  EXPECT_TIME_NEAR((x).sys_time,  (y).sys_time, 0.01); \
  EXPECT_NEAR((x).cpu_utilization, (y).cpu_utilization, 0.5); \
  EXPECT_NEAR((x).cpu_hyper_cores, (y).cpu_hyper_cores, 0.5)

#define EXPECT_MEM_NEAR(x, y, e) \
  EXPECT_NEAR(static_cast<double>(x), static_cast<double>(y), \
              std::fabs(((x) + (y)) / 2.0 * e))
#define EXPECT_SYS_MEMORY_NEAR(x, y) \
  EXPECT_MEM_NEAR((x).mem_vsize, (y).mem_vsize, 0.1); \
  EXPECT_MEM_NEAR((x).mem_vpeak, (y).mem_vpeak, 0.1); \
  EXPECT_MEM_NEAR((x).mem_rss, (y).mem_rss, 0.2); \
  EXPECT_MEM_NEAR((x).mem_data, (y).mem_data, 0.2);
// clang-format on

namespace fuzztest::internal {
namespace {

struct BigSlowThing {
  // NOTE: The order -- first gobble bytes, then waste time -- is important:
  // it gives the system memory a chance to settle (finish paging etc.) before
  // the returning, so the measurement results are more stable.
  BigSlowThing(int64_t gobble_bytes, absl::Duration waste_time) {
    const absl::Time start = absl::Now();

    big_mem.resize(gobble_bytes);
    // Touch the memory to cause it to actually materialize.
    for (std::string::size_type i = 0; i < big_mem.size(); i += 1000) {
      big_mem[i] = '$';
    }

    double cpu_waster = 1.23;
    while (absl::Now() - start < waste_time) {
      cpu_waster = std::cos(cpu_waster);
      absl::SleepFor(absl::Seconds(1));
    }
  }

  std::string big_mem;
};

void WasteTimeAndGobbleBytes() {
  {
    BigSlowThing big_slow_1{50'000'000, absl::Seconds(1)};
  }
  {
    BigSlowThing big_slow_2{20'000'000, absl::Seconds(1)};
    for (int i = 0; i < 3; ++i) {
      BigSlowThing big_slow_3{10'000'000, absl::Seconds(1)};
    }
  }
}

}  // namespace

TEST(RUsageProfilerTest, TimelapseSnapshots) {
  RPROF_THIS_FUNCTION_WITH_REPORT(/*enable=*/true);
  RPROF_START_TIMELAPSE(absl::Seconds(1), /*also_log=*/true, "Timelapse");
  WasteTimeAndGobbleBytes();
  RPROF_STOP_TIMELAPSE();
  RPROF_DUMP_REPORT_TO_LOG("Report");
}

// NOTE: Exclude this test from MSAN: 1) MSAN messes with the system memory
// and skews the test's memory measurements. 2) The test allocates large
// memory blocks to fight small number volatility of the system allocator, but
// MSAN's custom allocator can't cope and intermittently OOMs.
#if !defined(MEMORY_SANITIZER)
// Compare RUsageProfiler's manually taken snapshots against raw RUsageTiming
// and RUsageMemory numbers acquired approximately at the same time.
// "Approximately the same" is still not *the same*, so some discrepancies are
// fully expected.
TEST(RUsageProfilerTest, ValidateManualSnapshots) {
  // Allocate A LOT of memory to fight the small numbers volatility, in
  // particular in the virtual memory size and peak, which grow in page
  // increments.
  constexpr int64_t kGobbleBytes = 1'000'000'000;
  constexpr absl::Duration kWasteTime = absl::Seconds(2);
  const auto rusage_scope = RUsageScope::ThisProcess();

  RUsageProfiler rprof{rusage_scope,
                       RUsageProfiler::kAllMetrics,
                       RUsageProfiler::kRaiiOff,
                       {__FILE__, __LINE__}};

  const RUsageProfiler::Snapshot& before_snapshot =
      rprof.TakeSnapshot({__FILE__, __LINE__});
  // NOTE: Use RUsageProfiler's internal timer rather than RUsageTiming's
  // default global one (which starts when the process starts) to measure the
  // times on the same timeline.
  const RUsageTiming before_timing =
      RUsageTiming::Snapshot(rusage_scope, rprof.timer_);
  const RUsageMemory before_memory = RUsageMemory::Snapshot(rusage_scope);

  const BigSlowThing big_slow_thing{kGobbleBytes, kWasteTime};

  const RUsageProfiler::Snapshot& after_snapshot =
      rprof.TakeSnapshot({__FILE__, __LINE__});
  const RUsageTiming after_timing =
      RUsageTiming::Snapshot(rusage_scope, rprof.timer_);
  const RUsageMemory after_memory = RUsageMemory::Snapshot(rusage_scope);
  const RUsageTiming delta_timing = after_timing - before_timing;
  const RUsageMemory delta_memory = after_memory - before_memory;

  if (absl::GetFlag(FLAGS_verbose)) {
    LOG(INFO) << "before_snapshot:\n" << before_snapshot.FormattedMetricsStr();
    LOG(INFO) << "after_snapshot:\n" << after_snapshot.FormattedMetricsStr();
    LOG(INFO) << "";
    LOG(INFO) << "before_timing: " << before_timing.FormattedStr();
    LOG(INFO) << "after_timing:  " << after_timing.FormattedStr();
    LOG(INFO) << "delta_timing:  " << delta_timing.FormattedStr();
    LOG(INFO) << "";
    LOG(INFO) << "before_memory: " << before_memory.FormattedStr();
    LOG(INFO) << "after_memory:  " << after_memory.FormattedStr();
    LOG(INFO) << "delta_memory:  " << delta_memory.FormattedStr();
  }

  EXPECT_EQ(after_snapshot.delta_timing,
            after_snapshot.timing - before_snapshot.timing);
  EXPECT_EQ(after_snapshot.delta_memory,
            after_snapshot.memory - before_snapshot.memory);

  // NOTES: 1) The "before" timing numbers are close to 0 and extremely
  // volatile, especially for the CPU utilization. Therefore, exclude it, as
  // well as the delta timing partially determined by it, from validation.
  // 2) All *SANs slow down execution, so skip timing checks under them.
  // However, still run RUsageProfiler under them to catch any respective bugs.
#if !defined(MEMORY_SANITIZER) && !defined(ADDRESS_SANITIZER) && \
    !defined(THREAD_SANITIZER)
  // EXPECT_SYS_TIMING_NEAR(before_snapshot.timing, before_timing);
  EXPECT_SYS_TIMING_NEAR(after_snapshot.timing, after_timing);
  // EXPECT_SYS_TIMING_NEAR(after_snapshot.delta_timing, delta_timing);
#else
  LOG(WARNING) << "Validation of some test results omitted under *SANs";
#endif

  if (absl::GetFlag(FLAGS_enable_system_load_sensitive_tests)) {
    EXPECT_SYS_MEMORY_NEAR(before_snapshot.memory, before_memory);
    EXPECT_SYS_MEMORY_NEAR(after_snapshot.memory, after_memory);
    EXPECT_SYS_MEMORY_NEAR(after_snapshot.delta_memory, delta_memory);
  }
}
#endif  // MSAN is now back on.

TEST(RUsageProfilerTest, ValidateTimelapseSnapshots) {
  constexpr absl::Duration kWasteTime = absl::Seconds(2);
  constexpr absl::Duration kInterval = absl::Milliseconds(500);
  constexpr int kGobbleBytes = 100'000'000;
  const bool kAlsoLog = absl::GetFlag(FLAGS_verbose);

  RUsageProfiler rprof{RUsageScope::ThisProcess(),
                       RUsageProfiler::kAllMetrics,
                       kInterval,
                       kAlsoLog,
                       {__FILE__, __LINE__}};
  const BigSlowThing big_slow_thing{kGobbleBytes, kWasteTime};
  rprof.StopTimelapse();

  // NOTE: The sanitizers heavily instrument the code and skew any time
  //  measurements.
#if !defined(ADDRESS_SANITIZER) && !defined(THREAD_SANITIZER) && \
    !defined(MEMORY_SANITIZER)
  const auto& snapshots = rprof.GetSnapshots();
  ASSERT_NEAR(snapshots.size(), absl::FDivDuration(kWasteTime, kInterval), 1);
  for (int i = 1; i < snapshots.size(); ++i) {
    EXPECT_TIME_NEAR(snapshots[i].time - snapshots[i - 1].time, kInterval, .05);
  }
#else
  LOG(WARNING) << "Validation of some test results omitted under *SANs";
#endif
}

TEST(RUsageProfilerTest, ValidateReport) {
  constexpr int kGobbleBytes = 100'000'000;
  constexpr absl::Duration kWasteTime = absl::Seconds(3);

  RUsageProfiler rprof{RUsageScope::ThisProcess(),
                       RUsageProfiler::kAllMetrics,
                       RUsageProfiler::kRaiiOff,
                       {__FILE__, __LINE__}};
  {
    rprof.TakeSnapshot({__FILE__, __LINE__});
    const BigSlowThing big_slow_thing_1{kGobbleBytes, kWasteTime};
    rprof.TakeSnapshot({__FILE__, __LINE__});
    const BigSlowThing big_slow_thing_2{kGobbleBytes, kWasteTime};
    rprof.TakeSnapshot({__FILE__, __LINE__});
  }  // BigSlowThings release their memory.
  rprof.TakeSnapshot({__FILE__, __LINE__});

  class ReportCapture : public RUsageProfiler::ReportSink {
   public:
    ~ReportCapture() override = default;
    ReportCapture& operator<<(std::string_view fragment) override {
      LOG(INFO).NoPrefix() << fragment;
      return *this;
    }
  };

  ReportCapture report_capture{};
  rprof.GenerateReport(&report_capture);
}

}  // namespace fuzztest::internal

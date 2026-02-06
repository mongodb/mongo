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

#include "./centipede/rusage_stats.h"

#include <cmath>
#include <iosfwd>
#include <numeric>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/nullability.h"
#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/synchronization/barrier.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./centipede/thread_pool.h"
#include "./common/logging.h"  // IWYU pragma: keep

ABSL_FLAG(bool, verbose, false, "Print extra info for debugging");
ABSL_FLAG(bool, enable_system_load_sensitive_tests, false,
          "Enable tests that are sensitive to the overall execution "
          "environment on the current machine, e.g. the wall time accuracy or "
          "average CPU load");

namespace fuzztest::internal {
namespace {

class BigThing {
 public:
  explicit BigThing(MemSize gobble_bytes) : big_mem_(gobble_bytes, '#') {
    // Touch the memory to cause it to actually materialize.
    for (int i = 0; i < big_mem_.size(); i += 1000) {
      big_mem_[i] = '$';
    }
  }

 private:
  std::string big_mem_;
};

// Spawns N separate threads that idle for a while, then simultaneously hog the
// CPU for a while. The caller is notified just before the hogging starts, and
// just before it ends: this way, the caller can take a measurement right in the
// middle of hogging, not after it ends. The latter would make the test
// sensitive to fluctuations in the OS process scheduling and result flakiness
// when a measurement is taken a little too late, after the CPU activity
// subsides too much.
class CpuHog {
 public:
  CpuHog(                                                //
      absl::Duration idle_time,                          //
      absl::Duration hog_time,                           //
      int num_hogs,                                      //
      absl::Notification* absl_nonnull hogging_started,  //
      absl::Notification* absl_nonnull hogging_stopped)
      : hog_barrier_{num_hogs}, hog_pool_{num_hogs} {
    const auto hog_func = [=]() {
      const absl::Time start = absl::Now();
      double cpu_waster = 1.23;
      constexpr absl::Duration kPadHogTime = absl::Milliseconds(100);
      // Preload the CPU for a short while to get the utilization up somewhere
      // close to the target level.
      while (absl::Now() - start < kPadHogTime) {
        cpu_waster = std::cos(cpu_waster);
      }
      // Wait for all the other hogs to arrive.
      if (hog_barrier_.Block()) {
        // Ready to unleash the hogs: inform the caller that it's about time to
        // start sampling. NOTE: Only one Block() call returns true, so
        // calling Notify() is thread-safe.
        hogging_started->Notify();
      }
      // Unleash all the hogs at once.
      while (absl::Now() - start < hog_time) {
        cpu_waster = std::cos(cpu_waster);
      }
      // Let one of the hogs notify the caller that the hogging is over.
      {
        absl::MutexLock lock{&mu_};
        if (!hogging_stopped->HasBeenNotified()) {
          hogging_stopped->Notify();
        }
      }
      // Maintain the CPU load for a little longer while the caller wraps up
      // its sampling.
      while (absl::Now() - start < hog_time + kPadHogTime) {
        cpu_waster = std::cos(cpu_waster);
      }
    };

    // Idle.
    absl::SleepFor(idle_time);

    // Consume ~num_hogs cores at ~100% utilization for ~hog_time. The caller
    // gets notified when the hogging begins and ends, so they know when they
    // can sample the CPU utilization.
    for (int i = 0; i < num_hogs; ++i) {
      hog_pool_.Schedule(hog_func);
    }
  }

 private:
  // NOTE: The declaration order is important: mu_ is used inside the threads of
  // hog_pool_ and thus must outlive hog_pool_.
  absl::Mutex mu_;
  absl::Barrier hog_barrier_;
  ThreadPool hog_pool_;
};

// A simple histogram.
template <typename T>
class Histogram {
 public:
  void Add(T v) { vs_.push_back(v); }

  T Average() const {
    if (vs_.empty()) {
      return T{};
    } else {
      return std::accumulate(vs_.cbegin(), vs_.cend(), T{}) / vs_.size();
    }
  }

  friend std::ostream& operator<<(std::ostream& os, const Histogram& histo) {
    for (const auto& v : histo.vs_) {
      os << v << " ";
    }
    return os;
  }

 private:
  std::vector<T> vs_;
};

}  // namespace

TEST(RUsageTimingTest, Accuracy) {
  constexpr int kNumRuns = 3;
  constexpr double kIdleSecs = 1.;
  constexpr double kHogSecs = 4.;
  constexpr int kNumHogs = 5;
  constexpr double kWallSecs = kIdleSecs + kHogSecs;
  // The wall time is sensitive to the system load, but it is measured only
  // under --enable_system_load_sensitive_tests.
  const double kWallSecsLeeway = kWallSecs * 0.1;
  // The user time is reliably accurate.
  const double kUserSecsLeeway = kHogSecs * kNumHogs * 0.1;
  // The instantaneous CPU usage should be very reliable with the used thread
  // synchronization scheme.
  const double kCpuUtilLeeway = 0.1;
  // The average CPU usage numbers are volatile: use a generous leeway and
  // test only under --enable_system_load_sensitive_tests.
  const double kCpuCoresLeeway = kHogSecs / kWallSecs * 0.25;

  Histogram<double> user_time_histo;
  Histogram<double> wall_time_histo;
  Histogram<CpuUtilization> cpu_util_histo;
  Histogram<CpuHyperCores> cpu_cores_histo;

  for (int i = 0; i < kNumRuns; ++i) {
    const ProcessTimer timer;
    absl::Notification hogging_started, hogging_stopped;
    const auto before =
        RUsageTiming::Snapshot(RUsageScope::ThisProcess(), timer);

    // clang-format off
    [[maybe_unused]] CpuHog cpu_hog{
        absl::Seconds(kIdleSecs),
        absl::Seconds(kHogSecs),
        kNumHogs,
        &hogging_started,
        &hogging_stopped
    };
    // clang-format on

    hogging_started.WaitForNotification();
    do {
      const auto sample =
          RUsageTiming::Snapshot(RUsageScope::ThisProcess(), timer);
      cpu_util_histo.Add(sample.cpu_utilization);
      // NOTE: Do NOT sleep here: that throws off the measurements.
    } while (!hogging_stopped.HasBeenNotified());

    const auto after =
        RUsageTiming::Snapshot(RUsageScope::ThisProcess(), timer);
    const auto delta = after - before;

    user_time_histo.Add(absl::ToDoubleSeconds(delta.user_time));
    if (absl::GetFlag(FLAGS_enable_system_load_sensitive_tests)) {
      wall_time_histo.Add(absl::ToDoubleSeconds(delta.wall_time));
      cpu_cores_histo.Add(after.cpu_hyper_cores);
    }

    if (absl::GetFlag(FLAGS_verbose)) {
      LOG(INFO) << "before: " << before;
      LOG(INFO) << "after:  " << after;
      LOG(INFO) << "delta:  " << delta;
    }

    EXPECT_EQ(delta.user_time, after.user_time - before.user_time);
    EXPECT_EQ(delta.sys_time, after.sys_time - before.sys_time);
    EXPECT_EQ(delta.wall_time, after.wall_time - before.wall_time);
  }

  if (absl::GetFlag(FLAGS_enable_system_load_sensitive_tests)) {
    if (absl::GetFlag(FLAGS_verbose)) {
      LOG(INFO) << "user_time_histo:\n" << user_time_histo;
      LOG(INFO) << "cpu_util_histo:\n" << cpu_util_histo;
      LOG(INFO) << "wall_time_histo:\n" << wall_time_histo;
      LOG(INFO) << "cpu_cores_histo:\n" << cpu_cores_histo;
    }

    EXPECT_NEAR(  //
        user_time_histo.Average(), kHogSecs * kNumHogs, kUserSecsLeeway);
    EXPECT_NEAR(cpu_util_histo.Average(), 1.0, kCpuUtilLeeway);
    EXPECT_NEAR(wall_time_histo.Average(), kWallSecs, kWallSecsLeeway);
    // The average CPU use is the ratio between the hog CPU time and, since
    // CpuHog idles the rest of the time, the total time.
    EXPECT_NEAR(  //
        cpu_cores_histo.Average(), kHogSecs * kNumHogs / kWallSecs,
        kCpuCoresLeeway);
  }
}

TEST(RUsageMemoryTest, Accuracy) {
  constexpr int kNumRuns = 20;
  constexpr MemSize kBytes = 100 * 1024 * 1024;
  // The RSS should grow almost exactly by kBytes, since we're staying well
  // below a pageable increase in allocations (100M) on a non-cramped machine.
  [[maybe_unused]] constexpr MemSize kRssLeeway = kBytes * 0.1;
  // Data also includes the stack, so it may grow more than the RSS.
  [[maybe_unused]] constexpr MemSize kDataLeeway = kBytes * 0.2;
  Histogram<MemSize> mem_rss_histo;
  Histogram<MemSize> mem_data_histo;

  // Have to accumulate BigThings rather than allocate+free on every iteration.
  // That latter would bump the RSS/data the first couple of iterations, but
  // then fit new allocations in the just freed up space.
  [[maybe_unused]] std::vector<BigThing> big_things;
  for (int i = 0; i < kNumRuns; ++i) {
    const auto before = RUsageMemory::Snapshot(RUsageScope::ThisProcess());
    big_things.emplace_back(kBytes);
    const auto after = RUsageMemory::Snapshot(RUsageScope::ThisProcess());
    const auto delta = after - before;

    if (absl::GetFlag(FLAGS_verbose)) {
      LOG(INFO) << "before: " << before;
      LOG(INFO) << "after:  " << after;
      LOG(INFO) << "delta:  " << delta;
    }

// NOTE: The sanitizers heavily instrument the code and skew any time
//  measurements.
#if !defined(ADDRESS_SANITIZER) && !defined(THREAD_SANITIZER) && \
    !defined(MEMORY_SANITIZER)
    EXPECT_EQ(delta.mem_rss, after.mem_rss - before.mem_rss);
    EXPECT_EQ(delta.mem_data, after.mem_data - before.mem_data);
    // VPeak can only grow. VSize may grow to fit BigThing or fit it as-is.
    EXPECT_GE(after.mem_vpeak, before.mem_vpeak);
    // VPeak >= VSize.
    EXPECT_GE(before.mem_vpeak, before.mem_vsize);
    EXPECT_GE(after.mem_vpeak, after.mem_vsize);
    // RSS <= VSize.
    EXPECT_LE(before.mem_rss, before.mem_vsize);
    EXPECT_LE(after.mem_rss, after.mem_vsize);
#else
    LOG(WARNING) << "Validation of test results omitted under *SAN: see code";
#endif
    mem_rss_histo.Add(delta.mem_rss);
    mem_data_histo.Add(delta.mem_data);
  }

  if (absl::GetFlag(FLAGS_verbose)) {
    LOG(INFO) << "mem_rss_histo:\n" << mem_rss_histo;
    LOG(INFO) << "mem_data_histo:\n" << mem_data_histo;
  }

// NOTE: The sanitizers heavily instrument the code and skew any time
//  measurements.
#if !defined(ADDRESS_SANITIZER) && !defined(THREAD_SANITIZER) && \
    !defined(MEMORY_SANITIZER)
  EXPECT_NEAR(mem_rss_histo.Average(), kBytes, kRssLeeway) << mem_rss_histo;
#ifdef __APPLE__
  // `data` is not supported.
#else
  EXPECT_NEAR(mem_data_histo.Average(), kBytes, kDataLeeway) << mem_data_histo;
#endif
#else
  LOG(WARNING) << "Validation of test results omitted under *SAN: see code";
#endif
}

TEST(RUsageMemoryTest, BadScope) {
  constexpr pid_t kBadPid = 999999999;
  EXPECT_NO_FATAL_FAILURE(  //
      const auto timing =
          RUsageTiming::Snapshot(RUsageScope::Process(kBadPid));
      VLOG(1) << "Timing: " << timing;
  );
  EXPECT_NO_FATAL_FAILURE(  //
      const auto memory =
          RUsageMemory::Snapshot(RUsageScope::Process(kBadPid));
      VLOG(1) << "Memory: " << memory;
  );
}

TEST(RUsageTimingTest, ConstantsAndMath) {
  const RUsageTiming timing{/*wall_time=*/absl::Seconds(4),
                            /*user_time=*/absl::Seconds(2),
                            /*sys_time=*/absl::Seconds(1),
                            /*cpu_utilization=*/.8,
                            /*cpu_hyper_cores=*/.6,
                            /*is_delta=*/false};
  const RUsageTiming half_timing{/*wall_time=*/timing.wall_time / 2,
                                 /*user_time=*/timing.user_time / 2,
                                 /*sys_time=*/timing.sys_time / 2,
                                 /*cpu_utilization=*/timing.cpu_utilization / 2,
                                 /*cpu_hyper_cores=*/timing.cpu_hyper_cores / 2,
                                 /*is_delta=*/false};
  const RUsageTiming quarter_timing{
      /*wall_time=*/timing.wall_time / 4,
      /*user_time=*/timing.user_time / 4,
      /*sys_time=*/timing.sys_time / 4,
      /*cpu_utilization=*/timing.cpu_utilization / 4,
      /*cpu_hyper_cores=*/timing.cpu_hyper_cores / 4,
      /*is_delta=*/false};

  EXPECT_LT(RUsageTiming::Min(), RUsageTiming::Max());
  EXPECT_GT(timing, RUsageTiming::Min());
  EXPECT_LT(timing, RUsageTiming::Max());

  EXPECT_EQ(timing + RUsageTiming::Zero(), timing);
  EXPECT_EQ(timing - timing, RUsageTiming::Zero());
  EXPECT_EQ(timing - quarter_timing, half_timing + quarter_timing);
  EXPECT_EQ(timing / 2, half_timing);
  EXPECT_EQ(timing / 4, quarter_timing);

  EXPECT_LT(half_timing, timing);
  EXPECT_LE(half_timing, timing);
  EXPECT_GT(timing, half_timing);
  EXPECT_GE(timing, half_timing);

  EXPECT_TRUE((timing - half_timing).is_delta);
  EXPECT_FALSE((timing + half_timing).is_delta);
  EXPECT_TRUE((timing - half_timing + quarter_timing).is_delta);
  EXPECT_FALSE((timing / 2).is_delta);
  EXPECT_TRUE(((timing - half_timing) / 2).is_delta);
}

TEST(SysRecourcesTest, ConstantsAndMath) {
  const RUsageMemory memory{/*mem_vsize=*/1000,
                            /*mem_vpeak=*/2000,
                            /*mem_rss=*/500,
                            /*mem_data=*/400,
                            /*mem_shared=*/20};
  const RUsageMemory half_memory{/*mem_vsize=*/memory.mem_vsize / 2,
                                 /*mem_vpeak=*/memory.mem_vpeak / 2,
                                 /*mem_rss=*/memory.mem_rss / 2,
                                 /*mem_data=*/memory.mem_data / 2,
                                 /*mem_shared=*/memory.mem_shared / 2,
                                 /*is_delta=*/false};
  const RUsageMemory quarter_memory{/*mem_vsize=*/memory.mem_vsize / 4,
                                    /*mem_vpeak=*/memory.mem_vpeak / 4,
                                    /*mem_rss=*/memory.mem_rss / 4,
                                    /*mem_data=*/memory.mem_data / 4,
                                    /*mem_shared=*/memory.mem_shared / 4,
                                    /*is_delta=*/false};

  EXPECT_LT(RUsageMemory::Min(), RUsageMemory::Max());
  EXPECT_GT(memory, RUsageMemory::Min());
  EXPECT_LT(memory, RUsageMemory::Max());

  EXPECT_EQ(memory + RUsageMemory::Zero(), memory);
  EXPECT_EQ(memory - memory, RUsageMemory::Zero());
  EXPECT_EQ(memory - quarter_memory, half_memory + quarter_memory);
  EXPECT_EQ(memory / 2, half_memory);
  EXPECT_EQ(memory / 4, quarter_memory);

  EXPECT_LT(half_memory, memory);
  EXPECT_LE(half_memory, memory);
  EXPECT_GT(memory, half_memory);
  EXPECT_GE(memory, half_memory);

  EXPECT_TRUE((memory - half_memory).is_delta);
  EXPECT_FALSE((memory + half_memory).is_delta);
  EXPECT_TRUE((memory - half_memory + quarter_memory).is_delta);
  EXPECT_FALSE((memory / 2).is_delta);
  EXPECT_TRUE(((memory - half_memory) / 2).is_delta);
}

TEST(RUsageTimingTest, HighLowWater) {
  constexpr absl::Duration kLowTime = absl::Seconds(1);
  constexpr absl::Duration kHighTime = absl::Seconds(3);
  constexpr CpuUtilization kLowCpu = 0.1;
  constexpr CpuUtilization kHighCpu = 0.9;
  const RUsageTiming timing1{/*wall_time=*/kHighTime,
                             /*user_time=*/kLowTime,
                             /*sys_time=*/kLowTime,
                             /*cpu_utilization=*/kHighCpu,
                             /*cpu_hyper_cores=*/kLowCpu};
  const RUsageTiming timing2{/*wall_time=*/kLowTime,
                             /*user_time=*/kHighTime,
                             /*sys_time=*/kHighTime,
                             /*cpu_utilization=*/kLowCpu,
                             /*cpu_hyper_cores=*/kHighCpu};

  const RUsageTiming low_water = RUsageTiming::LowWater(timing1, timing2);
  const RUsageTiming kExpectedLowWater{/*wall_time=*/kLowTime,
                                       /*user_time=*/kLowTime,
                                       /*sys_time=*/kLowTime,
                                       /*cpu_utilization=*/kLowCpu,
                                       /*cpu_hyper_cores=*/kLowCpu};
  EXPECT_EQ(low_water, kExpectedLowWater);

  const RUsageTiming high_water = RUsageTiming::HighWater(timing1, timing2);
  const RUsageTiming kExpectedHighWater{/*wall_time=*/kHighTime,
                                        /*user_time=*/kHighTime,
                                        /*sys_time=*/kHighTime,
                                        /*cpu_utilization=*/kHighCpu,
                                        /*cpu_hyper_cores=*/kHighCpu};
  EXPECT_EQ(high_water, kExpectedHighWater);
}

TEST(RUsageMemoryTest, HighLowWater) {
  constexpr MemSize kLowMem = 1000;
  constexpr MemSize kHighMem = 1000000;
  const RUsageMemory memory1{/*mem_vsize=*/kLowMem,
                             /*mem_vpeak=*/kHighMem,
                             /*mem_rss=*/kLowMem,
                             /*mem_data=*/kHighMem,
                             /*mem_shared=*/kLowMem};
  const RUsageMemory memory2{/*mem_vsize=*/kHighMem,
                             /*mem_vpeak=*/kLowMem,
                             /*mem_rss=*/kHighMem,
                             /*mem_data=*/kLowMem,
                             /*mem_shared=*/kHighMem};
  const RUsageMemory low_water = RUsageMemory::LowWater(memory1, memory2);
  const RUsageMemory kExpectedLowWater{/*mem_vsize=*/kLowMem,
                                       /*mem_vpeak=*/kLowMem,
                                       /*mem_rss=*/kLowMem,
                                       /*mem_data=*/kLowMem,
                                       /*mem_shared=*/kLowMem};
  EXPECT_EQ(low_water, kExpectedLowWater);

  const RUsageMemory high_water = RUsageMemory::HighWater(memory1, memory2);
  const RUsageMemory kExpectedHighWater{/*mem_vsize=*/kHighMem,
                                        /*mem_vpeak=*/kHighMem,
                                        /*mem_rss=*/kHighMem,
                                        /*mem_data=*/kHighMem,
                                        /*mem_shared=*/kHighMem};
  EXPECT_EQ(high_water, kExpectedHighWater);
}

TEST(RUsageTimingTest, Logging) {
  RUsageTiming timing{/*wall_time=*/absl::Seconds(303.3),
                      /*user_time=*/absl::Microseconds(101.1),
                      /*sys_time=*/absl::Milliseconds(202.2),
                      /*cpu_utilization=*/0.4,
                      /*cpu_hyper_cores=*/0.6,
                      /*is_delta=*/false};

  std::stringstream ss;
  ss << timing;
  EXPECT_EQ(ss.str(), timing.ShortStr());

  EXPECT_EQ(  //
      timing.ShortStr(),
      "Wall: 303.30s | User: 101us | Sys: 202ms | "
      "CpuUtil: 40.00% | CpuCores: 0.60");
  EXPECT_EQ(  //
      timing.FormattedStr(),
      "Wall:       303.30s | User:         101us | Sys:          202ms | "
      "CpuUtil:     40.00% | CpuCores:      0.60");

  timing.is_delta = true;
  EXPECT_EQ(  //
      timing.ShortStr(),
      "Wall: +303.30s | User: +101us | Sys: +202ms | "
      "CpuUtil: +40.00% | CpuCores: +0.60");
  EXPECT_EQ(  //
      timing.FormattedStr(),
      "Wall:      +303.30s | User:        +101us | Sys:         +202ms | "
      "CpuUtil:    +40.00% | CpuCores:     +0.60");

  RUsageTiming timing2 = timing / -2;
  EXPECT_EQ(  //
      timing2.ShortStr(),
      "Wall: -151.65s | User: -51us | Sys: -101ms | "
      "CpuUtil: -20.00% | CpuCores: -0.30");
  EXPECT_EQ(  //
      timing2.FormattedStr(),
      "Wall:      -151.65s | User:         -51us | Sys:         -101ms | "
      "CpuUtil:    -20.00% | CpuCores:     -0.30");
}

TEST(RUsageMemoryTest, Logging) {
  RUsageMemory memory{/*mem_vsize=*/1L * 1024 * 1024 * 1024,
                      /*mem_vpeak=*/2L * 1024 * 1024 * 1024,
                      /*mem_rss=*/500L * 1024 * 1024,
                      /*mem_data=*/750L * 1024 * 1024,
                      /*mem_shared=*/250L * 1024,
                      /*is_delta=*/false};

  std::stringstream ss;
  ss << memory;
  EXPECT_EQ(ss.str(), memory.ShortStr());

  EXPECT_EQ(  //
      memory.ShortStr(),
      "RSS: 500.00M | VSize: 1.00G | VPeak: 2.00G | "
      "Data: 750.00M | ShMem: 250.0K");
  EXPECT_EQ(  //
      memory.FormattedStr(),
      "RSS:        500.00M | VSize:        1.00G | VPeak:        2.00G | "
      "Data:       750.00M | ShMem:       250.0K");

  memory.is_delta = true;
  EXPECT_EQ(  //
      memory.ShortStr(),
      "RSS: +500.00M | VSize: +1.00G | VPeak: +2.00G | "
      "Data: +750.00M | ShMem: +250.0K");
  EXPECT_EQ(  //
      memory.FormattedStr(),
      "RSS:       +500.00M | VSize:       +1.00G | VPeak:       +2.00G | "
      "Data:      +750.00M | ShMem:      +250.0K");

  RUsageMemory memory2 = memory / -2;
  EXPECT_EQ(  //
      memory2.ShortStr(),
      "RSS: -250.00M | VSize: -512.00M | VPeak: -1.00G | "
      "Data: -375.00M | ShMem: -125.0K");
  EXPECT_EQ(  //
      memory2.FormattedStr(),
      "RSS:       -250.00M | VSize:     -512.00M | VPeak:       -1.00G | "
      "Data:      -375.00M | ShMem:      -125.0K");
}

}  // namespace fuzztest::internal

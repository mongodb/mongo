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

#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#endif  // __APPLE__
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>  // NOLINT: For hardware_concurrency() only.

#include "absl/base/nullability.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace fuzztest::internal {

//------------------------------------------------------------------------------
//                               ProcessTimer
//------------------------------------------------------------------------------

ProcessTimer::ProcessTimer() : start_time_{absl::Now()}, start_rusage_{} {
  getrusage(RUSAGE_SELF, &start_rusage_);
}

void ProcessTimer::Get(double& user, double& sys, double& wall) const {
  struct rusage curr_rusage = {};
  getrusage(RUSAGE_SELF, &curr_rusage);
  // clang-format off
  user = absl::ToDoubleSeconds(
      absl::DurationFromTimeval(curr_rusage.ru_utime) -
      absl::DurationFromTimeval(start_rusage_.ru_utime));
  sys = absl::ToDoubleSeconds(
      absl::DurationFromTimeval(curr_rusage.ru_stime) -
      absl::DurationFromTimeval(start_rusage_.ru_stime));
  wall = absl::ToDoubleSeconds(absl::Now() - start_time_);
  // clang-format on
}

//------------------------------------------------------------------------------
//                               RUsageScope
//------------------------------------------------------------------------------

#ifdef __APPLE__
class RUsageScope::PlatformInfo {
 public:
  PlatformInfo(pid_t pid) : pid_(pid) {}

  pid_t pid() const { return pid_; }

 private:
  pid_t pid_;
};
#else
class RUsageScope::PlatformInfo {
 public:
  enum ProcFile : size_t {
    kSched = 0,
    kStatm = 1,
    kStatus = 2,
    kNumDoNotUseDirectly = 3
  };

  PlatformInfo(pid_t pid)
      : proc_file_paths_{
            absl::StrFormat("/proc/%d/sched", pid),
            absl::StrFormat("/proc/%d/statm", pid),
            absl::StrFormat("/proc/%d/status", pid),
        } {}

  // Returns a path to the /proc/<pid>/<file> or /proc/<pid>/task/<tid>/<file>.
  [[nodiscard]] const std::string& GetProcFilePath(ProcFile file) const {
    CHECK_LT(file, proc_file_paths_.size());
    return proc_file_paths_[file];
  }

 private:
  std::array<std::string, ProcFile::kNumDoNotUseDirectly> proc_file_paths_;
};
#endif

RUsageScope RUsageScope::ThisProcess() {  //
  return RUsageScope{getpid()};
}

RUsageScope RUsageScope::Process(pid_t pid) {  //
  return RUsageScope{pid};
}

RUsageScope::RUsageScope(pid_t pid)
    : description_{absl::StrFormat("PID=%d", pid)},
      info_(std::make_shared<PlatformInfo>(pid)) {}

namespace detail {
namespace {

// A global static is fine: this object depends on getrusage() syscall ONLY, and
// absolutely no other globals in the program.
const ProcessTimer global_process_timer;

//------------------------------------------------------------------------------
//                      Read values from /proc/* files
//------------------------------------------------------------------------------

bool ReadProcFileFields(const std::string& path,
                        const char* absl_nonnull format, ...) {
  bool success = false;
  va_list value_list;
  va_start(value_list, format);
  std::ifstream file{path};
  // TODO(b/265461840): Silently ignoring missing /proc/ files. The current
  // callers ignore the returned status too. Improve.
  if (file.good()) {
    std::stringstream contents;
    contents << file.rdbuf();
    if (contents.good()) {
      if (vsscanf(contents.str().c_str(), format, value_list) != EOF) {
        success = true;
      }
    }
  }
  va_end(value_list);
  return success;
}

template <typename T>
bool ReadProcFileKeyword(  //
    const std::string& path, const char* format, T* value) {
  std::ifstream file{path};
  // TODO(b/265461840): Silently ignoring missing /proc/ files. The current
  // callers ignore the returned status too. Improve.
  if (file.good()) {
    constexpr std::streamsize kMaxLineLen = 1024;
    char line[kMaxLineLen] = {0};
    while (file.good()) {
      file.getline(line, kMaxLineLen);
      if (sscanf(line, format, value) == 1) {
        return true;
      }
    }
  }
  return false;
}

//------------------------------------------------------------------------------
//                           Comparison overloads
//------------------------------------------------------------------------------

template <typename T>
std::string NormalizeSign(T* value, bool always_signed) {
  if (*value < T{}) {
    *value = -(*value);
    return "-";
  } else if (always_signed) {
    return "+";
  } else {
    return "";
  }
}

template <template <typename T> typename Op>
RUsageTiming RUsageTimingOp(  //
    const RUsageTiming& t1, const RUsageTiming& t2, bool is_delta) {
  const Op<absl::Duration> time_op;
  const Op<double> cpu_op;
  // clang-format off
  return RUsageTiming{
      /*wall_time=*/ time_op(t1.wall_time, t2.wall_time),
      /*user_time=*/ time_op(t1.user_time, t2.user_time),
      /*sys_time=*/ time_op(t1.sys_time, t2.sys_time),
      /*cpu_utilization=*/ cpu_op(t1.cpu_utilization, t2.cpu_utilization),
      /*cpu_hyper_cores=*/ cpu_op(t1.cpu_hyper_cores, t2.cpu_hyper_cores),
      /*is_delta=*/ is_delta
      };
  // clang-format on
}

template <template <typename T> typename Cmp>
bool RUsageTimingCmp(const RUsageTiming& t1, const RUsageTiming& t2) {
  Cmp<absl::Duration> time_cmp;
  Cmp<double> cpu_cmp;
  // clang-format off
  return
      time_cmp(t1.wall_time, t2.wall_time) &&
      time_cmp(t1.user_time, t2.user_time) &&
      time_cmp(t1.sys_time, t2.sys_time) &&
      cpu_cmp(t1.cpu_utilization, t2.cpu_utilization) &&
      cpu_cmp(t1.cpu_hyper_cores, t2.cpu_hyper_cores);
  // clang-format on
}

template <template <typename T> typename Op>
RUsageMemory RUsageMemoryOp(  //
    const RUsageMemory& t1, const RUsageMemory& t2, bool is_delta) {
  const Op<MemSize> mem_op;
  // clang-format off
  return RUsageMemory{
      /*mem_vsize=*/ mem_op(t1.mem_vsize, t2.mem_vsize),
      /*mem_vpeak=*/ mem_op(t1.mem_vpeak, t2.mem_vpeak),
      /*mem_rss=*/ mem_op(t1.mem_rss, t2.mem_rss),
      /*mem_data=*/ mem_op(t1.mem_data, t2.mem_data),
      /*mem_shared=*/ mem_op(t1.mem_shared, t2.mem_shared),
      /*is_delta=*/ is_delta
      };
  // clang-format on
}

template <template <typename T> typename Cmp>
bool RUsageMemoryCmp(const RUsageMemory& t1, const RUsageMemory& t2) {
  Cmp<MemSize> mem_cmp;
  // clang-format off
  return
      mem_cmp(t1.mem_vsize, t2.mem_vsize) &&
      mem_cmp(t1.mem_vpeak, t2.mem_vpeak) &&
      mem_cmp(t1.mem_rss, t2.mem_rss) &&
      mem_cmp(t1.mem_data, t2.mem_data) &&
      mem_cmp(t1.mem_shared, t2.mem_shared);
  // clang-format on
}

template <typename T>
struct Min {
  constexpr T operator()(T lhs, T rhs) const { return std::min(lhs, rhs); }
};

template <typename T>
struct Max {
  constexpr T operator()(T lhs, T rhs) const { return std::max(lhs, rhs); }
};

}  // namespace
}  // namespace detail

//------------------------------------------------------------------------------
//                       FormatInOptimalUnits() overloads
//------------------------------------------------------------------------------

std::string FormatInOptimalUnits(absl::Duration duration, bool always_signed) {
  std::string sign = detail::NormalizeSign(&duration, always_signed);
  if (duration == absl::InfiniteDuration()) {
    return absl::StrCat(sign, "inf");
  } else {
    // clang-format off
    struct Fmt { absl::Duration unit; std::string abbrev; int decimals; } fmt =
        duration < absl::Microseconds(1) ? Fmt{absl::Nanoseconds(1), "ns", 0} :
        duration < absl::Milliseconds(1) ? Fmt{absl::Microseconds(1), "us", 0} :
        duration < absl::Seconds(1)      ? Fmt{absl::Milliseconds(1), "ms", 0} :
                                           Fmt{absl::Seconds(1), "s", 2};
    return absl::StrFormat(
        "%s%.*f%s",
        sign, fmt.decimals, absl::FDivDuration(duration, fmt.unit), fmt.abbrev);
    // clang-format on
  }
}

std::string FormatInOptimalUnits(MemSize bytes, bool always_signed) {
  constexpr MemSize kB = {1};
  constexpr MemSize kKB = {kB * 1024};
  constexpr MemSize kMB = {kKB * 1024};
  constexpr MemSize kGB = {kMB * 1024};
  constexpr MemSize kTB = {kGB * 1024};
  constexpr MemSize kPB = {kTB * 1024};
  std::string sign = detail::NormalizeSign(&bytes, always_signed);
  // clang-format off
  struct Fmt { long double unit; std::string abbrev; int decimals; } fmt =
      bytes < kKB ? Fmt{kB,  "B", 0} :
      bytes < kMB ? Fmt{kKB, "K", 1} :
      bytes < kGB ? Fmt{kMB, "M", 2} :
      bytes < kTB ? Fmt{kGB, "G", 2} :
      bytes < kPB ? Fmt{kTB, "T", 2} :
                    Fmt{kPB, "P", 2};
  return absl::StrFormat(
      "%s%.*Lf%s", sign, fmt.decimals, bytes / fmt.unit, fmt.abbrev);
  // clang-format on
}

std::string FormatInOptimalUnits(CpuUtilization util, bool always_signed) {
  std::string sign = detail::NormalizeSign(&util, always_signed);
  return absl::StrFormat("%s%.2f%%", sign, util * 100.0 /*%*/);
}

std::string FormatInOptimalUnits(CpuHyperCores cores, bool always_signed) {
  std::string sign = detail::NormalizeSign(&cores, always_signed);
  return absl::StrFormat("%s%.2f", sign, cores);
}

//------------------------------------------------------------------------------
//                              RUsageTiming
//------------------------------------------------------------------------------

RUsageTiming RUsageTiming::Zero() { return {}; }

RUsageTiming RUsageTiming::Min() {
  // clang-format off
  return RUsageTiming{
      /*wall_time=*/ -absl::InfiniteDuration(),
      /*user_time=*/ -absl::InfiniteDuration(),
      /*sys_time=*/ -absl::InfiniteDuration(),
      /*cpu_utilization=*/ 0.0,
      /*cpu_hyper_cores=*/ 0.0,
      /*is_delta=*/ false,
  };
  // clang-format on
}

RUsageTiming RUsageTiming::Max() {
  // clang-format off
  return RUsageTiming{
      /*wall_time=*/ absl::InfiniteDuration(),
      /*user_time=*/ absl::InfiniteDuration(),
      /*sys_time=*/ absl::InfiniteDuration(),
      // Theoretical max CPU utilization is 100%, but real-life numbers can go
      // just a little higher (the OS scheduler's rounding errors?).
      /*cpu_utilization=*/ 1.0,
      // hardware_concurrency() returns the number of hyperthreaded contexts.
      /*cpu_hyper_cores=*/
          static_cast<double>(std::thread::hardware_concurrency()),
      /*is_delta=*/ false,
  };
  // clang-format on
}

RUsageTiming RUsageTiming::Snapshot(const RUsageScope& scope) {
  return Snapshot(scope, detail::global_process_timer);
}

RUsageTiming RUsageTiming::Snapshot(  //
    const RUsageScope& scope, const ProcessTimer& timer) {
  double user_time = 0, sys_time = 0, wall_time = 0;
  // TODO(b/265480321): This does not honor `scope`.
  timer.Get(user_time, sys_time, wall_time);
  double cpu_utilization = 0;
#ifdef __APPLE__
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, scope.info().pid()};
  struct kinfo_proc info = {};
  size_t size = sizeof(info);
  CHECK(sysctl(mib, sizeof(mib) / sizeof(mib[0]), &info, &size, NULL, 0) == 0)
      << "Error getting process information: " << strerror(errno);
  cpu_utilization = info.kp_proc.p_pctcpu;
#else   // __APPLE__
  // Get the CPU utilization in 1/1024th units of the maximum from
  // /proc/self/sched. The maximum se.avg.util_avg field == SCHED_CAPACITY_SCALE
  // == 1024, as defined by the Linux scheduler code.
  // TODO(b/265461840): Handle reading errors.
  (void)detail::ReadProcFileKeyword(  // ignore errors (which are unlikely)
      scope.info().GetProcFilePath(
          RUsageScope::PlatformInfo::ProcFile::kSched),  //
      "se.avg.util_avg : %lf",                           //
      &cpu_utilization);
  constexpr double kLinuxSchedCapacityScale = 1024;
  cpu_utilization /= kLinuxSchedCapacityScale;
#endif  // __APPLE__
  return RUsageTiming{/*wall_time=*/absl::Seconds(wall_time),
                      /*user_time=*/absl::Seconds(user_time),
                      /*sys_time=*/absl::Seconds(sys_time),
                      /*cpu_utilization=*/cpu_utilization,
                      /*cpu_hyper_cores=*/(user_time + sys_time) / wall_time,
                      /*is_delta=*/false};
}

std::string RUsageTiming::ShortStr() const {
  return absl::StrFormat(  //
      "Wall: %s | User: %s | Sys: %s | CpuUtil: %s | CpuCores: %s",
      FormatInOptimalUnits(wall_time, /*always_signed=*/is_delta),
      FormatInOptimalUnits(user_time, /*always_signed=*/is_delta),
      FormatInOptimalUnits(sys_time, /*always_signed=*/is_delta),
      FormatInOptimalUnits(cpu_utilization, /*always_signed=*/is_delta),
      FormatInOptimalUnits(cpu_hyper_cores, /*always_signed=*/is_delta));
}

std::string RUsageTiming::FormattedStr() const {
  return absl::StrFormat(  //
      "Wall:  %12s | User:  %12s | Sys:   %12s | CpuUtil:  %9s | CpuCores: %9s",
      FormatInOptimalUnits(wall_time, /*always_signed=*/is_delta),
      FormatInOptimalUnits(user_time, /*always_signed=*/is_delta),
      FormatInOptimalUnits(sys_time, /*always_signed=*/is_delta),
      FormatInOptimalUnits(cpu_utilization, /*always_signed=*/is_delta),
      FormatInOptimalUnits(cpu_hyper_cores, /*always_signed=*/is_delta));
}

RUsageTiming operator+(const RUsageTiming& t) {
  // Subtraction sets `is_delta` to true.
  return t - RUsageTiming::Zero();
}

RUsageTiming operator-(const RUsageTiming& t) {
  // Subtraction negates the value and sets `is_delta` to true.
  return RUsageTiming::Zero() - t;
}

RUsageTiming operator-(const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingOp<std::minus>(t1, t2, true);
}

RUsageTiming operator+(const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingOp<std::plus>(t1, t2, t1.is_delta || t2.is_delta);
}

RUsageTiming operator/(const RUsageTiming& t, int64_t div) {
  CHECK_NE(div, 0);
  // NOTE: Can't use RUsageTimingOp() as this operation is asymmetrical.
  // clang-format off
  return RUsageTiming{
      /*wall_time=*/ t.wall_time / div,
      /*user_time=*/ t.user_time / div,
      /*sys_time=*/ t.sys_time / div,
      /*cpu_utilization=*/ t.cpu_utilization / div,
      /*cpu_hyper_cores=*/ t.cpu_hyper_cores / div,
      /*is_delta=*/ t.is_delta,
  };
  // clang-format on
}

bool operator==(const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingCmp<std::equal_to>(t1, t2);
}

bool operator!=(const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingCmp<std::not_equal_to>(t1, t2);
}

bool operator<(const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingCmp<std::less>(t1, t2);
}

bool operator<=(const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingCmp<std::less_equal>(t1, t2);
}

bool operator>(const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingCmp<std::greater>(t1, t2);
}

bool operator>=(const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingCmp<std::greater_equal>(t1, t2);
}

RUsageTiming RUsageTiming::LowWater(  //
    const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingOp<detail::Min>(t1, t2, false);
}

RUsageTiming RUsageTiming::HighWater(  //
    const RUsageTiming& t1, const RUsageTiming& t2) {
  return detail::RUsageTimingOp<detail::Max>(t1, t2, false);
}

std::ostream& operator<<(std::ostream& os, const RUsageTiming& t) {
  return os << t.ShortStr();
}

//------------------------------------------------------------------------------
//                              RUsageMemory
//------------------------------------------------------------------------------

RUsageMemory RUsageMemory::Zero() { return {}; }

RUsageMemory RUsageMemory::Min() {
  // clang-format off
  return RUsageMemory{
      /*mem_vsize=*/ std::numeric_limits<int64_t>::min(),
      /*mem_vpeak=*/ std::numeric_limits<int64_t>::min(),
      /*mem_rss=*/ std::numeric_limits<int64_t>::min(),
      /*mem_data=*/ std::numeric_limits<int64_t>::min(),
      /*mem_shared=*/ std::numeric_limits<int64_t>::min(),
      /*is_delta=*/ false
  };
  // clang-format on
}

RUsageMemory RUsageMemory::Max() {
  // clang-format off
  return RUsageMemory{
      /*mem_vsize=*/ std::numeric_limits<int64_t>::max(),
      /*mem_vpeak=*/ std::numeric_limits<int64_t>::max(),
      /*mem_rss=*/ std::numeric_limits<int64_t>::max(),
      /*mem_data=*/ std::numeric_limits<int64_t>::max(),
      /*mem_shared=*/ std::numeric_limits<int64_t>::max(),
      /*is_delta=*/ false
      };
  // clang-format on
}

RUsageMemory RUsageMemory::Snapshot(const RUsageScope& scope) {
  [[maybe_unused]] MemSize vsize = 0, rss = 0, shared = 0, code = 0, unused = 0,
                           data = 0, vpeak = 0;
#ifdef __APPLE__
  if (scope.info().pid() != getpid()) return {};
  struct proc_taskinfo pti = {};
  CHECK(proc_pidinfo(scope.info().pid(), PROC_PIDTASKINFO, 0, &pti,
                     PROC_PIDTASKINFO_SIZE) == PROC_PIDTASKINFO_SIZE)
      << "Unable to get system resource information";
  vsize = pti.pti_virtual_size;
  rss = pti.pti_resident_size;
  struct rusage rusage = {};
  CHECK(getrusage(RUSAGE_SELF, &rusage) == 0)
      << "Failed to get memory stats by getrusage";
  // `data` and `shared` are not supported in MacOS.
  // MacOS does not have a builtin way to query the peak size of virtual memory.
  // Here provide an estimation assuming nothing is swapped out.
  //
  // Here we assume `ru_maxrss` is in bytes according to some experiments.
  vpeak = vsize + (rusage.ru_maxrss - rss);
#else   // __APPLE__
  // Get memory stats except the VM peak from /proc/self/statm (see `man proc`).
  // TODO(b/265461840): Handle reading errors.
  (void)detail::ReadProcFileFields(  // ignore errors
      scope.info().GetProcFilePath(
          RUsageScope::PlatformInfo::ProcFile::kStatm),  //
      "%lld %lld %lld %lld %lld %lld",                   //
      &vsize, &rss, &shared, &code, &unused, &data);
  // Get the VM peak from /proc/self/status (see `man proc`).
  // TODO(b/265461840): Handle reading errors.
  (void)detail::ReadProcFileKeyword(  // ignore errors
      scope.info().GetProcFilePath(
          RUsageScope::PlatformInfo::ProcFile::kStatus),  //
      "VmPeak : %" SCNd64 " kB",                          //
      &vpeak);
  static const int page_size = getpagesize();
  vsize *= page_size;
  rss *= page_size;
  data *= page_size;
  shared *= page_size;
  // NOTE: The units are specified in the file itself, but they are always kB.
  static constexpr int kVPeakUnits = 1024;
  vpeak *= kVPeakUnits;
#endif  // __APPLE__
  // clang-format off
  return RUsageMemory{
      /*mem_vsize=*/ vsize,
      /*mem_vpeak=*/ vpeak,
      /*mem_rss=*/ rss,
      /*mem_data=*/ data,
      /*mem_shared=*/ shared,
      /*is_delta=*/ false
  };
  // clang-format on
}

std::string RUsageMemory::ShortStr() const {
  return absl::StrFormat(  //
      "RSS: %s | VSize: %s | VPeak: %s | Data: %s | ShMem: %s",
      FormatInOptimalUnits(mem_rss, /*always_signed=*/is_delta),
      FormatInOptimalUnits(mem_vsize, /*always_signed=*/is_delta),
      FormatInOptimalUnits(mem_vpeak, /*always_signed=*/is_delta),
      FormatInOptimalUnits(mem_data, /*always_signed=*/is_delta),
      FormatInOptimalUnits(mem_shared, /*always_signed=*/is_delta));
}

std::string RUsageMemory::FormattedStr() const {
  return absl::StrFormat(  //
      "RSS:   %12s | VSize: %12s | VPeak: %12s | Data:  %12s | ShMem: %12s",
      FormatInOptimalUnits(mem_rss, /*always_signed=*/is_delta),
      FormatInOptimalUnits(mem_vsize, /*always_signed=*/is_delta),
      FormatInOptimalUnits(mem_vpeak, /*always_signed=*/is_delta),
      FormatInOptimalUnits(mem_data, /*always_signed=*/is_delta),
      FormatInOptimalUnits(mem_shared, /*always_signed=*/is_delta));
}

RUsageMemory operator+(const RUsageMemory& m) {
  // Subtraction sets `is_delta` to true.
  return m - RUsageMemory::Zero();
}

RUsageMemory operator-(const RUsageMemory& m) {
  // Subtraction negates the value and sets `is_delta` to true.
  return RUsageMemory::Zero() - m;
}

RUsageMemory operator-(const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryOp<std::minus>(m1, m2, true);
}

RUsageMemory operator+(const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryOp<std::plus>(m1, m2, m1.is_delta || m2.is_delta);
}

RUsageMemory operator/(const RUsageMemory& m, int64_t div) {
  CHECK_NE(div, 0);
  // NOTE: Can't use RUsageMemoryOp() as this operation is asymmetrical.
  // clang-format off
  return RUsageMemory{
      /*mem_vsize=*/ m.mem_vsize / div,
      /*mem_vpeak=*/ m.mem_vpeak / div,
      /*mem_rss=*/ m.mem_rss / div,
      /*mem_data=*/ m.mem_data / div,
      /*mem_shared=*/ m.mem_shared / div,
      /*is_delta=*/ m.is_delta
  };
  // clang-format on
}

RUsageMemory RUsageMemory::LowWater(  //
    const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryOp<detail::Min>(m1, m2, true);
}

RUsageMemory RUsageMemory::HighWater(  //
    const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryOp<detail::Max>(m1, m2, true);
}

bool operator==(const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryCmp<std::equal_to>(m1, m2);
}

bool operator!=(const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryCmp<std::not_equal_to>(m1, m2);
}

bool operator<(const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryCmp<std::less>(m1, m2);
}

bool operator<=(const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryCmp<std::less_equal>(m1, m2);
}

bool operator>(const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryCmp<std::greater>(m1, m2);
}

bool operator>=(const RUsageMemory& m1, const RUsageMemory& m2) {
  return detail::RUsageMemoryCmp<std::greater_equal>(m1, m2);
}

std::ostream& operator<<(std::ostream& os, const RUsageMemory& m) {
  return os << m.ShortStr();
}

}  // namespace fuzztest::internal

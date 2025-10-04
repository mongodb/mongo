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

#include <stdint.h>

#include <string>
#include <thread>  // NOLINT(build/c++11)

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/random/random.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// This test mutates global state, including triggering the activation of the
// per-CPU caches.  It should not be run along side other tests in the same
// process that may rely on an isolated global instance.
TEST(CpuCacheActivateTest, GlobalInstance) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CpuCache& cache = tc_globals.cpu_cache();

  absl::Notification done;

  std::thread t([&]() {
    const int num_cpus = NumCPUs();
    absl::BitGen rng;

    while (!done.HasBeenNotified()) {
      const double coin = absl::Uniform(rng, 0., 1.);
      const bool ready = tc_globals.CpuCacheActive();

      if (ready && coin < 0.25) {
        const int cpu = absl::Uniform(rng, 0, num_cpus);
        benchmark::DoNotOptimize(cache.UsedBytes(cpu));
      } else if (ready && coin < 0.5) {
        const int cpu = absl::Uniform(rng, 0, num_cpus);
        benchmark::DoNotOptimize(cache.Capacity(cpu));
      } else if (ready && coin < 0.75) {
        benchmark::DoNotOptimize(cache.TotalUsedBytes());
      } else {
        benchmark::DoNotOptimize(cache.CacheLimit());
      }
    }
  });

  // Trigger initialization of the CpuCache, confirming it was not initialized
  // at the start of the test and is afterwards.
  EXPECT_FALSE(tc_globals.CpuCacheActive());
  ASSERT_NE(&TCMalloc_Internal_ForceCpuCacheActivation, nullptr);
  Parameters::set_per_cpu_caches(true);
  TCMalloc_Internal_ForceCpuCacheActivation();
  EXPECT_TRUE(tc_globals.CpuCacheActive());

  absl::SleepFor(absl::Seconds(0.2));

  done.Notify();
  t.join();
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

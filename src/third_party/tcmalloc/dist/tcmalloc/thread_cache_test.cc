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

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <limits>
#include <string>
#include <thread>  // NOLINT(build/c++11)

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_stats.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

int64_t MemoryUsageSlow(pid_t pid) {
  int64_t ret = 0;

  FILE* f =
      fopen(absl::StrCat("/proc/", pid, "/task/", pid, "/smaps").c_str(), "r");
  CHECK_CONDITION(f != nullptr);

  char buf[BUFSIZ];
  while (fgets(buf, sizeof(buf), f) != nullptr) {
    size_t rss;
    if (sscanf(buf, "Rss: %zu kB", &rss) == 1) ret += rss;
  }
  CHECK_CONDITION(feof(f));
  fclose(f);

  // Rss is reported in KiB
  ret *= 1024;

  // A sanity check: our return value should be in the same ballpark as
  // GetMemoryStats.
  tcmalloc::tcmalloc_internal::MemoryStats stats;
  CHECK_CONDITION(tcmalloc::tcmalloc_internal::GetMemoryStats(&stats));
  EXPECT_GE(ret, 0.9 * stats.rss);
  EXPECT_LE(ret, 1.1 * stats.rss);

  return ret;
}

class ThreadCacheTest : public ::testing::Test {
 public:
  ThreadCacheTest() {
    // Explicitly disable guarded allocations for this test.  For aggressive
    // sample rates on PPC (with 64KB pages), RSS grows quickly due to
    // page-sized allocations that we don't release.
    MallocExtension::SetGuardedSamplingRate(-1);
  }
};

// Make sure that creating and destroying many mallocing threads
// does not leak memory.
TEST_F(ThreadCacheTest, NoLeakOnThreadDestruction) {
  // Test only valid in per-thread mode
  ASSERT_FALSE(MallocExtension::PerCpuCachesActive());

  // Force a small sample to initialize tagged page allocator.
  constexpr int64_t kAlloc = 8192;
  const int64_t num_allocs =
      32 * MallocExtension::GetProfileSamplingRate() / kAlloc;
  for (int64_t i = 0; i < num_allocs; ++i) {
    ::operator delete(::operator new(kAlloc));
  }

  // Prefault and mlock the currently mapped address space.  This avoids minor
  // faults during the test from appearing as an apparent memory leak due to RSS
  // growth.
  //
  // Previously, we tried to only mlock file-backed mappings, but page faults
  // for .bss are also problematic (either from small pages [PPC] or hugepages
  // [all platforms]) for test flakiness.
  //
  // We do *not* apply MCL_FUTURE, as to allow allocations during the test run
  // to be released.
  if (mlockall(MCL_CURRENT) != 0) {
    GTEST_SKIP();
  }
  const int64_t start_size = MemoryUsageSlow(getpid());
  ASSERT_GT(start_size, 0);

  static const size_t kThreads = 16 * 1024;

  for (int i = 0; i < kThreads; ++i) {
    std::thread t([]() {
      void* p = calloc(1024, 1);
      benchmark::DoNotOptimize(p);
      free(p);
    });

    t.join();
  }

  // Flush the page heap.  Our allocations may have been retained.
  if (TCMalloc_Internal_SetHugePageFillerSkipSubreleaseInterval != nullptr) {
    TCMalloc_Internal_SetHugePageFillerSkipSubreleaseInterval(
        absl::ZeroDuration());
  }
  MallocExtension::ReleaseMemoryToSystem(std::numeric_limits<size_t>::max());

  // Read RSS usage only after releasing page heap has had an opportunity to
  // reduce it.
  const int64_t end_size = MemoryUsageSlow(getpid());

  // This will detect a leak rate of 12 bytes per thread, which is well under 1%
  // of the allocation done.
  EXPECT_GE(192 * 1024, end_size - start_size)
      << "Before: " << start_size << " After: " << end_size;
}

}  // namespace
}  // namespace tcmalloc

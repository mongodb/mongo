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

#include <stddef.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/test_allocator_harness.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc {
namespace {

// Parse out a line like:
// cpu   3:         1234 bytes (    0.0 MiB) with     3145728 bytes unallocated
// for the bytes value.  Only trick is not mallocing in the process.
int64_t ParseCpuCacheSize(absl::string_view buf, int cpu) {
  char needlebuf[32];
  int len = absl::SNPrintF(needlebuf, sizeof(needlebuf), "\ncpu %3d: ", cpu);
  TC_CHECK(0 < len && len < sizeof(needlebuf));

  const absl::string_view needle = needlebuf;

  auto pos = buf.find(needle);
  if (pos == absl::string_view::npos) {
    return -1;
  }
  // skip over the prefix.  Should now look like "    <number> bytes".
  pos += needle.size();
  buf.remove_prefix(pos);

  pos = buf.find_first_not_of(' ');
  if (pos != absl::string_view::npos) {
    buf.remove_prefix(pos);
  }

  pos = buf.find(' ');
  if (pos != absl::string_view::npos) {
    buf.remove_suffix(buf.size() - pos);
  }

  int64_t result;
  if (!absl::SimpleAtoi(buf, &result)) {
    return -1;
  }
  return result;
}

void GetMallocStats(std::string* buffer) {
  buffer->resize(buffer->capacity());

  TC_CHECK(&TCMalloc_Internal_GetStats != nullptr);
  size_t required = TCMalloc_Internal_GetStats(buffer->data(), buffer->size());
  EXPECT_LE(required, buffer->size());

  buffer->resize(std::min(required, buffer->size()));
}

TEST(ReclaimTest, ReclaimWorks) {
  if (!MallocExtension::PerCpuCachesActive()) {
    GTEST_SKIP() << "Skipping test without per-CPU caches";
    return;
  }

  std::string before, after;
  // Allocate strings, so that they don't need to be reallocated below, and so
  // don't perturb what we're trying to measure.
  //
  // As of November 2021, this size, plus the null terminator on the
  // std::string, is well outside of the size classes used by TCMalloc, so a
  // resize will not perturb the per-CPU cache.
  before.reserve(1 << 19);
  after.reserve(1 << 19);

  // Generate some traffic to fill up caches.
  const int kThreads = 10;
  ThreadManager mgr;
  AllocatorHarness harness(kThreads);

  mgr.Start(kThreads, [&](int thread_id) { harness.Run(thread_id); });

  absl::SleepFor(absl::Seconds(2));

  mgr.Stop();

  // All CPUs (that we have access to...) should have full percpu caches.
  // Pick one.
  GetMallocStats(&before);
  int cpu = -1;
  ssize_t used_bytes = -1;
  for (int i = 0, num_cpus = tcmalloc_internal::NumCPUs(); i < num_cpus; ++i) {
    used_bytes = ParseCpuCacheSize(before, i);
    if (used_bytes > 0) {
      cpu = i;
      break;
    } else if (used_bytes < -1) {
      // this is the only way we can find out --per_cpu was requested, but
      // not available: no matching line in Stats.
      return;
    }
  }
  // should find at least one non-empty cpu...
  ASSERT_NE(-1, cpu);
  uint64_t released_bytes = MallocExtension::ReleaseCpuMemory(cpu);
  GetMallocStats(&after);
  // I suppose some background thread could malloc here, but I'm not too
  // worried.
  EXPECT_EQ(released_bytes, used_bytes);
  EXPECT_EQ(0, ParseCpuCacheSize(after, cpu));
}

TEST(ReclaimTest, ReclaimStable) {
  if (!MallocExtension::PerCpuCachesActive()) {
    GTEST_SKIP() << "Skipping test without per-CPU caches";
    return;
  }

  // make sure that reclamation under heavy load doesn't lead to
  // corruption.
  struct Reclaimer {
    static void Go(std::atomic<bool>* sync, bool initialize_rseq) {
      if (initialize_rseq) {
        // Require initialization to succeed.
        TC_CHECK(tcmalloc_internal::subtle::percpu::IsFast());
      } else {
        // Require that we have not initialized this thread with rseq yet.
        TC_CHECK(!tcmalloc_internal::subtle::percpu::IsFastNoInit());
      }

      int iter = 0;
      while (!sync->load(std::memory_order_acquire)) {
        iter++;
        for (int i = 0, num_cpus = tcmalloc_internal::NumCPUs(); i < num_cpus;
             ++i) {
          MallocExtension::ReleaseCpuMemory(i);
        }
      }
    }
  };

  std::atomic<bool> sync{false};
  std::thread releaser(Reclaimer::Go, &sync, true);
  std::thread no_rseq_releaser(Reclaimer::Go, &sync, false);

  const int kThreads = 10;
  ThreadManager mgr;
  AllocatorHarness harness(kThreads);

  mgr.Start(kThreads, [&](int thread_id) { harness.Run(thread_id); });

  absl::SleepFor(absl::Seconds(5));

  mgr.Stop();

  sync.store(true, std::memory_order_release);
  releaser.join();
  no_rseq_releaser.join();
}

}  // namespace
}  // namespace tcmalloc

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

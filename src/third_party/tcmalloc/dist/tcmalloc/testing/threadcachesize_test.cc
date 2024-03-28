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
//
// Check that thread cache size limits are obeyed.

#include <stddef.h>

#include <new>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/synchronization/barrier.h"
#include "absl/synchronization/mutex.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

// Number of allocations per thread
static const int kAllocationsPerThread = 100000;

// Number of threads to create
static const int kNumThreads = 8;

// Total thread cache size to set
static const size_t kTotalThreadCacheSize = 8 << 20;

// Get current total thread-cache size
static size_t CurrentThreadCacheSize() {
  return MallocExtension::GetNumericProperty(
             "tcmalloc.current_total_thread_cache_bytes")
      .value_or(0);
}

// Maximum cache size seen so far
ABSL_CONST_INIT static absl::Mutex max_lock(absl::kConstInit);
static size_t max_cache_size = 0;

// A thread that cycles through allocating lots of objects of varying
// size, in an attempt to fill up its thread cache.
void Filler(absl::Barrier* fill, absl::Barrier* filled) {
  fill->Block();

  int size = 0;
  for (int i = 0; i < kAllocationsPerThread; i++) {
    void* p = ::operator new(size);
    sized_delete(p, size);
    size += 64;
    if (size > (32 << 10)) size = 0;

    if ((i % (kAllocationsPerThread / 10)) == 0) {
      absl::MutexLock l(&max_lock);
      const size_t cache_size = CurrentThreadCacheSize();
      if (cache_size > max_cache_size) {
        max_cache_size = cache_size;
      }
    }
  }

  filled->Block();
}

TEST(ThreadCache, MaxCacheHonored) {
  // Set the maximum total cache size
  MallocExtension::SetMaxTotalThreadCacheBytes(kTotalThreadCacheSize);

  absl::Barrier startfill(kNumThreads + 1);
  absl::Barrier filled(kNumThreads + 1);

  // Start all threads
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int i = 0; i < kNumThreads; i++) {
    threads.push_back(std::thread(Filler, &startfill, &filled));
  }

  // Concurrently fill all caches
  startfill.Block();

  // Wait for caches to fill.
  filled.Block();

  // Check that the maximum cache size did not get too large
  ASSERT_LT(max_cache_size, kTotalThreadCacheSize + kTotalThreadCacheSize / 5);

  // Cleanup all threads
  for (int i = 0; i < threads.size(); i++) {
    threads[i].join();
  }
}

}  // namespace
}  // namespace tcmalloc

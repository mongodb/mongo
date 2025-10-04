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

#include <malloc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/random/random.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/declarations.h"
#include "tcmalloc/malloc_extension.h"

extern "C" ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_GetStats(
    std::string* ret);
extern "C" ABSL_ATTRIBUTE_WEAK int MallocExtension_Internal_GetStatsInPbtxt(
    char* buffer, int buffer_length);

namespace tcmalloc {
namespace {

static void BM_new_delete(benchmark::State& state) {
  const int arg = state.range(0);

  for (auto s : state) {
    void* ptr = ::operator new(arg);
    ::operator delete(ptr);
  }
}
BENCHMARK(BM_new_delete)->Range(1, 1 << 20);

template <int size>
static void BM_new_delete_fixed(benchmark::State& state) {
  for (auto s : state) {
    void* ptr = ::operator new(size);
    ::operator delete(ptr, size);
  }
}

BENCHMARK_TEMPLATE(BM_new_delete_fixed, 8);
BENCHMARK_TEMPLATE(BM_new_delete_fixed, 16);
BENCHMARK_TEMPLATE(BM_new_delete_fixed, 32);
BENCHMARK_TEMPLATE(BM_new_delete_fixed, 512);
BENCHMARK_TEMPLATE(BM_new_delete_fixed, 4096);

static void BM_new_sized_delete(benchmark::State& state) {
  const int arg = state.range(0);

  for (auto s : state) {
    void* ptr = ::operator new(arg);
    ::operator delete(ptr, arg);
  }
}
BENCHMARK(BM_new_sized_delete)->Range(1, 1 << 20);

static void BM_size_returning_new_delete(benchmark::State& state) {
  const int arg = state.range(0);

  for (auto s : state) {
    sized_ptr_t res = tcmalloc_size_returning_operator_new(arg);
    ::operator delete(res.p, res.n);
  }
}
BENCHMARK(BM_size_returning_new_delete)->Range(1, 1 << 20);

static void BM_nallocx_new_sized_delete(benchmark::State& state) {
  const int arg = state.range(0);

  for (auto s : state) {
    size_t size = nallocx(arg, 0);
    benchmark::DoNotOptimize(size);
    void* ptr = ::operator new(size);
    ::operator delete(ptr, size);
  }
}
BENCHMARK(BM_nallocx_new_sized_delete)->Range(1, 1 << 20);

static void BM_aligned_new(benchmark::State& state) {
  const int size = state.range(0);
  const int alignment = state.range(1);

  for (auto s : state) {
    void* ptr = operator new(size, static_cast<std::align_val_t>(alignment));
    operator delete(ptr, size, static_cast<std::align_val_t>(alignment));
  }
}
BENCHMARK(BM_aligned_new)->RangePair(1, 1 << 20, 8, 64)->ArgPair(65, 64);

static void BM_new_delete_slow_path(benchmark::State& state) {
  // The benchmark is intended to cover CpuCache overflow/underflow paths,
  // transfer cache and a bit of the central freelist.
  std::vector<void*> allocs((4 << 10) + 128);
  const size_t size = state.range(0);
  for (auto s : state) {
    for (void*& p : allocs) {
      p = ::operator new(size);
    }
    for (void* p : allocs) {
      ::operator delete(p, size);
    }
  }
}
BENCHMARK(BM_new_delete_slow_path)->Arg(8)->Arg(8192);

static void BM_new_delete_transfer_cache(benchmark::State& state) {
  // The benchmark is intended to cover CpuCache overflow/underflow paths
  // and transfer cache.
  constexpr size_t kSize = 256;
  std::vector<void*> allocs(512);
  for (auto s : state) {
    for (void*& p : allocs) {
      p = ::operator new(kSize);
    }
    for (void* p : allocs) {
      ::operator delete(p, kSize);
    }
  }
}
BENCHMARK(BM_new_delete_transfer_cache);

static void* malloc_pages(size_t pages) {
  using tcmalloc::tcmalloc_internal::kPageSize;
  size_t size = pages * kPageSize;
  void* ptr = memalign(kPageSize, size);
  return ptr;
}

static void BM_malloc_pages(benchmark::State& state) {
  const int arg = state.range(0);

  size_t pages = arg;
  for (auto s : state) {
    void* ptr = malloc_pages(pages);
    benchmark::DoNotOptimize(ptr);
    free(ptr);
  }
}

BENCHMARK(BM_malloc_pages)
    ->Range(1, 1024)
    ->Arg(511)
    ->Arg(513)
    ->Arg(1023)
    ->Arg(1 + 20 * 1024 * 1024 / (8 * 1024))
    ->Arg(256);

static void BM_random_malloc_pages(benchmark::State& state) {
  const int kMaxOnHeap = 5000;
  const int kMaxRequestSizePages = 127;

  // We don't want random number generation to be a large part of
  // what we measure, so create a table of numbers now.
  absl::BitGen rand;
  const int kRandomTableSize = 98765;
  std::vector<size_t> random_index(kRandomTableSize);
  std::vector<size_t> random_request_size(kRandomTableSize);
  for (int i = 0; i < kRandomTableSize; i++) {
    random_index[i] = absl::Uniform<int32_t>(rand, 0, kMaxOnHeap);
    random_request_size[i] =
        absl::Uniform<int32_t>(rand, 0, kMaxRequestSizePages) + 1;
  }
  void* v[kMaxOnHeap];
  memset(v, 0, sizeof(v));
  size_t r = 0;
  for (int i = 0; i < kMaxOnHeap; ++i) {
    v[i] = malloc_pages(random_request_size[r]);
    if (++r == kRandomTableSize) {
      r = 0;
    }
  }

  for (auto s : state) {
    size_t index = random_index[r];
    free(v[index]);
    v[index] = malloc_pages(random_request_size[r]);
    if (++r == kRandomTableSize) {
      r = 0;
    }
  }

  for (int j = 0; j < kMaxOnHeap; j++) {
    free(v[j]);
  }
}

BENCHMARK(BM_random_malloc_pages);

static void BM_random_new_delete(benchmark::State& state) {
  const int kMaxOnHeap = 5000;
  const int kMaxRequestSize = 5000;

  // We don't want random number generation to be a large part of
  // what we measure, so create a table of numbers now.
  absl::BitGen rand;
  const int kRandomTableSize = 98765;
  std::vector<int> random_index(kRandomTableSize);
  std::vector<int> random_request_size(kRandomTableSize);
  for (int i = 0; i < kRandomTableSize; i++) {
    random_index[i] = absl::Uniform<int32_t>(rand, 0, kMaxOnHeap);
    random_request_size[i] =
        absl::Uniform<int32_t>(rand, 0, kMaxRequestSize) + 1;
  }
  void* v[kMaxOnHeap];
  memset(v, 0, sizeof(v));

  int r = 0;
  for (auto s : state) {
    int index = random_index[r];
    ::operator delete(v[index]);
    v[index] = ::operator new(random_request_size[r]);
    if (++r == kRandomTableSize) {
      r = 0;
    }
  }
  for (int j = 0; j < kMaxOnHeap; j++) {
    ::operator delete(v[j]);
  }
}
BENCHMARK(BM_random_new_delete);

static void BM_get_stats(benchmark::State& state) {
  std::vector<std::unique_ptr<char[]>> allocations;
  const int num_allocations = state.range(0);
  allocations.reserve(num_allocations);

  // Perform randomly sized allocations which will be kept live whilst we
  // collect stats, allowing us to observe the impact of heap size on the time
  // taken to collect stats.
  absl::BitGen rand;
  for (int i = 0; i < num_allocations; i++) {
    const size_t size = absl::Uniform<size_t>(rand, 1, 1 << 20);
    allocations.emplace_back(new char[size]);
  }

  // Here we go; collect stats.
  for (auto s : state) {
    const std::string stats = MallocExtension::GetStats();
    benchmark::DoNotOptimize(stats);
  }
}
BENCHMARK(BM_get_stats)->Range(1, 1 << 20);

static void BM_get_stats_internal(benchmark::State& state) {
  if (&MallocExtension_Internal_GetStats == nullptr) {
    // Sanitizer builds don't provide this function.
    return;
  }
  std::vector<std::unique_ptr<char[]>> allocations;
  const int num_allocations = state.range(0);
  allocations.reserve(num_allocations);

  // Perform randomly sized allocations which will be kept live whilst we
  // collect stats, allowing us to observe the impact of heap size on the time
  // taken to collect stats.
  absl::BitGen rand;
  for (int i = 0; i < num_allocations; i++) {
    const size_t size = absl::Uniform<size_t>(rand, 1, 1 << 20);
    allocations.emplace_back(new char[size]);
  }

  // Collect stats into our string, avoiding repeated resize.
  std::string stats;
  stats.resize(1 << 18);
  for (auto s : state) {
    MallocExtension_Internal_GetStats(&stats);
  }
}
BENCHMARK(BM_get_stats_internal)
    ->Range(1, 1 << 20)
    ->Unit(benchmark::kMillisecond);

static void BM_get_stats_pageheap_lock(benchmark::State& state) {
  std::vector<std::unique_ptr<char[]>> allocations;
  const int num_allocations = state.range(0);
  allocations.reserve(num_allocations);

  // Perform randomly sized allocations which will be kept live whilst we
  // collect stats, allowing us to observe the impact of heap size on the time
  // taken to collect stats.
  absl::BitGen rand;
  for (int i = 0; i < num_allocations; i++) {
    const size_t size = absl::Uniform<size_t>(rand, 1, 1 << 20);
    allocations.emplace_back(new char[size]);
  }

  // Create a background thread which busy-loops calling
  // MallocExtension::GetStats().
  absl::Notification done;
  std::atomic<size_t> counter = 0;
  std::thread stats_thread([&] {
    while (!done.HasBeenNotified()) {
      const std::string stats = MallocExtension::GetStats();
      counter.fetch_add(1, std::memory_order_seq_cst);
      benchmark::DoNotOptimize(stats);
    }
  });

  // Repeatedly acquire and release pageheap_lock.
  // Since nothing else is going on in this thread, the average time to acquire
  // and release is a reasonable approximation to how long the stats_thread
  // holds pageheap lock.
  for (auto s : state) {
    absl::Duration elapsed;
    const size_t start_counter = counter;
    size_t end_counter;
    do {
      const auto start_ts = absl::Now();
      tcmalloc_internal::pageheap_lock.Lock();
      tcmalloc_internal::pageheap_lock.Unlock();
      const auto end_ts = absl::Now();
      elapsed = end_ts - start_ts;
      end_counter = counter;
    } while (start_counter == end_counter);

    state.SetIterationTime(absl::ToDoubleSeconds(elapsed) /
                           (end_counter - start_counter));
  }

  // End the background stats_thread.
  done.Notify();
  stats_thread.join();
}
BENCHMARK(BM_get_stats_pageheap_lock)
    ->Range(1, 1 << 20)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

static void BM_get_stats_pbtxt_internal(benchmark::State& state) {
  if (&MallocExtension_Internal_GetStatsInPbtxt == nullptr) {
    // Sanitizer builds don't provide this function.
    return;
  }
  std::vector<std::unique_ptr<char[]>> allocations;
  const int num_allocations = state.range(0);
  allocations.reserve(num_allocations);

  // Perform randomly sized allocations which will be kept live whilst we
  // collect stats, allowing us to observe the impact of heap size on the time
  // taken to collect stats.
  absl::BitGen rand;
  for (int i = 0; i < num_allocations; i++) {
    const size_t size = absl::Uniform<size_t>(rand, 1, 1 << 20);
    allocations.emplace_back(new char[size]);
  }

  // Collect stats into our buffer. We use 3MiB -- same as
  // net_http_handlers::MalloczHandler
  std::vector<char> buf(3 << 20);
  for (auto s : state) {
    int sz = MallocExtension_Internal_GetStatsInPbtxt(&buf[0], buf.size());
    benchmark::DoNotOptimize(sz);
  }
}
BENCHMARK(BM_get_stats_pbtxt_internal)
    ->Range(1, 1 << 20)
    ->Unit(benchmark::kMillisecond);

static void BM_get_stats_pbtxt_pageheap_lock(benchmark::State& state) {
  if (&MallocExtension_Internal_GetStatsInPbtxt == nullptr) {
    // Sanitizer builds don't provide this function.
    return;
  }
  std::vector<std::unique_ptr<char[]>> allocations;
  const int num_allocations = state.range(0);
  allocations.reserve(num_allocations);

  // Perform randomly sized allocations which will be kept live whilst we
  // collect stats, allowing us to observe the impact of heap size on the time
  // taken to collect stats.
  absl::BitGen rand;
  for (int i = 0; i < num_allocations; i++) {
    const size_t size = absl::Uniform<size_t>(rand, 1, 1 << 20);
    allocations.emplace_back(new char[size]);
  }

  // Create a background thread which busy-loops calling
  // MallocExtension::GetStats().
  absl::Notification done;
  std::atomic<size_t> counter = 0;
  std::thread stats_thread([&] {
    std::vector<char> buf(3 << 20);
    while (!done.HasBeenNotified()) {
      const int sz =
          MallocExtension_Internal_GetStatsInPbtxt(&buf[0], buf.size());
      counter.fetch_add(1, std::memory_order_seq_cst);
      benchmark::DoNotOptimize(sz);
    }
  });

  // Repeatedly acquire and release pageheap_lock.
  // Since nothing else is going on in this thread, the average time to acquire
  // and release is a reasonable approximation to how long the stats_thread
  // holds pageheap lock.
  for (auto s : state) {
    absl::Duration elapsed;
    const size_t start_counter = counter;
    size_t end_counter;
    do {
      const auto start_ts = absl::Now();
      tcmalloc_internal::pageheap_lock.Lock();
      tcmalloc_internal::pageheap_lock.Unlock();
      const auto end_ts = absl::Now();
      elapsed = end_ts - start_ts;
      end_counter = counter;
    } while (start_counter == end_counter);

    state.SetIterationTime(absl::ToDoubleSeconds(elapsed) /
                           (end_counter - start_counter));
  }

  // End the background stats_thread.
  done.Notify();
  stats_thread.join();
}
BENCHMARK(BM_get_stats_pbtxt_pageheap_lock)
    ->Range(1, 1 << 20)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

static void BM_get_heap_profile(benchmark::State& state) {
  std::vector<std::unique_ptr<char[]>> allocations;
  const int num_allocations = state.range(0);
  allocations.reserve(num_allocations);

  // Perform randomly sized allocations which will be kept live while we collect
  // the heap profile.
  absl::BitGen rand;
  for (int i = 0; i < num_allocations; i++) {
    const size_t size = absl::Uniform<size_t>(rand, 1, 1 << 20);
    allocations.emplace_back(new char[size]);
  }

  for (auto s : state) {
    MallocExtension::SnapshotCurrent(ProfileType::kHeap);
  }
}
BENCHMARK(BM_get_heap_profile)->Range(1, 1 << 20);

static void BM_get_heap_profile_while_allocating(benchmark::State& state) {
  std::vector<std::unique_ptr<char[]>> allocations;
  const int num_allocations = state.range(0);
  allocations.reserve(num_allocations);

  // Perform randomly sized allocations which will be kept live while we collect
  // heap profile.
  absl::BitGen rand;
  for (int i = 0; i < num_allocations; i++) {
    const size_t size = absl::Uniform<size_t>(rand, 1, 1 << 20);
    allocations.emplace_back(new char[size]);
  }

  // Create a background thread that keeps collecting the heap profile.
  absl::Notification done;
  std::thread profile_thread([&] {
    while (!done.HasBeenNotified()) {
      MallocExtension::SnapshotCurrent(ProfileType::kHeap);
    }
  });

  // Allocate large objects (> 256KB). This would hit the pageheap lock and
  // its performance would be affected by how long the profile_thread holds the
  // pageheap lock.
  for (auto s : state) {
    std::vector<std::unique_ptr<char[]>> large_allocations;
    large_allocations.reserve(100);
    for (int i = 0; i < 100; i++) {
      const size_t size = absl::Uniform<size_t>(rand, 256 * 1024, 1 << 20);
      large_allocations.emplace_back(new char[size]);
    }
  }

  // End the background profile_thread.
  done.Notify();
  profile_thread.join();
}
BENCHMARK(BM_get_heap_profile_while_allocating)->Range(1, 1 << 18);

}  // namespace
}  // namespace tcmalloc

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

#include "tcmalloc/cpu_cache.h"

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <tuple>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/optimization.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/random.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/affinity.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/percpu_tcmalloc.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/tcmalloc_policy.h"
#include "tcmalloc/testing/testutil.h"
#include "tcmalloc/testing/thread_manager.h"
#include "tcmalloc/transfer_cache.h"

namespace tcmalloc {
namespace tcmalloc_internal {
class CpuCachePeer {
 public:
  template <typename CpuCache>
  static uint8_t GetSlabShift(const CpuCache& cpu_cache) {
    return cpu_cache.freelist_.GetShift();
  }

  template <typename CpuCache>
  static void IncrementCacheMisses(CpuCache& cpu_cache) {
    cpu_cache.RecordCacheMissStat(/*cpu=*/0, /*is_alloc=*/true);
    cpu_cache.RecordCacheMissStat(/*cpu=*/0, /*is_alloc=*/false);
  }

  // Validate that we're using >90% of the available slab bytes.
  template <typename CpuCache>
  static void ValidateSlabBytes(const CpuCache& cpu_cache) {
    cpu_cache_internal::SlabShiftBounds bounds =
        cpu_cache.GetPerCpuSlabShiftBounds();
    for (uint8_t shift = bounds.initial_shift;
         shift <= bounds.max_shift &&
         shift > cpu_cache_internal::kInitialBasePerCpuShift;
         ++shift) {
      const auto [bytes_required, bytes_available] =
          EstimateSlabBytes(cpu_cache.GetMaxCapacityFunctor(shift));
      EXPECT_GT(bytes_required * 10, bytes_available * 9)
          << bytes_required << " " << bytes_available << " " << kNumaPartitions
          << " " << kNumBaseClasses << " " << kNumClasses;
      EXPECT_LE(bytes_required, bytes_available);
    }
  }

  template <typename CpuCache>
  static size_t ResizeInfoSize() {
    return sizeof(typename CpuCache::ResizeInfo);
  }
};

namespace {

enum class DynamicSlab { kGrow, kShrink, kNoop };

class TestStaticForwarder {
 public:
  TestStaticForwarder() : sharded_manager_(&owner_, &cpu_layout_) {
    numa_topology_.Init();
  }

  void InitializeShardedManager(int num_shards) {
    PageHeapSpinLockHolder l;
    cpu_layout_.Init(num_shards);
    sharded_manager_.Init();
  }

  static void* Alloc(size_t size, std::align_val_t alignment) {
    return mmap(nullptr, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }

  void* AllocReportedImpending(size_t size, std::align_val_t alignment) {
    arena_reported_impending_bytes_ -= static_cast<int64_t>(size);
    return mmap(nullptr, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }

  static void Dealloc(void* ptr, size_t size, std::align_val_t /*alignment*/) {
    munmap(ptr, size);
  }

  void ArenaUpdateAllocatedAndNonresident(int64_t allocated,
                                          int64_t nonresident) {
    if (nonresident == 0) {
      arena_reported_impending_bytes_ += allocated;
    } else {
      arena_reported_impending_bytes_ = 0;
    }
    arena_reported_nonresident_bytes_ += nonresident;
  }

  void ShrinkToUsageLimit() {
    EXPECT_GT(arena_reported_impending_bytes_, 0);
    ++shrink_to_usage_limit_calls_;
  }

  bool per_cpu_caches_dynamic_slab_enabled() { return dynamic_slab_enabled_; }

  double per_cpu_caches_dynamic_slab_grow_threshold() {
    if (dynamic_slab_grow_threshold_ >= 0) return dynamic_slab_grow_threshold_;
    return dynamic_slab_ == DynamicSlab::kGrow
               ? -1.0
               : std::numeric_limits<double>::max();
  }

  double per_cpu_caches_dynamic_slab_shrink_threshold() {
    if (dynamic_slab_shrink_threshold_ >= 0)
      return dynamic_slab_shrink_threshold_;
    return dynamic_slab_ == DynamicSlab::kShrink
               ? std::numeric_limits<double>::max()
               : -1.0;
  }

  size_t class_to_size(int size_class) const {
    if (size_map_.has_value()) {
      return size_map_->class_to_size(size_class);
    } else {
      return transfer_cache_.class_to_size(size_class);
    }
  }

  absl::Span<const size_t> cold_size_classes() const {
    if (size_map_.has_value()) {
      return size_map_->ColdSizeClasses();
    } else {
      return {};
    }
  }

  size_t num_objects_to_move(int size_class) const {
    if (size_map_.has_value()) {
      return size_map_->num_objects_to_move(size_class);
    } else {
      return transfer_cache_.num_objects_to_move(size_class);
    }
  }

  size_t max_capacity(int size_class) const {
    if (size_map_.has_value()) {
      return size_map_->max_capacity(size_class);
    } else {
      return 2048;
    }
  }

  const NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() const {
    return numa_topology_;
  }

  bool UseWiderSlabs() const { return wider_slabs_enabled_; }

  bool ConfigureSizeClassMaxCapacity() const {
    return configure_size_class_max_capacity_;
  }

  using ShardedManager =
      ShardedTransferCacheManagerBase<FakeShardedTransferCacheManager,
                                      FakeCpuLayout,
                                      MinimalFakeCentralFreeList>;

  ShardedManager& sharded_transfer_cache() { return sharded_manager_; }

  const ShardedManager& sharded_transfer_cache() const {
    return sharded_manager_;
  }

  TwoSizeClassManager<FakeCentralFreeList,
                      internal_transfer_cache::TransferCache>&
  transfer_cache() {
    return transfer_cache_;
  }

  bool UseGenericShardedCache() const { return owner_.UseGenericCache(); }
  void SetGenericShardedCache(bool value) { owner_.SetGenericCache(value); }
  bool UseShardedCacheForLargeClassesOnly() const {
    return owner_.EnableCacheForLargeClassesOnly();
  }
  void SetShardedCacheForLargeClassesOnly(bool value) {
    owner_.SetCacheForLargeClassesOnly(value);
  }

  size_t arena_reported_nonresident_bytes_ = 0;
  int64_t arena_reported_impending_bytes_ = 0;
  size_t shrink_to_usage_limit_calls_ = 0;
  bool dynamic_slab_enabled_ = false;
  double dynamic_slab_grow_threshold_ = -1;
  double dynamic_slab_shrink_threshold_ = -1;
  bool wider_slabs_enabled_ = false;
  bool configure_size_class_max_capacity_ = false;
  DynamicSlab dynamic_slab_ = DynamicSlab::kNoop;
  std::optional<SizeMap> size_map_;

 private:
  NumaTopology<kNumaPartitions, kNumBaseClasses> numa_topology_;
  FakeShardedTransferCacheManager owner_;
  FakeCpuLayout cpu_layout_;
  ShardedManager sharded_manager_;
  TwoSizeClassManager<FakeCentralFreeList,
                      internal_transfer_cache::TransferCache>
      transfer_cache_;
};

using CpuCache = cpu_cache_internal::CpuCache<TestStaticForwarder>;
using MissCount = CpuCache::MissCount;
using PerClassMissType = CpuCache::PerClassMissType;

TEST(CpuCacheTest, MinimumShardsForGenericCache) {
  if (!subtle::percpu::IsFast()) {
    return;
  }
  CpuCache cache;
  cache.Activate();

  using ShardedManager = TestStaticForwarder::ShardedManager;
  TestStaticForwarder& forwarder = cache.forwarder();
  forwarder.SetShardedCacheForLargeClassesOnly(false);
  forwarder.SetGenericShardedCache(true);

  ShardedManager& sharded_transfer_cache = forwarder.sharded_transfer_cache();
  constexpr int kNumShards = ShardedManager::kMinShardsAllowed - 1;
  TC_ASSERT_GT(kNumShards, 0);
  forwarder.InitializeShardedManager(kNumShards);

  constexpr int kCpuId = 0;
  ScopedFakeCpuId fake_cpu_id(kCpuId);
  EXPECT_FALSE(sharded_transfer_cache.shard_initialized(0));
  EXPECT_EQ(sharded_transfer_cache.NumActiveShards(), 0);
  EXPECT_EQ(forwarder.transfer_cache().tc_length(kSizeClass), 0);

  constexpr size_t kSizeClass = 1;
  const size_t num_to_move = cache.forwarder().num_objects_to_move(kSizeClass);

  // Allocate an object. As we are using less than kMinShardsAllowed number of
  // shards, we should bypass sharded transfer cache entirely.
  void* ptr = cache.Allocate(kSizeClass);
  for (int size_class = 1; size_class < kNumClasses; ++size_class) {
    EXPECT_FALSE(sharded_transfer_cache.should_use(size_class));
    EXPECT_EQ(sharded_transfer_cache.GetStats(size_class).capacity, 0);
    EXPECT_EQ(sharded_transfer_cache.GetStats(size_class).max_capacity, 0);
  }
  // No requests are sent to sharded transfer cache. So, it should stay
  // uninitialized.
  EXPECT_EQ(sharded_transfer_cache.tc_length(kCpuId, kSizeClass), 0);
  EXPECT_FALSE(sharded_transfer_cache.shard_initialized(0));
  EXPECT_EQ(sharded_transfer_cache.NumActiveShards(), 0);
  EXPECT_EQ(forwarder.transfer_cache().tc_length(kSizeClass), 0);

  cache.Deallocate(ptr, kSizeClass);
  cache.Reclaim(0);
  EXPECT_EQ(sharded_transfer_cache.tc_length(kCpuId, kSizeClass), 0);
  EXPECT_FALSE(sharded_transfer_cache.shard_initialized(0));
  EXPECT_EQ(sharded_transfer_cache.NumActiveShards(), 0);
  // We should deallocate directly to the LIFO transfer cache.
  EXPECT_EQ(forwarder.transfer_cache().tc_length(kSizeClass),
            num_to_move / 2 + 1);
}

TEST(CpuCacheTest, UsesShardedAsBackingCache) {
  if (!subtle::percpu::IsFast()) {
    return;
  }
  CpuCache cache;
  cache.Activate();

  using ShardedManager = TestStaticForwarder::ShardedManager;
  TestStaticForwarder& forwarder = cache.forwarder();
  forwarder.SetShardedCacheForLargeClassesOnly(false);
  forwarder.SetGenericShardedCache(true);

  ShardedManager& sharded_transfer_cache = forwarder.sharded_transfer_cache();
  constexpr int kNumShards = ShardedManager::kMinShardsAllowed;
  TC_ASSERT_GT(kNumShards, 0);
  forwarder.InitializeShardedManager(kNumShards);

  ScopedFakeCpuId fake_cpu_id(0);
  EXPECT_FALSE(sharded_transfer_cache.shard_initialized(0));
  EXPECT_EQ(sharded_transfer_cache.NumActiveShards(), 0);

  constexpr size_t kSizeClass = 1;
  TransferCacheStats sharded_stats =
      sharded_transfer_cache.GetStats(kSizeClass);
  EXPECT_EQ(sharded_stats.remove_hits, 0);
  EXPECT_EQ(sharded_stats.remove_misses, 0);
  EXPECT_EQ(sharded_stats.insert_hits, 0);
  EXPECT_EQ(sharded_stats.insert_misses, 0);

  // Allocate an object and make sure that we allocate from the sharded transfer
  // cache and that the sharded cache has been initialized.
  void* ptr = cache.Allocate(kSizeClass);
  sharded_stats = sharded_transfer_cache.GetStats(kSizeClass);
  EXPECT_EQ(sharded_stats.remove_hits, 0);
  EXPECT_EQ(sharded_stats.remove_misses, 1);
  EXPECT_EQ(sharded_stats.insert_hits, 0);
  EXPECT_EQ(sharded_stats.insert_misses, 0);
  EXPECT_TRUE(sharded_transfer_cache.shard_initialized(0));
  EXPECT_EQ(sharded_transfer_cache.NumActiveShards(), 1);

  // Free objects to confirm that they are indeed released back to the sharded
  // transfer cache.
  cache.Deallocate(ptr, kSizeClass);
  cache.Reclaim(0);
  sharded_stats = sharded_transfer_cache.GetStats(kSizeClass);
  EXPECT_EQ(sharded_stats.insert_hits, 1);
  EXPECT_EQ(sharded_stats.insert_misses, 0);

  // Ensure that we never use legacy transfer cache by checking that hits and
  // misses are zero.
  TransferCacheStats tc_stats = forwarder.transfer_cache().GetStats(kSizeClass);
  EXPECT_EQ(tc_stats.remove_hits, 0);
  EXPECT_EQ(tc_stats.remove_misses, 0);
  EXPECT_EQ(tc_stats.insert_hits, 0);
  EXPECT_EQ(tc_stats.insert_misses, 0);
  forwarder.SetGenericShardedCache(false);
  cache.Deactivate();
}

TEST(CpuCacheTest, ResizeInfoNoFalseSharing) {
  const size_t resize_info_size = CpuCachePeer::ResizeInfoSize<CpuCache>();
  EXPECT_EQ(resize_info_size % ABSL_CACHELINE_SIZE, 0) << resize_info_size;
}

TEST(CpuCacheTest, Metadata) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  const int num_cpus = NumCPUs();

  CpuCache cache;
  cache.Activate();

  cpu_cache_internal::SlabShiftBounds shift_bounds =
      cache.GetPerCpuSlabShiftBounds();

  PerCPUMetadataState r = cache.MetadataMemoryUsage();
  size_t slabs_size = subtle::percpu::GetSlabsAllocSize(
      subtle::percpu::ToShiftType(shift_bounds.max_shift), num_cpus);
  size_t resize_size = num_cpus * sizeof(bool);
  size_t begins_size = kNumClasses * sizeof(std::atomic<uint16_t>);
  EXPECT_EQ(r.virtual_size, slabs_size + resize_size + begins_size);
  EXPECT_EQ(r.resident_size, 0);

  auto count_cores = [&]() {
    int populated_cores = 0;
    for (int i = 0; i < num_cpus; i++) {
      if (cache.HasPopulated(i)) {
        populated_cores++;
      }
    }
    return populated_cores;
  };

  EXPECT_EQ(0, count_cores());

  int allowed_cpu_id;
  const size_t kSizeClass = 2;
  const size_t num_to_move = cache.forwarder().num_objects_to_move(kSizeClass);
  const size_t virtual_cpu_id_offset = subtle::percpu::UsingFlatVirtualCpus()
                                           ? offsetof(kernel_rseq, vcpu_id)
                                           : offsetof(kernel_rseq, cpu_id);
  void* ptr;
  {
    // Restrict this thread to a single core while allocating and processing the
    // slow path.
    //
    // TODO(b/151313823):  Without this restriction, we may access--for reading
    // only--other slabs if we end up being migrated.  These may cause huge
    // pages to be faulted for those cores, leading to test flakiness.
    tcmalloc_internal::ScopedAffinityMask mask(
        tcmalloc_internal::AllowedCpus()[0]);
    allowed_cpu_id =
        subtle::percpu::GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset);

    ptr = cache.Allocate(kSizeClass);

    if (mask.Tampered() ||
        allowed_cpu_id !=
            subtle::percpu::GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset)) {
      return;
    }
  }
  EXPECT_NE(ptr, nullptr);
  EXPECT_EQ(1, count_cores());

  r = cache.MetadataMemoryUsage();
  EXPECT_EQ(
      r.virtual_size,
      resize_size + begins_size +
          subtle::percpu::GetSlabsAllocSize(
              subtle::percpu::ToShiftType(shift_bounds.max_shift), num_cpus));

  // We expect to fault in a single core, but we may end up faulting an
  // entire hugepage worth of memory when we touch that core and another when
  // touching the header.
  const size_t core_slab_size = r.virtual_size / num_cpus;
  const size_t upper_bound =
      ((core_slab_size + kHugePageSize - 1) & ~(kHugePageSize - 1)) +
      kHugePageSize;

  // A single core may be less than the full slab (core_slab_size), since we
  // do not touch every page within the slab.
  EXPECT_GT(r.resident_size, 0);
  EXPECT_LE(r.resident_size, upper_bound)
      << count_cores() << " " << core_slab_size << " " << kHugePageSize;

  // This test is much more sensitive to implementation details of the per-CPU
  // cache.  It may need to be updated from time to time.  These numbers were
  // calculated by MADV_NOHUGEPAGE'ing the memory used for the slab and
  // measuring the resident size.
  switch (shift_bounds.max_shift) {
    case 12:
      EXPECT_GE(r.resident_size, 4096);
      break;
    case 18:
      EXPECT_GE(r.resident_size, 8192);
      break;
    default:
      ASSUME(false);
      break;
  }

  // Read stats from the CPU caches.  This should not impact resident_size.
  const size_t max_cpu_cache_size = Parameters::max_per_cpu_cache_size();
  size_t total_used_bytes = 0;
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    size_t used_bytes = cache.UsedBytes(cpu);
    total_used_bytes += used_bytes;

    if (cpu == allowed_cpu_id) {
      EXPECT_GT(used_bytes, 0);
      EXPECT_TRUE(cache.HasPopulated(cpu));
    } else {
      EXPECT_EQ(used_bytes, 0);
      EXPECT_FALSE(cache.HasPopulated(cpu));
    }

    EXPECT_LE(cache.Unallocated(cpu), max_cpu_cache_size);
    EXPECT_EQ(cache.Capacity(cpu), max_cpu_cache_size);
    EXPECT_EQ(cache.Allocated(cpu) + cache.Unallocated(cpu),
              cache.Capacity(cpu));
  }

  for (int size_class = 1; size_class < kNumClasses; ++size_class) {
    // This is sensitive to the current growth policies of CpuCache.  It may
    // require updating from time-to-time.
    EXPECT_EQ(cache.TotalObjectsOfClass(size_class),
              (size_class == kSizeClass ? num_to_move - 1 : 0))
        << size_class;
  }
  EXPECT_EQ(cache.TotalUsedBytes(), total_used_bytes);

  PerCPUMetadataState post_stats = cache.MetadataMemoryUsage();
  // Confirm stats are within expected bounds.
  EXPECT_GT(post_stats.resident_size, 0);
  EXPECT_LE(post_stats.resident_size, upper_bound) << count_cores();
  // Confirm stats are unchanged.
  EXPECT_EQ(r.resident_size, post_stats.resident_size);

  // Tear down.
  cache.Deallocate(ptr, kSizeClass);
  cache.Deactivate();
}

TEST(CpuCacheTest, CacheMissStats) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  const int num_cpus = NumCPUs();

  CpuCache cache;
  cache.Activate();

  //  The number of underflows and overflows must be zero for all the caches.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    CpuCache::CpuCacheMissStats total_misses =
        cache.GetTotalCacheMissStats(cpu);
    CpuCache::CpuCacheMissStats shuffle_misses =
        cache.GetIntervalCacheMissStats(cpu, MissCount::kShuffle);
    EXPECT_EQ(total_misses.underflows, 0);
    EXPECT_EQ(total_misses.overflows, 0);
    EXPECT_EQ(shuffle_misses.underflows, 0);
    EXPECT_EQ(shuffle_misses.overflows, 0);
  }

  int allowed_cpu_id;
  const size_t kSizeClass = 2;
  const size_t virtual_cpu_id_offset = subtle::percpu::UsingFlatVirtualCpus()
                                           ? offsetof(kernel_rseq, vcpu_id)
                                           : offsetof(kernel_rseq, cpu_id);
  void* ptr;
  {
    // Restrict this thread to a single core while allocating and processing the
    // slow path.
    //
    // TODO(b/151313823):  Without this restriction, we may access--for reading
    // only--other slabs if we end up being migrated.  These may cause huge
    // pages to be faulted for those cores, leading to test flakiness.
    tcmalloc_internal::ScopedAffinityMask mask(
        tcmalloc_internal::AllowedCpus()[0]);
    allowed_cpu_id =
        subtle::percpu::GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset);

    ptr = cache.Allocate(kSizeClass);

    if (mask.Tampered() ||
        allowed_cpu_id !=
            subtle::percpu::GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset)) {
      return;
    }
  }

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    CpuCache::CpuCacheMissStats total_misses =
        cache.GetTotalCacheMissStats(cpu);
    CpuCache::CpuCacheMissStats shuffle_misses =
        cache.GetIntervalCacheMissStats(cpu, MissCount::kShuffle);
    if (cpu == allowed_cpu_id) {
      EXPECT_EQ(total_misses.underflows, 1);
      EXPECT_EQ(shuffle_misses.underflows, 1);
    } else {
      EXPECT_EQ(total_misses.underflows, 0);
      EXPECT_EQ(shuffle_misses.underflows, 0);
    }
    EXPECT_EQ(total_misses.overflows, 0);
    EXPECT_EQ(shuffle_misses.overflows, 0);
  }

  // Tear down.
  cache.Deallocate(ptr, kSizeClass);
  cache.Deactivate();
}

static void ResizeSizeClasses(CpuCache& cache, const std::atomic<bool>& stop) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  // Wake up every 10ms to resize size classes. Let miss stats acummulate over
  // those 10ms.
  while (!stop.load(std::memory_order_acquire)) {
    cache.ResizeSizeClasses();
    absl::SleepFor(absl::Milliseconds(10));
  }
}

static void ShuffleThread(CpuCache& cache, const std::atomic<bool>& stop) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  // Wake up every 10ms to shuffle the caches so that we can allow misses to
  // accumulate during that interval
  while (!stop.load(std::memory_order_acquire)) {
    cache.ShuffleCpuCaches();
    absl::SleepFor(absl::Milliseconds(10));
  }
}

static void StressThread(CpuCache& cache, size_t thread_id,
                         const std::atomic<bool>& stop) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  std::vector<std::pair<size_t, void*>> blocks;
  absl::InsecureBitGen rnd;
  while (!stop.load(std::memory_order_acquire)) {
    const int what = absl::Uniform<int32_t>(rnd, 0, 2);
    if (what) {
      // Allocate an object for a class
      size_t size_class = absl::Uniform<int32_t>(rnd, 1, 3);
      void* ptr = cache.Allocate(size_class);
      blocks.emplace_back(std::make_pair(size_class, ptr));
    } else {
      // Deallocate an object for a class
      if (!blocks.empty()) {
        cache.Deallocate(blocks.back().second, blocks.back().first);
        blocks.pop_back();
      }
    }
  }

  // Cleaup. Deallocate rest of the allocated memory.
  for (int i = 0; i < blocks.size(); i++) {
    cache.Deallocate(blocks[i].second, blocks[i].first);
  }
}

TEST(CpuCacheTest, StressSizeClassResize) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CpuCache cache;
  cache.Activate();

  std::vector<std::thread> threads;
  std::thread resize_thread;
  const int n_threads = NumCPUs();
  std::atomic<bool> stop(false);

  for (size_t t = 0; t < n_threads; ++t) {
    threads.push_back(
        std::thread(StressThread, std::ref(cache), t, std::ref(stop)));
  }
  resize_thread =
      std::thread(ResizeSizeClasses, std::ref(cache), std::ref(stop));

  absl::SleepFor(absl::Seconds(5));
  stop = true;
  for (auto& t : threads) {
    t.join();
  }
  resize_thread.join();

  // Check that the total capacity is preserved after the stress test.
  size_t capacity = 0;
  const int num_cpus = NumCPUs();
  const size_t kTotalCapacity = num_cpus * Parameters::max_per_cpu_cache_size();
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    EXPECT_EQ(cache.Allocated(cpu) + cache.Unallocated(cpu),
              cache.Capacity(cpu));
    capacity += cache.Capacity(cpu);
  }
  EXPECT_EQ(capacity, kTotalCapacity);

  cache.Deactivate();
}

TEST(CpuCacheTest, StealCpuCache) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CpuCache cache;
  cache.Activate();

  std::vector<std::thread> threads;
  std::thread shuffle_thread;
  const int n_threads = NumCPUs();
  std::atomic<bool> stop(false);

  for (size_t t = 0; t < n_threads; ++t) {
    threads.push_back(
        std::thread(StressThread, std::ref(cache), t, std::ref(stop)));
  }
  shuffle_thread = std::thread(ShuffleThread, std::ref(cache), std::ref(stop));

  absl::SleepFor(absl::Seconds(5));
  stop = true;
  for (auto& t : threads) {
    t.join();
  }
  shuffle_thread.join();

  // Check that the total capacity is preserved after the shuffle.
  size_t capacity = 0;
  const int num_cpus = NumCPUs();
  const size_t kTotalCapacity = num_cpus * Parameters::max_per_cpu_cache_size();
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    EXPECT_EQ(cache.Allocated(cpu) + cache.Unallocated(cpu),
              cache.Capacity(cpu));
    capacity += cache.Capacity(cpu);
  }
  EXPECT_EQ(capacity, kTotalCapacity);

  cache.Deactivate();
}

// Test that when dynamic slab is enabled, nothing goes horribly wrong and that
// arena non-resident bytes increases as expected.
TEST(CpuCacheTest, DynamicSlab) {
  if (!subtle::percpu::IsFast()) {
    return;
  }
  CpuCache cache;
  TestStaticForwarder& forwarder = cache.forwarder();

  size_t prev_reported_nonresident_bytes =
      forwarder.arena_reported_nonresident_bytes_;
  EXPECT_EQ(forwarder.arena_reported_impending_bytes_, 0);
  size_t prev_shrink_to_usage_limit_calls =
      forwarder.shrink_to_usage_limit_calls_;
  forwarder.dynamic_slab_enabled_ = true;
  forwarder.dynamic_slab_ = DynamicSlab::kNoop;

  cache.Activate();

  std::vector<std::thread> threads;
  const int n_threads = NumCPUs();
  std::atomic<bool> stop(false);

  for (size_t t = 0; t < n_threads; ++t) {
    threads.push_back(
        std::thread(StressThread, std::ref(cache), t, std::ref(stop)));
  }

  cpu_cache_internal::SlabShiftBounds shift_bounds =
      cache.GetPerCpuSlabShiftBounds();
  int shift = shift_bounds.initial_shift;

  const auto repeat_dynamic_slab_ops = [&](DynamicSlab op, int shift_update,
                                           int end_shift) {
    const DynamicSlab ops[2] = {DynamicSlab::kNoop, op};
    int iters = end_shift > shift ? end_shift - shift : shift - end_shift;
    iters += 2;  // Test that we don't resize past end_shift.
    for (int i = 0; i < iters; ++i) {
      for (DynamicSlab dynamic_slab : ops) {
        EXPECT_EQ(shift, CpuCachePeer::GetSlabShift(cache));
        absl::SleepFor(absl::Milliseconds(100));
        forwarder.dynamic_slab_ = dynamic_slab;
        // If there were no misses in the current resize interval, then we may
        // not resize so we ensure non-zero misses.
        CpuCachePeer::IncrementCacheMisses(cache);
        cache.ResizeSlabIfNeeded();
        if (dynamic_slab != DynamicSlab::kNoop && shift != end_shift) {
          EXPECT_LT(prev_reported_nonresident_bytes,
                    forwarder.arena_reported_nonresident_bytes_);
          EXPECT_EQ(forwarder.shrink_to_usage_limit_calls_,
                    1 + prev_shrink_to_usage_limit_calls);
          shift += shift_update;
        } else {
          EXPECT_EQ(prev_reported_nonresident_bytes,
                    forwarder.arena_reported_nonresident_bytes_);
        }
        prev_reported_nonresident_bytes =
            forwarder.arena_reported_nonresident_bytes_;

        EXPECT_EQ(forwarder.arena_reported_impending_bytes_, 0);
        prev_shrink_to_usage_limit_calls =
            forwarder.shrink_to_usage_limit_calls_;
      }
    }
  };

  // First grow the slab to max size, then shrink it to min size.
  repeat_dynamic_slab_ops(DynamicSlab::kGrow, /*shift_update=*/1,
                          shift_bounds.max_shift);
  repeat_dynamic_slab_ops(DynamicSlab::kShrink, /*shift_update=*/-1,
                          shift_bounds.initial_shift);

  stop = true;
  for (auto& t : threads) {
    t.join();
  }

  cache.Deactivate();
}

void AllocateThenDeallocate(CpuCache& cache, int cpu, size_t size_class,
                            int ops) {
  std::vector<void*> objects;
  ScopedFakeCpuId fake_cpu_id(cpu);
  for (int i = 0; i < ops; ++i) {
    void* ptr = cache.Allocate(size_class);
    objects.push_back(ptr);
  }
  for (auto* ptr : objects) {
    cache.Deallocate(ptr, size_class);
  }
  objects.clear();
}

// In this test, we check if we can resize size classes based on the number of
// misses they encounter. First, we exhaust cache capacity by filling up
// larger size class as much as possible. Then, we try to allocate objects for
// the smaller size class. This should result in misses as we do not resize its
// capacity in the foreground when the feature is enabled. We confirm that it
// indeed encounters a capacity miss. When then resize size classes and allocate
// small size class objects again. We should be able to utilize an increased
// capacity for the size class to allocate and deallocate these objects. We also
// confirm that we do not lose the overall cpu cache capacity when we resize
// size class capacities.
TEST(CpuCacheTest, ResizeSizeClassesTest) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CpuCache cache;
  // Reduce cache capacity so that it will see need in stealing and rebalancing.
  const size_t max_cpu_cache_size = 128 << 10;
  cache.SetCacheLimit(max_cpu_cache_size);
  cache.Activate();

  // Temporarily fake being on the given CPU.
  constexpr int kCpuId = 0;
  constexpr int kCpuId1 = 1;

  constexpr int kSmallClass = 1;
  constexpr int kLargeClass = 2;
  const int kMaxCapacity = cache.forwarder().max_capacity(kLargeClass);

  const size_t large_class_size = cache.forwarder().class_to_size(kLargeClass);
  ASSERT_GT(large_class_size * kMaxCapacity, max_cpu_cache_size);

  const size_t batch_size_small =
      cache.forwarder().num_objects_to_move(kSmallClass);
  const size_t batch_size_large =
      cache.forwarder().num_objects_to_move(kLargeClass);

  size_t ops = 0;
  while (true) {
    // We allocate and deallocate additional batch_size number of objects each
    // time so that cpu cache suffers successive underflow and overflow, and it
    // can grow.
    ops += batch_size_large;
    if (ops > kMaxCapacity || cache.Allocated(kCpuId) == max_cpu_cache_size)
      break;

    AllocateThenDeallocate(cache, kCpuId, kLargeClass, ops);
  }

  EXPECT_EQ(cache.Unallocated(kCpuId), 0);
  EXPECT_EQ(cache.Allocated(kCpuId), max_cpu_cache_size);
  EXPECT_EQ(cache.TotalObjectsOfClass(kSmallClass), 0);

  size_t interval_misses = cache.GetIntervalSizeClassMisses(
      kCpuId, kSmallClass, PerClassMissType::kResize);
  EXPECT_EQ(interval_misses, 0);

  AllocateThenDeallocate(cache, kCpuId, kSmallClass, batch_size_small);

  interval_misses = cache.GetIntervalSizeClassMisses(kCpuId, kSmallClass,
                                                     PerClassMissType::kResize);
  EXPECT_EQ(interval_misses, 2 * batch_size_small);

  EXPECT_EQ(cache.Unallocated(kCpuId), 0);
  EXPECT_EQ(cache.Allocated(kCpuId), max_cpu_cache_size);
  EXPECT_EQ(cache.TotalObjectsOfClass(kSmallClass), 0);

  const int num_resizes = NumCPUs() / CpuCache::kNumCpuCachesToResize;
  {
    ScopedFakeCpuId fake_cpu_id_1(kCpuId1);
    for (int i = 0; i < num_resizes; ++i) {
      cache.ResizeSizeClasses();
    }
  }

  // Since we just resized size classes, we started a new interval. So, miss
  // this interval should be zero.
  interval_misses = cache.GetIntervalSizeClassMisses(kCpuId, kSmallClass,
                                                     PerClassMissType::kResize);
  EXPECT_EQ(interval_misses, 0);

  AllocateThenDeallocate(cache, kCpuId, kSmallClass, batch_size_small);
  interval_misses = cache.GetIntervalSizeClassMisses(kCpuId, kSmallClass,
                                                     PerClassMissType::kResize);
  EXPECT_EQ(interval_misses, 0);

  EXPECT_EQ(cache.Unallocated(kCpuId), 0);
  EXPECT_EQ(cache.Allocated(kCpuId), max_cpu_cache_size);
  EXPECT_EQ(cache.TotalObjectsOfClass(kSmallClass), batch_size_small);

  // Reclaim caches.
  cache.Deactivate();
}

// Runs a single allocate and deallocate operation to warm up the cache. Once a
// few objects are allocated in the cold cache, we can shuffle cpu caches to
// steal that capacity from the cold cache to the hot cache.
static void ColdCacheOperations(CpuCache& cache, int cpu_id,
                                size_t size_class) {
  // Temporarily fake being on the given CPU.
  ScopedFakeCpuId fake_cpu_id(cpu_id);
  void* ptr = cache.Allocate(size_class);
  cache.Deallocate(ptr, size_class);
}

// Runs multiple allocate and deallocate operation on the cpu cache to collect
// misses. Once we collect enough misses on this cache, we can shuffle cpu
// caches to steal capacity from colder caches to the hot cache.
static void HotCacheOperations(CpuCache& cache, int cpu_id) {
  constexpr size_t kPtrs = 4096;
  std::vector<void*> ptrs;
  ptrs.resize(kPtrs);

  // Temporarily fake being on the given CPU.
  ScopedFakeCpuId fake_cpu_id(cpu_id);

  // Allocate and deallocate objects to make sure we have enough misses on the
  // cache. This will make sure we have sufficient disparity in misses between
  // the hotter and colder cache, and that we may be able to steal bytes from
  // the colder cache.
  for (size_t size_class = 1; size_class <= 2; ++size_class) {
    for (auto& ptr : ptrs) {
      ptr = cache.Allocate(size_class);
    }
    for (void* ptr : ptrs) {
      cache.Deallocate(ptr, size_class);
    }
  }

  // We reclaim the cache to reset it so that we record underflows/overflows the
  // next time we allocate and deallocate objects. Without reclaim, the cache
  // would stay warmed up and it would take more time to drain the colder cache.
  cache.Reclaim(cpu_id);
}

class DynamicWideSlabTest
    : public testing::TestWithParam<
          std::tuple<bool /* use_wider_slab */,
                     bool /* configure_size_class_max_capacity */>> {
 public:
  bool use_wider_slab() { return std::get<0>(GetParam()); }
  bool configure_size_class_max_capacity() { return std::get<1>(GetParam()); }
};

INSTANTIATE_TEST_SUITE_P(TestDynamicWideSlab, DynamicWideSlabTest,
                         testing::Combine(testing::Bool(), testing::Bool()));

// Test that we are complying with the threshold when we grow the slab.
// When wider slab is enabled, we check if overflow/underflow ratio is above the
// threshold for individual cpu caches.
TEST_P(DynamicWideSlabTest, DynamicSlabThreshold) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  constexpr double kDynamicSlabGrowThreshold = 0.9;
  CpuCache cache;
  TestStaticForwarder& forwarder = cache.forwarder();
  forwarder.dynamic_slab_enabled_ = true;
  forwarder.dynamic_slab_grow_threshold_ = kDynamicSlabGrowThreshold;
  forwarder.wider_slabs_enabled_ = use_wider_slab();
  SizeMap size_map;
  size_map.Init(kSizeClasses.classes);
  forwarder.size_map_ = size_map;

  cache.Activate();

  constexpr int kCpuId0 = 0;
  constexpr int kCpuId1 = 1;

  // Accumulate overflows and underflows for kCpuId0.
  HotCacheOperations(cache, kCpuId0);
  CpuCache::CpuCacheMissStats interval_misses =
      cache.GetIntervalCacheMissStats(kCpuId0, MissCount::kSlabResize);
  // Make sure that overflows/underflows ratio is greater than the threshold
  // for kCpuId0 cache.
  ASSERT_GT(interval_misses.overflows,
            interval_misses.underflows * kDynamicSlabGrowThreshold);

  // Perform allocations on kCpuId1 so that we accumulate only underflows.
  // Reclaim after each allocation such that we have no objects in the cache
  // for the next allocation.
  for (int i = 0; i < 1024; ++i) {
    ColdCacheOperations(cache, kCpuId1, /*size_class=*/1);
    cache.Reclaim(kCpuId1);
  }

  // Total overflows/underflows ratio must be less than grow threshold now.
  CpuCache::CpuCacheMissStats total_misses =
      cache.GetIntervalCacheMissStats(kCpuId0, MissCount::kSlabResize);
  total_misses +=
      cache.GetIntervalCacheMissStats(kCpuId1, MissCount::kSlabResize);
  ASSERT_LT(total_misses.overflows,
            total_misses.underflows * kDynamicSlabGrowThreshold);

  cpu_cache_internal::SlabShiftBounds shift_bounds =
      cache.GetPerCpuSlabShiftBounds();
  const int shift = shift_bounds.initial_shift;
  EXPECT_EQ(CpuCachePeer::GetSlabShift(cache), shift);
  cache.ResizeSlabIfNeeded();

  // If wider slabs is enabled, we must use overflows and underflows of
  // individual cpu caches to decide whether to grow the slab. Hence, the
  // slab should have grown. If wider slabs is disabled, slab shift should
  // stay the same as total miss ratio is lower than
  // kDynamicSlabGrowThreshold.
  if (use_wider_slab()) {
    EXPECT_EQ(CpuCachePeer::GetSlabShift(cache), shift + 1);
  } else {
    EXPECT_EQ(CpuCachePeer::GetSlabShift(cache), shift);
  }
}

// Test that when dynamic slab parameters change, things still work.
TEST_P(DynamicWideSlabTest, DynamicSlabParamsChange) {
  if (!subtle::percpu::IsFast()) {
    return;
  }
  int n_threads = NumCPUs();
#ifdef UNDEFINED_BEHAVIOR_SANITIZER
  // Prevent timeout issues by using fewer stress threads with UBSan.
  n_threads = std::min(n_threads, 2);
#endif

  SizeMap size_map;
  size_map.Init(kSizeClasses.classes);
  for (bool initially_enabled : {false, true}) {
    for (DynamicSlab initial_dynamic_slab :
         {DynamicSlab::kGrow, DynamicSlab::kShrink, DynamicSlab::kNoop}) {
      CpuCache cache;
      TestStaticForwarder& forwarder = cache.forwarder();
      forwarder.dynamic_slab_enabled_ = initially_enabled;
      forwarder.dynamic_slab_ = initial_dynamic_slab;
      forwarder.wider_slabs_enabled_ = use_wider_slab();
      forwarder.size_map_ = size_map;

      cache.Activate();

      std::vector<std::thread> threads;
      std::atomic<bool> stop(false);

      for (size_t t = 0; t < n_threads; ++t) {
        threads.push_back(
            std::thread(StressThread, std::ref(cache), t, std::ref(stop)));
      }

      for (bool enabled : {false, true}) {
        for (DynamicSlab dynamic_slab :
             {DynamicSlab::kGrow, DynamicSlab::kShrink, DynamicSlab::kNoop}) {
          absl::SleepFor(absl::Milliseconds(100));
          forwarder.dynamic_slab_enabled_ = enabled;
          forwarder.dynamic_slab_ = dynamic_slab;
          cache.ResizeSlabIfNeeded();
        }
      }
      stop = true;
      for (auto& t : threads) {
        t.join();
      }

      cache.Deactivate();
    }
  }
}

TEST(CpuCacheTest, SlabUsage) {
  // Note: we can't do ValidateSlabBytes on the test-cpu-cache because in that
  // case, the slab only uses size classes 1 and 2.
  CpuCachePeer::ValidateSlabBytes(tc_globals.cpu_cache());
}

TEST(CpuCacheTest, ColdHotCacheShuffleTest) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CpuCache cache;
  // Reduce cache capacity so that it will see need in stealing and rebalancing.
  const size_t max_cpu_cache_size = 1 << 10;
  cache.SetCacheLimit(max_cpu_cache_size);
  cache.Activate();

  constexpr int hot_cpu_id = 0;
  constexpr int cold_cpu_id = 1;

  // Empirical tests suggest that we should be able to steal all the steal-able
  // capacity from colder cache in < 100 tries. Keeping enough buffer here to
  // make sure we steal from colder cache, while at the same time avoid timeouts
  // if something goes bad.
  constexpr int kMaxStealTries = 1000;

  // We allocate and deallocate a single highest size_class object.
  // This makes sure that we have a single large object in the cache that faster
  // cache can steal.
  const size_t size_class = 2;

  for (int num_tries = 0;
       num_tries < kMaxStealTries &&
       cache.Capacity(cold_cpu_id) >
           CpuCache::kCacheCapacityThreshold * max_cpu_cache_size;
       ++num_tries) {
    ColdCacheOperations(cache, cold_cpu_id, size_class);
    HotCacheOperations(cache, hot_cpu_id);
    cache.ShuffleCpuCaches();

    // Check that the capacity is preserved.
    EXPECT_EQ(cache.Allocated(cold_cpu_id) + cache.Unallocated(cold_cpu_id),
              cache.Capacity(cold_cpu_id));
    EXPECT_EQ(cache.Allocated(hot_cpu_id) + cache.Unallocated(hot_cpu_id),
              cache.Capacity(hot_cpu_id));
  }

  size_t cold_cache_capacity = cache.Capacity(cold_cpu_id);
  size_t hot_cache_capacity = cache.Capacity(hot_cpu_id);

  // Check that we drained cold cache to the lower capacity limit.
  // We also keep some tolerance, up to the largest class size, below the lower
  // capacity threshold that we can drain cold cache to.
  EXPECT_LT(cold_cache_capacity, max_cpu_cache_size);
  EXPECT_GT(cold_cache_capacity,
            CpuCache::kCacheCapacityThreshold * max_cpu_cache_size -
                cache.forwarder().class_to_size(size_class));

  // Check that we have at least stolen some capacity.
  EXPECT_GT(hot_cache_capacity, max_cpu_cache_size);

  // Perform a few more shuffles to make sure that lower cache capacity limit
  // has been reached for the cold cache. A few more shuffles should not
  // change the capacity of either of the caches.
  for (int i = 0; i < 100; ++i) {
    ColdCacheOperations(cache, cold_cpu_id, size_class);
    HotCacheOperations(cache, hot_cpu_id);
    cache.ShuffleCpuCaches();

    // Check that the capacity is preserved.
    EXPECT_EQ(cache.Allocated(cold_cpu_id) + cache.Unallocated(cold_cpu_id),
              cache.Capacity(cold_cpu_id));
    EXPECT_EQ(cache.Allocated(hot_cpu_id) + cache.Unallocated(hot_cpu_id),
              cache.Capacity(hot_cpu_id));
  }

  // Check that the capacity of cold and hot caches is same as before.
  EXPECT_EQ(cache.Capacity(cold_cpu_id), cold_cache_capacity)
      << CpuCache::kCacheCapacityThreshold * max_cpu_cache_size;
  EXPECT_EQ(cache.Capacity(hot_cpu_id), hot_cache_capacity);

  // Make sure that the total capacity is preserved.
  EXPECT_EQ(cache.Capacity(cold_cpu_id) + cache.Capacity(hot_cpu_id),
            2 * max_cpu_cache_size);

  // Reclaim caches.
  cache.Deactivate();
}

TEST(CpuCacheTest, ReclaimCpuCache) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CpuCache cache;
  cache.Activate();

  //  The number of underflows and overflows must be zero for all the caches.
  const int num_cpus = NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    // Check that reclaim miss metrics are reset.
    CpuCache::CpuCacheMissStats reclaim_misses =
        cache.GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
    EXPECT_EQ(reclaim_misses.underflows, 0);
    EXPECT_EQ(reclaim_misses.overflows, 0);

    // None of the caches should have been reclaimed yet.
    EXPECT_EQ(cache.GetNumReclaims(cpu), 0);

    // Check that caches are empty.
    uint64_t used_bytes = cache.UsedBytes(cpu);
    EXPECT_EQ(used_bytes, 0);
  }

  const size_t kSizeClass = 2;

  // We chose a different size class here so that we can populate different size
  // class slots and change the number of bytes used by the busy cache later in
  // our test.
  const size_t kBusySizeClass = 1;
  ASSERT_NE(kSizeClass, kBusySizeClass);

  // Perform some operations to warm up caches and make sure they are populated.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    ColdCacheOperations(cache, cpu, kSizeClass);
    EXPECT_TRUE(cache.HasPopulated(cpu));
  }

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    CpuCache::CpuCacheMissStats misses_last_interval =
        cache.GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
    CpuCache::CpuCacheMissStats total_misses =
        cache.GetTotalCacheMissStats(cpu);

    // Misses since the last reclaim (i.e. since we initialized the caches)
    // should match the total miss metrics.
    EXPECT_EQ(misses_last_interval.underflows, total_misses.underflows);
    EXPECT_EQ(misses_last_interval.overflows, total_misses.overflows);

    // Caches should have non-zero used bytes.
    EXPECT_GT(cache.UsedBytes(cpu), 0);
  }

  cache.TryReclaimingCaches();

  // Miss metrics since the last interval were non-zero and the change in used
  // bytes was non-zero, so none of the caches should get reclaimed.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    // As no cache operations were performed since the last reclaim
    // operation, the reclaim misses captured during the last interval (i.e.
    // since the last reclaim) should be zero.
    CpuCache::CpuCacheMissStats reclaim_misses =
        cache.GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
    EXPECT_EQ(reclaim_misses.underflows, 0);
    EXPECT_EQ(reclaim_misses.overflows, 0);

    // None of the caches should have been reclaimed as the caches were
    // accessed in the previous interval.
    EXPECT_EQ(cache.GetNumReclaims(cpu), 0);

    // Caches should not have been reclaimed; used bytes should be non-zero.
    EXPECT_GT(cache.UsedBytes(cpu), 0);
  }

  absl::BitGen rnd;
  const int busy_cpu = absl::Uniform<int32_t>(rnd, 0, NumCPUs());
  const size_t prev_used = cache.UsedBytes(busy_cpu);
  ColdCacheOperations(cache, busy_cpu, kBusySizeClass);
  EXPECT_GT(cache.UsedBytes(busy_cpu), prev_used);

  // Try reclaiming caches again.
  cache.TryReclaimingCaches();

  // All caches, except the busy cpu cache against which we performed some
  // operations in the previous interval, should have been reclaimed exactly
  // once.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    if (cpu == busy_cpu) {
      EXPECT_GT(cache.UsedBytes(cpu), 0);
      EXPECT_EQ(cache.GetNumReclaims(cpu), 0);
    } else {
      EXPECT_EQ(cache.UsedBytes(cpu), 0);
      EXPECT_EQ(cache.GetNumReclaims(cpu), 1);
    }
  }

  // Try reclaiming caches again.
  cache.TryReclaimingCaches();

  // All caches, including the busy cache, should have been reclaimed this
  // time. Note that the caches that were reclaimed in the previous interval
  // should not be reclaimed again and the number of reclaims reported for them
  // should still be one.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    EXPECT_EQ(cache.UsedBytes(cpu), 0);
    EXPECT_EQ(cache.GetNumReclaims(cpu), 1);
  }

  cache.Deactivate();
}

TEST(CpuCacheTest, SizeClassCapacityTest) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  CpuCache cache;
  cache.Activate();

  const int num_cpus = NumCPUs();
  constexpr size_t kSizeClass = 2;
  const size_t batch_size = cache.forwarder().num_objects_to_move(kSizeClass);

  // Perform some operations to warm up caches and make sure they are populated.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    ColdCacheOperations(cache, cpu, kSizeClass);
    EXPECT_TRUE(cache.HasPopulated(cpu));
  }

  for (int size_class = 1; size_class < kNumClasses; ++size_class) {
    SCOPED_TRACE(absl::StrFormat("Failed size_class: %d", size_class));
    CpuCache::SizeClassCapacityStats capacity_stats =
        cache.GetSizeClassCapacityStats(size_class);
    if (size_class == kSizeClass) {
      // As all the caches are populated and each cache stores batch_size number
      // of kSizeClass objects, all the stats below should be equal to
      // batch_size.
      EXPECT_EQ(capacity_stats.min_capacity, batch_size);
      EXPECT_DOUBLE_EQ(capacity_stats.avg_capacity, batch_size);
      EXPECT_EQ(capacity_stats.max_capacity, batch_size);
    } else {
      // Capacity stats for other size classes should be zero.
      EXPECT_EQ(capacity_stats.min_capacity, 0);
      EXPECT_DOUBLE_EQ(capacity_stats.avg_capacity, 0);
      EXPECT_EQ(capacity_stats.max_capacity, 0);
    }
  }

  // Next, we reclaim per-cpu caches, one at a time, to drain all the kSizeClass
  // objects cached by them. As we progressively reclaim per-cpu caches, the
  // capacity for kSizeClass averaged over all CPUs should also drop linearly.
  // We reclaim all but one per-cpu caches (we reclaim last per-cpu cache
  // outside the loop so that we can check for max_capacity=0 separately).
  for (int cpu = 0; cpu < num_cpus - 1; ++cpu) {
    SCOPED_TRACE(absl::StrFormat("Failed CPU: %d", cpu));
    cache.Reclaim(cpu);

    CpuCache::SizeClassCapacityStats capacity_stats =
        cache.GetSizeClassCapacityStats(kSizeClass);
    // Reclaiming even one per-cpu cache should set min_capacity to zero.
    EXPECT_EQ(capacity_stats.min_capacity, 0);

    // (cpu+1) number of caches have been reclaimed. So, (num_cpus-cpu-1) number
    // of caches are currently populated, with each cache storing batch_size
    // number of kSizeClass objects.
    double expected_avg =
        static_cast<double>(batch_size * (num_cpus - cpu - 1)) / num_cpus;
    EXPECT_DOUBLE_EQ(capacity_stats.avg_capacity, expected_avg);

    // At least one per-cpu cache exists that caches batch_size number of
    // kSizeClass objects.
    EXPECT_EQ(capacity_stats.max_capacity, batch_size);
  }

  // We finally reclaim last per-cpu cache. All the reported capacity stats
  // should drop to zero as none of the caches hold any objects.
  cache.Reclaim(num_cpus - 1);
  CpuCache::SizeClassCapacityStats capacity_stats =
      cache.GetSizeClassCapacityStats(kSizeClass);
  EXPECT_EQ(capacity_stats.min_capacity, 0);
  EXPECT_DOUBLE_EQ(capacity_stats.avg_capacity, 0);
  EXPECT_EQ(capacity_stats.max_capacity, 0);

  cache.Deactivate();
}

class CpuCacheEnvironment {
 public:
  CpuCacheEnvironment() : num_cpus_(NumCPUs()) {}
  ~CpuCacheEnvironment() { cache_.Deactivate(); }

  void Activate() {
    cache_.Activate();
    ready_.store(true, std::memory_order_release);
  }

  void RandomlyPoke(absl::BitGenRef rng) {
    // We run a random operation based on our random number generated.
    const int coin = absl::Uniform(rng, 0, 18);
    const bool ready = ready_.load(std::memory_order_acquire);

    // Pick a random CPU and size class.  We will likely need one or both.
    const int cpu = absl::Uniform(rng, 0, num_cpus_);
    const int size_class = absl::Uniform(rng, 1, 3);

    if (!ready || coin < 1) {
      benchmark::DoNotOptimize(cache_.CacheLimit());
      return;
    }

    // Methods beyond this point require the CpuCache to be activated.

    switch (coin) {
      case 1: {
        // Allocate, Deallocate
        void* ptr = cache_.Allocate(size_class);
        EXPECT_NE(ptr, nullptr);
        // Touch *ptr to allow sanitizers to see an access (and a potential
        // race, if synchronization is insufficient).
        *static_cast<char*>(ptr) = 1;
        benchmark::DoNotOptimize(*static_cast<char*>(ptr));

        cache_.Deallocate(ptr, size_class);
        break;
      }
      case 2:
        benchmark::DoNotOptimize(cache_.TotalUsedBytes());
        break;
      case 3:
        benchmark::DoNotOptimize(cache_.UsedBytes(cpu));
        break;
      case 4:
        benchmark::DoNotOptimize(cache_.Allocated(cpu));
        break;
      case 5:
        benchmark::DoNotOptimize(cache_.HasPopulated(cpu));
        break;
      case 6: {
        auto metadata = cache_.MetadataMemoryUsage();
        EXPECT_GE(metadata.virtual_size, metadata.resident_size);
        EXPECT_GT(metadata.virtual_size, 0);
        break;
      }
      case 7:
        benchmark::DoNotOptimize(cache_.TotalObjectsOfClass(size_class));
        break;
      case 8:
        benchmark::DoNotOptimize(cache_.Unallocated(cpu));
        break;
      case 9:
        benchmark::DoNotOptimize(cache_.Capacity(cpu));
        break;
      case 10: {
        absl::MutexLock lock(&background_mutex_);
        cache_.ShuffleCpuCaches();
        break;
      }
      case 11: {
        absl::MutexLock lock(&background_mutex_);
        cache_.TryReclaimingCaches();
        break;
      }
      case 12: {
        absl::MutexLock lock(&background_mutex_);
        cache_.Reclaim(cpu);
        break;
      }
      case 13:
        benchmark::DoNotOptimize(cache_.GetNumReclaims(cpu));
        break;
      case 14: {
        const auto total_misses = cache_.GetTotalCacheMissStats(cpu);
        const auto reclaim_misses =
            cache_.GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
        const auto shuffle_misses =
            cache_.GetIntervalCacheMissStats(cpu, MissCount::kShuffle);

        benchmark::DoNotOptimize(total_misses);
        benchmark::DoNotOptimize(reclaim_misses);
        benchmark::DoNotOptimize(shuffle_misses);
        break;
      }
      case 15: {
        const auto stats = cache_.GetSizeClassCapacityStats(size_class);
        EXPECT_GE(stats.max_capacity, stats.avg_capacity);
        EXPECT_GE(stats.avg_capacity, stats.min_capacity);
        break;
      }
      case 16: {
        std::string out;
        out.resize(128 << 10);
        ANNOTATE_MEMORY_IS_UNINITIALIZED(out.data(), out.size());
        Printer p(out.data(), out.size());
        PbtxtRegion r(&p, kTop);

        cache_.PrintInPbtxt(&r);

        benchmark::DoNotOptimize(out.data());
        break;
      }
      case 17: {
        std::string out;
        out.resize(128 << 10);
        ANNOTATE_MEMORY_IS_UNINITIALIZED(out.data(), out.size());
        Printer p(out.data(), out.size());

        cache_.Print(&p);

        benchmark::DoNotOptimize(out.data());
        break;
      }
      default:
        GTEST_FAIL() << "Unexpected value " << coin;
        break;
    }
  }

  CpuCache& cache() { return cache_; }

  int num_cpus() const { return num_cpus_; }

 private:
  const int num_cpus_;
  CpuCache cache_;
  // Protects operations executed on the background thread in real life.
  absl::Mutex background_mutex_;
  std::atomic<bool> ready_{false};
};

TEST(CpuCacheTest, Fuzz) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  const int kThreads = 10;
  struct ABSL_CACHELINE_ALIGNED ThreadState {
    absl::BitGen rng;
  };
  std::vector<ThreadState> thread_state(kThreads);

  CpuCacheEnvironment env;
  ThreadManager threads;
  threads.Start(10, [&](int thread_id) {
    // Ensure this thread has registered itself with the kernel to use
    // restartable sequences.
    ASSERT_TRUE(subtle::percpu::IsFast());
    env.RandomlyPoke(thread_state[thread_id].rng);
  });

  absl::SleepFor(absl::Seconds(0.1));
  env.Activate();
  absl::SleepFor(absl::Seconds(0.3));

  threads.Stop();

  // Inspect the CpuCache and validate invariants.

  // The number of caches * per-core limit should be equivalent to the bytes
  // managed by the cache.
  size_t capacity = 0;
  size_t allocated = 0;
  size_t unallocated = 0;
  for (int i = 0, n = env.num_cpus(); i < n; i++) {
    capacity += env.cache().Capacity(i);
    allocated += env.cache().Allocated(i);
    unallocated += env.cache().Unallocated(i);
  }

  EXPECT_EQ(allocated + unallocated, capacity);
  EXPECT_EQ(env.num_cpus() * env.cache().CacheLimit(), capacity);

  // Log mallocz content for manual inspection.
  std::string mallocz;
  mallocz.resize(128 << 10);
  Printer p(mallocz.data(), mallocz.size());
  env.cache().Print(&p);
  std::cout << mallocz;
}

// TODO(b/179516472):  Enable this test.
TEST(CpuCacheTest, DISABLED_ChangingSizes) {
  if (!subtle::percpu::IsFast()) {
    return;
  }

  constexpr int kThreads = 10;
  struct ABSL_CACHELINE_ALIGNED ThreadState {
    absl::BitGen rng;
  };
  std::vector<ThreadState> thread_state(kThreads);

  CpuCacheEnvironment env;
  ThreadManager threads;
  const size_t initial_size = env.cache().CacheLimit();
  ASSERT_GT(initial_size, 0);
  bool rseq_active_for_size_changing_thread = false;
  int index = 0;
  size_t last_cache_size = initial_size;

  env.Activate();

  threads.Start(kThreads, [&](int thread_id) {
    // Ensure this thread has registered itself with the kernel to use
    // restartable sequences.
    if (thread_id > 0) {
      ASSERT_TRUE(subtle::percpu::IsFast());
      env.RandomlyPoke(thread_state[thread_id].rng);
      return;
    }

    // Alternative between having the thread register for rseq and not, to
    // ensure that we can call SetCacheLimit with either precondition.
    std::optional<ScopedUnregisterRseq> rseq;
    if (rseq_active_for_size_changing_thread) {
      ASSERT_TRUE(subtle::percpu::IsFast());
    } else {
      rseq.emplace();
    }
    rseq_active_for_size_changing_thread =
        !rseq_active_for_size_changing_thread;

    // Vary the cache size up and down.  Exclude 1. from the list so that we
    // will always expect to see a nontrivial change after the threads stop
    // work.
    constexpr double kConversions[] = {0.25, 0.5, 0.75, 1.25, 1.5};
    size_t new_cache_size = initial_size * kConversions[index];
    index = (index + 1) % 5;

    env.cache().SetCacheLimit(new_cache_size);
    last_cache_size = new_cache_size;
  });

  absl::SleepFor(absl::Seconds(0.5));

  threads.Stop();

  // Inspect the CpuCache and validate invariants.

  // The number of caches * per-core limit should be equivalent to the bytes
  // managed by the cache.
  size_t capacity = 0;
  size_t allocated = 0;
  size_t unallocated = 0;
  for (int i = 0, n = env.num_cpus(); i < n; i++) {
    capacity += env.cache().Capacity(i);
    allocated += env.cache().Allocated(i);
    unallocated += env.cache().Unallocated(i);
  }

  EXPECT_EQ(allocated + unallocated, capacity);
  EXPECT_EQ(env.num_cpus() * last_cache_size, capacity);
}

TEST(CpuCacheTest, TargetOverflowRefillCount) {
  auto F = cpu_cache_internal::TargetOverflowRefillCount;
  // Args are: capacity, batch_length, successive.
  EXPECT_EQ(F(0, 8, 0), 1);
  EXPECT_EQ(F(0, 8, 10), 1);
  EXPECT_EQ(F(1, 8, 0), 1);
  EXPECT_EQ(F(1, 8, 1), 1);
  EXPECT_EQ(F(1, 8, 2), 1);
  EXPECT_EQ(F(1, 8, 3), 2);
  EXPECT_EQ(F(1, 8, 4), 2);
  EXPECT_EQ(F(2, 8, 0), 2);
  EXPECT_EQ(F(3, 8, 0), 3);
  EXPECT_EQ(F(4, 8, 0), 3);
  EXPECT_EQ(F(5, 8, 0), 4);
  EXPECT_EQ(F(6, 8, 0), 4);
  EXPECT_EQ(F(7, 8, 0), 5);
  EXPECT_EQ(F(8, 8, 0), 5);
  EXPECT_EQ(F(9, 8, 0), 6);
  EXPECT_EQ(F(100, 8, 0), 8);
  EXPECT_EQ(F(23, 8, 1), 13);
  EXPECT_EQ(F(24, 8, 1), 13);
  EXPECT_EQ(F(100, 8, 1), 16);
  EXPECT_EQ(F(24, 8, 2), 13);
  EXPECT_EQ(F(32, 8, 2), 17);
  EXPECT_EQ(F(40, 8, 2), 21);
  EXPECT_EQ(F(100, 8, 2), 32);
  EXPECT_EQ(F(48, 8, 3), 25);
  EXPECT_EQ(F(56, 8, 3), 29);
  EXPECT_EQ(F(100, 8, 3), 51);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

// Copyright 2020 The TCMalloc Authors
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

#ifndef TCMALLOC_MOCK_TRANSFER_CACHE_H_
#define TCMALLOC_MOCK_TRANSFER_CACHE_H_

#include <stddef.h>

#include <algorithm>
#include <memory>
#include <new>
#include <random>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "tcmalloc/common.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/transfer_cache.h"
#include "tcmalloc/transfer_cache_internals.h"

namespace tcmalloc {
namespace tcmalloc_internal {

inline constexpr size_t kClassSize = 8;
inline constexpr size_t kNumToMove = 32;
inline constexpr int kSizeClass = 1;

// TransferCacheManager with basic stubs for everything.
//
// Useful for benchmarks where you want to unrelated expensive operations.
class FakeTransferCacheManager {
 public:
  constexpr static size_t class_to_size(int size_class) { return kClassSize; }
  constexpr static size_t num_objects_to_move(int size_class) {
    // TODO(b/170732338): test with multiple different num_objects_to_move
    return kNumToMove;
  }
  void* Alloc(size_t size, std::align_val_t alignment = kAlignment) {
    memory_.push_back(std::make_unique<AlignedPtr>(
        ::operator new(size, alignment), alignment));
    return memory_.back()->ptr;
  }

 private:
  struct AlignedPtr {
    AlignedPtr(void* ptr, std::align_val_t alignment)
        : ptr(ptr), alignment(alignment) {}
    ~AlignedPtr() { ::operator delete(ptr, alignment); }
    void* ptr;
    std::align_val_t alignment;
  };
  std::vector<std::unique_ptr<AlignedPtr>> memory_;
};

// A transfer cache manager which allocates memory from a fixed size arena. This
// is necessary to prevent running into deadlocks in some cases, e.g. when the
// `ShardedTransferCacheManager` calls `Alloc()` which in turns tries to
// allocate memory using the normal malloc machinery, it leads to a deadlock
// on the pageheap_lock.
class ArenaBasedFakeTransferCacheManager {
 public:
  ArenaBasedFakeTransferCacheManager() { bytes_.resize(kTotalSize); }
  constexpr static size_t class_to_size(int size_class) {
    // Chosen >= min size for the sharded transfer cache to kick in.
    if (size_class == kSizeClass) return 4096;
    return 0;
  }
  constexpr static size_t num_objects_to_move(int size_class) {
    if (size_class == kSizeClass) return kNumToMove;
    return 0;
  }
  void* Alloc(size_t size, std::align_val_t alignment = kAlignment) {
    size_t space = kTotalSize - used_;
    if (space < size) return nullptr;
    void* head = &bytes_[used_];
    void* aligned =
        std::align(static_cast<size_t>(alignment), size, head, space);
    if (aligned != nullptr) {
      // Increase by the allocated size plus the alignment offset.
      used_ += size + (kTotalSize - space);
      TC_CHECK_LE(used_, bytes_.capacity());
    }
    return aligned;
  }
  size_t used() const { return used_; }

  static void SetPartialLegacyTransferCache(bool value) {
    partial_legacy_transfer_cache_ = value;
  }

 private:
  static constexpr size_t kTotalSize = 10000000;
  // We're not changing the size of this vector during the life of this object,
  // to avoid running into deadlocks.
  std::vector<char> bytes_;
  size_t used_ = 0;
  static bool partial_legacy_transfer_cache_;
};

// A manager that may provide different configurations of sharded transfer
// cache.
class FakeShardedTransferCacheManager
    : public ArenaBasedFakeTransferCacheManager {
 public:
  static void Init() {}
  static bool UseGenericCache() { return enable_generic_cache_; }
  static void SetGenericCache(bool value) { enable_generic_cache_ = value; }
  static bool EnableCacheForLargeClassesOnly() {
    return enable_cache_for_large_classes_only_;
  }
  static void SetCacheForLargeClassesOnly(bool value) {
    enable_cache_for_large_classes_only_ = value;
  }

 private:
  static bool enable_generic_cache_;
  static bool enable_cache_for_large_classes_only_;
};

// Wires up a largely functional TransferCache + TransferCacheManager +
// MockCentralFreeList.
//
// By default, it fills allocations and responds sensibly.  Because it backs
// onto malloc/free, it will detect leaks and memory misuse when run in asan or
// tsan.
//
// Exposes the underlying mocks to allow for more whitebox tests.
//
// Drains the cache and verifies that no data was lost in the destructor.
template <typename TransferCacheT>
class FakeTransferCacheEnvironment {
 public:
  using TransferCache = TransferCacheT;
  using Manager = typename TransferCache::Manager;
  using FreeList = typename TransferCache::FreeList;

  static constexpr int kMaxObjectsToMove =
      ::tcmalloc::tcmalloc_internal::kMaxObjectsToMove;
  static constexpr int kBatchSize = Manager::num_objects_to_move(1);

  FakeTransferCacheEnvironment()
      : manager_(),
        cache_(&manager_, 1, /*use_all_buckets_for_few_object_spans=*/false) {
    Init();
  }

  ~FakeTransferCacheEnvironment() { Drain(); }

  bool Shrink() { return cache_.ShrinkCache(kSizeClass); }
  bool Grow() { return cache_.IncreaseCacheCapacity(kSizeClass); }

  void Insert(int n, int batch = kBatchSize) {
    std::vector<void*> bufs;
    while (n > 0) {
      int b = std::min(n, batch);
      bufs.resize(b);
      central_freelist().AllocateBatch(&bufs[0], b);
      cache_.InsertRange(kSizeClass, absl::MakeSpan(bufs));
      n -= b;
    }
  }

  void Remove(int n, int batch = kBatchSize) {
    std::vector<void*> bufs;
    while (n > 0) {
      int b = std::min(n, batch);
      bufs.resize(b);
      int removed = cache_.RemoveRange(kSizeClass, &bufs[0], b);
      // Ensure we make progress.
      ASSERT_GT(removed, 0);
      ASSERT_LE(removed, b);
      central_freelist().FreeBatch({&bufs[0], static_cast<size_t>(removed)});
      n -= removed;
    }
  }

  void TryPlunder() { cache_.TryPlunder(kSizeClass); }

  void Drain() { Remove(cache_.tc_length()); }

  void RandomlyPoke() {
    absl::BitGen gen;
    // We want a probabilistic steady state size:
    // - grow/shrink balance on average
    // - insert/remove balance on average
    double choice = absl::Uniform(gen, 0.0, 1.0);
    if (choice < 0.1) {
      Shrink();
    } else if (choice < 0.2) {
      Grow();
    } else if (choice < 0.3) {
      cache_.HasSpareCapacity(kSizeClass);
    } else if (choice < 0.4) {
      Insert(absl::Uniform(gen, 1, kBatchSize));
    } else if (choice < 0.5) {
      Remove(absl::Uniform(gen, 1, kBatchSize));
    } else if (choice < 0.7) {
      Insert(kBatchSize);
    } else if (choice < 0.9) {
      Remove(kBatchSize);
    } else {
      TryPlunder();
    }
  }

  TransferCache& transfer_cache() { return cache_; }

  FreeList& central_freelist() { return cache_.freelist(); }

 private:
  void Init(){};

  Manager manager_;
  TransferCache cache_;
};

// A fake transfer cache manager class which supports two size classes instead
// of just the one. To make this work, we have to store the transfer caches
// inside the cache manager, like in production code.
template <typename FreeListT,
          template <typename FreeList, typename Manager> class TransferCacheT>
class TwoSizeClassManager : public FakeTransferCacheManager {
 public:
  using FreeList = FreeListT;
  using TransferCache = TransferCacheT<FreeList, TwoSizeClassManager>;

  // This is 3 instead of 2 because we hard code size_class == 0 to be invalid
  // in many places. We only use size_class 1 and 2 here.
  static constexpr size_t kClassSize1 = 8;
  static constexpr size_t kClassSize2 = 16 << 10;
  static constexpr size_t kNumToMove1 = 32;
  static constexpr size_t kNumToMove2 = 2;

  TwoSizeClassManager() {
    caches_.push_back(std::make_unique<TransferCache>(
        this, 0, /*use_all_buckets_for_few_object_spans=*/false));
    caches_.push_back(std::make_unique<TransferCache>(
        this, 1, /*use_all_buckets_for_few_object_spans=*/false));
    caches_.push_back(std::make_unique<TransferCache>(
        this, 2, /*use_all_buckets_for_few_object_spans=*/false));
  }

  constexpr static size_t class_to_size(int size_class) {
    switch (size_class) {
      case 1:
        return kClassSize1;
      case 2:
        return kClassSize2;
      default:
        return 0;
    }
  }
  constexpr static size_t num_objects_to_move(int size_class) {
    switch (size_class) {
      case 1:
        return kNumToMove1;
      case 2:
        return kNumToMove2;
      default:
        return 0;
    }
  }

  void InsertRange(int size_class, absl::Span<void*> batch) {
    caches_[size_class]->InsertRange(size_class, batch);
  }

  int RemoveRange(int size_class, void** batch, int N) {
    return caches_[size_class]->RemoveRange(size_class, batch, N);
  }

  size_t tc_length(int size_class) { return caches_[size_class]->tc_length(); }
  TransferCacheStats GetStats(int size_class) {
    return caches_[size_class]->GetStats();
  }

  std::vector<std::unique_ptr<TransferCache>> caches_;
};

class FakeCpuLayout {
 public:
  static constexpr int kNumCpus = 6;
  static constexpr int kCpusPerShard = 2;

  void Init(int shards) {
    TC_ASSERT_GT(shards, 0);
    TC_ASSERT_LE(shards * kCpusPerShard, kNumCpus);
    num_shards_ = shards;
  }

  void SetCurrentCpu(int cpu) {
    TC_ASSERT_GE(cpu, 0);
    TC_ASSERT_LT(cpu, kNumCpus);
    current_cpu_ = cpu;
  }

  unsigned NumShards() { return num_shards_; }
  int CurrentCpu() { return current_cpu_; }
  unsigned CpuShard(int cpu) { return cpu / kCpusPerShard; }

 private:
  int current_cpu_ = 0;
  int num_shards_ = 0;
};

// Defines transfer cache manager for testing legacy transfer cache.
class FakeMultiClassTransferCacheManager : public TransferCacheManager {
 public:
  void Init() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    InitCaches();
  }
};

// Wires up a largely functional TransferCache + TransferCacheManager +
// CentralFreeList.
//
// Unlike FakeTransferCacheEnvironment, this may be used to perform allocations
// and deallocations out of transfer cache for multiple size classes. It can
// also be wired with the real transfer cache manager and be used to test
// implementations in a real transfer cache, unlike TwoSizeClassEnv.
template <typename TransferCacheT>
class MultiSizeClassTransferCacheEnvironment {
 public:
  static constexpr int kSizeClasses = 4;
  using TransferCache = TransferCacheT;
  using Manager = typename TransferCache::Manager;
  using FreeList = typename TransferCache::FreeList;
  MultiSizeClassTransferCacheEnvironment() { manager_.Init(); }

  ~MultiSizeClassTransferCacheEnvironment() { Drain(); }

  void Insert(int size_class, int n) {
    const size_t batch_size = Manager::num_objects_to_move(size_class);
    std::vector<void*> bufs;
    while (n > 0) {
      int b = std::min<int>(n, batch_size);
      bufs.resize(b);
      int removed = central_freelist(size_class).RemoveRange(&bufs[0], b);
      ASSERT_GT(removed, 0);
      ASSERT_LE(removed, b);
      manager_.InsertRange(size_class, absl::MakeSpan(bufs));
      n -= b;
    }
  }

  void Remove(int size_class, int n) {
    const size_t batch_size = manager_.num_objects_to_move(size_class);
    std::vector<void*> bufs;
    while (n > 0) {
      const int b = std::min<int>(n, batch_size);
      bufs.resize(b);
      const int removed = manager_.RemoveRange(size_class, &bufs[0], b);
      // Ensure we make progress.
      ASSERT_GT(removed, 0);
      ASSERT_LE(removed, b);
      central_freelist(size_class)
          .InsertRange({&bufs[0], static_cast<size_t>(removed)});
      n -= removed;
    }
  }

  void Drain() {
    for (int i = 0; i < kNumClasses; ++i) {
      Remove(i, manager_.tc_length(i));
    }
  }

  void RandomlyPoke() {
    absl::BitGen gen;
    // Insert or remove from the transfer cache with equal probability.
    const double choice = absl::Uniform(gen, 0.0, 1.0);
    const size_t size_class = absl::Uniform<size_t>(gen, 1, kSizeClasses);
    const size_t batch_size = manager_.num_objects_to_move(size_class);
    if (choice < 0.5) {
      Insert(size_class, batch_size);
    } else {
      Remove(size_class, batch_size);
    }
  }

  Manager& transfer_cache_manager() { return manager_; }

  FreeList& central_freelist(int size_class) {
    return manager_.central_freelist(size_class);
  }

 private:
  Manager manager_;
};

class FakeShardedTransferCacheEnvironment {
 public:
  using Manager = FakeShardedTransferCacheManager;
  using ShardedManager =
      ShardedTransferCacheManagerBase<Manager, FakeCpuLayout,
                                      MinimalFakeCentralFreeList>;

  explicit FakeShardedTransferCacheEnvironment(int num_shards,
                                               bool use_generic_cache)
      : sharded_manager_(&owner_, &cpu_layout_) {
    if (use_generic_cache) {
      owner_.SetGenericCache(true);
    } else {
      owner_.SetCacheForLargeClassesOnly(true);
    }

    cpu_layout_.Init(num_shards);
    sharded_manager_.Init();
  }

  ~FakeShardedTransferCacheEnvironment() { Drain(); }

  void Remove(int cpu, int n) {
    cpu_layout_.SetCurrentCpu(cpu);
    std::vector<void*> bufs;
    for (int i = 0; i < n; ++i) {
      void* ptr = sharded_manager_.Pop(kSizeClass);
      // Ensure we make progress.
      ASSERT_NE(ptr, nullptr);
      central_freelist().FreeBatch({&ptr, 1});
    }
  }

  void Drain() {
    for (int cpu = 0; cpu < FakeCpuLayout::kNumCpus; ++cpu) {
      Remove(cpu, sharded_manager_.tc_length(cpu, kSizeClass));
    }
  }

  ShardedManager& sharded_manager() { return sharded_manager_; }
  Manager& transfer_cache_manager() { return owner_; }
  MinimalFakeCentralFreeList& central_freelist() { return freelist_; }
  void SetCurrentCpu(int cpu) { cpu_layout_.SetCurrentCpu(cpu); }
  size_t MetadataAllocated() const { return owner_.used(); }

 private:
  MinimalFakeCentralFreeList freelist_;
  FakeShardedTransferCacheManager owner_;
  FakeCpuLayout cpu_layout_;
  ShardedManager sharded_manager_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_MOCK_TRANSFER_CACHE_H_

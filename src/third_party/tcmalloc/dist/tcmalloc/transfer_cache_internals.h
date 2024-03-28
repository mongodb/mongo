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

#ifndef TCMALLOC_TRANSFER_CACHE_INTERNALS_H_
#define TCMALLOC_TRANSFER_CACHE_INTERNALS_H_

#include <sched.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/transfer_cache_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal::internal_transfer_cache {

struct alignas(8) SizeInfo {
  int32_t used;
  int32_t capacity;
};
static constexpr int kMaxCapacityInBatches = 64;
static constexpr int kInitialCapacityInBatches = 16;

// Records counters for different types of misses.
class MissCounts {
 public:
  void Inc(size_t value) {
    total_.store(total_.load(std::memory_order_relaxed) + value,
                 std::memory_order_relaxed);
  }

  size_t Total() const { return total_.load(std::memory_order_relaxed); }

  // Returns the number of misses since the last commit call.
  size_t Commit() {
    size_t t = total_.load(std::memory_order_relaxed);
    size_t c = total_committed_.exchange(t, std::memory_order_relaxed);
    if (ABSL_PREDICT_TRUE(t > c)) {
      return t - c;
    }
    // In case of a size_t overflow, we wrap around to 0.
    return 0;
  }

 private:
  std::atomic<size_t> total_ = {0};
  std::atomic<size_t> total_committed_ = {0};
};

// TransferCache is used to cache transfers of
// sizemap.num_objects_to_move(size_class) back and forth between
// thread caches and the central cache for a given size class.
template <typename CentralFreeList, typename TransferCacheManager>
class TransferCache {
 public:
  using Manager = TransferCacheManager;
  using FreeList = CentralFreeList;

  TransferCache(Manager *owner, int size_class,
                bool use_all_buckets_for_few_object_spans)
      : TransferCache(owner, size_class, CapacityNeeded(size_class),
                      use_all_buckets_for_few_object_spans) {}

  struct Capacity {
    int capacity;
    int max_capacity;
  };

  TransferCache(Manager *owner, int size_class, Capacity capacity,
                bool use_all_buckets_for_few_object_spans)
      : lock_(absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY),
        low_water_mark_(0),
        slot_info_(SizeInfo({0, capacity.capacity})),
        slots_(nullptr),
        freelist_do_not_access_directly_(),
        owner_(owner),
        max_capacity_(capacity.max_capacity) {
    freelist().Init(size_class, use_all_buckets_for_few_object_spans);
    slots_ = max_capacity_ != 0 ? reinterpret_cast<void **>(owner_->Alloc(
                                      max_capacity_ * sizeof(void *)))
                                : nullptr;
  }

  TransferCache(const TransferCache &) = delete;
  TransferCache &operator=(const TransferCache &) = delete;

  // Compute initial and max capacity that we should configure this cache for.
  static Capacity CapacityNeeded(size_t size_class) {
    // We need at least 2 slots to store list head and tail.
    static_assert(kMinObjectsToMove >= 2);

    const size_t bytes = Manager::class_to_size(size_class);
    if (size_class <= 0 || bytes <= 0) return {0, 0};

    // Limit the maximum size of the cache based on the size class.  If this
    // is not done, large size class objects will consume a lot of memory if
    // they just sit in the transfer cache.
    const size_t objs_to_move = Manager::num_objects_to_move(size_class);
    TC_ASSERT_GT(objs_to_move, 0);

    // Starting point for the maximum number of entries in the transfer cache.
    // This actual maximum for a given size class may be lower than this
    // maximum value.
    int max_capacity = kMaxCapacityInBatches * objs_to_move;
    // A transfer cache freelist can have anywhere from 0 to
    // max_capacity_ slots to put link list chains into.
    int capacity = kInitialCapacityInBatches * objs_to_move;

    // Limit each size class cache to at most 1MB of objects or one entry,
    // whichever is greater. Total transfer cache memory used across all
    // size classes then can't be greater than approximately
    // 1MB * kMaxNumTransferEntries.
    max_capacity = std::min<int>(
        max_capacity,
        std::max<int>(objs_to_move,
                      (1024 * 1024) / (bytes * objs_to_move) * objs_to_move));
    capacity = std::min(capacity, max_capacity);

    return {capacity, max_capacity};
  }

  // These methods all do internal locking.

  // Insert the specified batch into the transfer cache.  N is the number of
  // elements in the range.  RemoveRange() is the opposite operation.
  void InsertRange(int size_class, absl::Span<void *> batch)
      ABSL_LOCKS_EXCLUDED(lock_) {
    const int N = batch.size();
    TC_ASSERT(0 < N && N <= kMaxObjectsToMove);
    auto info = slot_info_.load(std::memory_order_relaxed);
    if (info.capacity > info.used) {
      AllocationGuardSpinLockHolder h(&lock_);
      // As caches are resized in the background, we do not attempt to grow
      // them here. Instead, we just check if they have spare free capacity.
      info = slot_info_.load(std::memory_order_relaxed);
      int got = std::min(N, info.capacity - info.used);
      if (got > 0) {
        info.used += got;
        SetSlotInfo(info);
        void **entry = GetSlot(info.used - got);
        memcpy(entry, batch.data(), sizeof(void *) * got);
        insert_hits_.LossyAdd(1);
        if (got == N) {
          return;
        }
        batch = {batch.data() + got, batch.size() - got};
      }
    }

    insert_misses_.LossyAdd(1);
    insert_object_misses_.Inc(batch.size());

    freelist().InsertRange(batch);
  }

  // Returns the actual number of fetched elements and stores elements in the
  // batch.
  ABSL_MUST_USE_RESULT int RemoveRange(int size_class, void **batch, int N)
      ABSL_LOCKS_EXCLUDED(lock_) {
    TC_ASSERT(0 < N && N <= kMaxObjectsToMove);
    auto info = slot_info_.load(std::memory_order_relaxed);
    if (info.used) {
      AllocationGuardSpinLockHolder h(&lock_);
      // Refetch with the lock
      info = slot_info_.load(std::memory_order_relaxed);
      int got = std::min(N, info.used);
      if (got) {
        info.used -= got;
        SetSlotInfo(info);
        void **entry = GetSlot(info.used);
        memcpy(batch, entry, sizeof(void *) * got);
        remove_hits_.LossyAdd(1);
        low_water_mark_ = std::min(low_water_mark_, info.used);
        return got;
      }
    }

    remove_misses_.LossyAdd(1);
    remove_object_misses_.Inc(N);
    return freelist().RemoveRange(batch, N);
  }

  // We record the lowest value of info.used in a low water mark since the last
  // call to TryPlunder. We plunder all those objects to the freelist, as the
  // objects not used within a full cycle are unlikely to be used again.
  void TryPlunder(int size_class) ABSL_LOCKS_EXCLUDED(lock_) {
    if (max_capacity_ == 0) return;
    if (!lock_.TryLock()) return;

    int to_return = low_water_mark_;
    SizeInfo info = GetSlotInfo();
    TC_ASSERT_LE(to_return, info.used);
    // Make sure to record number of used objects in the cache in the low water
    // mark at the start of each plunder. If we plunder objects below, we record
    // the new value of info.used in the low water mark as we progress.
    low_water_mark_ = info.used;
    while (true) {
      info = GetSlotInfo();
      const int B = Manager::num_objects_to_move(size_class);
      const size_t num_to_move = std::min({B, info.used, to_return});
      if (num_to_move == 0) break;

      void *buf[kMaxObjectsToMove];
      void **const entry = GetSlot(info.used - num_to_move);
      memcpy(buf, entry, sizeof(void *) * num_to_move);
      info.used -= num_to_move;
      to_return -= num_to_move;
      low_water_mark_ = info.used;
      SetSlotInfo(info);
      lock_.Unlock();

      freelist().InsertRange({buf, num_to_move});
      if (!lock_.TryLock()) return;
    }
    lock_.Unlock();
  }
  // Returns the number of free objects in the transfer cache.
  size_t tc_length() const {
    return static_cast<size_t>(slot_info_.load(std::memory_order_relaxed).used);
  }

  // Fetches the misses for the latest interval and commits them to the total.
  size_t FetchCommitIntervalMisses() ABSL_LOCKS_EXCLUDED(lock_) {
    return insert_object_misses_.Commit() + remove_object_misses_.Commit();
  }

  // Returns the number of transfer cache insert/remove hits/misses.
  TransferCacheStats GetStats() const ABSL_LOCKS_EXCLUDED(lock_) {
    TransferCacheStats stats;

    stats.insert_hits = insert_hits_.value();
    stats.remove_hits = remove_hits_.value();
    stats.insert_misses = insert_misses_.value();
    stats.insert_object_misses = insert_object_misses_.Total();
    stats.remove_misses = remove_misses_.value();
    stats.remove_object_misses = remove_object_misses_.Total();

    auto info = slot_info_.load(std::memory_order_relaxed);
    stats.used = info.used;
    stats.capacity = info.capacity;
    stats.max_capacity = max_capacity_;

    return stats;
  }

  SizeInfo GetSlotInfo() const {
    return slot_info_.load(std::memory_order_relaxed);
  }

  // Increases capacity of the cache by a batch size. Returns true if it
  // succeeded at growing the cache by a batch size. Else, returns false.
  bool IncreaseCacheCapacity(int size_class) ABSL_LOCKS_EXCLUDED(lock_) {
    int n = Manager::num_objects_to_move(size_class);

    AllocationGuardSpinLockHolder h(&lock_);
    auto info = slot_info_.load(std::memory_order_relaxed);
    // Check if we can expand this cache?
    if (info.capacity + n > max_capacity_) return false;
    // Increase capacity of the cache.
    info.capacity += n;
    SetSlotInfo(info);
    return true;
  }

  // Checks if the cache capacity may be increased by a batch size.
  bool CanIncreaseCapacity(int size_class) const ABSL_LOCKS_EXCLUDED(lock_) {
    int n = Manager::num_objects_to_move(size_class);
    auto info = GetSlotInfo();
    return max_capacity_ - info.capacity >= n;
  }

  // Checks if the cache has at least batch size number of free slots. Returns
  // false if (capacity - used) slots is less than the batch size.
  bool HasSpareCapacity(int size_class) const {
    int n = Manager::num_objects_to_move(size_class);
    auto info = GetSlotInfo();
    return info.capacity - info.used >= n;
  }

  // REQUIRES: lock_ is *not* held.
  // Tries to shrink the Cache.  Return false if it failed to shrink the cache.
  // Decreases cache_slots_ on success.
  bool ShrinkCache(int size_class) ABSL_LOCKS_EXCLUDED(lock_) {
    int N = Manager::num_objects_to_move(size_class);

    void *to_free[kMaxObjectsToMove];
    int num_to_free;
    {
      AllocationGuardSpinLockHolder h(&lock_);
      auto info = slot_info_.load(std::memory_order_relaxed);
      if (info.capacity == 0) return false;
      if (info.capacity <= N) return false;

      N = std::min(N, info.capacity);
      int unused = info.capacity - info.used;
      if (N <= unused) {
        info.capacity -= N;
        SetSlotInfo(info);
        return true;
      }

      num_to_free = N - unused;
      info.capacity -= N;
      info.used -= num_to_free;
      SetSlotInfo(info);

      low_water_mark_ = std::min(low_water_mark_, info.used);
      // Our internal slot array may get overwritten as soon as we drop the
      // lock, so copy the items to free to an on stack buffer.
      memcpy(to_free, GetSlot(info.used), sizeof(void *) * num_to_free);
    }

    // Access the freelist without holding the lock.
    freelist().InsertRange({to_free, static_cast<uint64_t>(num_to_free)});
    return true;
  }

  // This is a thin wrapper for the CentralFreeList.  It is intended to ensure
  // that we are not holding lock_ when we access it.
  ABSL_ATTRIBUTE_ALWAYS_INLINE FreeList &freelist() ABSL_LOCKS_EXCLUDED(lock_) {
    return freelist_do_not_access_directly_;
  }

  int32_t max_capacity() const { return max_capacity_; }

 private:
  // Returns first object of the i-th slot.
  void **GetSlot(size_t i) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    return slots_ + i;
  }

  void SetSlotInfo(SizeInfo info) {
    TC_ASSERT_LE(0, info.used);
    TC_ASSERT_LE(info.used, info.capacity);
    TC_ASSERT_LE(info.capacity, max_capacity_);
    slot_info_.store(info, std::memory_order_relaxed);
  }

  // Note: lock_ must be appear first (see b/313914119).
  // The lock and the associated member variables that are accessed when the
  // spinlock is acquired are placed together on the same cache line for cache
  // friendliness.
  absl::base_internal::SpinLock lock_;

  // All the following fields are accessed when holding lock_, so they should
  // be collocated with lock_ on the same cacheline. Align insert_hits_ to
  // ensure the following fields are on a separate cacheline.

  // Lowest value of "slot_info_.used" since last call to TryPlunder. All
  // elements not used for a full cycle (2 seconds) are unlikely to get used
  // again.
  int low_water_mark_ ABSL_GUARDED_BY(lock_);

  // insert_hits_ and remove_hits_ are logically guarded by lock_ for mutations
  // and use LossyAdd, but the thread annotations cannot indicate that we do not
  // need a lock for reads.
  StatsCounter insert_hits_;
  StatsCounter remove_hits_;

  // Number of currently used and available cached entries in slots_. This
  // variable is updated under a lock but can be read without one.
  // INVARIANT: [0 <= slot_info_.used <= slot_info.capacity <= max_cache_slots_]
  std::atomic<SizeInfo> slot_info_;

  // Pointer to array of free objects.  Use GetSlot() to get pointers to
  // entries.
  void **slots_ ABSL_GUARDED_BY(lock_);

  FreeList freelist_do_not_access_directly_;

  Manager *const owner_;

  // Maximum size of the cache.
  const int32_t max_capacity_;

  // The following 4 *_misses_ counters
  // are frequently updated, so they should reside in a separate cacheline from
  // lock_.

  // For these we are deliberately fast-and-loose. Some increments may be lost.
  StatsCounter insert_misses_;
  StatsCounter remove_misses_;

  MissCounts insert_object_misses_;
  MissCounts remove_object_misses_;
} ABSL_CACHELINE_ALIGNED;

template <typename Manager>
void ResizeCaches(Manager &manager, int start_size_class) {
  TC_ASSERT_GE(start_size_class, 0);
  TC_ASSERT_LE(start_size_class + Manager::kNumBaseClasses,
               Manager::kNumClasses);
  // Tracks misses per size class.
  struct MissInfo {
    int size_class;
    uint64_t misses;
  };

  std::array<MissInfo, Manager::kNumBaseClasses> misses;

  // Collect misses for all the size classes that were incurred during the
  // previous resize interval.
  for (int i = 0; i < Manager::kNumBaseClasses; ++i) {
    int size_class = start_size_class + i;
    size_t miss = manager.FetchCommitIntervalMisses(size_class);
    misses[i] = {.size_class = size_class, .misses = miss};
  }

  // Prioritize shrinking cache that had least number of misses.
  std::sort(misses.begin(), misses.end(),
            [](const MissInfo &a, const MissInfo &b) {
              if (a.misses == b.misses) {
                return a.size_class < b.size_class;
              }
              return a.misses > b.misses;
            });

  int total_grown = 0;
  int total_shrunk = 0;

  for (int to_grow = 0, to_shrink = Manager::kNumBaseClasses - 1;
       to_grow < to_shrink; ++to_grow, --to_shrink) {
    if (total_grown == Manager::kMaxSizeClassesToResize) break;
    if (!manager.CanIncreaseCapacity(misses[to_grow].size_class)) {
      ++to_shrink;
      continue;
    }

    // No one else wants to grow, so stop here.
    if (misses[to_grow].misses == 0) break;

    for (; to_grow < to_shrink; --to_shrink) {
      int shrink_size_class = misses[to_shrink].size_class;
      if (manager.ShrinkCache(shrink_size_class)) {
        ++total_shrunk;
        break;
      }
    }

    for (; to_grow < to_shrink; ++to_grow) {
      int grow_size_class = misses[to_grow].size_class;
      if (manager.IncreaseCacheCapacity(grow_size_class)) {
        ++total_grown;
        break;
      }
    }
  }

  // It is possible that we successfully shrank our last shrink but were
  // unable to grow our last grow, which would leave us with one spare
  // capacity.  If we don't find someone to grow, the entire system loses
  // capacity.
  while (ABSL_PREDICT_FALSE(total_grown < total_shrunk)) {
    for (int i = 0; i < Manager::kNumBaseClasses; ++i) {
      int grow_size_class = misses[i].size_class;
      if (manager.IncreaseCacheCapacity(grow_size_class)) {
        ++total_grown;
        break;
      }
    }
  }
}

template <typename Manager>
void TryResizingCaches(Manager &manager) {
  // Resize transfer caches for each set of kNumBaseClasses.

  // TODO(b/270726235): Revisit this once we start using expanded size classes
  // more effectively.
  for (int i = 0; i < Manager::kNumaPartitions; ++i) {
    ResizeCaches(manager, i * Manager::kNumBaseClasses);
  }
  if (Manager::kHasExpandedClasses) {
    ResizeCaches(manager, Manager::kExpandedClassesStart);
  }
}

}  // namespace tcmalloc::tcmalloc_internal::internal_transfer_cache
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_TRANSFER_CACHE_INTERNALS_H_

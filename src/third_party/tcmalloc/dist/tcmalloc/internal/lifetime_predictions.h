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

#ifndef TCMALLOC_INTERNAL_LIFETIME_PREDICTIONS_H_
#define TCMALLOC_INTERNAL_LIFETIME_PREDICTIONS_H_

#include <algorithm>
#include <cstdlib>
#include <functional>

#include "absl/algorithm/container.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/base/internal/spinlock.h"
#include "absl/debugging/stacktrace.h"
#include "absl/hash/hash.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/linked_list.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Counts how many times we observed objects with a particular stack trace
// that were short lived/long lived. Each LifetimeStats object is associated
// with a particular allocation site (i.e., allocation stack trace) and each
// allocation site has at most one LifetimeStats object. All accesses to
// LifetimeStats objects need to be synchronized via the page heap lock.
class LifetimeStats : public TList<LifetimeStats>::Elem {
 public:
  enum class Certainty { kLowCertainty, kHighCertainty };
  enum class Prediction { kShortLived, kLongLived };

  void Update(Prediction prediction) {
    if (prediction == Prediction::kShortLived) {
      short_lived_++;
    } else {
      long_lived_++;
    }
  }

  Prediction Predict(Certainty certainty) const {
    if (certainty == Certainty::kLowCertainty) {
      return (short_lived_ > long_lived_) ? Prediction::kShortLived
                                          : Prediction::kLongLived;
    } else {
      // If little data was collected, predict as long-lived (current behavior).
      return (short_lived_ > (long_lived_ + 10)) ? Prediction::kShortLived
                                                 : Prediction::kLongLived;
    }
  }

  // Reference counts are protected by LifetimeDatabase::table_lock_.

  // Increments the reference count of this entry.
  void IncRef() { ++refcount_; }

  // Returns true if and only if the reference count reaches 0.
  bool DecRef() { return --refcount_ == 0; }

 private:
  uint64_t refcount_ = 1;
  uint64_t short_lived_ = 0;
  uint64_t long_lived_ = 0;
};

// Manages stack traces and statistics about their associated lifetimes. Since
// the database can fill up, old entries are evicted. Evicted entries need to
// survive as long as the last lifetime tracker referencing them and are thus
// reference-counted.
class LifetimeDatabase {
 public:
  class Key {
   public:
    static Key RecordCurrentKey() {
      // This should not trigger a copy due to copy elision.
      return Key();
    }

    template <typename H>
    friend H AbslHashValue(H h, const Key& c) {
      return H::combine(H::combine_contiguous(std::move(h), c.stack_, c.depth_),
                        c.depth_);
    }

    friend bool operator==(const Key& lhs, const Key& rhs) {
      if (lhs.depth_ != rhs.depth_) {
        return false;
      }
      return std::equal(lhs.stack_, lhs.stack_ + lhs.depth_, rhs.stack_);
    }

   private:
    int depth_;  // Number of PC values stored in array below
    void* stack_[kMaxStackDepth];

    // We statically instantiate `Key` at the start of the allocation to acquire
    // the allocation stack trace. This can take a significant amount of time.
    Key() {
      depth_ = absl::GetStackTrace(stack_, kMaxStackDepth, /*skip_count=*/1);
    }
  };

  // Captures statistics associated with the low-level allocator backing the
  // memory used by the database.
  struct ArenaStats {
    uint64_t bytes_allocated;
  };

  static constexpr int kMaxDatabaseSize = 1024;

  LifetimeDatabase() {}
  ~LifetimeDatabase() {}

  // Not copyable or movable
  LifetimeDatabase(const LifetimeDatabase&) = delete;
  LifetimeDatabase& operator=(const LifetimeDatabase&) = delete;

  // Identifies the current stack trace and returns a handle to the lifetime
  // statistics associated with this stack trace. May run outside the page heap
  // lock -- we therefore need to do our own locking. This increments the
  // reference count of the lifetime stats object and the caller is responsible
  // for calling RemoveLifetimeStatsReference when finished with the object.
  LifetimeStats* LookupOrAddLifetimeStats(Key* k) {
    absl::base_internal::SpinLockHolder h(&table_lock_);
    auto it = table_.find(*k);
    LifetimeStats* s;
    if (it == table_.end()) {
      MaybeEvictLRU();
      // Allocate a new entry using the low-level allocator, which is safe
      // to call from within TCMalloc.
      s = stats_allocator_.allocate(1);
      new (s) LifetimeStats();
      table_.insert(std::make_pair(*k, s));
      stats_fifo_.append(s);
    } else {
      s = it->second;
      UpdateLRU(s);
    }
    s->IncRef();
    return s;
  }

  void RemoveLifetimeStatsReference(LifetimeStats* s) {
    absl::base_internal::SpinLockHolder h(&table_lock_);
    if (s->DecRef()) {
      stats_allocator_.deallocate(s, 1);
    }
  }

  size_t size() const {
    absl::base_internal::SpinLockHolder h(&table_lock_);
    return table_.size();
  }

  size_t evictions() const {
    absl::base_internal::SpinLockHolder h(&table_lock_);
    return n_evictions_;
  }

  static ArenaStats* arena_stats() {
    static ArenaStats stats = {0};
    return &stats;
  }

 protected:
  static const int kMaxStackDepth = 64;

  static absl::base_internal::LowLevelAlloc::Arena* GetArena() {
    static absl::base_internal::LowLevelAlloc::Arena* arena =
        absl::base_internal::LowLevelAlloc::NewArena(0);
    return arena;
  }

  void UpdateLRU(LifetimeStats* stats)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(table_lock_) {
    stats_fifo_.remove(stats);
    stats_fifo_.append(stats);
  }

  // If an entry is evicted, it is returned (nullptr otherwise).
  void MaybeEvictLRU() ABSL_EXCLUSIVE_LOCKS_REQUIRED(table_lock_) {
    if (table_.size() < kMaxDatabaseSize) {
      return;
    }
    n_evictions_++;
    LifetimeStats* evict = stats_fifo_.first();
    stats_fifo_.remove(evict);
    for (auto it = table_.begin(); it != table_.end(); ++it) {
      if (it->second == evict) {
        table_.erase(it);
        if (evict->DecRef()) {
          stats_allocator_.deallocate(evict, 1);
        }
        return;
      }
    }
    CHECK_CONDITION(false);  // Should not happen
  }

 private:
  template <typename T>
  class MyAllocator : public std::allocator<T> {
   public:
    template <typename U>
    struct rebind {
      using other = MyAllocator<U>;
    };

    MyAllocator() noexcept {}

    template <typename U>
    explicit MyAllocator(const MyAllocator<U>&) noexcept {}

    T* allocate(size_t num_objects, const void* = nullptr) {
      size_t bytes = num_objects * sizeof(T);
      arena_stats()->bytes_allocated += bytes;
      return static_cast<T*>(absl::base_internal::LowLevelAlloc::AllocWithArena(
          bytes, GetArena()));
    }

    void deallocate(T* p, size_t num_objects) {
      size_t bytes = num_objects * sizeof(T);
      arena_stats()->bytes_allocated -= bytes;
      absl::base_internal::LowLevelAlloc::Free(p);
    }
  };

  MyAllocator<LifetimeStats> stats_allocator_ ABSL_GUARDED_BY(table_lock_);
  mutable absl::base_internal::SpinLock table_lock_{
      absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY};

  // Stores the current mapping from allocation site to LifetimeStats.
  std::unordered_map<Key, LifetimeStats*, absl::Hash<Key>, std::equal_to<Key>,
                     MyAllocator<std::pair<const Key, LifetimeStats*>>>
      table_ ABSL_GUARDED_BY(table_lock_);

  // Stores the entries ordered by how many times they have been accessed.
  TList<LifetimeStats> stats_fifo_ ABSL_GUARDED_BY(table_lock_);
  size_t n_evictions_ ABSL_GUARDED_BY(table_lock_) = 0;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_LIFETIME_PREDICTIONS_H_

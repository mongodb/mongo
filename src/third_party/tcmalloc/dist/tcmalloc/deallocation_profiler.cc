// Copyright 2022 The TCMalloc Authors
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

#include "tcmalloc/deallocation_profiler.h"

#include <algorithm>
#include <cmath>    // for std::lround
#include <cstddef>
#include <cstdint>  // for uintptr_t
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/macros.h"
#include "absl/container/flat_hash_map.h"
#include "absl/debugging/stacktrace.h"  // for GetStackTrace
#include "absl/functional/function_ref.h"
#include "absl/hash/hash.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/cache_topology.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sampled_allocation.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace deallocationz {
namespace {
using ::absl::base_internal::SpinLock;
using tcmalloc_internal::AllocationGuardSpinLockHolder;

// STL adaptor for an arena based allocator which provides the following:
//   static void* Alloc::Allocate(size_t size);
//   static void Alloc::Free(void* ptr, size_t size);
template <typename T, class Alloc>
class AllocAdaptor final {
 public:
  using value_type = T;

  AllocAdaptor() {}
  AllocAdaptor(const AllocAdaptor&) {}

  template <class T1>
  using rebind = AllocAdaptor<T1, Alloc>;

  template <class T1>
  explicit AllocAdaptor(const AllocAdaptor<T1, Alloc>&) {}

  T* allocate(size_t n) {
    // Check if n is too big to allocate.
    TC_ASSERT_EQ((n * sizeof(T)) / sizeof(T), n);
    return static_cast<T*>(Alloc::Allocate(n * sizeof(T)));
  }
  void deallocate(T* p, size_t n) { Alloc::Free(p, n * sizeof(T)); }
};

const int64_t kMaxStackDepth = 64;

// Stores stack traces and metadata for any allocation or deallocation
// encountered by the profiler.
struct DeallocationSampleRecord {
  double weight = 0.0;
  size_t requested_size = 0;
  size_t requested_alignment = 0;
  size_t allocated_size = 0;  // size after sizeclass/page rounding

  int depth = 0;  // Number of PC values stored in array below
  void* stack[kMaxStackDepth];

  // creation_time is used to capture the life_time of sampled allocations
  absl::Time creation_time;
  int cpu_id = -1;
  int vcpu_id = -1;
  int l3_id = -1;
  int numa_id = -1;
  pid_t thread_id = 0;

  template <typename H>
  friend H AbslHashValue(H h, const DeallocationSampleRecord& c) {
    return H::combine(H::combine_contiguous(std::move(h), c.stack, c.depth),
                      c.depth, c.requested_size, c.requested_alignment,
                      c.allocated_size);
  }

  bool operator==(const DeallocationSampleRecord& other) const {
    if (depth != other.depth || requested_size != other.requested_size ||
        requested_alignment != other.requested_alignment ||
        allocated_size != other.allocated_size) {
      return false;
    }
    return std::equal(stack, stack + depth, other.stack);
  }
};

// Tracks whether an object was allocated/deallocated by the same CPU/thread.
struct CpuThreadMatchingStatus {
  constexpr CpuThreadMatchingStatus(bool physical_cpu_matched,
                                    bool virtual_cpu_matched, bool l3_matched,
                                    bool numa_matched, bool thread_matched)
      : physical_cpu_matched(physical_cpu_matched),
        virtual_cpu_matched(virtual_cpu_matched),
        l3_matched(l3_matched),
        numa_matched(numa_matched),
        thread_matched(thread_matched),
        value((static_cast<int>(physical_cpu_matched) << 4) |
              (static_cast<int>(virtual_cpu_matched) << 3) |
              (static_cast<int>(l3_matched) << 2) |
              (static_cast<int>(numa_matched) << 1) |
              static_cast<int>(thread_matched)) {}
  bool physical_cpu_matched;
  bool virtual_cpu_matched;
  bool l3_matched;
  bool numa_matched;
  bool thread_matched;
  int value;
};

struct RpcMatchingStatus {
  static constexpr int ComputeValue(uint64_t alloc, uint64_t dealloc) {
    if (alloc != 0 && dealloc != 0) {
      return static_cast<int>(alloc == dealloc);
    } else {
      return 2;
    }
  }

  constexpr RpcMatchingStatus(uint64_t alloc, uint64_t dealloc)
      : value(ComputeValue(alloc, dealloc)) {}

  int value;
};

int ComputeIndex(CpuThreadMatchingStatus status, RpcMatchingStatus rpc_status) {
  return status.value * 3 + rpc_status.value;
}

int GetL3Id(int cpu_id) {
  return cpu_id >= 0
             ? tcmalloc_internal::CacheTopology::Instance().GetL3FromCpuId(
                   cpu_id)
             : -1;
}

int GetNumaId(int cpu_id) {
  return cpu_id >= 0
             ? tcmalloc_internal::tc_globals.numa_topology().GetCpuPartition(
                   cpu_id)
             : -1;
}

constexpr std::pair<CpuThreadMatchingStatus, RpcMatchingStatus> kAllCases[] = {
    // clang-format off
    {CpuThreadMatchingStatus(false, false, false, false, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, false, false, false, true), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, false, false, true, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, false, false, true, true), RpcMatchingStatus(0, 0)},

    {CpuThreadMatchingStatus(false, false, false, false, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, false, false, false, true), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, false, false, true, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, false, false, true, true), RpcMatchingStatus(1, 2)},

    {CpuThreadMatchingStatus(false, false, false, false, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, false, false, false, true), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, false, false, true, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, false, false, true, true), RpcMatchingStatus(1, 1)},

    {CpuThreadMatchingStatus(false, false, true, false, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, false, true, false, true), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, false, true, true, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, false, true, true, true), RpcMatchingStatus(0, 0)},

    {CpuThreadMatchingStatus(false, false, true, false, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, false, true, false, true), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, false, true, true, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, false, true, true, true), RpcMatchingStatus(1, 2)},

    {CpuThreadMatchingStatus(false, false, true, false, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, false, true, false, true), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, false, true, true, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, false, true, true, true), RpcMatchingStatus(1, 1)},

    {CpuThreadMatchingStatus(false, true, false, false, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, true, false, false, true), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, true, false, true, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, true, false, true, true), RpcMatchingStatus(0, 0)},

    {CpuThreadMatchingStatus(false, true, false, false, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, true, false, false, true), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, true, false, true, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, true, false, true, true), RpcMatchingStatus(1, 2)},

    {CpuThreadMatchingStatus(false, true, false, false, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, true, false, false, true), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, true, false, true, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, true, false, true, true), RpcMatchingStatus(1, 1)},

    {CpuThreadMatchingStatus(false, true, true, false, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, true, true, false, true), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, true, true, true, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(false, true, true, true, true), RpcMatchingStatus(0, 0)},

    {CpuThreadMatchingStatus(false, true, true, false, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, true, true, false, true), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, true, true, true, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(false, true, true, true, true), RpcMatchingStatus(1, 2)},

    {CpuThreadMatchingStatus(false, true, true, false, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, true, true, false, true), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, true, true, true, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(false, true, true, true, true), RpcMatchingStatus(1, 1)},

    {CpuThreadMatchingStatus(true, false, false, false, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, false, false, false, true), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, false, false, true, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, false, false, true, true), RpcMatchingStatus(0, 0)},

    {CpuThreadMatchingStatus(true, false, false, false, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, false, false, false, true), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, false, false, true, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, false, false, true, true), RpcMatchingStatus(1, 2)},

    {CpuThreadMatchingStatus(true, false, false, false, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, false, false, false, true), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, false, false, true, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, false, false, true, true), RpcMatchingStatus(1, 1)},

    {CpuThreadMatchingStatus(true, false, true, false, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, false, true, false, true), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, false, true, true, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, false, true, true, true), RpcMatchingStatus(0, 0)},

    {CpuThreadMatchingStatus(true, false, true, false, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, false, true, false, true), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, false, true, true, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, false, true, true, true), RpcMatchingStatus(1, 2)},

    {CpuThreadMatchingStatus(true, false, true, false, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, false, true, false, true), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, false, true, true, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, false, true, true, true), RpcMatchingStatus(1, 1)},

    {CpuThreadMatchingStatus(true, true, false, false, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, true, false, false, true), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, true, false, true, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, true, false, true, true), RpcMatchingStatus(0, 0)},

    {CpuThreadMatchingStatus(true, true, false, false, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, true, false, false, true), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, true, false, true, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, true, false, true, true), RpcMatchingStatus(1, 2)},

    {CpuThreadMatchingStatus(true, true, false, false, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, true, false, false, true), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, true, false, true, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, true, false, true, true), RpcMatchingStatus(1, 1)},

    {CpuThreadMatchingStatus(true, true, true, false, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, true, true, false, true), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, true, true, true, false), RpcMatchingStatus(0, 0)},
    {CpuThreadMatchingStatus(true, true, true, true, true), RpcMatchingStatus(0, 0)},

    {CpuThreadMatchingStatus(true, true, true, false, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, true, true, false, true), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, true, true, true, false), RpcMatchingStatus(1, 2)},
    {CpuThreadMatchingStatus(true, true, true, true, true), RpcMatchingStatus(1, 2)},

    {CpuThreadMatchingStatus(true, true, true, false, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, true, true, false, true), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, true, true, true, false), RpcMatchingStatus(1, 1)},
    {CpuThreadMatchingStatus(true, true, true, true, true), RpcMatchingStatus(1, 1)},
    // clang-format on
};
}  // namespace

class DeallocationProfiler {
 private:
  // Arena and allocator used to back STL objects used by DeallocationProfiler
  // Shared between all instances of DeallocationProfiler
  // TODO(b/248332543): Use TCMalloc's own arena allocator instead of defining a
  // new one here. The need for refcount management could be the reason for
  // using a custom allocator in the first place.
  class MyAllocator {
   public:
    static void* Allocate(size_t n) {
      return absl::base_internal::LowLevelAlloc::AllocWithArena(n, arena_);
    }
    static void Free(const void* p, size_t /* n */) {
      absl::base_internal::LowLevelAlloc::Free(const_cast<void*>(p));
    }

    // The lifetime of the arena is managed using a reference count and
    // determined by how long at least one emitted Profile remains alive.
    struct LowLevelArenaReference {
      LowLevelArenaReference() {
        AllocationGuardSpinLockHolder h(&arena_lock_);
        if ((refcount_++) == 0) {
          TC_CHECK_EQ(arena_, nullptr);
          arena_ = absl::base_internal::LowLevelAlloc::NewArena(0);
        }
      }

      ~LowLevelArenaReference() {
        AllocationGuardSpinLockHolder h(&arena_lock_);
        if ((--refcount_) == 0) {
          TC_CHECK(absl::base_internal::LowLevelAlloc::DeleteArena(arena_));
          arena_ = nullptr;
        }
      }
    };

   private:
    // We need to protect the arena with a mutex and ensure that every thread
    // acquires that mutex before it uses the arena for the first time. Once
    // it has acquired the mutex, it is guaranteed that arena won't change
    // between that point in time and when the thread stops accessing it (as
    // enforced by LowLevelArenaReference below).
    ABSL_CONST_INIT static SpinLock arena_lock_;
    static absl::base_internal::LowLevelAlloc::Arena* arena_;

    // We assume that launching a new deallocation profiler takes too long
    // to cause this to overflow within the sampling period. The reason this
    // is not using std::shared_ptr is that we do not only need to protect the
    // value of the reference count but also the pointer itself (and therefore
    // need a separate mutex either way).
    static uint32_t refcount_;
  };

  // This must be the first member of the class to be initialized. The
  // underlying arena must stay alive as long as the profiler.
  MyAllocator::LowLevelArenaReference arena_ref_;

  // All active profilers are stored in a list.
  DeallocationProfiler* next_;
  DeallocationProfilerList* list_ = nullptr;
  friend class DeallocationProfilerList;

  using AllocsTable = absl::flat_hash_map<
      tcmalloc_internal::AllocHandle, DeallocationSampleRecord,
      absl::Hash<tcmalloc_internal::AllocHandle>,
      std::equal_to<tcmalloc_internal::AllocHandle>,
      AllocAdaptor<std::pair<const tcmalloc_internal::AllocHandle,
                             DeallocationSampleRecord>,
                   MyAllocator>>;

  class DeallocationStackTraceTable final
      : public tcmalloc_internal::ProfileBase {
   public:
    // We define the dtor to ensure it is placed in the desired text section.
    ~DeallocationStackTraceTable() override = default;
    void AddTrace(const DeallocationSampleRecord& alloc_trace,
                  const DeallocationSampleRecord& dealloc_trace);

    void Iterate(
        absl::FunctionRef<void(const Profile::Sample&)> func) const override;

    ProfileType Type() const override {
      return tcmalloc::ProfileType::kLifetimes;
    }

    absl::Duration Duration() const override {
      return stop_time_ - start_time_;
    }

    void StopAndRecord(const AllocsTable& allocs);

   private:
    // This must be the first member of the class to be initialized. The
    // underlying arena must stay alive as long as the profile.
    MyAllocator::LowLevelArenaReference arena_ref_;

    static constexpr int kNumCases = ABSL_ARRAYSIZE(kAllCases);

    struct Key {
      DeallocationSampleRecord alloc;
      DeallocationSampleRecord dealloc;

      Key(const DeallocationSampleRecord& alloc,
          const DeallocationSampleRecord& dealloc)
          : alloc(alloc), dealloc(dealloc) {}

      template <typename H>
      friend H AbslHashValue(H h, const Key& c) {
        return H::combine(std::move(h), c.alloc, c.dealloc);
      }

      bool operator==(const Key& other) const {
        return (alloc == other.alloc) && (dealloc == other.dealloc);
      }
    };

    struct Value {
      // for each possible cases, we collect repetition count and avg lifetime
      // we also collect the minimum and maximum lifetimes, as well as the sum
      // of squares (to calculate the standard deviation).
      double counts[kNumCases] = {0.0};
      double mean_life_times_ns[kNumCases] = {0.0};
      double variance_life_times_ns[kNumCases] = {0.0};
      double min_life_times_ns[kNumCases] = {0.0};
      double max_life_times_ns[kNumCases] = {0.0};

      Value() {
        std::fill_n(min_life_times_ns, kNumCases,
                    std::numeric_limits<double>::max());
      }
    };

    absl::flat_hash_map<Key, Value, absl::Hash<Key>, std::equal_to<Key>,
                        AllocAdaptor<std::pair<const Key, Value>, MyAllocator>>
        table_;

    absl::Time start_time_ = absl::Now();
    absl::Time stop_time_;
  };

  // Keep track of allocations that are in flight
  AllocsTable allocs_;

  // Table to store lifetime information collected by this profiler
  std::unique_ptr<DeallocationStackTraceTable> reports_ = nullptr;

 public:
  explicit DeallocationProfiler(DeallocationProfilerList* list) : list_(list) {
    reports_ = std::make_unique<DeallocationStackTraceTable>();
    list_->Add(this);
  }

  ~DeallocationProfiler() {
    if (reports_ != nullptr) {
      Stop();
    }
  }

  const tcmalloc::Profile Stop() {
    if (reports_ != nullptr) {
      // We first remove the profiler from the list to avoid racing with
      // potential allocations which may modify the allocs_ table.
      list_->Remove(this);
      reports_->StopAndRecord(allocs_);
      return tcmalloc_internal::ProfileAccessor::MakeProfile(
          std::move(reports_));
    }
    return tcmalloc::Profile();
  }

  void ReportMalloc(const tcmalloc_internal::StackTrace& stack_trace) {
    // store sampled alloc in the hashmap
    DeallocationSampleRecord& allocation =
        allocs_[stack_trace.sampled_alloc_handle];

    allocation.allocated_size = stack_trace.allocated_size;
    allocation.requested_size = stack_trace.requested_size;
    allocation.requested_alignment = stack_trace.requested_alignment;
    allocation.depth = stack_trace.depth;
    memcpy(allocation.stack, stack_trace.stack,
           sizeof(void*) * std::min(static_cast<int64_t>(stack_trace.depth),
                                    kMaxStackDepth));
    // TODO(mmaas): Do we need to worry about b/65384231 anymore?
    allocation.creation_time = stack_trace.allocation_time;
    allocation.cpu_id = tcmalloc_internal::subtle::percpu::GetCurrentCpu();
    allocation.vcpu_id =
        tcmalloc_internal::subtle::percpu::GetCurrentVirtualCpuUnsafe();
    allocation.l3_id = GetL3Id(allocation.cpu_id);
    allocation.numa_id = GetNumaId(allocation.cpu_id);
    allocation.thread_id = absl::base_internal::GetTID();
    // We divide by the requested size to obtain the number of allocations.
    // TODO(b/248332543): Consider using AllocatedBytes from sampler.h.
    allocation.weight = static_cast<double>(stack_trace.weight) /
                        (stack_trace.requested_size + 1);
  }

  void ReportFree(tcmalloc_internal::AllocHandle handle) {
    auto it = allocs_.find(handle);

    // Handle the case that we observed the deallocation but not the allocation
    if (it == allocs_.end()) {
      return;
    }

    DeallocationSampleRecord sample = it->second;
    allocs_.erase(it);

    DeallocationSampleRecord deallocation;
    deallocation.allocated_size = sample.allocated_size;
    deallocation.requested_alignment = sample.requested_alignment;
    deallocation.requested_size = sample.requested_size;
    deallocation.creation_time = absl::Now();
    deallocation.cpu_id = tcmalloc_internal::subtle::percpu::GetCurrentCpu();
    deallocation.vcpu_id =
        tcmalloc_internal::subtle::percpu::GetCurrentVirtualCpuUnsafe();
    deallocation.l3_id = GetL3Id(deallocation.cpu_id);
    deallocation.numa_id = GetNumaId(deallocation.cpu_id);
    deallocation.thread_id = absl::base_internal::GetTID();
    deallocation.depth =
        absl::GetStackTrace(deallocation.stack, kMaxStackDepth, 1);

    reports_->AddTrace(sample, deallocation);
  }
};

void DeallocationProfilerList::Add(DeallocationProfiler* profiler) {
  AllocationGuardSpinLockHolder h(&profilers_lock_);
  profiler->next_ = first_;
  first_ = profiler;

  // Whenever a new profiler is created, we seed it with live allocations.
  tcmalloc_internal::tc_globals.sampled_allocation_recorder().Iterate(
      [profiler](
          const tcmalloc_internal::SampledAllocation& sampled_allocation) {
        profiler->ReportMalloc(sampled_allocation.sampled_stack);
      });
}

// This list is very short and we're nowhere near a hot path, just walk
void DeallocationProfilerList::Remove(DeallocationProfiler* profiler) {
  AllocationGuardSpinLockHolder h(&profilers_lock_);
  DeallocationProfiler** link = &first_;
  DeallocationProfiler* cur = first_;
  while (cur != profiler) {
    TC_CHECK_NE(cur, nullptr);
    link = &cur->next_;
    cur = cur->next_;
  }
  *link = profiler->next_;
}

void DeallocationProfilerList::ReportMalloc(
    const tcmalloc_internal::StackTrace& stack_trace) {
  AllocationGuardSpinLockHolder h(&profilers_lock_);
  DeallocationProfiler* cur = first_;
  while (cur != nullptr) {
    cur->ReportMalloc(stack_trace);
    cur = cur->next_;
  }
}

void DeallocationProfilerList::ReportFree(
    tcmalloc_internal::AllocHandle handle) {
  AllocationGuardSpinLockHolder h(&profilers_lock_);
  DeallocationProfiler* cur = first_;
  while (cur != nullptr) {
    cur->ReportFree(handle);
    cur = cur->next_;
  }
}

// Initialize static variables
absl::base_internal::LowLevelAlloc::Arena*
    DeallocationProfiler::MyAllocator::arena_ = nullptr;
uint32_t DeallocationProfiler::MyAllocator::refcount_ = 0;
ABSL_CONST_INIT SpinLock DeallocationProfiler::MyAllocator::arena_lock_(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

void DeallocationProfiler::DeallocationStackTraceTable::StopAndRecord(
    const AllocsTable& allocs) {
  stop_time_ = absl::Now();

  // Insert a dummy DeallocationSampleRecord since the table stores pairs. This
  // allows us to make minimal changes to the rest of the sample processing
  // steps reducing special casing for censored samples. This also allows us to
  // aggregate censored samples just like regular deallocation samples.
  const DeallocationSampleRecord censored{
      .creation_time = stop_time_,
  };
  for (const auto& [unused, alloc] : allocs) {
    AddTrace(alloc, censored);
  }
}

void DeallocationProfiler::DeallocationStackTraceTable::AddTrace(
    const DeallocationSampleRecord& alloc_trace,
    const DeallocationSampleRecord& dealloc_trace) {
  CpuThreadMatchingStatus status =
      CpuThreadMatchingStatus(alloc_trace.cpu_id == dealloc_trace.cpu_id,
                              alloc_trace.vcpu_id == dealloc_trace.vcpu_id,
                              alloc_trace.l3_id == dealloc_trace.l3_id,
                              alloc_trace.numa_id == dealloc_trace.numa_id,
                              alloc_trace.thread_id == dealloc_trace.thread_id);

  // Initialize a default rpc matched status.
  RpcMatchingStatus rpc_status(/*alloc=*/0, /*dealloc=*/0);

  const int index = ComputeIndex(status, rpc_status);

  DeallocationStackTraceTable::Value& v =
      table_[DeallocationStackTraceTable::Key(alloc_trace, dealloc_trace)];

  const absl::Duration life_time =
      dealloc_trace.creation_time - alloc_trace.creation_time;
  double life_time_ns = absl::ToDoubleNanoseconds(life_time);

  // Update mean and variance using Welford’s online algorithm.
  TC_ASSERT_LT(index, ABSL_ARRAYSIZE(v.counts));

  double old_mean_ns = v.mean_life_times_ns[index];
  v.mean_life_times_ns[index] +=
      (life_time_ns - old_mean_ns) / static_cast<double>(v.counts[index] + 1);
  v.variance_life_times_ns[index] +=
      (life_time_ns - v.mean_life_times_ns[index]) *
      (v.mean_life_times_ns[index] - old_mean_ns);

  v.min_life_times_ns[index] =
      std::min(v.min_life_times_ns[index], life_time_ns);
  v.max_life_times_ns[index] =
      std::max(v.max_life_times_ns[index], life_time_ns);
  v.counts[index]++;
}

void DeallocationProfiler::DeallocationStackTraceTable::Iterate(
    absl::FunctionRef<void(const Profile::Sample&)> func) const {
  uint64_t pair_id = 1;

  for (auto& it : table_) {
    const Key& k = it.first;
    const Value& v = it.second;

    // Report total bytes that are a multiple of the object size.
    size_t allocated_size = k.alloc.allocated_size;

    for (const auto& matching_case : kAllCases) {
      const int index = ComputeIndex(matching_case.first, matching_case.second);
      if (v.counts[index] == 0) {
        continue;
      }

      uintptr_t bytes =
          std::lround(v.counts[index] * k.alloc.weight * allocated_size);
      int64_t count = (bytes + allocated_size - 1) / allocated_size;
      int64_t sum = count * allocated_size;

      // The variance should be >= 0, but it's not impossible that it drops
      // below 0 for numerical reasons. We don't want to crash in this case,
      // so we ensure to return 0 if this happens.
      double stddev_life_time_ns =
          sqrt(std::max(0.0, v.variance_life_times_ns[index] /
                                 static_cast<double>((v.counts[index]))));

      const auto bucketize = internal::LifetimeNsToBucketedDuration;
      Profile::Sample sample{
          .sum = sum,
          .requested_size = k.alloc.requested_size,
          .requested_alignment = k.alloc.requested_alignment,
          .allocated_size = allocated_size,
          .profile_id = pair_id++,
          // Set the is_censored flag so that when we create a proto
          // sample later we can treat the *_lifetime accordingly.
          .is_censored = (k.dealloc.depth == 0),
          .avg_lifetime = bucketize(v.mean_life_times_ns[index]),
          .stddev_lifetime = bucketize(stddev_life_time_ns),
          .min_lifetime = bucketize(v.min_life_times_ns[index]),
          .max_lifetime = bucketize(v.max_life_times_ns[index])};
      // Only set the cpu and thread matched flags if the sample is not
      // censored.
      if (!sample.is_censored) {
        sample.allocator_deallocator_physical_cpu_matched =
            matching_case.first.physical_cpu_matched;
        sample.allocator_deallocator_virtual_cpu_matched =
            matching_case.first.virtual_cpu_matched;
        sample.allocator_deallocator_l3_matched =
            matching_case.first.l3_matched;
        sample.allocator_deallocator_numa_matched =
            matching_case.first.numa_matched;
        sample.allocator_deallocator_thread_matched =
            matching_case.first.thread_matched;
      }

      // first for allocation
      sample.count = count;
      sample.depth = k.alloc.depth;
      std::copy(k.alloc.stack, k.alloc.stack + k.alloc.depth, sample.stack);
      func(sample);

      // If this is a right-censored allocation (i.e. we did not observe the
      // deallocation) then do not emit a deallocation sample pair.
      if (sample.is_censored) {
        continue;
      }

      // second for deallocation
      static_assert(
          std::is_signed<decltype(tcmalloc::Profile::Sample::count)>::value,
          "Deallocation samples are tagged with negative count values.");
      sample.count = -1 * count;
      sample.depth = k.dealloc.depth;
      std::copy(k.dealloc.stack, k.dealloc.stack + k.dealloc.depth,
                sample.stack);
      func(sample);
    }
  }
}

DeallocationSample::DeallocationSample(DeallocationProfilerList* list) {
  profiler_ = std::make_unique<DeallocationProfiler>(list);
}

tcmalloc::Profile DeallocationSample::Stop() && {
  if (profiler_ != nullptr) {
    tcmalloc::Profile profile = profiler_->Stop();
    profiler_.reset();
    return profile;
  }
  return tcmalloc::Profile();
}

namespace internal {

// Lifetimes below 1ns are truncated to 1ns.  Lifetimes between 1ns and 1ms
// are rounded to the next smaller power of 10.  Lifetimes above 1ms are rounded
// down to the nearest millisecond.
absl::Duration LifetimeNsToBucketedDuration(double lifetime_ns) {
  if (lifetime_ns < 1000000.0) {
    if (lifetime_ns <= 1) {
      // Avoid negatives.  We can't allocate in a negative amount of time or
      // even as quickly as a nanosecond (microbenchmarks of
      // allocation/deallocation in a tight loop are several nanoseconds), so
      // results this small indicate probable clock skew or other confounding
      // factors in the data.
      return absl::Nanoseconds(1);
    }

    for (uint64_t cutoff_ns = 10; cutoff_ns <= 1000000; cutoff_ns *= 10) {
      if (lifetime_ns < cutoff_ns) {
        return absl::Nanoseconds(cutoff_ns / 10);
      }
    }
  }

  // Round down to nearest millisecond.
  return absl::Nanoseconds(static_cast<uint64_t>(lifetime_ns / 1000000.0) *
                           1000000L);
}

}  // namespace internal
}  // namespace deallocationz
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

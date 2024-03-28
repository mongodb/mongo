// Copyright 2024 The TCMalloc Authors
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

#include "tcmalloc/internal/percpu_tcmalloc.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/functional/function_ref.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linux_syscall_support.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/sysinfo.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace subtle {
namespace percpu {

void TcmallocSlab::Init(
    size_t num_classes,
    absl::FunctionRef<void*(size_t, std::align_val_t)> alloc, void* slabs,
    absl::FunctionRef<size_t(size_t)> capacity, Shift shift) {
  TC_ASSERT(num_classes_ == 0 && num_classes != 0);
  num_classes_ = num_classes;
  if (UsingFlatVirtualCpus()) {
    virtual_cpu_id_offset_ = offsetof(kernel_rseq, vcpu_id);
  }
  stopped_ = new (alloc(sizeof(stopped_[0]) * NumCPUs(),
                        std::align_val_t{ABSL_CACHELINE_SIZE}))
      std::atomic<bool>[NumCPUs()];
  for (int cpu = NumCPUs() - 1; cpu >= 0; cpu--) {
    stopped_[cpu].store(false, std::memory_order_relaxed);
  }
  begins_ = static_cast<std::atomic<uint16_t>*>(alloc(
      sizeof(begins_[0]) * num_classes, std::align_val_t{ABSL_CACHELINE_SIZE}));
  InitSlabs(slabs, shift, capacity);

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  // This is needed only for tests that create/destroy slabs,
  // w/o this cpu_id_start may contain wrong offset for a new slab.
  __rseq_abi.cpu_id_start = 0;
#endif
}

void TcmallocSlab::InitSlabs(void* slabs, Shift shift,
                             absl::FunctionRef<size_t(size_t)> capacity) {
  slabs_and_shift_.store({slabs, shift}, std::memory_order_relaxed);
  size_t consumed_bytes = (num_classes_ * sizeof(Header) + sizeof(void*) - 1) &
                          ~(sizeof(void*) - 1);
  bool prev_empty = false;
  for (size_t size_class = 1; size_class < num_classes_; ++size_class) {
    size_t cap = capacity(size_class);
    TC_CHECK_EQ(static_cast<uint16_t>(cap), cap);
    // One extra element for prefetch/begin marker.
    if (!prev_empty) {
      consumed_bytes += sizeof(void*);
    }
    prev_empty = cap == 0;
    begins_[size_class].store(consumed_bytes / sizeof(void*),
                              std::memory_order_relaxed);
    consumed_bytes += cap * sizeof(void*);
    if (consumed_bytes > (1 << ToUint8(shift))) {
      TC_BUG("per-CPU memory exceeded, have %v, need %v, size_class %v",
             1 << ToUint8(shift), consumed_bytes, size_class);
    }
  }
}

void TcmallocSlab::InitCpu(int cpu,
                           absl::FunctionRef<size_t(size_t)> capacity) {
  ScopedSlabCpuStop cpu_stop(*this, cpu);
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  InitCpuImpl(slabs, shift, cpu, capacity);
}

void TcmallocSlab::InitCpuImpl(void* slabs, Shift shift, int cpu,
                               absl::FunctionRef<size_t(size_t)> capacity) {
  TC_CHECK(stopped_[cpu].load(std::memory_order_relaxed));
  TC_CHECK_LE((1 << ToUint8(shift)), (1 << 16) * sizeof(void*));

  // Initialize prefetch target and compute the offsets for the
  // boundaries of each size class' cache.
  void* curr_slab = CpuMemoryStart(slabs, shift, cpu);
  void** elems = reinterpret_cast<void**>(
      (reinterpret_cast<uintptr_t>(GetHeader(slabs, shift, cpu, num_classes_)) +
       sizeof(void*) - 1) &
      ~(sizeof(void*) - 1));
  bool prev_empty = false;
  for (size_t size_class = 1; size_class < num_classes_; ++size_class) {
    size_t cap = capacity(size_class);
    TC_CHECK_EQ(static_cast<uint16_t>(cap), cap);

    // This item serves both as the marker of slab begin (Pop checks for low bit
    // set to understand that it reached begin), and as prefetching stub
    // (Pop prefetches the previous element and prefetching an invalid pointer
    // is slow, this is a valid pointer for prefetching).
    if (!prev_empty) {
      *elems = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(elems) |
                                       kBeginMark);
      ++elems;
    }
    prev_empty = cap == 0;

    Header hdr = {};
    hdr.current = elems - reinterpret_cast<void**>(curr_slab);
    hdr.end = hdr.current;
    StoreHeader(GetHeader(slabs, shift, cpu, size_class), hdr);

    elems += cap;
    const size_t bytes_used_on_curr_slab =
        reinterpret_cast<char*>(elems) - reinterpret_cast<char*>(curr_slab);
    if (bytes_used_on_curr_slab > (1 << ToUint8(shift))) {
      TC_BUG("per-CPU memory exceeded, have %v, need %v", 1 << ToUint8(shift),
             bytes_used_on_curr_slab);
    }
  }
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
std::pair<int, bool> TcmallocSlab::CacheCpuSlabSlow() {
  TC_ASSERT(!(tcmalloc_slabs & TCMALLOC_CACHED_SLABS_MASK));
  int cpu = -1;
  for (;;) {
    tcmalloc_slabs = TCMALLOC_CACHED_SLABS_MASK;
    CompilerBarrier();
    cpu = GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset_);
    auto slabs_and_shift = slabs_and_shift_.load(std::memory_order_relaxed);
    const auto [slabs, shift] = slabs_and_shift.Get();
    void* start = CpuMemoryStart(slabs, shift, cpu);
    uintptr_t new_val =
        reinterpret_cast<uintptr_t>(start) | TCMALLOC_CACHED_SLABS_MASK;
    if (!StoreCurrentCpu(&tcmalloc_slabs, new_val)) {
      continue;
    }
    // If ResizeSlabs is concurrently modifying slabs_and_shift_, we may
    // cache the offset with the shift that won't match slabs pointer used
    // by Push/Pop operations later. To avoid this, we check stopped_ after
    // the calculation. Coupled with setting of stopped_ and a Fence
    // in ResizeSlabs, this prevents possibility of mismatching shift/slabs.
    CompilerBarrier();
    if (stopped_[cpu].load(std::memory_order_acquire)) {
      tcmalloc_slabs = 0;
      return {-1, true};
    }
    // Ensure that we've cached the current slabs pointer.
    // Without this check the following bad interleaving is possible.
    // Thread 1 executes ResizeSlabs, stops all CPUs and executes Fence.
    // Now thread 2 executes CacheCpuSlabSlow, reads old slabs and caches
    // the pointer. Now thread 1 stores the new slabs pointer and resets
    // stopped_[cpu]. Now thread 2 resumes, checks that stopped_[cpu] is not
    // set and proceeds with using the old slabs pointer. Since we use
    // acquire/release on stopped_[cpu], if this thread observes reset
    // stopped_[cpu], it's also guaranteed to observe the new value of slabs
    // and retry. In the very unlikely case that slabs are resized twice in
    // between (to new slabs and then back to old slabs), the check below will
    // not lead to a retry, but changing slabs back also implies another Fence,
    // so this thread won't have old slabs cached already (Fence invalidates
    // the cached pointer).
    if (slabs_and_shift != slabs_and_shift_.load(std::memory_order_relaxed)) {
      continue;
    }
    return {cpu, true};
  }
}
#endif

void TcmallocSlab::DrainCpu(void* slabs, Shift shift, int cpu,
                            DrainHandler drain_handler) {
  TC_ASSERT(stopped_[cpu].load(std::memory_order_relaxed));
  for (size_t size_class = 1; size_class < num_classes_; ++size_class) {
    uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
    auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    if (hdr.current == 0) {
      continue;
    }
    const size_t size = hdr.current - begin;
    const size_t cap = hdr.end - begin;

    void** batch =
        reinterpret_cast<void**>(CpuMemoryStart(slabs, shift, cpu)) + begin;
    TSANAcquireBatch(batch, size);
    drain_handler(cpu, size_class, batch, size, cap);
    hdr.current = begin;
    hdr.end = begin;
    StoreHeader(hdrp, hdr);
  }
}

auto TcmallocSlab::ResizeSlabs(Shift new_shift, void* new_slabs,
                               absl::FunctionRef<size_t(size_t)> capacity,
                               absl::FunctionRef<bool(size_t)> populated,
                               DrainHandler drain_handler) -> ResizeSlabsInfo {
  // Phase 1: Stop all CPUs and initialize any CPUs in the new slab that have
  // already been populated in the old slab.
  const auto [old_slabs, old_shift] =
      GetSlabsAndShift(std::memory_order_relaxed);
  TC_ASSERT_NE(new_shift, old_shift);
  const int num_cpus = NumCPUs();
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    TC_CHECK(!stopped_[cpu].load(std::memory_order_relaxed));
    stopped_[cpu].store(true, std::memory_order_relaxed);
    if (populated(cpu)) {
      InitCpuImpl(new_slabs, new_shift, cpu, capacity);
    }
  }
  FenceAllCpus();

  // Phase 2: Return pointers from the old slab to the TransferCache.
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    DrainCpu(old_slabs, old_shift, cpu, drain_handler);
  }

  // Phase 3: Atomically update slabs and shift.
  InitSlabs(new_slabs, new_shift, capacity);

  // Phase 4: Re-start all CPUs.
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    stopped_[cpu].store(false, std::memory_order_release);
  }

  return {old_slabs, GetSlabsAllocSize(old_shift, num_cpus)};
}

void* TcmallocSlab::Destroy(
    absl::FunctionRef<void(void*, size_t, std::align_val_t)> free) {
  free(stopped_, sizeof(stopped_[0]) * NumCPUs(),
       std::align_val_t{ABSL_CACHELINE_SIZE});
  stopped_ = nullptr;
  free(begins_, sizeof(begins_[0]) * num_classes_,
       std::align_val_t{ABSL_CACHELINE_SIZE});
  begins_ = nullptr;
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  free(slabs, GetSlabsAllocSize(shift, NumCPUs()), kPhysicalPageAlign);
  slabs_and_shift_.store({nullptr, shift}, std::memory_order_relaxed);
  return slabs;
}

size_t TcmallocSlab::GrowOtherCache(
    int cpu, size_t size_class, size_t len,
    absl::FunctionRef<size_t(uint8_t)> max_capacity) {
  TC_ASSERT(stopped_[cpu].load(std::memory_order_relaxed));
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t max_cap = max_capacity(ToUint8(shift));
  auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
  Header hdr = LoadHeader(hdrp);
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  uint16_t to_grow = std::min<uint16_t>(len, max_cap - (hdr.end - begin));
  hdr.end += to_grow;
  StoreHeader(hdrp, hdr);
  return to_grow;
}

size_t TcmallocSlab::ShrinkOtherCache(int cpu, size_t size_class, size_t len,
                                      ShrinkHandler shrink_handler) {
  TC_ASSERT(stopped_[cpu].load(std::memory_order_relaxed));
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);

  auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
  Header hdr = LoadHeader(hdrp);

  // If we do not have len number of items to shrink, we try to pop items from
  // the list first to create enough capacity that can be shrunk.
  // If we pop items, we also execute callbacks.
  const uint16_t unused = hdr.end - hdr.current;
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  if (unused < len && hdr.current != begin) {
    uint16_t pop = std::min<uint16_t>(len - unused, hdr.current - begin);
    void** batch = reinterpret_cast<void**>(CpuMemoryStart(slabs, shift, cpu)) +
                   hdr.current - pop;
    TSANAcquireBatch(batch, pop);
    shrink_handler(size_class, batch, pop);
    hdr.current -= pop;
  }

  // Shrink the capacity.
  const uint16_t to_shrink = std::min<uint16_t>(len, hdr.end - hdr.current);
  hdr.end -= to_shrink;
  StoreHeader(hdrp, hdr);
  return to_shrink;
}

void TcmallocSlab::Drain(int cpu, DrainHandler drain_handler) {
  ScopedSlabCpuStop cpu_stop(*this, cpu);
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  DrainCpu(slabs, shift, cpu, drain_handler);
}

void TcmallocSlab::StopCpu(int cpu) {
  TC_ASSERT(cpu >= 0 && cpu < NumCPUs(), "cpu=%d", cpu);
  TC_CHECK(!stopped_[cpu].load(std::memory_order_relaxed));
  stopped_[cpu].store(true, std::memory_order_relaxed);
  FenceCpu(cpu, virtual_cpu_id_offset_);
}

void TcmallocSlab::StartCpu(int cpu) {
  TC_ASSERT(cpu >= 0 && cpu < NumCPUs(), "cpu=%d", cpu);
  TC_ASSERT(stopped_[cpu].load(std::memory_order_relaxed));
  stopped_[cpu].store(false, std::memory_order_release);
}

PerCPUMetadataState TcmallocSlab::MetadataMemoryUsage() const {
  PerCPUMetadataState result;
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  size_t slabs_size = GetSlabsAllocSize(shift, NumCPUs());
  size_t stopped_size = NumCPUs() * sizeof(stopped_[0]);
  size_t begins_size = num_classes_ * sizeof(begins_[0]);
  result.virtual_size = stopped_size + slabs_size + begins_size;
  result.resident_size = MInCore::residence(slabs, slabs_size);
  return result;
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

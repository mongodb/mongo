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

#include "tcmalloc/system-alloc.h"

#include <asm/unistd.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/numeric/bits.h"
#include "absl/strings/str_format.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"

// On systems (like freebsd) that don't define MAP_ANONYMOUS, use the old
// form of the name instead.
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifndef MADV_FREE
#define MADV_FREE 8
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// The <sys/prctl.h> on some systems may not define these macros yet even though
// the kernel may have support for the new PR_SET_VMA syscall, so we explicitly
// define them here.
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif

#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

// Solaris has a bug where it doesn't declare madvise() for C++.
//    http://www.opensolaris.org/jive/thread.jspa?threadID=21035&tstart=0
#if defined(__sun) && defined(__SVR4)
#include <sys/types.h>
extern "C" int madvise(caddr_t, size_t, int);
#endif

#ifdef __linux__
#include <linux/mempolicy.h>
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Check that no bit is set at position ADDRESS_BITS or higher.
template <int ADDRESS_BITS>
void CheckAddressBits(uintptr_t ptr) {
  TC_ASSERT_EQ(ptr >> ADDRESS_BITS, 0);
}

// Specialize for the bit width of a pointer to avoid undefined shift.
template <>
ABSL_ATTRIBUTE_UNUSED void CheckAddressBits<8 * sizeof(void*)>(uintptr_t ptr) {}

static_assert(kAddressBits <= 8 * sizeof(void*),
              "kAddressBits must be smaller than the pointer size");

// Structure for discovering alignment
union MemoryAligner {
  void* p;
  double d;
  size_t s;
} ABSL_CACHELINE_ALIGNED;

static_assert(sizeof(MemoryAligner) < kMinSystemAlloc,
              "hugepage alignment too small");

ABSL_CONST_INIT absl::base_internal::SpinLock spinlock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

// Page size is initialized on demand
ABSL_CONST_INIT size_t preferred_alignment ABSL_GUARDED_BY(spinlock) = 0;

// The current region factory.
ABSL_CONST_INIT AddressRegionFactory* region_factory ABSL_GUARDED_BY(spinlock) =
    nullptr;

// Rounds size down to a multiple of alignment.
size_t RoundDown(const size_t size, const size_t alignment) {
  // Checks that the alignment has only one bit set.
  TC_ASSERT(absl::has_single_bit(alignment));
  return (size) & ~(alignment - 1);
}

// Rounds size up to a multiple of alignment.
size_t RoundUp(const size_t size, const size_t alignment) {
  return RoundDown(size + alignment - 1, alignment);
}

class MmapRegion final : public AddressRegion {
 public:
  MmapRegion(uintptr_t start, size_t size, AddressRegionFactory::UsageHint hint)
      : start_(start), free_size_(size), hint_(hint) {}
  std::pair<void*, size_t> Alloc(size_t size, size_t alignment) override
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(spinlock);
  ~MmapRegion() override = default;

 private:
  const uintptr_t start_;
  size_t free_size_;
  const AddressRegionFactory::UsageHint hint_;
};

class MmapRegionFactory final : public AddressRegionFactory {
 public:
  AddressRegion* Create(void* start, size_t size, UsageHint hint) override;
  size_t GetStats(absl::Span<char> buffer) override;
  size_t GetStatsInPbtxt(absl::Span<char> buffer) override;
  ~MmapRegionFactory() override = default;

 private:
  std::atomic<size_t> bytes_reserved_{0};
};
ABSL_CONST_INIT std::aligned_storage<sizeof(MmapRegionFactory),
                                     alignof(MmapRegionFactory)>::type
    mmap_space ABSL_GUARDED_BY(spinlock){};

class RegionManager {
 public:
  std::pair<void*, size_t> Alloc(size_t size, size_t alignment, MemoryTag tag)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(spinlock);

  void DiscardMappedRegions() ABSL_EXCLUSIVE_LOCKS_REQUIRED(spinlock) {
    std::fill(normal_region_.begin(), normal_region_.end(), nullptr);
    sampled_region_ = nullptr;
    cold_region_ = nullptr;
    metadata_region_ = nullptr;
  }

 private:
  // Checks that there is sufficient space available in the reserved region
  // for the next allocation, if not allocate a new region.
  // Then returns a pointer to the new memory.
  std::pair<void*, size_t> Allocate(size_t size, size_t alignment,
                                    MemoryTag tag)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(spinlock);

  std::array<AddressRegion*, kNumaPartitions> normal_region_{{nullptr}};
  AddressRegion* sampled_region_{nullptr};
  AddressRegion* cold_region_{nullptr};
  AddressRegion* metadata_region_{nullptr};
};
ABSL_CONST_INIT
std::aligned_storage<sizeof(RegionManager), alignof(RegionManager)>::type
    region_manager_space ABSL_GUARDED_BY(spinlock){};
ABSL_CONST_INIT RegionManager* region_manager ABSL_GUARDED_BY(spinlock) =
    nullptr;

std::pair<void*, size_t> MmapRegion::Alloc(size_t request_size,
                                           size_t alignment) {
  // Align on kMinSystemAlloc boundaries to reduce external fragmentation for
  // future allocations.
  size_t size = RoundUp(request_size, kMinSystemAlloc);
  if (size < request_size) return {nullptr, 0};
  alignment = std::max(alignment, preferred_alignment);

  // Tries to allocate size bytes from the end of [start_, start_ + free_size_),
  // aligned to alignment.
  uintptr_t end = start_ + free_size_;
  uintptr_t result = end - size;
  if (result > end) return {nullptr, 0};  // Underflow.
  result &= ~(alignment - 1);
  if (result < start_) return {nullptr, 0};  // Out of memory in region.
  size_t actual_size = end - result;

  TC_ASSERT_EQ(result % GetPageSize(), 0);
  void* result_ptr = reinterpret_cast<void*>(result);
  if (mprotect(result_ptr, actual_size, PROT_READ | PROT_WRITE) != 0) {
    TC_LOG("mprotect(%p, %v) failed (%s)", result_ptr, actual_size,
           strerror(errno));
    return {nullptr, 0};
  }
  // For cold regions (kInfrequentAccess) and sampled regions
  // (kInfrequentAllocation), we want as granular of access telemetry as
  // possible; this hint means we can get 4kiB granularity instead of 2MiB.
  if (hint_ == AddressRegionFactory::UsageHint::kInfrequentAccess ||
      hint_ == AddressRegionFactory::UsageHint::kInfrequentAllocation) {
    // This is only advisory, so ignore the error.
    ErrnoRestorer errno_restorer;
    (void)madvise(result_ptr, actual_size, MADV_NOHUGEPAGE);
  }
  free_size_ -= actual_size;
  return {result_ptr, actual_size};
}

AddressRegion* MmapRegionFactory::Create(void* start, size_t size,
                                         UsageHint hint) {
  void* region_space = MallocInternal(sizeof(MmapRegion));
  if (!region_space) return nullptr;
  bytes_reserved_.fetch_add(size, std::memory_order_relaxed);
  return new (region_space)
      MmapRegion(reinterpret_cast<uintptr_t>(start), size, hint);
}

size_t MmapRegionFactory::GetStats(absl::Span<char> buffer) {
  Printer printer(buffer.data(), buffer.size());
  size_t allocated = bytes_reserved_.load(std::memory_order_relaxed);
  constexpr double MiB = 1048576.0;
  printer.printf("MmapSysAllocator: %zu bytes (%.1f MiB) reserved\n", allocated,
                 allocated / MiB);

  return printer.SpaceRequired();
}

size_t MmapRegionFactory::GetStatsInPbtxt(absl::Span<char> buffer) {
  Printer printer(buffer.data(), buffer.size());
  size_t allocated = bytes_reserved_.load(std::memory_order_relaxed);
  printer.printf(" mmap_sys_allocator: %lld\n", allocated);

  return printer.SpaceRequired();
}

static AddressRegionFactory::UsageHint TagToHint(MemoryTag tag) {
  using UsageHint = AddressRegionFactory::UsageHint;
  switch (tag) {
    case MemoryTag::kNormal:
      if (tc_globals.numa_topology().numa_aware()) {
        return UsageHint::kNormalNumaAwareS0;
      }
      return UsageHint::kNormal;
      break;
    case MemoryTag::kNormalP1:
      if (tc_globals.numa_topology().numa_aware()) {
        return UsageHint::kNormalNumaAwareS1;
      }
      return UsageHint::kNormal;
      break;
    case MemoryTag::kSampled:
      return UsageHint::kInfrequentAllocation;
      break;
    case MemoryTag::kCold:
      return UsageHint::kInfrequentAccess;
    case MemoryTag::kMetadata:
      return UsageHint::kInfrequentAllocation;
  }

  ASSUME(false);
  __builtin_unreachable();
}

std::pair<void*, size_t> RegionManager::Alloc(size_t request_size,
                                              size_t alignment,
                                              const MemoryTag tag) {
  constexpr uintptr_t kTagFree = uintptr_t{1} << kTagShift;

  // We do not support size or alignment larger than kTagFree.
  // TODO(b/141325493): Handle these large allocations.
  if (request_size > kTagFree || alignment > kTagFree) return {nullptr, 0};

  // If we are dealing with large sizes, or large alignments we do not
  // want to throw away the existing reserved region, so instead we
  // return a new region specifically targeted for the request.
  if (request_size > kMinMmapAlloc || alignment > kMinMmapAlloc) {
    // Align on kMinSystemAlloc boundaries to reduce external fragmentation for
    // future allocations.
    size_t size = RoundUp(request_size, kMinSystemAlloc);
    if (size < request_size) return {nullptr, 0};
    alignment = std::max(alignment, preferred_alignment);
    void* ptr = MmapAligned(size, alignment, tag);
    if (!ptr) return {nullptr, 0};

    const auto region_type = TagToHint(tag);
    AddressRegion* region = region_factory->Create(ptr, size, region_type);
    if (!region) {
      munmap(ptr, size);
      return {nullptr, 0};
    }
    std::pair<void*, size_t> result = region->Alloc(size, alignment);
    if (result.first != nullptr) {
      TC_ASSERT_EQ(result.first, ptr);
      TC_ASSERT_EQ(result.second, size);
    } else {
      TC_ASSERT_EQ(result.second, 0);
    }
    return result;
  }
  return Allocate(request_size, alignment, tag);
}

std::pair<void*, size_t> RegionManager::Allocate(size_t size, size_t alignment,
                                                 const MemoryTag tag) {
  AddressRegion*& region = *[&]() {
    switch (tag) {
      case MemoryTag::kNormal:
        return &normal_region_[0];
      case MemoryTag::kNormalP1:
        return &normal_region_[1];
      case MemoryTag::kSampled:
        return &sampled_region_;
      case MemoryTag::kCold:
        return &cold_region_;
      case MemoryTag::kMetadata:
        return &metadata_region_;
    }

    ASSUME(false);
    __builtin_unreachable();
  }();
  // For sizes that fit in our reserved range first of all check if we can
  // satisfy the request from what we have available.
  if (region) {
    std::pair<void*, size_t> result = region->Alloc(size, alignment);
    if (result.first) return result;
  }

  // Allocation failed so we need to reserve more memory.
  // Reserve new region and try allocation again.
  void* ptr = MmapAligned(kMinMmapAlloc, kMinMmapAlloc, tag);
  if (!ptr) return {nullptr, 0};

  const auto region_type = TagToHint(tag);
  region = region_factory->Create(ptr, kMinMmapAlloc, region_type);
  if (!region) {
    munmap(ptr, kMinMmapAlloc);
    return {nullptr, 0};
  }
  return region->Alloc(size, alignment);
}

void InitSystemAllocatorIfNecessary() ABSL_EXCLUSIVE_LOCKS_REQUIRED(spinlock) {
  if (region_factory) return;
  // Sets the preferred alignment to be the largest of either the alignment
  // returned by mmap() or our minimum allocation size. The minimum allocation
  // size is usually a multiple of page size, but this need not be true for
  // SMALL_BUT_SLOW where we do not allocate in units of huge pages.
  preferred_alignment = std::max(GetPageSize(), kMinSystemAlloc);
  region_manager = new (&region_manager_space) RegionManager();
  region_factory = new (&mmap_space) MmapRegionFactory();
}

// Bind the memory region spanning `size` bytes starting from `base` to NUMA
// nodes assigned to `partition`. Returns zero upon success, or a standard
// error code upon failure.
void BindMemory(void* const base, const size_t size, const size_t partition) {
  auto& topology = tc_globals.numa_topology();

  // If NUMA awareness is unavailable or disabled, or the user requested that
  // we don't bind memory then do nothing.
  const NumaBindMode bind_mode = topology.bind_mode();
  if (!topology.numa_aware() || bind_mode == NumaBindMode::kNone) {
    return;
  }

  const uint64_t nodemask = topology.GetPartitionNodes(partition);
  int err =
      syscall(__NR_mbind, base, size, MPOL_BIND | MPOL_F_STATIC_NODES,
              &nodemask, sizeof(nodemask) * 8, MPOL_MF_STRICT | MPOL_MF_MOVE);
  if (err == 0) {
    return;
  }

  if (bind_mode == NumaBindMode::kAdvisory) {
    TC_LOG("Warning: Unable to mbind memory (errno=%d, base=%p, nodemask=%v)",
           errno, base, nodemask);
    return;
  }

  TC_ASSERT_EQ(bind_mode, NumaBindMode::kStrict);
  TC_BUG("Unable to mbind memory (errno=%d, base=%p, nodemask=%v)", errno, base,
         nodemask);
}

ABSL_CONST_INIT std::atomic<int> system_release_errors(0);

int MapFixedNoReplaceFlagAvailable() {
  ABSL_CONST_INIT static int noreplace_flag;
  ABSL_CONST_INIT static absl::once_flag flag;

  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    void* ptr =
        mmap(nullptr, kPageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TC_CHECK_NE(ptr, MAP_FAILED);

    // Try to map over ptr.  If we get a different address, we don't have
    // MAP_FIXED_NOREPLACE.
    //
    // We try to specify a region that overlaps with ptr, but adjust the start
    // downward so it doesn't.  This allows us to detect if the pre-4.19 bug
    // (https://github.com/torvalds/linux/commit/7aa867dd89526e9cfd9714d8b9b587c016eaea34)
    // is present.
    uintptr_t uptr = reinterpret_cast<uintptr_t>(ptr);
    TC_CHECK_GT(uptr, kPageSize);
    void* target = reinterpret_cast<void*>(uptr - kPageSize);

    void* ptr2 = mmap(target, 2 * kPageSize, PROT_NONE,
                      MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const bool rejected = ptr2 == MAP_FAILED;
    if (!rejected) {
      if (ptr2 == target) {
        // [ptr2, 2 * kPageSize] overlaps with [ptr, kPageSize], so we only need
        // to unmap [ptr2, kPageSize].  The second call to munmap below will
        // unmap the rest.
        munmap(ptr2, kPageSize);
      } else {
        munmap(ptr2, 2 * kPageSize);
      }
    }
    munmap(ptr, kPageSize);

    noreplace_flag = rejected ? MAP_FIXED_NOREPLACE : 0;
  });

  return noreplace_flag;
}

}  // namespace

AddressRange SystemAlloc(size_t bytes, size_t alignment, const MemoryTag tag) {
  // If default alignment is set request the minimum alignment provided by
  // the system.
  alignment = std::max(alignment, GetPageSize());

  // Discard requests that overflow
  if (bytes + alignment < bytes) return {nullptr, 0};

  AllocationGuardSpinLockHolder lock_holder(&spinlock);

  InitSystemAllocatorIfNecessary();

  auto [result, actual_bytes] = region_manager->Alloc(bytes, alignment, tag);

  if (result != nullptr) {
    CheckAddressBits<kAddressBits>(reinterpret_cast<uintptr_t>(result) +
                                   actual_bytes - 1);
    TC_ASSERT_EQ(GetMemoryTag(result), tag);
  }
  return {result, actual_bytes};
}

static bool ReleasePages(void* start, size_t length) {
  ErrnoRestorer errno_restorer;

  int ret;
  // Note -- ignoring most return codes, because if this fails it
  // doesn't matter...
  // Moreover, MADV_REMOVE *will* fail (with EINVAL) on private memory,
  // but that's harmless.
#ifdef MADV_REMOVE
  // MADV_REMOVE deletes any backing storage for tmpfs or anonymous shared
  // memory.
  do {
    ret = madvise(start, length, MADV_REMOVE);
  } while (ret == -1 && errno == EAGAIN);

  if (ret == 0) {
    return true;
  }
#endif

#ifdef MADV_FREE
  const bool do_madvfree = []() {
    if (Parameters::madvise_free()) {
      return true;
    }

    switch (Parameters::madvise()) {
      case MadvisePreference::kFreeAndDontNeed:
      case MadvisePreference::kFreeOnly:
        return true;
      case MadvisePreference::kDontNeed:
        return false;
    }

    ABSL_UNREACHABLE();
  }();

  if (do_madvfree) {
    do {
      ret = madvise(start, length, MADV_FREE);
    } while (ret == -1 && errno == EAGAIN);
  }
#endif
#ifdef MADV_DONTNEED
  const bool do_madvdontneed = []() {
    switch (Parameters::madvise()) {
      case MadvisePreference::kDontNeed:
      case MadvisePreference::kFreeAndDontNeed:
        return true;
      case MadvisePreference::kFreeOnly:
        return false;
    }

    ABSL_UNREACHABLE();
  }();

  // MADV_DONTNEED drops page table info and any anonymous pages.
  if (do_madvdontneed) {
    do {
      ret = madvise(start, length, MADV_DONTNEED);
    } while (ret == -1 && errno == EAGAIN);
  }

  if (ret == 0) {
    return true;
  }
#endif

  return false;
}

int SystemReleaseErrors() {
  return system_release_errors.load(std::memory_order_relaxed);
}

bool SystemRelease(void* start, size_t length) {
  bool result = false;

#if defined(MADV_DONTNEED) || defined(MADV_REMOVE)
  ErrnoRestorer errno_restorer;
  const size_t pagemask = GetPageSize() - 1;

  size_t new_start = reinterpret_cast<size_t>(start);
  size_t end = new_start + length;
  size_t new_end = end;

  // Round up the starting address and round down the ending address
  // to be page aligned:
  new_start = (new_start + GetPageSize() - 1) & ~pagemask;
  new_end = new_end & ~pagemask;

  TC_ASSERT_EQ(new_start & pagemask, 0);
  TC_ASSERT_EQ(new_end & pagemask, 0);
  TC_ASSERT_GE(new_start, reinterpret_cast<size_t>(start));
  TC_ASSERT_LE(new_end, end);

  if (new_end > new_start) {
    void* new_ptr = reinterpret_cast<void*>(new_start);
    size_t new_length = new_end - new_start;

    if (!ReleasePages(new_ptr, new_length)) {
      // Try unlocking.
      int ret;
      do {
        ret = munlock(reinterpret_cast<char*>(new_start), new_end - new_start);
      } while (ret == -1 && errno == EAGAIN);

      if (ret != 0 || !ReleasePages(new_ptr, new_length)) {
        // If we fail to munlock *or* fail our second attempt at madvise,
        // increment our failure count.
        system_release_errors.fetch_add(1, std::memory_order_relaxed);
      } else {
        result = true;
      }
    } else {
      result = true;
    }
  }
#endif

  return result;
}

AddressRegionFactory* GetRegionFactory() {
  AllocationGuardSpinLockHolder lock_holder(&spinlock);
  InitSystemAllocatorIfNecessary();
  return region_factory;
}

void SetRegionFactory(AddressRegionFactory* factory) {
  AllocationGuardSpinLockHolder lock_holder(&spinlock);
  InitSystemAllocatorIfNecessary();
  region_manager->DiscardMappedRegions();
  region_factory = factory;
}

static uintptr_t RandomMmapHint(size_t size, size_t alignment,
                                const MemoryTag tag) {
  // Rely on kernel's mmap randomization to seed our RNG.
  ABSL_CONST_INIT static uintptr_t rnd;
  ABSL_CONST_INIT static absl::once_flag flag;

  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    void* seed =
        mmap(nullptr, kPageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (seed == MAP_FAILED) {
      TC_BUG("Initial mmap() reservation failed (errno=%v, size=%v)", errno,
             kPageSize);
    }
    munmap(seed, kPageSize);
    rnd = reinterpret_cast<uintptr_t>(seed);
  });

#if !defined(MEMORY_SANITIZER) && !defined(THREAD_SANITIZER)
  // We don't use the following bits:
  //
  //  *  The top bits that are forbidden for use by the hardware (or are
  //     required to be set to the same value as the next bit, which we also
  //     don't use).
  //
  //  *  Below that, the top highest the hardware allows us to use, since it is
  //     reserved for kernel space addresses.
  constexpr uintptr_t kAddrMask = (uintptr_t{1} << (kAddressBits - 1)) - 1;
#else
  // MSan and TSan use up all of the lower address space, so we allow use of
  // mid-upper address space when they're active.  This only matters for
  // TCMalloc-internal tests, since sanitizers install their own malloc/free.
  constexpr uintptr_t kAddrMask = (uintptr_t{0xF} << (kAddressBits - 5)) - 1;
#endif

  // Ensure alignment >= size so we're guaranteed the full mapping has the same
  // tag.
  alignment = absl::bit_ceil(std::max(alignment, size));

  rnd = ExponentialBiased::NextRandom(rnd);
  uintptr_t addr = rnd & kAddrMask & ~(alignment - 1) & ~kTagMask;
  addr |= static_cast<uintptr_t>(tag) << kTagShift;
  TC_ASSERT_EQ(GetMemoryTag(reinterpret_cast<const void*>(addr)), tag);
  return addr;
}

void* MmapAligned(size_t size, size_t alignment, const MemoryTag tag) {
  TC_ASSERT_LE(size, kTagMask);
  TC_ASSERT_LE(alignment, kTagMask);

  static uintptr_t next_sampled_addr = 0;
  static std::array<uintptr_t, kNumaPartitions> next_normal_addr = {0};
  static uintptr_t next_cold_addr = 0;
  static uintptr_t next_metadata_addr = 0;

  std::optional<int> numa_partition;
  uintptr_t& next_addr = *[&]() {
    switch (tag) {
      case MemoryTag::kSampled:
        return &next_sampled_addr;
      case MemoryTag::kNormalP0:
        numa_partition = 0;
        return &next_normal_addr[0];
      case MemoryTag::kNormalP1:
        numa_partition = 1;
        return &next_normal_addr[1];
      case MemoryTag::kCold:
        return &next_cold_addr;
      case MemoryTag::kMetadata:
        return &next_metadata_addr;
    }

    ASSUME(false);
    __builtin_unreachable();
  }();

  if (!next_addr || next_addr & (alignment - 1) ||
      GetMemoryTag(reinterpret_cast<void*>(next_addr)) != tag ||
      GetMemoryTag(reinterpret_cast<void*>(next_addr + size - 1)) != tag) {
    next_addr = RandomMmapHint(size, alignment, tag);
  }
  const int map_fixed_noreplace_flag = MapFixedNoReplaceFlagAvailable();
  void* hint;
  for (int i = 0; i < 1000; ++i) {
    hint = reinterpret_cast<void*>(next_addr);
    TC_ASSERT_EQ(GetMemoryTag(hint), tag);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | map_fixed_noreplace_flag;

    void* result = mmap(hint, size, PROT_NONE, flags, -1, 0);
    if (result == hint) {
      if (numa_partition.has_value()) {
        BindMemory(result, size, *numa_partition);
      }
      // Attempt to keep the next mmap contiguous in the common case.
      next_addr += size;
      TC_CHECK(kAddressBits == std::numeric_limits<uintptr_t>::digits ||
               next_addr <= uintptr_t{1} << kAddressBits);

      TC_ASSERT_EQ(reinterpret_cast<uintptr_t>(result) & (alignment - 1), 0);
      // Give the mmaped region a name based on its tag.
#ifdef __linux__
      // Make a best-effort attempt to name the allocated region based on its
      // tag.
      //
      // The call to prctl() may fail if the kernel was not configured with the
      // CONFIG_ANON_VMA_NAME kernel option.  This is OK since the call is
      // primarily a debugging aid.
      char name[256];
      absl::SNPrintF(name, sizeof(name), "tcmalloc_region_%s",
                     MemoryTagToLabel(tag));
      // Save the existing errno and restore it after the prctl system call.
      // Since PR_SET_VMA is a best effort call, we don't want it to overwrite
      // the existing errno value.
      ErrnoRestorer errno_restorer;
      prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, result, size, name);
#endif  // __linux__
      return result;
    }
    if (map_fixed_noreplace_flag) {
      // If MAP_FIXED_NOREPLACE was correctly detected, we should either get
      // result == hint or MAP_FAILED.  Any other value indicates incorrect
      // detection.
      TC_CHECK_EQ(result, MAP_FAILED);
    } else {
      if (result == MAP_FAILED) {
        TC_LOG("mmap(%p, %v) reservation failed (%s)", hint, size,
               strerror(errno));
        return nullptr;
      }
      if (int err = munmap(result, size)) {
        TC_LOG("munmap(%p, %v) failed (%s)", result, size, strerror(errno));
        TC_ASSERT_EQ(err, 0);
      }
    }
    next_addr = RandomMmapHint(size, alignment, tag);
  }

  TC_LOG(
      "MmapAligned() failed - unable to allocate with tag (hint=%p, size=%v, "
      "alignment=%v) - is something limiting address placement?",
      hint, size, alignment);
  return nullptr;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

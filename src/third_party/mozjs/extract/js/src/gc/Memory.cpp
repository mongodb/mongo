/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Memory.h"

#include "mozilla/Atomics.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/RandomNum.h"
#include "mozilla/TaggedAnonymousMemory.h"

#include "js/HeapAPI.h"
#include "util/Memory.h"
#include "vm/Runtime.h"

#ifdef XP_WIN

#  include "util/Windows.h"
#  include <psapi.h>

#elif defined(__wasi__)

/* nothing */

#else

#  include <algorithm>
#  include <errno.h>
#  include <sys/mman.h>
#  include <sys/resource.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>

#endif

namespace js {
namespace gc {

/*
 * System allocation functions generally require the allocation size
 * to be an integer multiple of the page size of the running process.
 */
static size_t pageSize = 0;

/* The OS allocation granularity may not match the page size. */
static size_t allocGranularity = 0;

/* The number of bits used by addresses on this platform. */
static size_t numAddressBits = 0;

/* An estimate of the number of bytes available for virtual memory. */
static size_t virtualMemoryLimit = size_t(-1);

/*
 * System allocation functions may hand out regions of memory in increasing or
 * decreasing order. This ordering is used as a hint during chunk alignment to
 * reduce the number of system calls. On systems with 48-bit addresses, our
 * workarounds to obtain 47-bit pointers cause addresses to be handed out in
 * increasing order.
 *
 * We do not use the growth direction on Windows, as constraints on VirtualAlloc
 * would make its application failure prone and complex. Tests indicate that
 * VirtualAlloc always hands out regions of memory in increasing order.
 */
#if defined(XP_DARWIN)
static mozilla::Atomic<int, mozilla::Relaxed> growthDirection(1);
#elif defined(XP_UNIX)
static mozilla::Atomic<int, mozilla::Relaxed> growthDirection(0);
#endif

/*
 * Data from OOM crashes shows there may be up to 24 chunk-sized but unusable
 * chunks available in low memory situations. These chunks may all need to be
 * used up before we gain access to remaining *alignable* chunk-sized regions,
 * so we use a generous limit of 32 unusable chunks to ensure we reach them.
 */
static const int MaxLastDitchAttempts = 32;

#ifdef JS_64BIT
/*
 * On some 64-bit platforms we can use a random, scattershot allocator that
 * tries addresses from the available range at random. If the address range
 * is large enough this will have a high chance of success and additionally
 * makes the memory layout of our process less predictable.
 *
 * However, not all 64-bit platforms have a very large address range. For
 * example, AArch64 on Linux defaults to using 39-bit addresses to limit the
 * number of translation tables used. On such configurations the scattershot
 * approach to allocation creates a conflict with our desire to reserve large
 * regions of memory for applications like WebAssembly: Small allocations may
 * inadvertently block off all available 4-6GiB regions, and conversely
 * reserving such regions may lower the success rate for smaller allocations to
 * unacceptable levels.
 *
 * So we make a compromise: Instead of using the scattershot on all 64-bit
 * platforms, we only use it on platforms that meet a minimum requirement for
 * the available address range. In addition we split the address range,
 * reserving the upper half for huge allocations and the lower half for smaller
 * allocations. We use a limit of 43 bits so that at least 42 bits are available
 * for huge allocations - this matches the 8TiB per process address space limit
 * that we're already subject to on Windows.
 */
static const size_t MinAddressBitsForRandomAlloc = 43;

/* The lower limit for huge allocations. This is fairly arbitrary. */
static const size_t HugeAllocationSize = 1024 * 1024 * 1024;

/* The minimum and maximum valid addresses that can be allocated into. */
static size_t minValidAddress = 0;
static size_t maxValidAddress = 0;

/* The upper limit for smaller allocations and the lower limit for huge ones. */
static size_t hugeSplit = 0;
#endif

size_t SystemPageSize() { return pageSize; }

size_t SystemAddressBits() { return numAddressBits; }

size_t VirtualMemoryLimit() { return virtualMemoryLimit; }

bool UsingScattershotAllocator() {
#ifdef JS_64BIT
  return numAddressBits >= MinAddressBitsForRandomAlloc;
#else
  return false;
#endif
}

enum class Commit : bool {
  No = false,
  Yes = true,
};

#ifdef XP_WIN
enum class PageAccess : DWORD {
  None = PAGE_NOACCESS,
  Read = PAGE_READONLY,
  ReadWrite = PAGE_READWRITE,
  Execute = PAGE_EXECUTE,
  ReadExecute = PAGE_EXECUTE_READ,
  ReadWriteExecute = PAGE_EXECUTE_READWRITE,
};
#elif defined(__wasi__)
enum class PageAccess : int {
  None = 0,
  Read = 0,
  ReadWrite = 0,
  Execute = 0,
  ReadExecute = 0,
  ReadWriteExecute = 0,
};
#else
enum class PageAccess : int {
  None = PROT_NONE,
  Read = PROT_READ,
  ReadWrite = PROT_READ | PROT_WRITE,
  Execute = PROT_EXEC,
  ReadExecute = PROT_READ | PROT_EXEC,
  ReadWriteExecute = PROT_READ | PROT_WRITE | PROT_EXEC,
};
#endif

template <bool AlwaysGetNew = true>
static bool TryToAlignChunk(void** aRegion, void** aRetainedRegion,
                            size_t length, size_t alignment);

#ifndef __wasi__
static void* MapAlignedPagesSlow(size_t length, size_t alignment);
#endif  // wasi
static void* MapAlignedPagesLastDitch(size_t length, size_t alignment);

#ifdef JS_64BIT
static void* MapAlignedPagesRandom(size_t length, size_t alignment);
#endif

void* TestMapAlignedPagesLastDitch(size_t length, size_t alignment) {
  return MapAlignedPagesLastDitch(length, alignment);
}

bool DecommitEnabled() { return SystemPageSize() == PageSize; }

/* Returns the offset from the nearest aligned address at or below |region|. */
static inline size_t OffsetFromAligned(void* region, size_t alignment) {
  return uintptr_t(region) % alignment;
}

template <Commit commit, PageAccess prot>
static inline void* MapInternal(void* desired, size_t length) {
  void* region = nullptr;
#ifdef XP_WIN
  DWORD flags =
      (commit == Commit::Yes ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE);
  region = VirtualAlloc(desired, length, flags, DWORD(prot));
#elif defined(__wasi__)
  if (int err = posix_memalign(&region, gc::SystemPageSize(), length)) {
    MOZ_RELEASE_ASSERT(err == ENOMEM);
    return nullptr;
  }
  if (region) {
    memset(region, 0, length);
  }
#else
  int flags = MAP_PRIVATE | MAP_ANON;
  region = MozTaggedAnonymousMmap(desired, length, int(prot), flags, -1, 0,
                                  "js-gc-heap");
  if (region == MAP_FAILED) {
    return nullptr;
  }
#endif
  return region;
}

static inline void UnmapInternal(void* region, size_t length) {
  MOZ_ASSERT(region && OffsetFromAligned(region, allocGranularity) == 0);
  MOZ_ASSERT(length > 0 && length % pageSize == 0);

#ifdef XP_WIN
  MOZ_RELEASE_ASSERT(VirtualFree(region, 0, MEM_RELEASE) != 0);
#elif defined(__wasi__)
  free(region);
#else
  if (munmap(region, length)) {
    MOZ_RELEASE_ASSERT(errno == ENOMEM);
  }
#endif
}

template <Commit commit = Commit::Yes, PageAccess prot = PageAccess::ReadWrite>
static inline void* MapMemory(size_t length) {
  MOZ_ASSERT(length > 0);

  return MapInternal<commit, prot>(nullptr, length);
}

/*
 * Attempts to map memory at the given address, but allows the system
 * to return a different address that may still be suitable.
 */
template <Commit commit = Commit::Yes, PageAccess prot = PageAccess::ReadWrite>
static inline void* MapMemoryAtFuzzy(void* desired, size_t length) {
  MOZ_ASSERT(desired && OffsetFromAligned(desired, allocGranularity) == 0);
  MOZ_ASSERT(length > 0);

  // Note that some platforms treat the requested address as a hint, so the
  // returned address might not match the requested address.
  return MapInternal<commit, prot>(desired, length);
}

/*
 * Attempts to map memory at the given address, returning nullptr if
 * the system returns any address other than the requested one.
 */
template <Commit commit = Commit::Yes, PageAccess prot = PageAccess::ReadWrite>
static inline void* MapMemoryAt(void* desired, size_t length) {
  MOZ_ASSERT(desired && OffsetFromAligned(desired, allocGranularity) == 0);
  MOZ_ASSERT(length > 0);

  void* region = MapInternal<commit, prot>(desired, length);
  if (!region) {
    return nullptr;
  }

  // On some platforms mmap treats the desired address as a hint, so
  // check that the address we got is the address we requested.
  if (region != desired) {
    UnmapInternal(region, length);
    return nullptr;
  }
  return region;
}

#ifdef JS_64BIT

/* Returns a random number in the given range. */
static inline uint64_t GetNumberInRange(uint64_t minNum, uint64_t maxNum) {
  const uint64_t MaxRand = UINT64_C(0xffffffffffffffff);
  maxNum -= minNum;
  uint64_t binSize = 1 + (MaxRand - maxNum) / (maxNum + 1);

  uint64_t rndNum;
  do {
    mozilla::Maybe<uint64_t> result;
    do {
      result = mozilla::RandomUint64();
    } while (!result);
    rndNum = result.value() / binSize;
  } while (rndNum > maxNum);

  return minNum + rndNum;
}

#  ifndef XP_WIN
static inline uint64_t FindAddressLimitInner(size_t highBit, size_t tries);

/*
 * The address range available to applications depends on both hardware and
 * kernel configuration. For example, AArch64 on Linux uses addresses with
 * 39 significant bits by default, but can be configured to use addresses with
 * 48 significant bits by enabling a 4th translation table. Unfortunately,
 * there appears to be no standard way to query the limit at runtime
 * (Windows exposes this via GetSystemInfo()).
 *
 * This function tries to find the address limit by performing a binary search
 * on the index of the most significant set bit in the addresses it attempts to
 * allocate. As the requested address is often treated as a hint by the
 * operating system, we use the actual returned addresses to narrow the range.
 * We return the number of bits of an address that may be set.
 */
static size_t FindAddressLimit() {
  // Use 32 bits as a lower bound in case we keep getting nullptr.
  uint64_t low = 31;
  uint64_t highestSeen = (UINT64_C(1) << 32) - allocGranularity - 1;

  // Exclude 48-bit and 47-bit addresses first.
  uint64_t high = 47;
  for (; high >= std::max(low, UINT64_C(46)); --high) {
    highestSeen = std::max(FindAddressLimitInner(high, 4), highestSeen);
    low = mozilla::FloorLog2(highestSeen);
  }
  // If those didn't work, perform a modified binary search.
  while (high - 1 > low) {
    uint64_t middle = low + (high - low) / 2;
    highestSeen = std::max(FindAddressLimitInner(middle, 4), highestSeen);
    low = mozilla::FloorLog2(highestSeen);
    if (highestSeen < (UINT64_C(1) << middle)) {
      high = middle;
    }
  }
  // We can be sure of the lower bound, but check the upper bound again.
  do {
    high = low + 1;
    highestSeen = std::max(FindAddressLimitInner(high, 8), highestSeen);
    low = mozilla::FloorLog2(highestSeen);
  } while (low >= high);

  // `low` is the highest set bit, so `low + 1` is the number of bits.
  return low + 1;
}

static inline uint64_t FindAddressLimitInner(size_t highBit, size_t tries) {
  const size_t length = allocGranularity;  // Used as both length and alignment.

  uint64_t highestSeen = 0;
  uint64_t startRaw = UINT64_C(1) << highBit;
  uint64_t endRaw = 2 * startRaw - length - 1;
  uint64_t start = (startRaw + length - 1) / length;
  uint64_t end = (endRaw - (length - 1)) / length;
  for (size_t i = 0; i < tries; ++i) {
    uint64_t desired = length * GetNumberInRange(start, end);
    void* address = MapMemoryAtFuzzy(reinterpret_cast<void*>(desired), length);
    uint64_t actual = uint64_t(address);
    if (address) {
      UnmapInternal(address, length);
    }
    if (actual > highestSeen) {
      highestSeen = actual;
      if (actual >= startRaw) {
        break;
      }
    }
  }
  return highestSeen;
}
#  endif  // !defined(XP_WIN)

#endif  // defined(JS_64BIT)

void InitMemorySubsystem() {
  if (pageSize == 0) {
#ifdef XP_WIN
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    pageSize = sysinfo.dwPageSize;
    allocGranularity = sysinfo.dwAllocationGranularity;
#else
    pageSize = size_t(sysconf(_SC_PAGESIZE));
    allocGranularity = pageSize;
#endif
#ifdef JS_64BIT
#  ifdef XP_WIN
    minValidAddress = size_t(sysinfo.lpMinimumApplicationAddress);
    maxValidAddress = size_t(sysinfo.lpMaximumApplicationAddress);
    numAddressBits = mozilla::FloorLog2(maxValidAddress) + 1;
#  else
    // No standard way to determine these, so fall back to FindAddressLimit().
    numAddressBits = FindAddressLimit();
    minValidAddress = allocGranularity;
    maxValidAddress = (UINT64_C(1) << numAddressBits) - 1 - allocGranularity;
#  endif
    // Sanity check the address to ensure we don't use more than 47 bits.
    uint64_t maxJSAddress = UINT64_C(0x00007fffffffffff) - allocGranularity;
    if (maxValidAddress > maxJSAddress) {
      maxValidAddress = maxJSAddress;
      hugeSplit = UINT64_C(0x00003fffffffffff) - allocGranularity;
    } else {
      hugeSplit = (UINT64_C(1) << (numAddressBits - 1)) - 1 - allocGranularity;
    }
#else  // !defined(JS_64BIT)
    numAddressBits = 32;
#endif
#ifdef RLIMIT_AS
    rlimit as_limit;
    if (getrlimit(RLIMIT_AS, &as_limit) == 0 &&
        as_limit.rlim_max != RLIM_INFINITY) {
      virtualMemoryLimit = as_limit.rlim_max;
    }
#endif
  }
}

#ifdef JS_64BIT
/* The JS engine uses 47-bit pointers; all higher bits must be clear. */
static inline bool IsInvalidRegion(void* region, size_t length) {
  const uint64_t invalidPointerMask = UINT64_C(0xffff800000000000);
  return (uintptr_t(region) + length - 1) & invalidPointerMask;
}
#endif

void* MapAlignedPages(size_t length, size_t alignment) {
  MOZ_RELEASE_ASSERT(length > 0 && alignment > 0);
  MOZ_RELEASE_ASSERT(length % pageSize == 0);
  MOZ_RELEASE_ASSERT(std::max(alignment, allocGranularity) %
                         std::min(alignment, allocGranularity) ==
                     0);

  // Smaller alignments aren't supported by the allocation functions.
  if (alignment < allocGranularity) {
    alignment = allocGranularity;
  }

#ifdef __wasi__
  void* region = nullptr;
  if (int err = posix_memalign(&region, alignment, length)) {
    MOZ_ASSERT(err == ENOMEM);
    return nullptr;
  }
  MOZ_ASSERT(region != nullptr);
  memset(region, 0, length);
  return region;
#else

#  ifdef JS_64BIT
  // Use the scattershot allocator if the address range is large enough.
  if (UsingScattershotAllocator()) {
    void* region = MapAlignedPagesRandom(length, alignment);

    MOZ_RELEASE_ASSERT(!IsInvalidRegion(region, length));
    MOZ_ASSERT(OffsetFromAligned(region, alignment) == 0);

    return region;
  }
#  endif

  // Try to allocate the region. If the returned address is aligned,
  // either we OOMed (region is nullptr) or we're done.
  void* region = MapMemory(length);
  if (OffsetFromAligned(region, alignment) == 0) {
    return region;
  }

  // Try to align the region. On success, TryToAlignChunk() returns
  // true and we can return the aligned region immediately.
  void* retainedRegion;
  if (TryToAlignChunk(&region, &retainedRegion, length, alignment)) {
    MOZ_ASSERT(region && OffsetFromAligned(region, alignment) == 0);
    MOZ_ASSERT(!retainedRegion);
    return region;
  }

  // On failure, the unaligned region is retained unless we OOMed. We don't
  // use the retained region on this path (see the last ditch allocator).
  if (retainedRegion) {
    UnmapInternal(retainedRegion, length);
  }

  // If it fails to align the given region, TryToAlignChunk() returns the
  // next valid region that we might be able to align (unless we OOMed).
  if (region) {
    MOZ_ASSERT(OffsetFromAligned(region, alignment) != 0);
    UnmapInternal(region, length);
  }

  // Since we couldn't align the first region, fall back to allocating a
  // region large enough that we can definitely align it.
  region = MapAlignedPagesSlow(length, alignment);
  if (!region) {
    // If there wasn't enough contiguous address space left for that,
    // try to find an alignable region using the last ditch allocator.
    region = MapAlignedPagesLastDitch(length, alignment);
  }

  // At this point we should either have an aligned region or nullptr.
  MOZ_ASSERT(OffsetFromAligned(region, alignment) == 0);
  return region;
#endif  // !__wasi__
}

#ifdef JS_64BIT

/*
 * This allocator takes advantage of the large address range on some 64-bit
 * platforms to allocate in a scattershot manner, choosing addresses at random
 * from the range. By controlling the range we can avoid returning addresses
 * that have more than 47 significant bits (as required by SpiderMonkey).
 * This approach also has some other advantages over the methods employed by
 * the other allocation functions in this file:
 * 1) Allocations are extremely likely to succeed on the first try.
 * 2) The randomness makes our memory layout becomes harder to predict.
 * 3) The low probability of reusing regions guards against use-after-free.
 *
 * The main downside is that detecting physical OOM situations becomes more
 * difficult; to guard against this, we occasionally try a regular allocation.
 * In addition, sprinkling small allocations throughout the full address range
 * might get in the way of large address space reservations such as those
 * employed by WebAssembly. To avoid this (or the opposite problem of such
 * reservations reducing the chance of success for smaller allocations) we
 * split the address range in half, with one half reserved for huge allocations
 * and the other for regular (usually chunk sized) allocations.
 */
static void* MapAlignedPagesRandom(size_t length, size_t alignment) {
  uint64_t minNum, maxNum;
  if (length < HugeAllocationSize) {
    // Use the lower half of the range.
    minNum = (minValidAddress + alignment - 1) / alignment;
    maxNum = (hugeSplit - (length - 1)) / alignment;
  } else {
    // Use the upper half of the range.
    minNum = (hugeSplit + 1 + alignment - 1) / alignment;
    maxNum = (maxValidAddress - (length - 1)) / alignment;
  }

  // Try to allocate in random aligned locations.
  void* region = nullptr;
  for (size_t i = 1; i <= 1024; ++i) {
    if (i & 0xf) {
      uint64_t desired = alignment * GetNumberInRange(minNum, maxNum);
      region = MapMemoryAtFuzzy(reinterpret_cast<void*>(desired), length);
      if (!region) {
        continue;
      }
    } else {
      // Check for OOM.
      region = MapMemory(length);
      if (!region) {
        return nullptr;
      }
    }
    if (IsInvalidRegion(region, length)) {
      UnmapInternal(region, length);
      continue;
    }
    if (OffsetFromAligned(region, alignment) == 0) {
      return region;
    }
    void* retainedRegion = nullptr;
    if (TryToAlignChunk<false>(&region, &retainedRegion, length, alignment)) {
      MOZ_ASSERT(region && OffsetFromAligned(region, alignment) == 0);
      MOZ_ASSERT(!retainedRegion);
      return region;
    }
    MOZ_ASSERT(region && !retainedRegion);
    UnmapInternal(region, length);
  }

  if (numAddressBits < 48) {
    // Try the reliable fallback of overallocating.
    // Note: This will not respect the address space split.
    region = MapAlignedPagesSlow(length, alignment);
    if (region) {
      return region;
    }
  }
  if (length < HugeAllocationSize) {
    MOZ_CRASH("Couldn't allocate even after 1000 tries!");
  }

  return nullptr;
}

#endif  // defined(JS_64BIT)

#ifndef __wasi__
static void* MapAlignedPagesSlow(size_t length, size_t alignment) {
  void* alignedRegion = nullptr;
  do {
    size_t reserveLength = length + alignment - pageSize;
#  ifdef XP_WIN
    // Don't commit the requested pages as we won't use the region directly.
    void* region = MapMemory<Commit::No>(reserveLength);
#  else
    void* region = MapMemory(reserveLength);
#  endif
    if (!region) {
      return nullptr;
    }
    alignedRegion =
        reinterpret_cast<void*>(AlignBytes(uintptr_t(region), alignment));
#  ifdef XP_WIN
    // Windows requires that map and unmap calls be matched, so deallocate
    // and immediately reallocate at the desired (aligned) address.
    UnmapInternal(region, reserveLength);
    alignedRegion = MapMemoryAt(alignedRegion, length);
#  else
    // munmap allows us to simply unmap the pages that don't interest us.
    if (alignedRegion != region) {
      UnmapInternal(region, uintptr_t(alignedRegion) - uintptr_t(region));
    }
    void* regionEnd =
        reinterpret_cast<void*>(uintptr_t(region) + reserveLength);
    void* alignedEnd =
        reinterpret_cast<void*>(uintptr_t(alignedRegion) + length);
    if (alignedEnd != regionEnd) {
      UnmapInternal(alignedEnd, uintptr_t(regionEnd) - uintptr_t(alignedEnd));
    }
#  endif
    // On Windows we may have raced with another thread; if so, try again.
  } while (!alignedRegion);

  return alignedRegion;
}
#endif  // wasi

/*
 * In a low memory or high fragmentation situation, alignable chunks of the
 * desired length may still be available, even if there are no more contiguous
 * free chunks that meet the |length + alignment - pageSize| requirement of
 * MapAlignedPagesSlow. In this case, try harder to find an alignable chunk
 * by temporarily holding onto the unaligned parts of each chunk until the
 * allocator gives us a chunk that either is, or can be aligned.
 */
static void* MapAlignedPagesLastDitch(size_t length, size_t alignment) {
  void* tempMaps[MaxLastDitchAttempts];
  int attempt = 0;
  void* region = MapMemory(length);
  if (OffsetFromAligned(region, alignment) == 0) {
    return region;
  }
  for (; attempt < MaxLastDitchAttempts; ++attempt) {
    if (TryToAlignChunk(&region, tempMaps + attempt, length, alignment)) {
      MOZ_ASSERT(region && OffsetFromAligned(region, alignment) == 0);
      MOZ_ASSERT(!tempMaps[attempt]);
      break;  // Success!
    }
    if (!region || !tempMaps[attempt]) {
      break;  // We ran out of memory, so give up.
    }
  }
  if (OffsetFromAligned(region, alignment)) {
    UnmapInternal(region, length);
    region = nullptr;
  }
  while (--attempt >= 0) {
    UnmapInternal(tempMaps[attempt], length);
  }
  return region;
}

#ifdef XP_WIN

/*
 * On Windows, map and unmap calls must be matched, so we deallocate the
 * unaligned chunk, then reallocate the unaligned part to block off the
 * old address and force the allocator to give us a new one.
 */
template <bool>
static bool TryToAlignChunk(void** aRegion, void** aRetainedRegion,
                            size_t length, size_t alignment) {
  void* region = *aRegion;
  MOZ_ASSERT(region && OffsetFromAligned(region, alignment) != 0);

  size_t retainedLength = 0;
  void* retainedRegion = nullptr;
  do {
    size_t offset = OffsetFromAligned(region, alignment);
    if (offset == 0) {
      // If the address is aligned, either we hit OOM or we're done.
      break;
    }
    UnmapInternal(region, length);
    retainedLength = alignment - offset;
    retainedRegion = MapMemoryAt<Commit::No>(region, retainedLength);
    region = MapMemory(length);

    // If retainedRegion is null here, we raced with another thread.
  } while (!retainedRegion);

  bool result = OffsetFromAligned(region, alignment) == 0;
  if (result && retainedRegion) {
    UnmapInternal(retainedRegion, retainedLength);
    retainedRegion = nullptr;
  }

  *aRegion = region;
  *aRetainedRegion = retainedRegion;
  return region && result;
}

#else  // !defined(XP_WIN)

/*
 * mmap calls don't have to be matched with calls to munmap, so we can unmap
 * just the pages we don't need. However, as we don't know a priori if addresses
 * are handed out in increasing or decreasing order, we have to try both
 * directions (depending on the environment, one will always fail).
 */
template <bool AlwaysGetNew>
static bool TryToAlignChunk(void** aRegion, void** aRetainedRegion,
                            size_t length, size_t alignment) {
  void* regionStart = *aRegion;
  MOZ_ASSERT(regionStart && OffsetFromAligned(regionStart, alignment) != 0);

  bool addressesGrowUpward = growthDirection > 0;
  bool directionUncertain = -8 < growthDirection && growthDirection <= 8;
  size_t offsetLower = OffsetFromAligned(regionStart, alignment);
  size_t offsetUpper = alignment - offsetLower;
  for (size_t i = 0; i < 2; ++i) {
    if (addressesGrowUpward) {
      void* upperStart =
          reinterpret_cast<void*>(uintptr_t(regionStart) + offsetUpper);
      void* regionEnd =
          reinterpret_cast<void*>(uintptr_t(regionStart) + length);
      if (MapMemoryAt(regionEnd, offsetUpper)) {
        UnmapInternal(regionStart, offsetUpper);
        if (directionUncertain) {
          ++growthDirection;
        }
        regionStart = upperStart;
        break;
      }
    } else {
      auto* lowerStart =
          reinterpret_cast<void*>(uintptr_t(regionStart) - offsetLower);
      auto* lowerEnd = reinterpret_cast<void*>(uintptr_t(lowerStart) + length);
      if (MapMemoryAt(lowerStart, offsetLower)) {
        UnmapInternal(lowerEnd, offsetLower);
        if (directionUncertain) {
          --growthDirection;
        }
        regionStart = lowerStart;
        break;
      }
    }
    // If we're confident in the growth direction, don't try the other.
    if (!directionUncertain) {
      break;
    }
    addressesGrowUpward = !addressesGrowUpward;
  }

  void* retainedRegion = nullptr;
  bool result = OffsetFromAligned(regionStart, alignment) == 0;
  if (AlwaysGetNew && !result) {
    // If our current chunk cannot be aligned, just get a new one.
    retainedRegion = regionStart;
    regionStart = MapMemory(length);
    // Our new region might happen to already be aligned.
    result = OffsetFromAligned(regionStart, alignment) == 0;
    if (result) {
      UnmapInternal(retainedRegion, length);
      retainedRegion = nullptr;
    }
  }

  *aRegion = regionStart;
  *aRetainedRegion = retainedRegion;
  return regionStart && result;
}

#endif

void UnmapPages(void* region, size_t length) {
  MOZ_RELEASE_ASSERT(region &&
                     OffsetFromAligned(region, allocGranularity) == 0);
  MOZ_RELEASE_ASSERT(length > 0 && length % pageSize == 0);

  // ASan does not automatically unpoison memory, so we have to do this here.
  MOZ_MAKE_MEM_UNDEFINED(region, length);

  UnmapInternal(region, length);
}

static void CheckDecommit(void* region, size_t length) {
  MOZ_RELEASE_ASSERT(region);
  MOZ_RELEASE_ASSERT(length > 0);

  // pageSize == ArenaSize doesn't necessarily hold, but this function is
  // used by the GC to decommit unused Arenas, so we don't want to assert
  // if pageSize > ArenaSize.
  MOZ_ASSERT(OffsetFromAligned(region, ArenaSize) == 0);
  MOZ_ASSERT(length % ArenaSize == 0);

  MOZ_RELEASE_ASSERT(OffsetFromAligned(region, pageSize) == 0);
  MOZ_RELEASE_ASSERT(length % pageSize == 0);
}

bool MarkPagesUnusedSoft(void* region, size_t length) {
  MOZ_ASSERT(DecommitEnabled());
  CheckDecommit(region, length);

  MOZ_MAKE_MEM_NOACCESS(region, length);

#if defined(XP_WIN)
  return VirtualAlloc(region, length, MEM_RESET,
                      DWORD(PageAccess::ReadWrite)) == region;
#elif defined(__wasi__)
  return 0;
#else
  int status;
  do {
#  if defined(XP_DARWIN)
    status = madvise(region, length, MADV_FREE_REUSABLE);
#  elif defined(XP_SOLARIS)
    status = posix_madvise(region, length, POSIX_MADV_DONTNEED);
#  else
    status = madvise(region, length, MADV_DONTNEED);
#  endif
  } while (status == -1 && errno == EAGAIN);
  return status == 0;
#endif
}

bool MarkPagesUnusedHard(void* region, size_t length) {
  CheckDecommit(region, length);

  MOZ_MAKE_MEM_NOACCESS(region, length);

  if (!DecommitEnabled()) {
    return true;
  }

#if defined(XP_WIN)
  return VirtualFree(region, length, MEM_DECOMMIT);
#else
  return MarkPagesUnusedSoft(region, length);
#endif
}

void MarkPagesInUseSoft(void* region, size_t length) {
  MOZ_ASSERT(DecommitEnabled());
  CheckDecommit(region, length);

#if defined(XP_DARWIN)
  while (madvise(region, length, MADV_FREE_REUSE) == -1 && errno == EAGAIN) {
  }
#endif

  MOZ_MAKE_MEM_UNDEFINED(region, length);
}

bool MarkPagesInUseHard(void* region, size_t length) {
  if (js::oom::ShouldFailWithOOM()) {
    return false;
  }

  CheckDecommit(region, length);

  MOZ_MAKE_MEM_UNDEFINED(region, length);

  if (!DecommitEnabled()) {
    return true;
  }

#if defined(XP_WIN)
  return VirtualAlloc(region, length, MEM_COMMIT,
                      DWORD(PageAccess::ReadWrite)) == region;
#else
  return true;
#endif
}

size_t GetPageFaultCount() {
#ifdef XP_WIN
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) == 0) {
    return 0;
  }
  return pmc.PageFaultCount;
#elif defined(__wasi__)
  return 0;
#else
  struct rusage usage;
  int err = getrusage(RUSAGE_SELF, &usage);
  if (err) {
    return 0;
  }
  return usage.ru_majflt;
#endif
}

void* AllocateMappedContent(int fd, size_t offset, size_t length,
                            size_t alignment) {
#ifdef __wasi__
  MOZ_CRASH("Not yet supported for WASI");
#else
  if (length == 0 || alignment == 0 || offset % alignment != 0 ||
      std::max(alignment, allocGranularity) %
              std::min(alignment, allocGranularity) !=
          0) {
    return nullptr;
  }

  size_t alignedOffset = offset - (offset % allocGranularity);
  size_t alignedLength = length + (offset % allocGranularity);

  // We preallocate the mapping using MapAlignedPages, which expects
  // the length parameter to be an integer multiple of the page size.
  size_t mappedLength = alignedLength;
  if (alignedLength % pageSize != 0) {
    mappedLength += pageSize - alignedLength % pageSize;
  }

#  ifdef XP_WIN
  HANDLE hFile = reinterpret_cast<HANDLE>(intptr_t(fd));

  // This call will fail if the file does not exist.
  HANDLE hMap = CreateFileMapping(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!hMap) {
    return nullptr;
  }

  DWORD offsetH = uint32_t(uint64_t(alignedOffset) >> 32);
  DWORD offsetL = uint32_t(alignedOffset);

  uint8_t* map = nullptr;
  for (;;) {
    // The value of a pointer is technically only defined while the region
    // it points to is allocated, so explicitly treat this one as a number.
    uintptr_t region = uintptr_t(MapAlignedPages(mappedLength, alignment));
    if (region == 0) {
      break;
    }
    UnmapInternal(reinterpret_cast<void*>(region), mappedLength);
    // If the offset or length are out of bounds, this call will fail.
    map = static_cast<uint8_t*>(
        MapViewOfFileEx(hMap, FILE_MAP_COPY, offsetH, offsetL, alignedLength,
                        reinterpret_cast<void*>(region)));

    // Retry if another thread mapped the address we were trying to use.
    if (map || GetLastError() != ERROR_INVALID_ADDRESS) {
      break;
    }
  }

  // This just decreases the file mapping object's internal reference count;
  // it won't actually be destroyed until we unmap the associated view.
  CloseHandle(hMap);

  if (!map) {
    return nullptr;
  }
#  else  // !defined(XP_WIN)
  // Sanity check the offset and length, as mmap does not do this for us.
  struct stat st;
  if (fstat(fd, &st) || offset >= uint64_t(st.st_size) ||
      length > uint64_t(st.st_size) - offset) {
    return nullptr;
  }

  void* region = MapAlignedPages(mappedLength, alignment);
  if (!region) {
    return nullptr;
  }

  // Calling mmap with MAP_FIXED will replace the previous mapping, allowing
  // us to reuse the region we obtained without racing with other threads.
  uint8_t* map =
      static_cast<uint8_t*>(mmap(region, alignedLength, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_FIXED, fd, alignedOffset));
  if (map == MAP_FAILED) {
    UnmapInternal(region, mappedLength);
    return nullptr;
  }
#  endif

#  ifdef DEBUG
  // Zero out data before and after the desired mapping to catch errors early.
  if (offset != alignedOffset) {
    memset(map, 0, offset - alignedOffset);
  }
  if (alignedLength % pageSize) {
    memset(map + alignedLength, 0, pageSize - (alignedLength % pageSize));
  }
#  endif

  return map + (offset - alignedOffset);
#endif  // __wasi__
}

void DeallocateMappedContent(void* region, size_t length) {
#ifdef __wasi__
  MOZ_CRASH("Not yet supported for WASI");
#else
  if (!region) {
    return;
  }

  // Due to bug 1502562, the following assertion does not currently hold.
  // MOZ_RELEASE_ASSERT(length > 0);

  // Calculate the address originally returned by the system call.
  // This is needed because AllocateMappedContent returns a pointer
  // that might be offset from the mapping, as the beginning of a
  // mapping must be aligned with the allocation granularity.
  uintptr_t map = uintptr_t(region) - (uintptr_t(region) % allocGranularity);
#  ifdef XP_WIN
  MOZ_RELEASE_ASSERT(UnmapViewOfFile(reinterpret_cast<void*>(map)) != 0);
#  else
  size_t alignedLength = length + (uintptr_t(region) % allocGranularity);
  if (munmap(reinterpret_cast<void*>(map), alignedLength)) {
    MOZ_RELEASE_ASSERT(errno == ENOMEM);
  }
#  endif
#endif  // __wasi__
}

static inline void ProtectMemory(void* region, size_t length, PageAccess prot) {
  MOZ_RELEASE_ASSERT(region && OffsetFromAligned(region, pageSize) == 0);
  MOZ_RELEASE_ASSERT(length > 0 && length % pageSize == 0);
#ifdef XP_WIN
  DWORD oldProtect;
  MOZ_RELEASE_ASSERT(VirtualProtect(region, length, DWORD(prot), &oldProtect) !=
                     0);
#elif defined(__wasi__)
  /* nothing */
#else
  MOZ_RELEASE_ASSERT(mprotect(region, length, int(prot)) == 0);
#endif
}

void ProtectPages(void* region, size_t length) {
  ProtectMemory(region, length, PageAccess::None);
}

void MakePagesReadOnly(void* region, size_t length) {
  ProtectMemory(region, length, PageAccess::Read);
}

void UnprotectPages(void* region, size_t length) {
  ProtectMemory(region, length, PageAccess::ReadWrite);
}

}  // namespace gc
}  // namespace js

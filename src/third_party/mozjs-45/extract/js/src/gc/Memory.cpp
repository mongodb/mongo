/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Memory.h"

#include "mozilla/Atomics.h"
#include "mozilla/TaggedAnonymousMemory.h"

#include "js/HeapAPI.h"
#include "vm/Runtime.h"

#if defined(XP_WIN)

#include "jswin.h"
#include <psapi.h>

// MONGODB Modification: See SERVER-22927
#elif 0 && defined(SOLARIS)

#include <sys/mman.h>
#include <unistd.h>

#elif defined(XP_UNIX)

#include <algorithm>
#include <errno.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#endif

namespace js {
namespace gc {

// The GC can only safely decommit memory when the page size of the
// running process matches the compiled arena size.
static size_t pageSize = 0;

// The OS allocation granularity may not match the page size.
static size_t allocGranularity = 0;

#if defined(XP_UNIX)
// The addresses handed out by mmap may grow up or down.
static mozilla::Atomic<int, mozilla::Relaxed> growthDirection(0);
#endif

// Data from OOM crashes shows there may be up to 24 chunksized but unusable
// chunks available in low memory situations. These chunks may all need to be
// used up before we gain access to remaining *alignable* chunksized regions,
// so we use a generous limit of 32 unusable chunks to ensure we reach them.
static const int MaxLastDitchAttempts = 32;

static void GetNewChunk(void** aAddress, void** aRetainedAddr, size_t size, size_t alignment);
static void* MapAlignedPagesSlow(size_t size, size_t alignment);
static void* MapAlignedPagesLastDitch(size_t size, size_t alignment);

size_t
SystemPageSize()
{
    return pageSize;
}

static bool
DecommitEnabled()
{
    return pageSize == ArenaSize;
}

/*
 * This returns the offset of address p from the nearest aligned address at
 * or below p - or alternatively, the number of unaligned bytes at the end of
 * the region starting at p (as we assert that allocation size is an integer
 * multiple of the alignment).
 */
static inline size_t
OffsetFromAligned(void* p, size_t alignment)
{
    return uintptr_t(p) % alignment;
}

void*
TestMapAlignedPagesLastDitch(size_t size, size_t alignment)
{
    return MapAlignedPagesLastDitch(size, alignment);
}


#if defined(XP_WIN)

void
InitMemorySubsystem()
{
    if (pageSize == 0) {
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        pageSize = sysinfo.dwPageSize;
        allocGranularity = sysinfo.dwAllocationGranularity;
    }
}

#  if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

static inline void*
MapMemoryAt(void* desired, size_t length, int flags, int prot = PAGE_READWRITE)
{
    return VirtualAlloc(desired, length, flags, prot);
}

static inline void*
MapMemory(size_t length, int flags, int prot = PAGE_READWRITE)
{
    return VirtualAlloc(nullptr, length, flags, prot);
}

void*
MapAlignedPages(size_t size, size_t alignment)
{
    MOZ_ASSERT(size >= alignment);
    MOZ_ASSERT(size % alignment == 0);
    MOZ_ASSERT(size % pageSize == 0);
    MOZ_ASSERT(alignment % allocGranularity == 0);

    void* p = MapMemory(size, MEM_COMMIT | MEM_RESERVE);

    /* Special case: If we want allocation alignment, no further work is needed. */
    if (alignment == allocGranularity)
        return p;

    if (OffsetFromAligned(p, alignment) == 0)
        return p;

    void* retainedAddr;
    GetNewChunk(&p, &retainedAddr, size, alignment);
    if (retainedAddr)
        UnmapPages(retainedAddr, size);
    if (p) {
        if (OffsetFromAligned(p, alignment) == 0)
            return p;
        UnmapPages(p, size);
    }

    p = MapAlignedPagesSlow(size, alignment);
    if (!p)
        return MapAlignedPagesLastDitch(size, alignment);

    MOZ_ASSERT(OffsetFromAligned(p, alignment) == 0);
    return p;
}

static void*
MapAlignedPagesSlow(size_t size, size_t alignment)
{
    /*
     * Windows requires that there be a 1:1 mapping between VM allocation
     * and deallocation operations.  Therefore, take care here to acquire the
     * final result via one mapping operation.  This means unmapping any
     * preliminary result that is not correctly aligned.
     */
    void* p;
    do {
        /*
         * Over-allocate in order to map a memory region that is definitely
         * large enough, then deallocate and allocate again the correct size,
         * within the over-sized mapping.
         *
         * Since we're going to unmap the whole thing anyway, the first
         * mapping doesn't have to commit pages.
         */
        size_t reserveSize = size + alignment - pageSize;
        p = MapMemory(reserveSize, MEM_RESERVE);
        if (!p)
            return nullptr;
        void* chunkStart = (void*)AlignBytes(uintptr_t(p), alignment);
        UnmapPages(p, reserveSize);
        p = MapMemoryAt(chunkStart, size, MEM_COMMIT | MEM_RESERVE);

        /* Failure here indicates a race with another thread, so try again. */
    } while (!p);

    return p;
}

/*
 * In a low memory or high fragmentation situation, alignable chunks of the
 * desired size may still be available, even if there are no more contiguous
 * free chunks that meet the |size + alignment - pageSize| requirement of
 * MapAlignedPagesSlow. In this case, try harder to find an alignable chunk
 * by temporarily holding onto the unaligned parts of each chunk until the
 * allocator gives us a chunk that either is, or can be aligned.
 */
static void*
MapAlignedPagesLastDitch(size_t size, size_t alignment)
{
    void* tempMaps[MaxLastDitchAttempts];
    int attempt = 0;
    void* p = MapMemory(size, MEM_COMMIT | MEM_RESERVE);
    if (OffsetFromAligned(p, alignment) == 0)
        return p;
    for (; attempt < MaxLastDitchAttempts; ++attempt) {
        GetNewChunk(&p, tempMaps + attempt, size, alignment);
        if (OffsetFromAligned(p, alignment) == 0) {
            if (tempMaps[attempt])
                UnmapPages(tempMaps[attempt], size);
            break;
        }
        if (!tempMaps[attempt])
            break; /* Bail if GetNewChunk failed. */
    }
    if (OffsetFromAligned(p, alignment)) {
        UnmapPages(p, size);
        p = nullptr;
    }
    while (--attempt >= 0)
        UnmapPages(tempMaps[attempt], size);
    return p;
}

/*
 * On Windows, map and unmap calls must be matched, so we deallocate the
 * unaligned chunk, then reallocate the unaligned part to block off the
 * old address and force the allocator to give us a new one.
 */
static void
GetNewChunk(void** aAddress, void** aRetainedAddr, size_t size, size_t alignment)
{
    void* address = *aAddress;
    void* retainedAddr = nullptr;
    do {
        size_t retainedSize;
        size_t offset = OffsetFromAligned(address, alignment);
        if (!offset)
            break;
        UnmapPages(address, size);
        retainedSize = alignment - offset;
        retainedAddr = MapMemoryAt(address, retainedSize, MEM_RESERVE);
        address = MapMemory(size, MEM_COMMIT | MEM_RESERVE);
        /* If retainedAddr is null here, we raced with another thread. */
    } while (!retainedAddr);
    *aAddress = address;
    *aRetainedAddr = retainedAddr;
}

void
UnmapPages(void* p, size_t size)
{
    MOZ_ALWAYS_TRUE(VirtualFree(p, 0, MEM_RELEASE));
}

bool
MarkPagesUnused(void* p, size_t size)
{
    if (!DecommitEnabled())
        return true;

    MOZ_ASSERT(OffsetFromAligned(p, pageSize) == 0);
    LPVOID p2 = MapMemoryAt(p, size, MEM_RESET);
    return p2 == p;
}

bool
MarkPagesInUse(void* p, size_t size)
{
    if (!DecommitEnabled())
        return true;

    MOZ_ASSERT(OffsetFromAligned(p, pageSize) == 0);
    return true;
}

size_t
GetPageFaultCount()
{
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return 0;
    return pmc.PageFaultCount;
}

#  else // Various APIs are unavailable.

void*
MapAlignedPages(size_t size, size_t alignment)
{
    MOZ_ASSERT(size >= alignment);
    MOZ_ASSERT(size % alignment == 0);
    MOZ_ASSERT(size % pageSize == 0);
    MOZ_ASSERT(alignment % allocGranularity == 0);

    void* p = _aligned_malloc(size, alignment);

    MOZ_ASSERT(OffsetFromAligned(p, alignment) == 0);
    return p;
}

static void*
MapAlignedPagesLastDitch(size_t size, size_t alignment)
{
    return nullptr;
}

void
UnmapPages(void* p, size_t size)
{
    _aligned_free(p);
}

bool
MarkPagesUnused(void* p, size_t size)
{
    MOZ_ASSERT(OffsetFromAligned(p, pageSize) == 0);
    return true;
}

bool
MarkPagesInUse(void* p, size_t size)
{
    MOZ_ASSERT(OffsetFromAligned(p, pageSize) == 0);
    return true;
}

size_t
GetPageFaultCount()
{
    // GetProcessMemoryInfo is unavailable.
    return 0;
}

#  endif

void*
AllocateMappedContent(int fd, size_t offset, size_t length, size_t alignment)
{
    // TODO: Bug 988813 - Support memory mapped array buffer for Windows platform.
    return nullptr;
}

// Deallocate mapped memory for object.
void
DeallocateMappedContent(void* p, size_t length)
{
    // TODO: Bug 988813 - Support memory mapped array buffer for Windows platform.
}

// MONGODB Modification: See SERVER-22927
#elif 0 && defined(SOLARIS)

#ifndef MAP_NOSYNC
# define MAP_NOSYNC 0
#endif

void
InitMemorySubsystem()
{
    if (pageSize == 0)
        pageSize = allocGranularity = size_t(sysconf(_SC_PAGESIZE));
}

void*
MapAlignedPages(size_t size, size_t alignment)
{
    MOZ_ASSERT(size >= alignment);
    MOZ_ASSERT(size % alignment == 0);
    MOZ_ASSERT(size % pageSize == 0);
    MOZ_ASSERT(alignment % allocGranularity == 0);

    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANON | MAP_ALIGN | MAP_NOSYNC;

    void* p = mmap((caddr_t)alignment, size, prot, flags, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;
    return p;
}

static void*
MapAlignedPagesLastDitch(size_t size, size_t alignment)
{
    return nullptr;
}

void
UnmapPages(void* p, size_t size)
{
    MOZ_ALWAYS_TRUE(0 == munmap((caddr_t)p, size));
}

bool
MarkPagesUnused(void* p, size_t size)
{
    MOZ_ASSERT(OffsetFromAligned(p, pageSize) == 0);
    return true;
}

bool
MarkPagesInUse(void* p, size_t size)
{
    if (!DecommitEnabled())
        return true;

    MOZ_ASSERT(OffsetFromAligned(p, pageSize) == 0);
    return true;
}

size_t
GetPageFaultCount()
{
    return 0;
}

void*
AllocateMappedContent(int fd, size_t offset, size_t length, size_t alignment)
{
    // Not implemented.
    return nullptr;
}

// Deallocate mapped memory for object.
void
DeallocateMappedContent(void* p, size_t length)
{
    // Not implemented.
}

#elif defined(XP_UNIX)

void
InitMemorySubsystem()
{
    if (pageSize == 0)
        pageSize = allocGranularity = size_t(sysconf(_SC_PAGESIZE));
}

static inline void*
MapMemoryAt(void* desired, size_t length, int prot = PROT_READ | PROT_WRITE,
            int flags = MAP_PRIVATE | MAP_ANON, int fd = -1, off_t offset = 0)
{
#if defined(__ia64__) || (defined(__sparc64__) && defined(__NetBSD__)) || \
    defined(SOLARIS) || defined(__aarch64__)
    MOZ_ASSERT(0xffff800000000000ULL & (uintptr_t(desired) + length - 1) == 0);
#endif
    void* region = mmap(desired, length, prot, flags, fd, offset);
    if (region == MAP_FAILED)
        return nullptr;
    /*
     * mmap treats the given address as a hint unless the MAP_FIXED flag is
     * used (which isn't usually what you want, as this overrides existing
     * mappings), so check that the address we got is the address we wanted.
     */
    if (region != desired) {
        if (munmap(region, length))
            MOZ_ASSERT(errno == ENOMEM);
        return nullptr;
    }
    return region;
}

static inline void*
MapMemory(size_t length, int prot = PROT_READ | PROT_WRITE,
          int flags = MAP_PRIVATE | MAP_ANON, int fd = -1, off_t offset = 0)
{
#if defined(__ia64__) || (defined(__sparc64__) && defined(__NetBSD__))
    /*
     * The JS engine assumes that all allocated pointers have their high 17 bits clear,
     * which ia64's mmap doesn't support directly. However, we can emulate it by passing
     * mmap an "addr" parameter with those bits clear. The mmap will return that address,
     * or the nearest available memory above that address, providing a near-guarantee
     * that those bits are clear. If they are not, we return nullptr below to indicate
     * out-of-memory.
     *
     * The addr is chosen as 0x0000070000000000, which still allows about 120TB of virtual
     * address space.
     *
     * See Bug 589735 for more information.
     */
    void* region = mmap((void*)0x0000070000000000, length, prot, flags, fd, offset);
    if (region == MAP_FAILED)
        return nullptr;
    /*
     * If the allocated memory doesn't have its upper 17 bits clear, consider it
     * as out of memory.
     */
    if ((uintptr_t(region) + (length - 1)) & 0xffff800000000000) {
        if (munmap(region, length))
            MOZ_ASSERT(errno == ENOMEM);
        return nullptr;
    }
    return region;
#elif defined(__aarch64__) || defined(SOLARIS)
   /*
    * There might be similar virtual address issue on arm64 which depends on
    * hardware and kernel configurations. But the work around is slightly
    * different due to the different mmap behavior.
    *
    * TODO: Merge with the above code block if this implementation works for
    * ia64 and sparc64.
    */
    const uintptr_t start = UINT64_C(0x0000070000000000);
    const uintptr_t end   = UINT64_C(0x0000800000000000);
    const uintptr_t step  = ChunkSize;
   /*
    * Optimization options if there are too many retries in practice:
    * 1. Examine /proc/self/maps to find an available address. This file is
    *    not always available, however. In addition, even if we examine
    *    /proc/self/maps, we may still need to retry several times due to
    *    racing with other threads.
    * 2. Use a global/static variable with lock to track the addresses we have
    *    allocated or tried.
    */
    uintptr_t hint;
    void* region = MAP_FAILED;
    for (hint = start; region == MAP_FAILED && hint + length <= end; hint += step) {
        region = mmap((void*)hint, length, prot, flags, fd, offset);
        if (region != MAP_FAILED) {
            if ((uintptr_t(region) + (length - 1)) & 0xffff800000000000) {
                if (munmap(region, length)) {
                    MOZ_ASSERT(errno == ENOMEM);
                }
                region = MAP_FAILED;
            }
        }
    }
    return region == MAP_FAILED ? nullptr : region;
#else
    void* region = MozTaggedAnonymousMmap(nullptr, length, prot, flags, fd, offset, "js-gc-heap");
    if (region == MAP_FAILED)
        return nullptr;
    return region;
#endif
}

void*
MapAlignedPages(size_t size, size_t alignment)
{
    MOZ_ASSERT(size >= alignment);
    MOZ_ASSERT(size % alignment == 0);
    MOZ_ASSERT(size % pageSize == 0);
    MOZ_ASSERT(alignment % allocGranularity == 0);

    void* p = MapMemory(size);

    /* Special case: If we want page alignment, no further work is needed. */
    if (alignment == allocGranularity)
        return p;

    if (OffsetFromAligned(p, alignment) == 0)
        return p;

    void* retainedAddr;
    GetNewChunk(&p, &retainedAddr, size, alignment);
    if (retainedAddr)
        UnmapPages(retainedAddr, size);
    if (p) {
        if (OffsetFromAligned(p, alignment) == 0)
            return p;
        UnmapPages(p, size);
    }

    p = MapAlignedPagesSlow(size, alignment);
    if (!p)
        return MapAlignedPagesLastDitch(size, alignment);

    MOZ_ASSERT(OffsetFromAligned(p, alignment) == 0);
    return p;
}

static void*
MapAlignedPagesSlow(size_t size, size_t alignment)
{
    /* Overallocate and unmap the region's edges. */
    size_t reqSize = size + alignment - pageSize;
    void* region = MapMemory(reqSize);
    if (!region)
        return nullptr;

    void* regionEnd = (void*)(uintptr_t(region) + reqSize);
    void* front;
    void* end;
    if (growthDirection <= 0) {
        size_t offset = OffsetFromAligned(regionEnd, alignment);
        end = (void*)(uintptr_t(regionEnd) - offset);
        front = (void*)(uintptr_t(end) - size);
    } else {
        size_t offset = OffsetFromAligned(region, alignment);
        front = (void*)(uintptr_t(region) + (offset ? alignment - offset : 0));
        end = (void*)(uintptr_t(front) + size);
    }

    if (front != region)
        UnmapPages(region, uintptr_t(front) - uintptr_t(region));
    if (end != regionEnd)
        UnmapPages(end, uintptr_t(regionEnd) - uintptr_t(end));

    return front;
}

/*
 * In a low memory or high fragmentation situation, alignable chunks of the
 * desired size may still be available, even if there are no more contiguous
 * free chunks that meet the |size + alignment - pageSize| requirement of
 * MapAlignedPagesSlow. In this case, try harder to find an alignable chunk
 * by temporarily holding onto the unaligned parts of each chunk until the
 * allocator gives us a chunk that either is, or can be aligned.
 */
static void*
MapAlignedPagesLastDitch(size_t size, size_t alignment)
{
    void* tempMaps[MaxLastDitchAttempts];
    int attempt = 0;
    void* p = MapMemory(size);
    if (OffsetFromAligned(p, alignment) == 0)
        return p;
    for (; attempt < MaxLastDitchAttempts; ++attempt) {
        GetNewChunk(&p, tempMaps + attempt, size, alignment);
        if (OffsetFromAligned(p, alignment) == 0) {
            if (tempMaps[attempt])
                UnmapPages(tempMaps[attempt], size);
            break;
        }
        if (!tempMaps[attempt])
            break; /* Bail if GetNewChunk failed. */
    }
    if (OffsetFromAligned(p, alignment)) {
        UnmapPages(p, size);
        p = nullptr;
    }
    while (--attempt >= 0)
        UnmapPages(tempMaps[attempt], size);
    return p;
}

/*
 * mmap calls don't have to be matched with calls to munmap, so we can unmap
 * just the pages we don't need. However, as we don't know a priori if addresses
 * are handed out in increasing or decreasing order, we have to try both
 * directions (depending on the environment, one will always fail).
 */
static void
GetNewChunk(void** aAddress, void** aRetainedAddr, size_t size, size_t alignment)
{
    void* address = *aAddress;
    void* retainedAddr = nullptr;
    bool addrsGrowDown = growthDirection <= 0;
    int i = 0;
    for (; i < 2; ++i) {
        /* Try the direction indicated by growthDirection. */
        if (addrsGrowDown) {
            size_t offset = OffsetFromAligned(address, alignment);
            void* head = (void*)((uintptr_t)address - offset);
            void* tail = (void*)((uintptr_t)head + size);
            if (MapMemoryAt(head, offset)) {
                UnmapPages(tail, offset);
                if (growthDirection >= -8)
                    --growthDirection;
                address = head;
                break;
            }
        } else {
            size_t offset = alignment - OffsetFromAligned(address, alignment);
            void* head = (void*)((uintptr_t)address + offset);
            void* tail = (void*)((uintptr_t)address + size);
            if (MapMemoryAt(tail, offset)) {
                UnmapPages(address, offset);
                if (growthDirection <= 8)
                    ++growthDirection;
                address = head;
                break;
            }
        }
        /* If we're confident in the growth direction, don't try the other. */
        if (growthDirection < -8 || growthDirection > 8)
            break;
        /* If that failed, try the opposite direction. */
        addrsGrowDown = !addrsGrowDown;
    }
    /* If our current chunk cannot be aligned, see if the next one is aligned. */
    if (OffsetFromAligned(address, alignment)) {
        retainedAddr = address;
        address = MapMemory(size);
    }
    *aAddress = address;
    *aRetainedAddr = retainedAddr;
}

void
UnmapPages(void* p, size_t size)
{
    if (munmap(p, size))
        MOZ_ASSERT(errno == ENOMEM);
}

bool
MarkPagesUnused(void* p, size_t size)
{
    if (!DecommitEnabled())
        return false;

    MOZ_ASSERT(OffsetFromAligned(p, pageSize) == 0);
    int result = madvise(p, size, MADV_DONTNEED);
    return result != -1;
}

bool
MarkPagesInUse(void* p, size_t size)
{
    if (!DecommitEnabled())
        return true;

    MOZ_ASSERT(OffsetFromAligned(p, pageSize) == 0);
    return true;
}

size_t
GetPageFaultCount()
{
    struct rusage usage;
    int err = getrusage(RUSAGE_SELF, &usage);
    if (err)
        return 0;
    return usage.ru_majflt;
}

void*
AllocateMappedContent(int fd, size_t offset, size_t length, size_t alignment)
{
#define NEED_PAGE_ALIGNED 0
    size_t pa_start; // Page aligned starting
    size_t pa_end; // Page aligned ending
    size_t pa_size; // Total page aligned size
    struct stat st;
    uint8_t* buf;

    // Make sure file exists and do sanity check for offset and size.
    if (fstat(fd, &st) < 0 || offset >= (size_t) st.st_size ||
        length == 0 || length > (size_t) st.st_size - offset)
        return nullptr;

    // Check for minimal alignment requirement.
#if NEED_PAGE_ALIGNED
    alignment = std::max(alignment, pageSize);
#endif
    if (offset & (alignment - 1))
        return nullptr;

    // Page aligned starting of the offset.
    pa_start = offset & ~(pageSize - 1);
    // Calculate page aligned ending by adding one page to the page aligned
    // starting of data end position(offset + length - 1).
    pa_end = ((offset + length - 1) & ~(pageSize - 1)) + pageSize;
    pa_size = pa_end - pa_start;

    // Ask for a continuous memory location.
    buf = (uint8_t*) MapMemory(pa_size);
    if (!buf)
        return nullptr;

    buf = (uint8_t*) MapMemoryAt(buf, pa_size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_FIXED, fd, pa_start);
    if (!buf)
        return nullptr;

    // Reset the data before target file, which we don't need to see.
    memset(buf, 0, offset - pa_start);

    // Reset the data after target file, which we don't need to see.
    memset(buf + (offset - pa_start) + length, 0, pa_end - (offset + length));

    return buf + (offset - pa_start);
}

void
DeallocateMappedContent(void* p, size_t length)
{
    void* pa_start; // Page aligned starting
    size_t total_size; // Total allocated size

    pa_start = (void*)(uintptr_t(p) & ~(pageSize - 1));
    total_size = ((uintptr_t(p) + length) & ~(pageSize - 1)) + pageSize - uintptr_t(pa_start);
    if (munmap(pa_start, total_size))
        MOZ_ASSERT(errno == ENOMEM);
}

#else
#error "Memory mapping functions are not defined for your OS."
#endif

void
ProtectPages(void* p, size_t size)
{
    MOZ_ASSERT(size % pageSize == 0);
#if defined(XP_WIN)
    DWORD oldProtect;
    if (!VirtualProtect(p, size, PAGE_NOACCESS, &oldProtect))
        MOZ_CRASH("VirtualProtect(PAGE_NOACCESS) failed");
    MOZ_ASSERT(oldProtect == PAGE_READWRITE);
#else  // assume Unix
    if (mprotect(p, size, PROT_NONE))
        MOZ_CRASH("mprotect(PROT_NONE) failed");
#endif
}

void
UnprotectPages(void* p, size_t size)
{
    MOZ_ASSERT(size % pageSize == 0);
#if defined(XP_WIN)
    DWORD oldProtect;
    if (!VirtualProtect(p, size, PAGE_READWRITE, &oldProtect))
        MOZ_CRASH("VirtualProtect(PAGE_READWRITE) failed");
    MOZ_ASSERT(oldProtect == PAGE_NOACCESS);
#else  // assume Unix
    if (mprotect(p, size, PROT_READ | PROT_WRITE))
        MOZ_CRASH("mprotect(PROT_READ | PROT_WRITE) failed");
#endif
}

} // namespace gc
} // namespace js

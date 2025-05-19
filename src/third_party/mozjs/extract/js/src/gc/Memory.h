/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Memory_h
#define gc_Memory_h

#include <stddef.h>

namespace js {
namespace gc {

// Sanity check that our compiled configuration matches the currently
// running instance and initialize any runtime data needed for allocation.
void InitMemorySubsystem();

// The page size as reported by the operating system.
size_t SystemPageSize();

// The number of bits that may be set in a valid address, as
// reported by the operating system or measured at startup.
size_t SystemAddressBits();

// The number of bytes of virtual memory that may be allocated or mapped, as
// reported by the operating system on certain platforms. If no limit was able
// to be determined, then it will be size_t(-1).
size_t VirtualMemoryLimit();

// The scattershot allocator is used on platforms that have a large address
// range. On these platforms we allocate at random addresses.
bool UsingScattershotAllocator();

// Allocate or deallocate pages from the system with the given alignment.
// Pages will be read/write-able.
void* MapAlignedPages(size_t length, size_t alignment);
void UnmapPages(void* region, size_t length);

// We can only decommit unused pages if the page size is less than or equal to
// the hardcoded Arena size for the running process.
bool DecommitEnabled();

// Tell the OS that the given pages are not in use, so they should not be
// written to a paging file. This may be a no-op on some platforms.
bool MarkPagesUnusedSoft(void* region, size_t length);

// Tell the OS that the given pages are not in use and it can decommit them
// immediately. This may defer to MarkPagesUnusedSoft and must be paired with
// MarkPagesInUse to use the pages again.
bool MarkPagesUnusedHard(void* region, size_t length);

// Undo |MarkPagesUnusedSoft|: tell the OS that the given pages are of interest
// and should be paged in and out normally. This may be a no-op on some
// platforms.  May make pages read/write-able.
void MarkPagesInUseSoft(void* region, size_t length);

// Undo |MarkPagesUnusedHard|: tell the OS that the given pages are of interest
// and should be paged in and out normally. This may be a no-op on some
// platforms. Callers must check the result, false could mean that the pages
// are not available.  May make pages read/write.
[[nodiscard]] bool MarkPagesInUseHard(void* region, size_t length);

// Returns #(hard faults) + #(soft faults)
size_t GetPageFaultCount();

// Allocate memory mapped content.
// The offset must be aligned according to alignment requirement.
void* AllocateMappedContent(int fd, size_t offset, size_t length,
                            size_t alignment);

// Deallocate memory mapped content.
void DeallocateMappedContent(void* region, size_t length);

void* TestMapAlignedPagesLastDitch(size_t length, size_t alignment);

void ProtectPages(void* region, size_t length);
void MakePagesReadOnly(void* region, size_t length);
void UnprotectPages(void* region, size_t length);

}  // namespace gc
}  // namespace js

#endif /* gc_Memory_h */

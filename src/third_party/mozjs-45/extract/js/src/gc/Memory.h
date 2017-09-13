/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

size_t SystemPageSize();

// Allocate or deallocate pages from the system with the given alignment.
void* MapAlignedPages(size_t size, size_t alignment);
void UnmapPages(void* p, size_t size);

// Tell the OS that the given pages are not in use, so they should not be
// written to a paging file. This may be a no-op on some platforms.
bool MarkPagesUnused(void* p, size_t size);

// Undo |MarkPagesUnused|: tell the OS that the given pages are of interest
// and should be paged in and out normally. This may be a no-op on some
// platforms.
bool MarkPagesInUse(void* p, size_t size);

// Returns #(hard faults) + #(soft faults)
size_t GetPageFaultCount();

// Allocate memory mapped content.
// The offset must be aligned according to alignment requirement.
void* AllocateMappedContent(int fd, size_t offset, size_t length, size_t alignment);

// Deallocate memory mapped content.
void DeallocateMappedContent(void* p, size_t length);

void* TestMapAlignedPagesLastDitch(size_t size, size_t alignment);

void ProtectPages(void* p, size_t size);
void UnprotectPages(void* p, size_t size);

} // namespace gc
} // namespace js

#endif /* gc_Memory_h */

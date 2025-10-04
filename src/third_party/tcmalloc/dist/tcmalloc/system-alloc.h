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
//
// Routine that uses sbrk/mmap to allocate memory from the system.
// Useful for implementing malloc.

#ifndef TCMALLOC_SYSTEM_ALLOC_H_
#define TCMALLOC_SYSTEM_ALLOC_H_

#include <stddef.h>

#include "absl/base/attributes.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct AddressRange {
  void* ptr;
  size_t bytes;
};

// REQUIRES: "alignment" is a power of two or "0" to indicate default alignment
// REQUIRES: "alignment" and "size" <= kTagMask
//
// Allocate and return "bytes" of zeroed memory.  The allocator may optionally
// return more bytes than asked for (i.e. return an entire "huge" page).
//
// The returned pointer is a multiple of "alignment" if non-zero. The
// returned pointer will always be aligned suitably for holding a
// void*, double, or size_t. In addition, if this platform defines
// ABSL_CACHELINE_ALIGNED, the return pointer will always be cacheline
// aligned.
//
// The returned pointer is guaranteed to satisfy GetMemoryTag(ptr) == "tag".
// Returns nullptr when out of memory.
AddressRange SystemAlloc(size_t bytes, size_t alignment, MemoryTag tag);

// Returns the number of times we failed to give pages back to the OS after a
// call to SystemRelease.
int SystemReleaseErrors();

// This call is a hint to the operating system that the pages
// contained in the specified range of memory will not be used for a
// while, and can be released for use by other processes or the OS.
// Pages which are released in this way may be destroyed (zeroed) by
// the OS.  The benefit of this function is that it frees memory for
// use by the system, the cost is that the pages are faulted back into
// the address space next time they are touched, which can impact
// performance.  (Only pages fully covered by the memory region will
// be released, partial pages will not.)
//
// Returns true on success.
ABSL_MUST_USE_RESULT bool SystemRelease(void* start, size_t length);

// This call is the inverse of SystemRelease: the pages in this range
// are in use and should be faulted in.  (In principle this is a
// best-effort hint, but in practice we will unconditionally fault the
// range.)
// REQUIRES: [start, start + length) is a range aligned to 4KiB boundaries.
inline void SystemBack(void* start, size_t length) {
  // TODO(b/134694141): use madvise when we have better support for that;
  // taking faults is not free.

  // TODO(b/134694141): enable this, if we can avoid causing trouble for apps
  // that routinely make large mallocs they never touch (sigh).
}

// Returns the current address region factory.
AddressRegionFactory* GetRegionFactory();

// Sets the current address region factory to factory.
void SetRegionFactory(AddressRegionFactory* factory);

// Reserves using mmap() a region of memory of the requested size and alignment,
// with the bits specified by kTagMask set according to tag.
//
// REQUIRES: pagesize <= alignment <= kTagMask
// REQUIRES: size <= kTagMask
void* MmapAligned(size_t size, size_t alignment, MemoryTag tag);

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SYSTEM_ALLOC_H_

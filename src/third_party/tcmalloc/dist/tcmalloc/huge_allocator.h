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
// Tracking information for the available range of hugepages,
// and a basic allocator for unmapped hugepages.
#ifndef TCMALLOC_HUGE_ALLOCATOR_H_
#define TCMALLOC_HUGE_ALLOCATOR_H_

#include <stddef.h>

#include "absl/base/attributes.h"
#include "tcmalloc/huge_address_map.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/metadata_allocator.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// these typedefs allow replacement of tcmalloc::System* for tests.
class VirtualAllocator {
 public:
  VirtualAllocator() = default;
  virtual ~VirtualAllocator() = default;

  VirtualAllocator(const VirtualAllocator&) = delete;
  VirtualAllocator(VirtualAllocator&&) = delete;
  VirtualAllocator& operator=(const VirtualAllocator&) = delete;
  VirtualAllocator& operator=(VirtualAllocator&&) = delete;

  // Allocates bytes of virtual address space with align alignment.
  ABSL_MUST_USE_RESULT virtual AddressRange operator()(size_t bytes,
                                                       size_t align) = 0;
};

// This tracks available ranges of hugepages and fulfills requests for
// usable memory, allocating more from the system as needed.  All
// hugepages are treated as (and assumed to be) unbacked.
class HugeAllocator {
 public:
  constexpr HugeAllocator(
      VirtualAllocator& allocate ABSL_ATTRIBUTE_LIFETIME_BOUND,
      MetadataAllocator& meta_allocate ABSL_ATTRIBUTE_LIFETIME_BOUND)
      : free_(meta_allocate), allocate_(allocate) {}

  // Obtain a range of n unbacked hugepages, distinct from all other
  // calls to Get (other than those that have been Released.)
  HugeRange Get(HugeLength n);

  // Returns a range of hugepages for reuse by subsequent Gets().
  // REQUIRES: <r> is the return value (or a subrange thereof) of a previous
  // call to Get(); neither <r> nor any overlapping range has been released
  // since that Get().
  void Release(HugeRange r);

  // Total memory requested from the system, whether in use or not,
  HugeLength system() const { return from_system_; }
  // Unused memory in the allocator.
  HugeLength size() const { return from_system_ - in_use_; }

  void AddSpanStats(SmallSpanStats* small, LargeSpanStats* large) const;

  BackingStats stats() const {
    BackingStats s;
    s.system_bytes = system().in_bytes();
    s.free_bytes = 0;
    s.unmapped_bytes = size().in_bytes();
    return s;
  }

  void Print(Printer* out);
  void PrintInPbtxt(PbtxtRegion* hpaa) const;

 private:
  // We're constrained in several ways by existing code.  Hard requirements:
  // * no radix tree or similar O(address space) external space tracking
  // * support sub releasing
  // * low metadata overhead
  // * no pre-allocation.
  // * reasonable space overhead
  //
  // We use a treap ordered on addresses to track.  This isn't the most
  // efficient thing ever but we're about to hit 100usec+/hugepage
  // backing costs if we've gotten this far; the last few bits of performance
  // don't matter, and most of the simple ideas can't hit all of the above
  // requirements.
  HugeAddressMap free_;
  HugeAddressMap::Node* Find(HugeLength n);

  void CheckFreelist();
  void DebugCheckFreelist() {
#ifndef NDEBUG
    CheckFreelist();
#endif
  }

  HugeLength from_system_{NHugePages(0)};
  HugeLength in_use_{NHugePages(0)};

  VirtualAllocator& allocate_;
  HugeRange AllocateRange(HugeLength n);
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_ALLOCATOR_H_

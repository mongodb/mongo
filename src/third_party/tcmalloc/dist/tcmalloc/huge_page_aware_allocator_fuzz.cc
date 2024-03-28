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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/mock_huge_page_static_forwarder.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

namespace {
using tcmalloc::tcmalloc_internal::AccessDensityPrediction;
using tcmalloc::tcmalloc_internal::BackingStats;
using tcmalloc::tcmalloc_internal::HugePageFillerAllocsOption;
using tcmalloc::tcmalloc_internal::HugeRegionUsageOption;
using tcmalloc::tcmalloc_internal::kMaxSize;
using tcmalloc::tcmalloc_internal::kMinObjectsToMove;
using tcmalloc::tcmalloc_internal::kNumaPartitions;
using tcmalloc::tcmalloc_internal::kPagesPerHugePage;
using tcmalloc::tcmalloc_internal::kTop;
using tcmalloc::tcmalloc_internal::Length;
using tcmalloc::tcmalloc_internal::MemoryTag;
using tcmalloc::tcmalloc_internal::PageHeapSpinLockHolder;
using tcmalloc::tcmalloc_internal::PbtxtRegion;
using tcmalloc::tcmalloc_internal::Printer;
using tcmalloc::tcmalloc_internal::SizeMap;
using tcmalloc::tcmalloc_internal::Span;
using tcmalloc::tcmalloc_internal::SpanAllocInfo;
using tcmalloc::tcmalloc_internal::huge_page_allocator_internal::
    FakeStaticForwarder;
using tcmalloc::tcmalloc_internal::huge_page_allocator_internal::
    HugePageAwareAllocator;
using tcmalloc::tcmalloc_internal::huge_page_allocator_internal::
    HugePageAwareAllocatorOptions;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 13 || size > 100000) {
    // size < 13 for needing some entropy to initialize huge page aware
    // allocator.
    //
    // size > 100000 for avoiding overly large inputs given we do extra
    // checking.
    return 0;
  }

#if ABSL_HAVE_ADDRESS_SANITIZER
  // Since asan introduces runtime overhead, limit size of fuzz targets further.
  if (size > 10000) {
    return 0;
  }
#endif

  // We interpret data as a small DSL for exploring the state space of
  // HugePageAwareAllocator.
  //
  // [0] - Memory tag.
  // [1] - HugeRegionsMode.
  // [2:4] - Reserved.
  // [5] - Determine if we use separate filler allocs based on number of
  // objects per span.
  // [6:12] - Reserved.
  //
  // Afterwards, we read 9 bytes at a time until the buffer is exhausted.
  // [i + 0]        - Specifies an operation to perform on the allocator
  // [i + 1, i + 8] - Specifies an integer. We use this as a source of
  //                  deterministic entropy to allow inputs to be replayed.
  //                  For example, this input can provide a Length to
  //                  allocate, or the index of the previous allocation to
  //                  deallocate.

  constexpr MemoryTag kTagOptions[] = {
      MemoryTag::kSampled, MemoryTag::kNormalP0, MemoryTag::kNormalP1,
      MemoryTag::kNormal, MemoryTag::kCold};
  constexpr int kTagSize = sizeof(kTagOptions) / sizeof(MemoryTag);
  static_assert(kTagSize > 0);
  MemoryTag tag = kTagOptions[data[0] % kTagSize];
  // Use kNormalP1 memory tag only if we have more than one NUMA partitions.
  tag = (kNumaPartitions == 1 && tag == MemoryTag::kNormalP1)
            ? MemoryTag::kNormalP0
            : tag;

  const HugeRegionUsageOption huge_region_option =
      data[1] >= 128 ? HugeRegionUsageOption::kDefault
                     : HugeRegionUsageOption::kUseForAllLargeAllocs;

  const HugePageFillerAllocsOption allocs_option =
      data[5] >= 128 ? HugePageFillerAllocsOption::kUnifiedAllocs
                     : HugePageFillerAllocsOption::kSeparateAllocs;

  // data[6:12] - Reserve additional bytes for any features we might want to add
  // in the future.
  data += 13;
  size -= 13;

  // HugePageAwareAllocator can't be destroyed cleanly, so we store a pointer
  // to one and construct in place.
  void* p = malloc(sizeof(HugePageAwareAllocator<FakeStaticForwarder>));
  HugePageAwareAllocatorOptions options;
  options.tag = tag;
  options.use_huge_region_more_often = huge_region_option;
  options.allocs_for_sparse_and_dense_spans = allocs_option;
  HugePageAwareAllocator<FakeStaticForwarder>* allocator;
  allocator = new (p) HugePageAwareAllocator<FakeStaticForwarder>(options);
  auto& forwarder = allocator->forwarder();

  struct SpanInfo {
    Span* span;
    size_t objects_per_span;
  };
  std::vector<SpanInfo> allocs;
  Length allocated;

  for (size_t i = 0; i + 9 <= size; i += 9) {
    const uint16_t op = data[i];
    uint64_t value;
    memcpy(&value, &data[i + 1], sizeof(value));

    switch (op & 0x7) {
      case 0: {
        // Aligned allocate.  We divide up our random value by:
        //
        // value[0:15]  - We choose a Length to allocate.
        // value[16:31] - We select num_to_objects, i.e. the number of objects
        // to allocate.
        // value[32:47] - Alignment.
        // value[48] - Should we use aligned allocate?
        // value[49] - Is the span sparsely- or densely-accessed?
        // value[63:50] - Reserved.
        const Length length(std::clamp<size_t>(
            value & 0xFFFF, 1, kPagesPerHugePage.raw_num() - 1));
        size_t num_objects = std::max<size_t>((value >> 16) & 0xFFFF, 1);
        size_t object_size = length.in_bytes() / num_objects;
        const bool use_aligned = ((value >> 48) & 0x1) == 0;
        const Length align(
            use_aligned ? std::clamp<size_t>((value >> 32) & 0xFFFF, 1,
                                             kPagesPerHugePage.raw_num() - 1)
                        : 1);

        AccessDensityPrediction density = ((value >> 49) & 0x1) == 0
                                              ? AccessDensityPrediction::kSparse
                                              : AccessDensityPrediction::kDense;
        if (object_size > kMaxSize || align > Length(1)) {
          // Truncate to a single object.
          num_objects = 1;
          // TODO(b/283843066): Revisit this once we have fluid partitioning.
          density = AccessDensityPrediction::kSparse;
        } else if (!SizeMap::IsValidSizeClass(object_size, length.raw_num(),
                                              kMinObjectsToMove)) {
          // This is an invalid size class, so skip it.
          break;
        }

        // Allocation is too big for filler if we try to allocate >
        // kPagesPerHugePage / 2 run of pages. The allocations may go to
        // HugeRegion and that might lead to donations with kSparse density.
        if (length > kPagesPerHugePage / 2) {
          density = AccessDensityPrediction::kSparse;
        }

        Span* s;
        SpanAllocInfo alloc_info = {.objects_per_span = num_objects,
                                    .density = density};
        if (use_aligned) {
          s = allocator->NewAligned(length, align, alloc_info);
        } else {
          s = allocator->New(length, alloc_info);
        }
        TC_CHECK_NE(s, nullptr);
        CHECK_GE(s->num_pages().raw_num(), length.raw_num());

        allocs.push_back(SpanInfo{s, num_objects});
        allocated += s->num_pages();
        break;
      }
      case 1: {
        // Deallocate.  We divide up our random value by:
        //
        // value - We choose index in allocs to deallocate a span.

        if (allocs.empty()) break;

        const size_t pos = value % allocs.size();
        std::swap(allocs[pos], allocs[allocs.size() - 1]);

        SpanInfo span_info = allocs[allocs.size() - 1];
        allocs.resize(allocs.size() - 1);
        allocated -= span_info.span->num_pages();
        {
          PageHeapSpinLockHolder l;
          allocator->Delete(span_info.span, span_info.objects_per_span);
        }
        break;
      }
      case 2: {
        // Release pages.  We divide up our random value by:
        //
        // value[7:0] - Choose number of pages to release.
        // value[63:8] - Reserved.
        Length desired(value & 0x00FF);
        {
          PageHeapSpinLockHolder l;
          allocator->ReleaseAtLeastNPages(desired);
        }
        break;
      }
      case 3: {
        // Release pages by breaking hugepages.  We divide up our random value
        // by:
        //
        // value[15:0] - Choose number of pages to release.
        // value[63:16] - Reserved.
        Length desired(value & 0xFFFF);
        Length released;
        size_t releasable_bytes;
        {
          PageHeapSpinLockHolder l;
          releasable_bytes = allocator->FillerStats().free_bytes +
                             allocator->RegionsFreeBacked().in_bytes();
          released = allocator->ReleaseAtLeastNPagesBreakingHugepages(desired);
        }

        if (forwarder.release_succeeds()) {
          CHECK_GE(released.in_bytes(),
                   std::min(desired.in_bytes(), releasable_bytes));
        } else {
          // TODO(b/271282540):  This is not strict equality due to
          // HugePageFiller's unmapping_unaccounted_ state.  Narrow this bound.
          CHECK_GE(released.in_bytes(), 0);
        }
        break;
      }
      case 4: {
        // Gather stats in pbtxt format.
        //
        // value is unused.
        std::string s;
        s.resize(1 << 20);
        Printer p(&s[0], s.size());
        PbtxtRegion region(&p, kTop);
        allocator->PrintInPbtxt(&region);
        break;
      }
      case 5: {
        // Print stats.
        //
        // value[0]: Choose if we print everything.
        // value[63:1]: Reserved.
        std::string s;
        s.resize(1 << 20);
        Printer p(&s[0], s.size());
        bool everything = (value % 2 == 0);
        allocator->Print(&p, everything);
        break;
      }
      case 6: {
        // Gather and check stats.
        //
        // value is unused.
        BackingStats stats;
        {
          PageHeapSpinLockHolder l;
          stats = allocator->stats();
        }
        uint64_t used_bytes =
            stats.system_bytes - stats.free_bytes - stats.unmapped_bytes;
        CHECK_EQ(used_bytes, allocated.in_bytes());
        break;
      }
      case 7: {
        // Change a runtime parameter.
        //
        // value[0:2] - Select parameter
        // value[3:7] - Reserved
        // value[8:63] - The value
        const uint64_t actual_value = value >> 8;
        switch (value & 0x7) {
          case 0:
            forwarder.set_filler_skip_subrelease_interval(
                absl::Nanoseconds(actual_value));
            forwarder.set_filler_skip_subrelease_short_interval(
                absl::ZeroDuration());
            forwarder.set_filler_skip_subrelease_long_interval(
                absl::ZeroDuration());
            break;
          case 1:
            forwarder.set_filler_skip_subrelease_interval(absl::ZeroDuration());
            forwarder.set_filler_skip_subrelease_short_interval(
                absl::Nanoseconds(actual_value));
            break;
          case 2:
            forwarder.set_filler_skip_subrelease_interval(absl::ZeroDuration());
            forwarder.set_filler_skip_subrelease_long_interval(
                absl::Nanoseconds(actual_value));
            break;
          case 3:
            forwarder.set_release_partial_alloc_pages(actual_value & 0x1);
            break;
          case 4:
            forwarder.set_hpaa_subrelease(actual_value & 0x1);
            break;
          case 5:
            forwarder.set_release_succeeds(actual_value & 0x1);
            break;
        }

        break;
      }
    }
  }

  // Clean up.
  for (auto span_info : allocs) {
    PageHeapSpinLockHolder l;
    allocated -= span_info.span->num_pages();
    allocator->Delete(span_info.span, span_info.objects_per_span);
  }
  CHECK_EQ(allocated.in_bytes(), 0);
  free(allocator);
  return 0;
}

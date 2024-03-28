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

#include "tcmalloc/page_heap.h"

#include <stddef.h>
#include <stdlib.h>

#include <memory>
#include <new>

#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "absl/memory/memory.h"
#include "tcmalloc/common.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/static_vars.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// PageHeap expands by kMinSystemAlloc by default, so use this as the minimum
// Span length to not get more memory than expected.
constexpr Length kMinSpanLength = BytesToLengthFloor(kMinSystemAlloc);

void CheckStats(const PageHeap* ph, Length system_pages, Length free_pages,
                Length unmapped_pages) ABSL_LOCKS_EXCLUDED(pageheap_lock) {
  BackingStats stats;
  {
    PageHeapSpinLockHolder l;
    stats = ph->stats();
  }

  ASSERT_EQ(system_pages.in_bytes(), stats.system_bytes);
  ASSERT_EQ(free_pages.in_bytes(), stats.free_bytes);
  ASSERT_EQ(unmapped_pages.in_bytes(), stats.unmapped_bytes);
}

static void Delete(PageHeap* ph, Span* s, size_t objects_per_span)
    ABSL_LOCKS_EXCLUDED(pageheap_lock) {
  {
    PageHeapSpinLockHolder l;
    ph->Delete(s, objects_per_span);
  }
}

static Length Release(PageHeap* ph, Length n) {
  PageHeapSpinLockHolder l;
  return ph->ReleaseAtLeastNPages(n);
}

class PageHeapTest : public ::testing::Test {
 public:
  PageHeapTest() {
    // If this test is not linked against TCMalloc, the global arena used for
    // metadata will not be initialized.
    tc_globals.InitIfNecessary();
  }
};

// TODO(b/36484267): replace this test wholesale.
TEST_F(PageHeapTest, Stats) {
  auto pagemap = std::make_unique<PageMap>();
  void* memory = calloc(1, sizeof(PageHeap));
  PageHeap* ph = new (memory) PageHeap(pagemap.get(), MemoryTag::kNormal);

  // Empty page heap
  CheckStats(ph, Length(0), Length(0), Length(0));

  // Allocate a span 's1'
  constexpr SpanAllocInfo kSpanAllocInfo = {10,
                                            AccessDensityPrediction::kSparse};
  Span* s1 = ph->New(kMinSpanLength, kSpanAllocInfo);
  CheckStats(ph, kMinSpanLength, Length(0), Length(0));

  // Allocate an aligned span 's2'
  static const Length kHalf = kMinSpanLength / 2;
  Span* s2 = ph->NewAligned(kHalf, kHalf, kSpanAllocInfo);
  ASSERT_EQ(s2->first_page().index() % kHalf.raw_num(), 0);
  CheckStats(ph, kMinSpanLength * 2, Length(0), kHalf);

  // Delete the old one
  Delete(ph, s1, kSpanAllocInfo.objects_per_span);
  CheckStats(ph, kMinSpanLength * 2, kMinSpanLength, kHalf);

  // Release the space from there:
  Length released = Release(ph, Length(1));
  ASSERT_EQ(released, kMinSpanLength);
  CheckStats(ph, kMinSpanLength * 2, Length(0), kHalf + kMinSpanLength);

  // and delete the new one
  Delete(ph, s2, kSpanAllocInfo.objects_per_span);
  CheckStats(ph, kMinSpanLength * 2, kHalf, kHalf + kMinSpanLength);

  free(memory);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

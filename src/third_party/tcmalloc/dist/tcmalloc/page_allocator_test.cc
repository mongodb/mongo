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
// Tests for infrastructure common to page allocator implementations
// (stats and logging.)
#include "tcmalloc/page_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <new>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/spinlock.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/page_allocator_test_util.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class PageAllocatorTest : public testing::Test {
 protected:
  // Not in constructor so subclasses can mess about with environment
  // variables.
  void SetUp() override {
    // If this test is not linked against TCMalloc, the global arena used for
    // metadata will not be initialized.
    tc_globals.InitIfNecessary();

    before_ = MallocExtension::GetRegionFactory();
    extra_ = new ExtraRegionFactory(before_);
    MallocExtension::SetRegionFactory(extra_);
    void* p = malloc(sizeof(PageAllocator));
    allocator_ = new (p) PageAllocator;
  }
  void TearDown() override {
    MallocExtension::SetRegionFactory(before_);
    delete extra_;
    free(allocator_);
  }

  Span* New(Length n, SpanAllocInfo span_alloc_info,
            MemoryTag tag = MemoryTag::kNormal) {
    return allocator_->New(n, span_alloc_info, tag);
  }
  Span* NewAligned(Length n, Length align, SpanAllocInfo span_alloc_info,
                   MemoryTag tag = MemoryTag::kNormal) {
    return allocator_->NewAligned(n, align, span_alloc_info, tag);
  }
  void Delete(Span* s, size_t objects_per_span,
              MemoryTag tag = MemoryTag::kNormal) {
    PageHeapSpinLockHolder l;
    allocator_->Delete(s, objects_per_span, tag);
  }

  std::string Print() {
    std::vector<char> buf(1024 * 1024);
    Printer out(&buf[0], buf.size());
    allocator_->Print(&out, MemoryTag::kNormal);

    return std::string(&buf[0]);
  }

  PageAllocator* allocator_;
  ExtraRegionFactory* extra_;
  AddressRegionFactory* before_;
};

// We've already tested in stats_test that PageAllocInfo keeps good stats;
// here we're just testing that we make the proper Record calls.
TEST_F(PageAllocatorTest, Record) {
  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/7,
                                       AccessDensityPrediction::kSparse};
  for (int i = 0; i < 15; ++i) {
    Delete(New(Length(1), kSpanInfo), kSpanInfo.objects_per_span);
  }

  std::vector<Span*> spans;
  for (int i = 0; i < 20; ++i) {
    spans.push_back(New(Length(2), kSpanInfo));
  }

  for (int i = 0; i < 25; ++i) {
    Delete(NewAligned(Length(3), Length(2), kSpanInfo),
           kSpanInfo.objects_per_span);
  }
  {
    PageHeapSpinLockHolder l;
    auto info = allocator_->info(MemoryTag::kNormal);

    ASSERT_EQ(15, info.counts_for(Length(1)).nalloc);
    ASSERT_EQ(15, info.counts_for(Length(1)).nfree);

    ASSERT_EQ(20, info.counts_for(Length(2)).nalloc);
    ASSERT_EQ(0, info.counts_for(Length(2)).nfree);

    ASSERT_EQ(25, info.counts_for(Length(3)).nalloc);
    ASSERT_EQ(25, info.counts_for(Length(3)).nfree);

    for (auto i = Length(4); i <= kMaxPages; ++i) {
      ASSERT_EQ(0, info.counts_for(i).nalloc);
      ASSERT_EQ(0, info.counts_for(i).nfree);
    }

    const Length absurd =
        Length(uintptr_t{1} << (kAddressBits - 1 - kPageShift));
    for (Length i = kMaxPages + Length(1); i < absurd; i *= 2) {
      ASSERT_EQ(0, info.counts_for(i).nalloc);
      ASSERT_EQ(0, info.counts_for(i).nfree);
    }
  }
  for (auto s : spans) Delete(s, kSpanInfo.objects_per_span);
}

// And that we call the print method properly.
TEST_F(PageAllocatorTest, PrintIt) {
  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/17,
                                       AccessDensityPrediction::kDense};
  Delete(New(Length(1), kSpanInfo), kSpanInfo.objects_per_span);
  std::string output = Print();
  EXPECT_THAT(output, testing::ContainsRegex("stats on allocation sizes"));
}

TEST_F(PageAllocatorTest, ShrinkFailureTest) {
  // Turn off subrelease so that we take the ShrinkHardBy path.
  const bool old_subrelease = Parameters::hpaa_subrelease();
  Parameters::set_hpaa_subrelease(false);

  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/1,
                                       AccessDensityPrediction::kSparse};
  Span* normal = New(kPagesPerHugePage / 2, kSpanInfo, MemoryTag::kNormal);
  Span* sampled = New(kPagesPerHugePage / 2, kSpanInfo, MemoryTag::kSampled);

  BackingStats stats;
  {
    PageHeapSpinLockHolder l;
    stats = allocator_->stats();
  }
  EXPECT_EQ(stats.system_bytes, 2 * kHugePageSize);
  EXPECT_EQ(stats.free_bytes, kHugePageSize);
  EXPECT_EQ(stats.unmapped_bytes, 0);

  // Choose a limit so that we hit and we are not able to satisfy it.
  allocator_->set_limit(kPagesPerHugePage.in_bytes(), PageAllocator::kSoft);
  {
    PageHeapSpinLockHolder l;
    allocator_->ShrinkToUsageLimit(Length(0));
  }
  EXPECT_LE(1, allocator_->limit_hits(PageAllocator::kSoft));
  EXPECT_LE(
      0, allocator_->successful_shrinks_after_limit_hit(PageAllocator::kSoft));

  Delete(normal, kSpanInfo.objects_per_span, MemoryTag::kNormal);
  Delete(sampled, kSpanInfo.objects_per_span, MemoryTag::kSampled);
  Parameters::set_hpaa_subrelease(old_subrelease);
}

TEST_F(PageAllocatorTest, b270916852) {
  // Turn off subrelease so that we take the ShrinkHardBy path.
  const bool old_subrelease = Parameters::hpaa_subrelease();
  Parameters::set_hpaa_subrelease(false);

  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/1,
                                       AccessDensityPrediction::kSparse};
  Span* normal = New(kPagesPerHugePage / 2, kSpanInfo, MemoryTag::kNormal);
  Span* sampled = New(kPagesPerHugePage / 2, kSpanInfo, MemoryTag::kSampled);

  BackingStats stats;
  {
    PageHeapSpinLockHolder l;
    stats = allocator_->stats();
  }
  EXPECT_EQ(stats.system_bytes, 2 * kHugePageSize);
  EXPECT_EQ(stats.free_bytes, kHugePageSize);
  EXPECT_EQ(stats.unmapped_bytes, 0);

  // Choose a limit so that
  // 1.  We hit it.  It should be less than stats.system_bytes.
  // 2.  It is below current usage.
  // 3.  It is above what can be released from a single page heap.
  const size_t metadata_bytes = []() {
    PageHeapSpinLockHolder l;
    return tc_globals.metadata_bytes();
  }();
  allocator_->set_limit(
      metadata_bytes + (3 * kPagesPerHugePage / 2).in_bytes() + kPageSize,
      PageAllocator::kSoft);
  {
    PageHeapSpinLockHolder l;
    allocator_->ShrinkToUsageLimit(Length(0));
  }
  EXPECT_LE(1, allocator_->limit_hits(PageAllocator::kSoft));
  EXPECT_LE(
      1, allocator_->successful_shrinks_after_limit_hit(PageAllocator::kSoft));

  Delete(normal, kSpanInfo.objects_per_span, MemoryTag::kNormal);
  Delete(sampled, kSpanInfo.objects_per_span, MemoryTag::kSampled);
  Parameters::set_hpaa_subrelease(old_subrelease);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

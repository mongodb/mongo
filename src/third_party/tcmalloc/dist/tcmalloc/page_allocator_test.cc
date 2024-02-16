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
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/page_allocator_test_util.h"
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

  Span* New(Length n, size_t objects_per_span) {
    return allocator_->New(n, objects_per_span, MemoryTag::kNormal);
  }
  Span* NewAligned(Length n, Length align, size_t objects_per_span) {
    return allocator_->NewAligned(n, align, objects_per_span,
                                  MemoryTag::kNormal);
  }
  void Delete(Span* s, size_t objects_per_span) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    allocator_->Delete(s, objects_per_span, MemoryTag::kNormal);
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
  constexpr size_t kObjectsPerSpan = 7;
  for (int i = 0; i < 15; ++i) {
    Delete(New(Length(1), kObjectsPerSpan), kObjectsPerSpan);
  }

  std::vector<Span*> spans;
  for (int i = 0; i < 20; ++i) {
    spans.push_back(New(Length(2), kObjectsPerSpan));
  }

  for (int i = 0; i < 25; ++i) {
    Delete(NewAligned(Length(3), Length(2), kObjectsPerSpan), kObjectsPerSpan);
  }
  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    auto info = allocator_->info(MemoryTag::kNormal);

    CHECK_CONDITION(15 == info.counts_for(Length(1)).nalloc);
    CHECK_CONDITION(15 == info.counts_for(Length(1)).nfree);

    CHECK_CONDITION(20 == info.counts_for(Length(2)).nalloc);
    CHECK_CONDITION(0 == info.counts_for(Length(2)).nfree);

    CHECK_CONDITION(25 == info.counts_for(Length(3)).nalloc);
    CHECK_CONDITION(25 == info.counts_for(Length(3)).nfree);

    for (auto i = Length(4); i <= kMaxPages; ++i) {
      CHECK_CONDITION(0 == info.counts_for(i).nalloc);
      CHECK_CONDITION(0 == info.counts_for(i).nfree);
    }

    const Length absurd =
        Length(uintptr_t{1} << (kAddressBits - 1 - kPageShift));
    for (Length i = kMaxPages + Length(1); i < absurd; i *= 2) {
      CHECK_CONDITION(0 == info.counts_for(i).nalloc);
      CHECK_CONDITION(0 == info.counts_for(i).nfree);
    }
  }
  for (auto s : spans) Delete(s, kObjectsPerSpan);
}

// And that we call the print method properly.
TEST_F(PageAllocatorTest, PrintIt) {
  constexpr size_t kObjectsPerSpan = 17;
  Delete(New(Length(1), kObjectsPerSpan), kObjectsPerSpan);
  std::string output = Print();
  EXPECT_THAT(output, testing::ContainsRegex("stats on allocation sizes"));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

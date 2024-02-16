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

#include "tcmalloc/pagemap.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"

// Note: we leak memory every time a map is constructed, so do not
// create too many maps.

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Pick span pointer to use for page numbered i
Span* span(intptr_t i) { return reinterpret_cast<Span*>(i + 1); }

// Pick sizeclass to use for page numbered i
uint8_t sc(intptr_t i) { return i % 16; }

class PageMapTest : public ::testing::TestWithParam<int> {
 public:
  PageMapTest() {
    // Arrange to pass zero-filled memory as the backing store for map.
    memset(storage, 0, sizeof(Map));
    map = new (storage) Map();
  }

  ~PageMapTest() override {
    for (void* ptr : *ptrs()) {
      ::operator delete(ptr);
    }
    ptrs()->clear();
  }

 private:
  static std::vector<void*>* ptrs() {
    static std::vector<void*>* ret = new std::vector<void*>();
    return ret;
  }

  static void* alloc(size_t n) {
    void* ptr = ::operator new(n);
    ptrs()->push_back(ptr);
    return ptr;
  }

 public:
  using Map = PageMap2<20, alloc>;
  Map* map;

 private:
  alignas(Map) char storage[sizeof(Map)];
};

TEST_P(PageMapTest, Sequential) {
  const intptr_t limit = GetParam();

  for (intptr_t i = 0; i < limit; i++) {
    map->Ensure(i, 1);
    map->set(i, span(i));
    ASSERT_EQ(map->get(i), span(i));

    // Test size class handling
    ASSERT_EQ(0, map->sizeclass(i));
    map->set_with_sizeclass(i, span(i), sc(i));
    ASSERT_EQ(sc(i), map->sizeclass(i));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map->get(i), span(i));
  }
}

TEST_P(PageMapTest, Bulk) {
  const intptr_t limit = GetParam();

  map->Ensure(0, limit);
  for (intptr_t i = 0; i < limit; i++) {
    map->set(i, span(i));
    ASSERT_EQ(map->get(i), span(i));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map->get(i), span(i));
  }
}

TEST_P(PageMapTest, Overflow) {
  const intptr_t kLimit = 1 << 20;
  ASSERT_FALSE(map->Ensure(kLimit, kLimit + 1));
}

TEST_P(PageMapTest, RandomAccess) {
  const intptr_t limit = GetParam();

  std::vector<intptr_t> elements;
  for (intptr_t i = 0; i < limit; i++) {
    elements.push_back(i);
  }
  std::shuffle(elements.begin(), elements.end(), absl::BitGen());

  for (intptr_t i = 0; i < limit; i++) {
    map->Ensure(elements[i], 1);
    map->set(elements[i], span(elements[i]));
    ASSERT_EQ(map->get(elements[i]), span(elements[i]));
  }
  for (intptr_t i = 0; i < limit; i++) {
    ASSERT_EQ(map->get(i), span(i));
  }
}

INSTANTIATE_TEST_SUITE_P(Limits, PageMapTest, ::testing::Values(100, 1 << 20));

// Surround pagemap with unused memory. This isolates it so that it does not
// share pages with any other structures. This avoids the risk that adjacent
// objects might cause it to be mapped in. The padding is of sufficient size
// that this is true even if this structure is mapped with huge pages.
static struct PaddedPageMap {
  constexpr PaddedPageMap() : padding_before{}, pagemap{}, padding_after{} {}
  uint64_t padding_before[kHugePageSize / sizeof(uint64_t)];
  PageMap pagemap;
  uint64_t padding_after[kHugePageSize / sizeof(uint64_t)];
} padded_pagemap_;

TEST(TestMemoryFootprint, Test) {
  uint64_t pagesize = sysconf(_SC_PAGESIZE);
  ASSERT_NE(pagesize, 0);
  size_t pages = sizeof(PageMap) / pagesize + 1;
  std::vector<unsigned char> present(pages);

  // mincore needs the address rounded to the start page
  uint64_t basepage =
      reinterpret_cast<uintptr_t>(&padded_pagemap_.pagemap) & ~(pagesize - 1);
  ASSERT_EQ(mincore(reinterpret_cast<void*>(basepage), sizeof(PageMap),
                    present.data()),
            0);
  for (int i = 0; i < pages; i++) {
    EXPECT_EQ(present[i], 0);
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

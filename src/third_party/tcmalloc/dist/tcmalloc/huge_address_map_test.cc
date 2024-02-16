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

#include "tcmalloc/huge_address_map.h"

#include <stddef.h>
#include <stdlib.h>

#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class HugeAddressMapTest : public ::testing::Test {
 protected:
  HugeAddressMapTest() : map_(MallocMetadata) { metadata_allocs_.clear(); }

  ~HugeAddressMapTest() override {
    for (void* p : metadata_allocs_) {
      free(p);
    }
  }

  std::vector<HugeRange> Contents() {
    std::vector<HugeRange> ret;
    auto node = map_.first();
    while (node) {
      ret.push_back(node->range());
      node = node->next();
    }

    return ret;
  }

  HugePage hp(size_t i) { return {i}; }
  HugeLength hl(size_t i) { return NHugePages(i); }

  HugeAddressMap map_;

 private:
  static void* MallocMetadata(size_t size) {
    void* ptr = malloc(size);
    metadata_allocs_.push_back(ptr);
    return ptr;
  }

  static std::vector<void*> metadata_allocs_;
};

std::vector<void*> HugeAddressMapTest::metadata_allocs_;

// This test verifies that HugeAddressMap merges properly.
TEST_F(HugeAddressMapTest, Merging) {
  const HugeRange r1 = HugeRange::Make(hp(0), hl(1));
  const HugeRange r2 = HugeRange::Make(hp(1), hl(1));
  const HugeRange r3 = HugeRange::Make(hp(2), hl(1));
  const HugeRange all = Join(r1, Join(r2, r3));
  map_.Insert(r1);
  map_.Check();
  EXPECT_THAT(Contents(), testing::ElementsAre(r1));
  map_.Insert(r3);
  map_.Check();
  EXPECT_THAT(Contents(), testing::ElementsAre(r1, r3));
  map_.Insert(r2);
  map_.Check();
  EXPECT_THAT(Contents(), testing::ElementsAre(all));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

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

#include "tcmalloc/internal/mincore.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

#include <cstdint>
#include <set>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tcmalloc/internal/page_size.h"

namespace tcmalloc {
namespace tcmalloc_internal {

using ::testing::Eq;

// Mock interface to mincore() which has reports residence based on
// an array provided at construction.
class MInCoreMock : public MInCoreInterface {
 public:
  MInCoreMock() : mapped_() {}
  ~MInCoreMock() override {}

  // Implementation of minCore that reports presence based on provided array.
  int mincore(void* addr, size_t length, unsigned char* result) override {
    const size_t kPageSize = GetPageSize();
    uintptr_t uAddress = reinterpret_cast<uintptr_t>(addr);
    // Check that we only pass page aligned addresses into mincore().
    EXPECT_THAT(uAddress & (kPageSize - 1), Eq(0));

    uintptr_t uEndAddress = uAddress + length;
    int index = 0;
    // Check for presence of the target pages in the map.
    while (uAddress < uEndAddress) {
      result[index] = (mapped_.find(uAddress) != mapped_.end() ? 1 : 0);
      uAddress += kPageSize;
      index++;
    }
    return 0;
  }

  void addPage(uintptr_t uAddress) { mapped_.insert(uAddress); }

 private:
  std::set<uintptr_t> mapped_;
};

// Friend class of MInCore which calls the mincore mock.
class MInCoreTest {
 public:
  MInCoreTest() : mcm_() {}
  ~MInCoreTest() {}

  size_t residence(uintptr_t addr, size_t size) {
    return MInCore::residence_impl(reinterpret_cast<void*>(addr), size, &mcm_);
  }

  void addPage(uintptr_t page) { mcm_.addPage(page); }

  // Expose the internal size of array that we use to call mincore() so
  // that we can be sure to need multiple calls to cover large memory regions.
  const size_t chunkSize() { return MInCore::kArrayLength; }

 private:
  MInCoreMock mcm_;
};

namespace {

using ::testing::Eq;

TEST(MInCoreTest, TestResidence) {
  MInCoreTest mct;
  const size_t kPageSize = GetPageSize();

  // Set up a pattern with a few resident pages.
  // page 0 not mapped
  mct.addPage(kPageSize);
  // page 2 not mapped
  mct.addPage(3 * kPageSize);
  mct.addPage(4 * kPageSize);

  // An object of size zero should have a residence of zero.
  EXPECT_THAT(mct.residence(320, 0), Eq(0));

  // Check that an object entirely on the first page is
  // reported as entirely unmapped.
  EXPECT_THAT(mct.residence(320, 55), Eq(0));

  // Check that an object entirely on the second page is
  // reported as entirely mapped.
  EXPECT_THAT(mct.residence(kPageSize + 320, 55), Eq(55));

  // An object of size zero should have a residence of zero.
  EXPECT_THAT(mct.residence(kPageSize + 320, 0), Eq(0));

  // Check that an object over a mapped and unmapped page is half mapped.
  EXPECT_THAT(mct.residence(kPageSize / 2, kPageSize), Eq(kPageSize / 2));

  // Check that an object which spans two pages is reported as being mapped
  // only on the page that's resident.
  EXPECT_THAT(mct.residence(kPageSize / 2 * 3, kPageSize), Eq(kPageSize / 2));

  // Check that an object that is on two mapped pages is reported as entirely
  // resident.
  EXPECT_THAT(mct.residence(kPageSize / 2 * 7, kPageSize), Eq(kPageSize));

  // Check that an object that is on one mapped page is reported as only
  // resident on the mapped page.
  EXPECT_THAT(mct.residence(kPageSize * 2, kPageSize + 1), Eq(1));

  // Check that an object that is on one mapped page is reported as only
  // resident on the mapped page.
  EXPECT_THAT(mct.residence(kPageSize + 1, kPageSize + 1), Eq(kPageSize - 1));

  // Check that an object which spans beyond the mapped pages is reported
  // as unmapped
  EXPECT_THAT(mct.residence(kPageSize * 6, kPageSize), Eq(0));

  // Check an object that spans three pages, two of them mapped.
  EXPECT_THAT(mct.residence(kPageSize / 2 * 7 + 1, kPageSize * 2),
              Eq(kPageSize * 3 / 2 - 1));
}

// Test whether we are correctly handling multiple calls to mincore.
TEST(MInCoreTest, TestLargeResidence) {
  MInCoreTest mct;
  uintptr_t uAddress = 0;
  const size_t kPageSize = GetPageSize();
  // Set up a pattern covering 6 * page size *  MInCore::kArrayLength to
  // allow us to test for situations where the region we're checking
  // requires multiple calls to mincore().
  // Use a mapped/unmapped/unmapped pattern, this will mean that
  // the regions examined by mincore() do not have regular alignment
  // with the pattern.
  for (int i = 0; i < 2 * mct.chunkSize(); i++) {
    mct.addPage(uAddress);
    uAddress += 3 * kPageSize;
  }

  uintptr_t baseAddress = 0;
  for (int size = kPageSize; size < 32 * 1024 * 1024; size += 2 * kPageSize) {
    uintptr_t unit = kPageSize * 3;
    EXPECT_THAT(mct.residence(baseAddress, size),
                Eq(kPageSize * ((size + unit - 1) / unit)));
  }
}

TEST(MInCoreTest, UnmappedMemory) {
  const size_t kPageSize = GetPageSize();
  const int kNumPages = 16;

  // Overallocate kNumPages of memory, so we can munmap the page before and
  // after it.
  void* p = mmap(nullptr, (kNumPages + 2) * kPageSize, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  ASSERT_NE(p, MAP_FAILED) << errno;
  ASSERT_EQ(munmap(p, kPageSize), 0);
  void* q = reinterpret_cast<char*>(p) + kPageSize;
  void* last = reinterpret_cast<char*>(p) + (kNumPages + 1) * kPageSize;
  ASSERT_EQ(munmap(last, kPageSize), 0);

  memset(q, 0, kNumPages * kPageSize);
  ::benchmark::DoNotOptimize(q);

  EXPECT_EQ(0, MInCore::residence(nullptr, kPageSize));
  EXPECT_EQ(0, MInCore::residence(p, kPageSize));
  for (int i = 0; i <= kNumPages; i++) {
    EXPECT_EQ(i * kPageSize, MInCore::residence(q, i * kPageSize));
  }

  // Note we can only query regions that are entirely mapped, but we should also
  // test the edge case of incomplete pages.
  EXPECT_EQ((kNumPages - 1) * kPageSize,
            MInCore::residence(reinterpret_cast<char*>(q) + 7,
                               (kNumPages - 1) * kPageSize));

  ASSERT_EQ(munmap(q, kNumPages * kPageSize), 0);
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

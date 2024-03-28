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
// This is a unit test for large allocations in malloc and friends.
// "Large" means "so large that they overflow the address space".

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

#include <limits>
#include <new>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Alloc a size that should always fail.
void TryAllocExpectFail(size_t size) {
  void* p1 = malloc(size);
  ASSERT_EQ(p1, nullptr);

  void* p2 = malloc(1);
  ASSERT_NE(p2, nullptr);

  void* p3 = realloc(p2, size);
  ASSERT_EQ(p3, nullptr);

  free(p2);
}

// Alloc a size that might work and might fail.
// If it does work, touch some pages.

void TryAllocMightFail(size_t size) {
  unsigned char* p = static_cast<unsigned char*>(malloc(size));
  if (p != nullptr) {
    unsigned char volatile* vp = p;  // prevent optimizations
    static const size_t kPoints = 1024;

    for (size_t i = 0; i < kPoints; ++i) {
      vp[i * (size / kPoints)] = static_cast<unsigned char>(i);
    }

    for (size_t i = 0; i < kPoints; ++i) {
      ASSERT_EQ(vp[i * (size / kPoints)], static_cast<unsigned char>(i));
    }

    vp[size - 1] = 'M';
    ASSERT_EQ(vp[size - 1], 'M');
  } else {
    ASSERT_EQ(errno, ENOMEM);
  }

  free(p);
}

class NoErrnoRegion final : public AddressRegion {
 public:
  explicit NoErrnoRegion(AddressRegion* underlying) : underlying_(underlying) {}

  std::pair<void*, size_t> Alloc(size_t size, size_t alignment) override {
    std::pair<void*, size_t> result = underlying_->Alloc(size, alignment);
    errno = 0;
    return result;
  }

 private:
  AddressRegion* underlying_;
};

class NoErrnoRegionFactory final : public AddressRegionFactory {
 public:
  explicit NoErrnoRegionFactory(AddressRegionFactory* underlying)
      : underlying_(underlying) {}
  ~NoErrnoRegionFactory() override {}

  AddressRegion* Create(void* start, size_t size, UsageHint hint) override {
    AddressRegion* underlying_region = underlying_->Create(start, size, hint);
    TC_CHECK_NE(underlying_region, nullptr);
    void* region_space = MallocInternal(sizeof(NoErrnoRegion));
    TC_CHECK_NE(region_space, nullptr);
    return new (region_space) NoErrnoRegion(underlying_region);
  }

  // Get a human-readable description of the current state of the
  // allocator.
  size_t GetStats(absl::Span<char> buffer) override {
    return underlying_->GetStats(buffer);
  }

 private:
  AddressRegionFactory* const underlying_;
};

class LargeAllocationTest : public ::testing::Test {
 public:

  LargeAllocationTest() {
    old_ = MallocExtension::GetRegionFactory();
    MallocExtension::SetRegionFactory(new NoErrnoRegionFactory(old_));

    // Grab some memory so that some later allocations are guaranteed to fail.
    small_ = ::operator new(4 << 20);
  }

  ~LargeAllocationTest() override {
    ::operator delete(small_);

    auto* current = MallocExtension::GetRegionFactory();

    MallocExtension::SetRegionFactory(old_);
    delete current;
  }

 private:
  AddressRegionFactory* old_;
  void* small_;
};

// Allocate some 0-byte objects.  They better be unique.  0 bytes is not large
// but it exercises some paths related to large-allocation code.
TEST_F(LargeAllocationTest, UniqueAddresses) {
  constexpr int kZeroTimes = 1024;

  absl::flat_hash_set<void*> ptrs;
  for (int i = 0; i < kZeroTimes; ++i) {
    void* p = malloc(1);
    ASSERT_NE(p, nullptr);
    EXPECT_THAT(ptrs, ::testing::Not(::testing::Contains(p)));
    ptrs.insert(p);
  }

  for (auto* p : ptrs) {
    free(p);
  }
}

TEST_F(LargeAllocationTest, MaxSize) {
  // Test sizes up near the maximum size_t.  These allocations test the
  // wrap-around code.
  constexpr size_t zero = 0;
  constexpr size_t kMinusNTimes = 16384;
  for (size_t i = 1; i < kMinusNTimes; ++i) {
    TryAllocExpectFail(zero - i);
  }
}

TEST_F(LargeAllocationTest, NearMaxSize) {
  // Test sizes a bit smaller.  The small malloc above guarantees that all these
  // return nullptr.
  constexpr size_t zero = 0;
  constexpr size_t kMinusMBMinusNTimes = 16384;
  for (size_t i = 0; i < kMinusMBMinusNTimes; ++i) {
    TryAllocExpectFail(zero - 1048576 - i);
  }
}

TEST_F(LargeAllocationTest, Half) {
  // Test sizes at half of size_t.
  // These might or might not fail to allocate.
  constexpr size_t kHalfPlusMinusTimes = 64;
  constexpr size_t half = std::numeric_limits<size_t>::max() / 2 + 1;
  for (size_t i = 0; i < kHalfPlusMinusTimes; ++i) {
    TryAllocMightFail(half - i);
    TryAllocMightFail(half + i);
  }
}

TEST_F(LargeAllocationTest, NearMaxAddressBits) {
  // Tests sizes near the maximum address space size.
  // For -1 <= i < 5, we expect all allocations to fail.  For -6 <= i < -1, the
  // allocation might succeed but create so much pagemap metadata that we exceed
  // test memory limits and OOM.  So we skip that range.
  for (int i = -10; i < -6; ++i) {
    TryAllocMightFail(size_t{1} << (kAddressBits + i));
  }
  for (int i = -1; i < 5; ++i) {
    TryAllocExpectFail(size_t{1} << (kAddressBits + i));
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

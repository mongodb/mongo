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

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/debugging/leak_check.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

struct Aligned4 {
  int32_t a;
};

static_assert(alignof(Aligned4) == 4, "Unexpected alignment");

struct Aligned8 {
  double a;
  int32_t b;
};

static_assert(alignof(Aligned8) == 8, "Unexpected alignment");

struct Aligned16 {
  long double a;
  void* b;
};

static_assert(alignof(Aligned16) == 16, "Unexpected alignment");

struct alignas(32) Aligned32 {
  int32_t a[4];
};

static_assert(alignof(Aligned32) == 32, "Unexpected alignment");

struct alignas(64) Aligned64 {
  int32_t a[8];
} ABSL_ATTRIBUTE_PACKED;

static_assert(alignof(Aligned64) == 64, "Unexpected alignment");

template <typename T>
class AlignedNew : public ::testing::Test {
 protected:
  std::vector<std::unique_ptr<T>> ptrs;
};

TYPED_TEST_SUITE_P(AlignedNew);

TYPED_TEST_P(AlignedNew, AlignedTest) {
  const int kAllocations = 1 << 22;
  this->ptrs.reserve(kAllocations);

  auto token = MallocExtension::StartAllocationProfiling();

  for (int i = 0; i < kAllocations; i++) {
    TypeParam* p = new TypeParam();
    benchmark::DoNotOptimize(p);
    ASSERT_EQ(0, reinterpret_cast<uintptr_t>(p) & (alignof(TypeParam) - 1));

    this->ptrs.emplace_back(p);
  }

  auto profile = std::move(token).Stop();

  // Verify the alignment was explicitly requested if alignof(TypeParam) >
  // __STDCPP_DEFAULT_NEW_ALIGNMENT__.
  //
  // (size, alignment) -> count
  using CountMap = absl::flat_hash_map<std::pair<size_t, size_t>, size_t>;
  CountMap counts;

  profile.Iterate([&](const Profile::Sample& e) {
    counts[{e.requested_size, e.requested_alignment}] += e.count;
  });

  size_t expected_alignment = 0;
#if defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
  if (alignof(TypeParam) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    expected_alignment = alignof(TypeParam);
  }
#endif
  EXPECT_GT((counts[{sizeof(TypeParam), expected_alignment}]), 0);

  ASSERT_EQ(kAllocations, this->ptrs.size());
}

TYPED_TEST_P(AlignedNew, SizeCheckSampling) {
#if defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
  if (alignof(TypeParam) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    // Allocations will not hit the sized+aligned delete path.
    return;
  }
#endif

  // The HeapLeakChecker initializes malloc hooks which we call prior to looking
  // up the size class (and asserting the correct size was passed to TCMalloc).
  ASSERT_FALSE(absl::LeakCheckerIsActive());

  // Allocate enough objects to ensure we sample one.
  const int allocations =
      32 * MallocExtension::GetProfileSamplingRate() / sizeof(TypeParam);

  for (int i = 0; i < allocations; i++) {
    this->ptrs.emplace_back(new TypeParam());
  }

  ASSERT_EQ(allocations, this->ptrs.size());

  // Trigger destruction.
  this->ptrs.clear();
}

TYPED_TEST_P(AlignedNew, ArraySizeCheckSampling) {
#if defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
  if (alignof(TypeParam) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    // Allocations will not hit the sized+aligned delete path.
    return;
  }
#endif

  // NonTrival is not trivially destructible, so the compiler needs to keep
  // track of the true size of arrays so that the proper number of destructors
  // can be invoked.
  struct NonTrivial {
    virtual ~NonTrivial() {}

    TypeParam p;
  };

  static_assert(!std::is_trivially_destructible<NonTrivial>::value,
                "NonTrivial should have a nontrivial destructor.");

  // The HeapLeakChecker initializes malloc hooks which we call prior to looking
  // up the size class (and asserting the correct size was passed to TCMalloc).
  ASSERT_FALSE(absl::LeakCheckerIsActive());

  // Allocate enough objects to ensure we sample one.
  const int allocations =
      32 * MallocExtension::GetProfileSamplingRate() / sizeof(TypeParam);

  std::vector<std::unique_ptr<NonTrivial[]>> objects;
  for (int i = 0; i < allocations; i++) {
    objects.emplace_back(new NonTrivial[10]);
  }

  ASSERT_EQ(allocations, objects.size());

  // Trigger destruction.
  objects.clear();
}

REGISTER_TYPED_TEST_SUITE_P(AlignedNew, AlignedTest, SizeCheckSampling,
                            ArraySizeCheckSampling);

typedef ::testing::Types<Aligned4, Aligned8, Aligned16, Aligned32, Aligned64>
    MyTypes;
INSTANTIATE_TYPED_TEST_SUITE_P(My, AlignedNew, MyTypes);

}  // namespace
}  // namespace tcmalloc

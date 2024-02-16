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

#include <string>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/random/random.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/size_class_info.h"
#include "tcmalloc/span.h"
#include "tcmalloc/tcmalloc_policy.h"

namespace tcmalloc {
namespace tcmalloc_internal {

// Moved out of anonymous namespace so that it can be found by friend class in
// span.h. This allows tests to access span internals so that we can
// validate that scaling by a reciprocal correctly converts a pointer into
// an offset within a span.
class SpanTestPeer {
 public:
  static uint16_t CalcReciprocal(size_t size) {
    return Span::CalcReciprocal(size);
  }
  static Span::ObjIdx TestOffsetToIdx(uintptr_t offset, size_t size,
                                      uint16_t reciprocal) {
    return Span::TestOffsetToIdx(offset, size, reciprocal);
  }
};

namespace {

size_t Alignment(size_t size) {
  size_t ret = static_cast<size_t>(kAlignment);
  if (size >= 1024) {
    // SizeMap::ClassIndexMaybe requires 128-byte alignment for sizes >=1024.
    ret = 128;
  } else if (size >= 512) {
    // Per //tcmalloc/span.h, we have 64 byte alignment for sizes
    // >=512.
    ret = 64;
  } else if (size >= 8) {
    ret = 8;
  }

  return ret;
}

class SizeClassesTest
    : public ::testing::TestWithParam<absl::Span<const SizeClassInfo>> {
 protected:
  SizeClassesTest() { m_.Init(GetParam()); }

  SizeMap m_;
};

TEST_P(SizeClassesTest, SmallClassesSinglePage) {
  // Per //tcmalloc/span.h, the compressed index implementation
  // added by cl/126729493 requires small size classes to be placed on a single
  // page span so they can be addressed.
  for (int c = 1; c < kNumClasses; c++) {
    const size_t max_size_in_class = m_.class_to_size(c);
    if (max_size_in_class >= SizeMap::kMultiPageSize) {
      continue;
    }
    if (max_size_in_class == 0) {
      continue;
    }
    EXPECT_EQ(m_.class_to_pages(c), 1) << max_size_in_class;
  }
}

TEST_P(SizeClassesTest, SpanPages) {
  for (int c = 1; c < kNumClasses; c++) {
    const size_t max_size_in_class = m_.class_to_size(c);
    if (max_size_in_class == 0) {
      continue;
    }
    // A span of class_to_pages(c) must be able to hold at least one object.
    EXPECT_GE(Length(m_.class_to_pages(c)).in_bytes(), max_size_in_class);
  }
}

TEST_P(SizeClassesTest, ValidateSufficientBitmapCapacity) {
  // Validate that all the objects in a span can fit into a bitmap.
  // The cut-off for using a bitmap is kBitmapMinObjectSize, so it is
  // theoretically possible that a span could exceed this threshold
  // for object size and contain more than 64 objects.
  for (int c = 1; c < kNumClasses; ++c) {
    const size_t max_size_in_class = m_.class_to_size(c);
    if (max_size_in_class >= kBitmapMinObjectSize) {
      const size_t objects_per_span =
          Length(m_.class_to_pages(c)).in_bytes() / m_.class_to_size(c);
      // Span can hold at most 64 objects of this size.
      EXPECT_LE(objects_per_span, 64);
    }
  }
}

TEST_P(SizeClassesTest, ValidateCorrectScalingByReciprocal) {
  // Validate that multiplying by the reciprocal works for all size classes.
  // When converting an offset within a span into an index we avoid a
  // division operation by scaling by the reciprocal. The test ensures
  // that this approach works for all objects in a span, for all object
  // sizes.
  for (int c = 1; c < kNumClasses; ++c) {
    const size_t max_size_in_class = m_.class_to_size(c);
    // Only test for sizes where object availability is recorded in a bitmap.
    if (max_size_in_class < kBitmapMinObjectSize) {
      continue;
    }
    size_t reciprocal = SpanTestPeer::CalcReciprocal(max_size_in_class);
    const size_t objects_per_span =
        Length(m_.class_to_pages(c)).in_bytes() / m_.class_to_size(c);
    for (int index = 0; index < objects_per_span; index++) {
      // Calculate the address of the object.
      uintptr_t address = index * max_size_in_class;
      // Calculate the index into the page using the reciprocal method.
      int idx =
          SpanTestPeer::TestOffsetToIdx(address, max_size_in_class, reciprocal);
      // Check that the starting address back is correct.
      ASSERT_EQ(address, idx * max_size_in_class);
    }
  }
}

TEST_P(SizeClassesTest, Aligned) {
  // Validate that each size class is properly aligned.
  for (int c = 1; c < kNumClasses; c++) {
    const size_t max_size_in_class = m_.class_to_size(c);
    size_t alignment = Alignment(max_size_in_class);

    EXPECT_EQ(0, max_size_in_class % alignment) << max_size_in_class;
  }
}

TEST_P(SizeClassesTest, Distinguishable) {
  // Validate that the size to class lookup table is able to distinguish each
  // size class from one another.
  //
  // ClassIndexMaybe provides 8 byte granularity below 1024 bytes and 128 byte
  // granularity for larger sizes, so our chosen size classes cannot be any
  // finer (otherwise they would map to the same entry in the lookup table).
  //
  // We don't check expanded size classes which are intentionally duplicated.
  for (int partition = 0; partition < kNumaPartitions; partition++) {
    for (int c = (partition * kNumBaseClasses) + 1;
         c < (partition + 1) * kNumBaseClasses; c++) {
      const size_t max_size_in_class = m_.class_to_size(c);
      if (max_size_in_class == 0) {
        continue;
      }
      const int class_index = m_.SizeClass(
          CppPolicy().InNumaPartition(partition), max_size_in_class);

      EXPECT_EQ(c, class_index) << max_size_in_class;
    }
  }
}

// This test is disabled until we use a different span size allocation
// algorithm (such as the one in effect from cl/130150125 until cl/139955211).
TEST_P(SizeClassesTest, DISABLED_WastedSpan) {
  // Validate that each size class does not waste (number of objects) *
  // (alignment) at the end of the span.
  for (int c = 1; c < kNumClasses; c++) {
    const size_t span_size = kPageSize * m_.class_to_pages(c);
    const size_t max_size_in_class = m_.class_to_size(c);
    const size_t alignment = Alignment(max_size_in_class);
    const size_t n_objects = span_size / max_size_in_class;
    const size_t waste = span_size - n_objects * max_size_in_class;

    EXPECT_LT(waste, n_objects * alignment) << max_size_in_class;
  }
}

TEST_P(SizeClassesTest, DoubleCheckedConsistency) {
  // Validate that every size on [0, kMaxSize] maps to a size class that is
  // neither too big nor too small.
  for (size_t size = 0; size <= kMaxSize; size++) {
    const int sc = m_.SizeClass(CppPolicy(), size);
    EXPECT_GT(sc, 0) << size;
    EXPECT_LT(sc, kNumClasses) << size;

    if ((sc % kNumBaseClasses) > 1) {
      EXPECT_GT(size, m_.class_to_size(sc - 1))
          << "Allocating unnecessarily large class";
    }

    const size_t s = m_.class_to_size(sc);
    EXPECT_LE(size, s);
    EXPECT_NE(s, 0) << size;
  }
}

TEST_P(SizeClassesTest, NumToMove) {
  for (int c = 1; c < kNumClasses; c++) {
    // For non-empty size classes, we should move at least 1 object to/from each
    // layer of the caches.
    const size_t max_size_in_class = m_.class_to_size(c);
    if (max_size_in_class == 0) {
      continue;
    }
    EXPECT_GT(m_.num_objects_to_move(c), 0) << max_size_in_class;
  }
}

TEST_P(SizeClassesTest, MaxSize) {
  // kMaxSize should appear as one of the size classes.  As of 10/2021, we crash
  // during SizeClass::Init anyways, but this test exists to further document
  // that requirement.
  bool found = false;

  for (int c = 1; c < kNumClasses; c++) {
    // For non-empty size classes, we should move at least 1 object to/from each
    // layer of the caches.
    const size_t max_size_in_class = m_.class_to_size(c);
    if (max_size_in_class == kMaxSize) {
      found = true;
      break;
    }
  }

  EXPECT_TRUE(found) << "Could not find " << kMaxSize;
}

class TestingSizeMap : public SizeMap {
 public:
  // Re-export as public.
  using SizeMap::ValidSizeClasses;
};

TEST_P(SizeClassesTest, Validate) {
  // The default size classes also need to be valid.
  TestingSizeMap m;
  EXPECT_TRUE(m.ValidSizeClasses(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(AllSizeClasses, SizeClassesTest,
                         testing::Values(kSizeClasses,
                                         kExperimentalPow2SizeClasses,
                                         kLegacySizeClasses));

class RunTimeSizeClassesTest : public ::testing::Test {
 protected:
  RunTimeSizeClassesTest() {}

  TestingSizeMap m_;
};

TEST_F(RunTimeSizeClassesTest, ExpandedSizeClasses) {
  // Verify that none of the default size classes are considered expanded size
  // classes.
  for (int i = 0; i < kNumClasses; i++) {
    EXPECT_EQ(i < (kNumBaseClasses * kNumaPartitions), !IsExpandedSizeClass(i))
        << i;
  }
}

TEST_F(RunTimeSizeClassesTest, ValidateClassSizeIncreases) {
  SizeClassInfo parsed[] = {
      {0, 0, 0},
      {16, 1, 14},
      {32, 1, 15},
      {kMaxSize, 64, 15},
  };
  EXPECT_TRUE(m_.ValidSizeClasses(parsed));

  parsed[2].size = 8;  // Change 32 to 8
  EXPECT_FALSE(m_.ValidSizeClasses(parsed));
}

TEST_F(RunTimeSizeClassesTest, ValidateClassSizeMax) {
  SizeClassInfo parsed[] = {
      {0, 0, 0},
      {kMaxSize - 128, 64, 15},
  };
  // Last class must cover kMaxSize
  EXPECT_FALSE(m_.ValidSizeClasses(parsed));

  // Check Max Size is allowed 256 KiB = 262144
  parsed[1].size = kMaxSize;
  EXPECT_TRUE(m_.ValidSizeClasses(parsed));
  // But kMaxSize + 128 is not allowed
  parsed[1].size = kMaxSize + 128;
  EXPECT_FALSE(m_.ValidSizeClasses(parsed));
}

TEST_F(RunTimeSizeClassesTest, ValidateClassSizesAlignment) {
  SizeClassInfo parsed[] = {
      {0, 0, 0},
      {8, 1, 14},
      {kMaxSize, 64, 15},
  };
  EXPECT_TRUE(m_.ValidSizeClasses(parsed));
  // Doesn't meet alignment requirements
  parsed[1].size = 7;
  EXPECT_FALSE(m_.ValidSizeClasses(parsed));

  // Over 512, expect alignment of 64 bytes.
  // 512 + 64 = 576
  parsed[1].size = 576;
  EXPECT_TRUE(m_.ValidSizeClasses(parsed));
  // 512 + 8
  parsed[1].size = 520;
  EXPECT_FALSE(m_.ValidSizeClasses(parsed));

  // Over 1024, expect alignment of 128 bytes.
  // 1024 + 128 = 1152
  parsed[1].size = 1024 + 128;
  EXPECT_TRUE(m_.ValidSizeClasses(parsed));
  // 1024 + 64 = 1088
  parsed[1].size = 1024 + 64;
  EXPECT_FALSE(m_.ValidSizeClasses(parsed));
}

TEST_F(RunTimeSizeClassesTest, ValidateBatchSize) {
  SizeClassInfo parsed[] = {
      {0, 0, 0},
      {8, 1, kMaxObjectsToMove},
      {kMaxSize, 64, 15},
  };
  EXPECT_TRUE(m_.ValidSizeClasses(parsed));

  ++parsed[1].num_to_move;
  EXPECT_FALSE(m_.ValidSizeClasses(parsed));
}

TEST_F(RunTimeSizeClassesTest, ValidatePageSize) {
  SizeClassInfo parsed[] = {
      {0, 0, 0},
      {1024, 2, kMaxObjectsToMove},
      {kMaxSize, 64, 15},
  };
  EXPECT_TRUE(m_.ValidSizeClasses(parsed));

  parsed[1].pages = 256;
  EXPECT_FALSE(m_.ValidSizeClasses(parsed));
}

TEST(SizeMapTest, GetSizeClass) {
  absl::BitGen rng;
  constexpr int kTrials = 1000;

  SizeMap m;
  // Before m.Init(), SizeClass should always return 0 or the equivalent in a
  // non-zero NUMA partition.
  for (int i = 0; i < kTrials; ++i) {
    const size_t size = absl::LogUniform(rng, 0, 4 << 20);
    uint32_t size_class;
    if (m.GetSizeClass(CppPolicy(), size, &size_class)) {
      EXPECT_EQ(size_class % kNumBaseClasses, 0) << size;
      EXPECT_LT(size_class, kExpandedClassesStart) << size;
    } else {
      // We should only fail to lookup the size class when size is outside of
      // the size classes.
      ASSERT_GT(size, kMaxSize);
    }
  }

  // After m.Init(), GetSizeClass should return a size class.
  m.Init(kSizeClasses);

  for (int i = 0; i < kTrials; ++i) {
    const size_t size = absl::LogUniform(rng, 0, 4 << 20);
    uint32_t size_class;
    if (m.GetSizeClass(CppPolicy(), size, &size_class)) {
      const size_t mapped_size = m.class_to_size(size_class);
      // The size class needs to hold size.
      ASSERT_GE(mapped_size, size);
    } else {
      // We should only fail to lookup the size class when size is outside of
      // the size classes.
      ASSERT_GT(size, kMaxSize);
    }
  }
}

TEST(SizeMapTest, GetSizeClassWithAlignment) {
  absl::BitGen rng;
  constexpr int kTrials = 1000;

  SizeMap m;
  // Before m.Init(), SizeClass should always return 0 or the equivalent in a
  // non-zero NUMA partition.
  for (int i = 0; i < kTrials; ++i) {
    const size_t size = absl::LogUniform(rng, 0, 4 << 20);
    const size_t alignment = 1 << absl::Uniform(rng, 0u, kHugePageShift);
    uint32_t size_class;
    if (m.GetSizeClass(CppPolicy().AlignAs(alignment), size, &size_class)) {
      EXPECT_EQ(size_class % kNumBaseClasses, 0) << size << " " << alignment;
      EXPECT_LT(size_class, kExpandedClassesStart) << size << " " << alignment;
    } else if (alignment <= kPageSize) {
      // When alignment > kPageSize, we do not produce a size class.
      //
      // We should only fail to lookup the size class when size is large.
      ASSERT_GT(size, kMaxSize) << alignment;
    }
  }

  // After m.Init(), GetSizeClass should return a size class.
  m.Init(kSizeClasses);

  for (int i = 0; i < kTrials; ++i) {
    const size_t size = absl::LogUniform(rng, 0, 4 << 20);
    const size_t alignment = 1 << absl::Uniform(rng, 0u, kHugePageShift);
    uint32_t size_class;
    if (m.GetSizeClass(CppPolicy().AlignAs(alignment), size, &size_class)) {
      const size_t mapped_size = m.class_to_size(size_class);
      // The size class needs to hold size.
      ASSERT_GE(mapped_size, size);
      // The size needs to be a multiple of alignment.
      ASSERT_EQ(mapped_size % alignment, 0);
    } else if (alignment <= kPageSize) {
      // When alignment > kPageSize, we do not produce a size class.
      //
      // We should only fail to lookup the size class when size is large.
      ASSERT_GT(size, kMaxSize) << alignment;
    }
  }
}

TEST(SizeMapTest, SizeClass) {
  absl::BitGen rng;
  constexpr int kTrials = 1000;

  SizeMap m;
  // Before m.Init(), SizeClass should always return 0 or the equivalent in a
  // non-zero NUMA partition.
  for (int i = 0; i < kTrials; ++i) {
    const size_t size = absl::LogUniform<size_t>(rng, 0u, kMaxSize);
    const uint32_t size_class = m.SizeClass(CppPolicy(), size);
    EXPECT_EQ(size_class % kNumBaseClasses, 0) << size;
    EXPECT_LT(size_class, kExpandedClassesStart) << size;
  }

  // After m.Init(), SizeClass should return a size class.
  m.Init(kSizeClasses);

  for (int i = 0; i < kTrials; ++i) {
    const size_t size = absl::LogUniform<size_t>(rng, 0u, kMaxSize);
    uint32_t size_class = m.SizeClass(CppPolicy(), size);

    const size_t mapped_size = m.class_to_size(size_class);
    // The size class needs to hold size.
    ASSERT_GE(mapped_size, size);
  }
}

TEST(SizeMapTest, Preinit) {
  ABSL_CONST_INIT static SizeMap m;

  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    EXPECT_EQ(m.class_to_size(size_class), 0) << size_class;
    EXPECT_EQ(m.class_to_pages(size_class), 0) << size_class;
    EXPECT_EQ(m.num_objects_to_move(size_class), 0) << size_class;
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

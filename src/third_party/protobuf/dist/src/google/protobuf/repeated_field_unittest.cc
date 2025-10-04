// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// Author: kenton@google.com (Kenton Varda)
//  Based on original Protocol Buffers design by
//  Sanjay Ghemawat, Jeff Dean, and others.
//
// TODO:  Improve this unittest to bring it up to the standards of
//   other proto2 unittests.

#include "google/protobuf/repeated_field.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/log/absl_check.h"
#include "absl/numeric/bits.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "google/protobuf/arena_test_util.h"
#include "google/protobuf/internal_visibility_for_testing.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/parse_context.h"
#include "google/protobuf/repeated_ptr_field.h"
#include "google/protobuf/unittest.pb.h"


// Must be included last.
#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {
namespace {

using ::protobuf_unittest::TestAllTypes;
using ::protobuf_unittest::TestMessageWithManyRepeatedPtrFields;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Ge;
using ::testing::Le;

TEST(RepeatedField, ConstInit) {
  PROTOBUF_CONSTINIT static RepeatedField<int> field{};  // NOLINT
  EXPECT_TRUE(field.empty());
}

// Test operations on a small RepeatedField.
TEST(RepeatedField, Small) {
  RepeatedField<int> field;

  EXPECT_TRUE(field.empty());
  EXPECT_EQ(field.size(), 0);

  field.Add(5);

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 1);
  EXPECT_EQ(field.Get(0), 5);
  EXPECT_EQ(field.at(0), 5);

  field.Add(42);

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 2);
  EXPECT_EQ(field.Get(0), 5);
  EXPECT_EQ(field.at(0), 5);
  EXPECT_EQ(field.Get(1), 42);
  EXPECT_EQ(field.at(1), 42);

  field.Set(1, 23);

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 2);
  EXPECT_EQ(field.Get(0), 5);
  EXPECT_EQ(field.at(0), 5);
  EXPECT_EQ(field.Get(1), 23);
  EXPECT_EQ(field.at(1), 23);

  field.at(1) = 25;

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 2);
  EXPECT_EQ(field.Get(0), 5);
  EXPECT_EQ(field.at(0), 5);
  EXPECT_EQ(field.Get(1), 25);
  EXPECT_EQ(field.at(1), 25);

  field.RemoveLast();

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 1);
  EXPECT_EQ(field.Get(0), 5);
  EXPECT_EQ(field.at(0), 5);

  field.Clear();

  EXPECT_TRUE(field.empty());
  EXPECT_EQ(field.size(), 0);
  // Additional bytes are for 'struct Rep' header.
  int expected_usage =
      (sizeof(Arena*) > sizeof(int) ? sizeof(Arena*) / sizeof(int) : 3) *
          sizeof(int) +
      sizeof(Arena*);
  EXPECT_GE(field.SpaceUsedExcludingSelf(), expected_usage);
}


// Test operations on a RepeatedField which is large enough to allocate a
// separate array.
TEST(RepeatedField, Large) {
  RepeatedField<int> field;

  for (int i = 0; i < 16; i++) {
    field.Add(i * i);
  }

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 16);

  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(field.Get(i), i * i);
  }

  int expected_usage = 16 * sizeof(int);
  EXPECT_GE(field.SpaceUsedExcludingSelf(), expected_usage);
}

template <typename Rep>
void CheckAllocationSizes(bool is_ptr) {
  using T = typename Rep::value_type;
  // Use a large initial block to make the checks below easier to predict.
  std::string buf(1 << 20, 0);

  Arena arena(&buf[0], buf.size());
  auto* rep = Arena::CreateMessage<Rep>(&arena);
  size_t prev = arena.SpaceUsed();

  for (int i = 0; i < 100; ++i) {
    rep->Add(T{});
    if (sizeof(void*) == 8) {
      // For RepeatedPtrField we also allocate the T in the arena.
      // Subtract those from the count.
      size_t new_used = arena.SpaceUsed() - (is_ptr ? sizeof(T) * (i + 1) : 0);
      size_t last_alloc = new_used - prev;
      prev = new_used;

      // When we actually allocated something, check the size.
      if (last_alloc != 0) {
        // Must be `>= 16`, as expected by the Arena.
        ASSERT_GE(last_alloc, 16);
        // Must be of a power of two.
        size_t log2 = absl::bit_width(last_alloc) - 1;
        ASSERT_EQ((1 << log2), last_alloc);
      }

      // The byte size must be a multiple of 8.
      ASSERT_EQ(rep->Capacity() * sizeof(T) % 8, 0);
    }
  }
}

TEST(RepeatedField, ArenaAllocationSizesMatchExpectedValues) {
  // RepeatedField guarantees that in 64-bit mode we never allocate anything
  // smaller than 16 bytes from an arena.
  // This is important to avoid a branch in the reallocation path.
  // This is also important because allocating anything less would be wasting
  // memory.
  // If the allocation size is wrong, ReturnArrayMemory will ABSL_DCHECK.
  CheckAllocationSizes<RepeatedField<bool>>(false);
  CheckAllocationSizes<RepeatedField<uint32_t>>(false);
  CheckAllocationSizes<RepeatedField<uint64_t>>(false);
}

TEST(RepeatedField, NaturalGrowthOnArenasReuseBlocks) {
  Arena arena;
  std::vector<RepeatedField<int>*> values;

  static constexpr int kNumFields = 100;
  static constexpr int kNumElems = 1000;
  for (int i = 0; i < kNumFields; ++i) {
    values.push_back(Arena::CreateMessage<RepeatedField<int>>(&arena));
    auto& field = *values.back();
    for (int j = 0; j < kNumElems; ++j) {
      field.Add(j);
    }
  }

  size_t expected = values.size() * values[0]->Capacity() * sizeof(int);
  // Use a 2% slack for other overhead. If we were not reusing the blocks, the
  // actual value would be ~2x the expected.
  EXPECT_THAT(arena.SpaceUsed(), AllOf(Ge(expected), Le(1.02 * expected)));
}

// Test swapping between various types of RepeatedFields.
TEST(RepeatedField, SwapSmallSmall) {
  RepeatedField<int> field1;
  RepeatedField<int> field2;

  field1.Add(5);
  field1.Add(42);

  EXPECT_FALSE(field1.empty());
  EXPECT_EQ(field1.size(), 2);
  EXPECT_EQ(field1.Get(0), 5);
  EXPECT_EQ(field1.Get(1), 42);

  EXPECT_TRUE(field2.empty());
  EXPECT_EQ(field2.size(), 0);

  field1.Swap(&field2);

  EXPECT_TRUE(field1.empty());
  EXPECT_EQ(field1.size(), 0);

  EXPECT_FALSE(field2.empty());
  EXPECT_EQ(field2.size(), 2);
  EXPECT_EQ(field2.Get(0), 5);
  EXPECT_EQ(field2.Get(1), 42);
}

TEST(RepeatedField, SwapLargeSmall) {
  RepeatedField<int> field1;
  RepeatedField<int> field2;

  for (int i = 0; i < 16; i++) {
    field1.Add(i * i);
  }
  field2.Add(5);
  field2.Add(42);
  field1.Swap(&field2);

  EXPECT_EQ(field1.size(), 2);
  EXPECT_EQ(field1.Get(0), 5);
  EXPECT_EQ(field1.Get(1), 42);
  EXPECT_EQ(field2.size(), 16);
  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(field2.Get(i), i * i);
  }
}

TEST(RepeatedField, SwapLargeLarge) {
  RepeatedField<int> field1;
  RepeatedField<int> field2;

  field1.Add(5);
  field1.Add(42);
  for (int i = 0; i < 16; i++) {
    field1.Add(i);
    field2.Add(i * i);
  }
  field2.Swap(&field1);

  EXPECT_EQ(field1.size(), 16);
  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(field1.Get(i), i * i);
  }
  EXPECT_EQ(field2.size(), 18);
  EXPECT_EQ(field2.Get(0), 5);
  EXPECT_EQ(field2.Get(1), 42);
  for (int i = 2; i < 18; i++) {
    EXPECT_EQ(field2.Get(i), i - 2);
  }
}

template <int kSize>
void TestMemswap() {
  SCOPED_TRACE(kSize);

  const auto a_char = [](int i) -> char { return (i % ('z' - 'a')) + 'a'; };
  const auto b_char = [](int i) -> char { return (i % ('Z' - 'A')) + 'A'; };
  std::string a, b;
  for (int i = 0; i < kSize; ++i) {
    a += a_char(i);
    b += b_char(i);
  }
  // We will not swap these.
  a += '+';
  b += '-';

  std::string expected_a = b, expected_b = a;
  expected_a.back() = '+';
  expected_b.back() = '-';

  internal::memswap<kSize>(&a[0], &b[0]);

  // ODR use the functions in a way that forces the linker to keep them. That
  // way we can see their generated code.
  volatile auto odr_use_for_asm_dump = &internal::memswap<kSize>;
  (void)odr_use_for_asm_dump;

  EXPECT_EQ(expected_a, a);
  EXPECT_EQ(expected_b, b);
}

TEST(Memswap, VerifyWithSmallAndLargeSizes) {
  // Arbitrary sizes
  TestMemswap<0>();
  TestMemswap<1>();
  TestMemswap<10>();
  TestMemswap<100>();
  TestMemswap<1000>();
  TestMemswap<10000>();
  TestMemswap<100000>();
  TestMemswap<1000000>();

  // Pointer aligned sizes
  TestMemswap<sizeof(void*) * 1>();
  TestMemswap<sizeof(void*) * 7>();
  TestMemswap<sizeof(void*) * 17>();
  TestMemswap<sizeof(void*) * 27>();

  // Test also just the block size and no leftover.
  TestMemswap<64 * 1>();
  TestMemswap<64 * 2>();
  TestMemswap<64 * 3>();
  TestMemswap<64 * 4>();
}

// Determines how much space was reserved by the given field by adding elements
// to it until it re-allocates its space.
static int ReservedSpace(RepeatedField<int>* field) {
  const int* ptr = field->data();
  do {
    field->Add(0);
  } while (field->data() == ptr);

  return field->size() - 1;
}

TEST(RepeatedField, ReserveMoreThanDouble) {
  // Reserve more than double the previous space in the field and expect the
  // field to reserve exactly the amount specified.
  RepeatedField<int> field;
  field.Reserve(20);

  EXPECT_LE(20, ReservedSpace(&field));
}

TEST(RepeatedField, ReserveLessThanDouble) {
  // Reserve less than double the previous space in the field and expect the
  // field to grow by double instead.
  RepeatedField<int> field;
  field.Reserve(20);
  int capacity = field.Capacity();
  field.Reserve(capacity * 1.5);

  EXPECT_LE(2 * capacity, ReservedSpace(&field));
}

TEST(RepeatedField, ReserveLessThanExisting) {
  // Reserve less than the previous space in the field and expect the
  // field to not re-allocate at all.
  RepeatedField<int> field;
  field.Reserve(20);
  const int* previous_ptr = field.data();
  field.Reserve(10);

  EXPECT_EQ(previous_ptr, field.data());
  EXPECT_LE(20, ReservedSpace(&field));
}

TEST(RepeatedField, Resize) {
  RepeatedField<int> field;
  field.Resize(2, 1);
  EXPECT_EQ(2, field.size());
  field.Resize(5, 2);
  EXPECT_EQ(5, field.size());
  field.Resize(4, 3);
  ASSERT_EQ(4, field.size());
  EXPECT_EQ(1, field.Get(0));
  EXPECT_EQ(1, field.Get(1));
  EXPECT_EQ(2, field.Get(2));
  EXPECT_EQ(2, field.Get(3));
  field.Resize(0, 4);
  EXPECT_TRUE(field.empty());
}

TEST(RepeatedField, ReserveNothing) {
  RepeatedField<int> field;
  EXPECT_EQ(0, field.Capacity());

  field.Reserve(-1);
  EXPECT_EQ(0, field.Capacity());
}

TEST(RepeatedField, ReserveLowerClamp) {
  int clamped_value = internal::CalculateReserveSize<bool, sizeof(void*)>(0, 1);
  EXPECT_GE(clamped_value, sizeof(void*) / sizeof(bool));
  EXPECT_EQ((internal::RepeatedFieldLowerClampLimit<bool, sizeof(void*)>()),
            clamped_value);
  // EXPECT_EQ(clamped_value, (internal::CalculateReserveSize<bool,
  // sizeof(void*)>( clamped_value, 2)));

  clamped_value = internal::CalculateReserveSize<int, sizeof(void*)>(0, 1);
  EXPECT_GE(clamped_value, sizeof(void*) / sizeof(int));
  EXPECT_EQ((internal::RepeatedFieldLowerClampLimit<int, sizeof(void*)>()),
            clamped_value);
  // EXPECT_EQ(clamped_value, (internal::CalculateReserveSize<int,
  // sizeof(void*)>( clamped_value, 2)));
}

TEST(RepeatedField, ReserveGrowth) {
  // Make sure the field capacity doubles in size on repeated reservation.
  for (int size = internal::RepeatedFieldLowerClampLimit<int, sizeof(void*)>(),
           i = 0;
       i < 4; ++i) {
    int next =
        sizeof(Arena*) >= sizeof(int)
            ?
            // for small enough elements, we double number of total bytes
            ((2 * (size * sizeof(int) + sizeof(Arena*))) - sizeof(Arena*)) /
                sizeof(int)
            :
            // we just double the number of elements if too large size.
            size * 2;
    EXPECT_EQ(next, (internal::CalculateReserveSize<int, sizeof(void*)>(
                        size, size + 1)));
    size = next;
  }
}

TEST(RepeatedField, ReserveLarge) {
  const int old_size = 10;
  // This is a size we won't get by doubling:
  const int new_size = old_size * 3 + 1;

  // Reserving more than 2x current capacity should grow directly to that size.
  EXPECT_EQ(new_size, (internal::CalculateReserveSize<int, sizeof(void*)>(
                          old_size, new_size)));
}

TEST(RepeatedField, ReserveHuge) {
#if defined(ABSL_HAVE_ADDRESS_SANITIZER) || defined(ABSL_HAVE_MEMORY_SANITIZER)
  GTEST_SKIP() << "Disabled because sanitizer is active";
#endif
  // Largest value that does not clamp to the large limit:
  constexpr int non_clamping_limit =
      (std::numeric_limits<int>::max() - sizeof(Arena*)) / 2;
  ASSERT_LT(2 * non_clamping_limit, std::numeric_limits<int>::max());
  EXPECT_LT((internal::CalculateReserveSize<int, sizeof(void*)>(
                non_clamping_limit, non_clamping_limit + 1)),
            std::numeric_limits<int>::max());

  // Smallest size that *will* clamp to the upper limit:
  constexpr int min_clamping_size = std::numeric_limits<int>::max() / 2 + 1;
  EXPECT_EQ((internal::CalculateReserveSize<int, sizeof(void*)>(
                min_clamping_size, min_clamping_size + 1)),
            std::numeric_limits<int>::max());

#ifdef PROTOBUF_TEST_ALLOW_LARGE_ALLOC
  // The rest of this test may allocate several GB of memory, so it is only
  // built if explicitly requested.
  RepeatedField<int> huge_field;

  // Reserve a size for huge_field that will clamp.
  huge_field.Reserve(min_clamping_size);
  EXPECT_GE(huge_field.Capacity(), min_clamping_size);
  ASSERT_LT(huge_field.Capacity(), std::numeric_limits<int>::max() - 1);

  // The array containing all the fields is, in theory, up to MAXINT-1 in size.
  // However, some compilers can't handle a struct whose size is larger
  // than 2GB, and the protocol buffer format doesn't handle more than 2GB of
  // data at once, either.  So we limit it, but the code below accesses beyond
  // that limit.

  // Allocation may return more memory than we requested. However, the updated
  // size must still be clamped to a valid range.
  huge_field.Reserve(huge_field.Capacity() + 1);
  EXPECT_EQ(huge_field.Capacity(), std::numeric_limits<int>::max());
#endif  // PROTOBUF_TEST_ALLOW_LARGE_ALLOC
}

TEST(RepeatedField, MergeFrom) {
  RepeatedField<int> source, destination;
  source.Add(4);
  source.Add(5);
  destination.Add(1);
  destination.Add(2);
  destination.Add(3);

  destination.MergeFrom(source);

  ASSERT_EQ(5, destination.size());
  EXPECT_EQ(1, destination.Get(0));
  EXPECT_EQ(2, destination.Get(1));
  EXPECT_EQ(3, destination.Get(2));
  EXPECT_EQ(4, destination.Get(3));
  EXPECT_EQ(5, destination.Get(4));
}


TEST(RepeatedField, CopyFrom) {
  RepeatedField<int> source, destination;
  source.Add(4);
  source.Add(5);
  destination.Add(1);
  destination.Add(2);
  destination.Add(3);

  destination.CopyFrom(source);

  ASSERT_EQ(2, destination.size());
  EXPECT_EQ(4, destination.Get(0));
  EXPECT_EQ(5, destination.Get(1));
}

TEST(RepeatedField, CopyFromSelf) {
  RepeatedField<int> me;
  me.Add(3);
  me.CopyFrom(me);
  ASSERT_EQ(1, me.size());
  EXPECT_EQ(3, me.Get(0));
}

TEST(RepeatedField, Erase) {
  RepeatedField<int> me;
  RepeatedField<int>::iterator it = me.erase(me.begin(), me.end());
  EXPECT_TRUE(me.begin() == it);
  EXPECT_EQ(0, me.size());

  me.Add(1);
  me.Add(2);
  me.Add(3);
  it = me.erase(me.begin(), me.end());
  EXPECT_TRUE(me.begin() == it);
  EXPECT_EQ(0, me.size());

  me.Add(4);
  me.Add(5);
  me.Add(6);
  it = me.erase(me.begin() + 2, me.end());
  EXPECT_TRUE(me.begin() + 2 == it);
  EXPECT_EQ(2, me.size());
  EXPECT_EQ(4, me.Get(0));
  EXPECT_EQ(5, me.Get(1));

  me.Add(6);
  me.Add(7);
  me.Add(8);
  it = me.erase(me.begin() + 1, me.begin() + 3);
  EXPECT_TRUE(me.begin() + 1 == it);
  EXPECT_EQ(3, me.size());
  EXPECT_EQ(4, me.Get(0));
  EXPECT_EQ(7, me.Get(1));
  EXPECT_EQ(8, me.Get(2));
}

// Add contents of empty container to an empty field.
TEST(RepeatedField, AddRange1) {
  RepeatedField<int> me;
  std::vector<int> values;

  me.Add(values.begin(), values.end());
  ASSERT_EQ(me.size(), 0);
}

// Add contents of container with one thing to an empty field.
TEST(RepeatedField, AddRange2) {
  RepeatedField<int> me;
  std::vector<int> values;
  values.push_back(-1);

  me.Add(values.begin(), values.end());
  ASSERT_EQ(me.size(), 1);
  ASSERT_EQ(me.Get(0), values[0]);
}

// Add contents of container with more than one thing to an empty field.
TEST(RepeatedField, AddRange3) {
  RepeatedField<int> me;
  std::vector<int> values;
  values.push_back(0);
  values.push_back(1);

  me.Add(values.begin(), values.end());
  ASSERT_EQ(me.size(), 2);
  ASSERT_EQ(me.Get(0), values[0]);
  ASSERT_EQ(me.Get(1), values[1]);
}

// Add contents of container with more than one thing to a non-empty field.
TEST(RepeatedField, AddRange4) {
  RepeatedField<int> me;
  me.Add(0);
  me.Add(1);

  std::vector<int> values;
  values.push_back(2);
  values.push_back(3);

  me.Add(values.begin(), values.end());
  ASSERT_EQ(me.size(), 4);
  ASSERT_EQ(me.Get(0), 0);
  ASSERT_EQ(me.Get(1), 1);
  ASSERT_EQ(me.Get(2), values[0]);
  ASSERT_EQ(me.Get(3), values[1]);
}

// Add contents of a stringstream in order to test code paths where there is
// an input iterator.
TEST(RepeatedField, AddRange5) {
  RepeatedField<int> me;
  me.Add(0);

  std::stringstream ss;
  ss << 1 << ' ' << 2;

  me.Add(std::istream_iterator<int>(ss), std::istream_iterator<int>());
  ASSERT_EQ(me.size(), 3);
  ASSERT_EQ(me.Get(0), 0);
  ASSERT_EQ(me.Get(1), 1);
  ASSERT_EQ(me.Get(2), 2);
}

// Add contents of container with a quirky iterator like std::vector<bool>
TEST(RepeatedField, AddRange6) {
  RepeatedField<bool> me;
  me.Add(true);
  me.Add(false);

  std::vector<bool> values;
  values.push_back(true);
  values.push_back(true);
  values.push_back(false);

  me.Add(values.begin(), values.end());
  ASSERT_EQ(me.size(), 5);
  ASSERT_EQ(me.Get(0), true);
  ASSERT_EQ(me.Get(1), false);
  ASSERT_EQ(me.Get(2), true);
  ASSERT_EQ(me.Get(3), true);
  ASSERT_EQ(me.Get(4), false);
}

// Add contents of absl::Span which evaluates to const T on access.
TEST(RepeatedField, AddRange7) {
  int ints[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  absl::Span<const int> span(ints);
  auto p = span.begin();
  static_assert(std::is_convertible<decltype(p), const int*>::value, "");
  RepeatedField<int> me;
  me.Add(span.begin(), span.end());

  ASSERT_EQ(me.size(), 10);
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(me.Get(i), i);
  }
}

TEST(RepeatedField, AddAndAssignRanges) {
  RepeatedField<int> field;

  int vals[] = {2, 27, 2875, 609250};
  field.Assign(std::begin(vals), std::end(vals));

  ASSERT_EQ(field.size(), 4);
  EXPECT_EQ(field.Get(0), 2);
  EXPECT_EQ(field.Get(1), 27);
  EXPECT_EQ(field.Get(2), 2875);
  EXPECT_EQ(field.Get(3), 609250);

  field.Add(std::begin(vals), std::end(vals));
  ASSERT_EQ(field.size(), 8);
  EXPECT_EQ(field.Get(0), 2);
  EXPECT_EQ(field.Get(1), 27);
  EXPECT_EQ(field.Get(2), 2875);
  EXPECT_EQ(field.Get(3), 609250);
  EXPECT_EQ(field.Get(4), 2);
  EXPECT_EQ(field.Get(5), 27);
  EXPECT_EQ(field.Get(6), 2875);
  EXPECT_EQ(field.Get(7), 609250);
}

TEST(RepeatedField, CopyConstructIntegers) {
  auto token = internal::InternalVisibilityForTesting{};
  using RepeatedType = RepeatedField<int>;
  RepeatedType original;
  original.Add(1);
  original.Add(2);

  RepeatedType fields1(original);
  ASSERT_EQ(2, fields1.size());
  EXPECT_EQ(1, fields1.Get(0));
  EXPECT_EQ(2, fields1.Get(1));

  RepeatedType fields2(token, nullptr, original);
  ASSERT_EQ(2, fields1.size());
  EXPECT_EQ(1, fields1.Get(0));
  EXPECT_EQ(2, fields1.Get(1));
}

TEST(RepeatedField, CopyConstructCords) {
  auto token = internal::InternalVisibilityForTesting{};
  using RepeatedType = RepeatedField<absl::Cord>;
  RepeatedType original;
  original.Add(absl::Cord("hello"));
  original.Add(absl::Cord("world and text to avoid SSO"));

  RepeatedType fields1(original);
  ASSERT_EQ(2, fields1.size());
  EXPECT_EQ("hello", fields1.Get(0));
  EXPECT_EQ("world and text to avoid SSO", fields1.Get(1));

  RepeatedType fields2(token, nullptr, original);
  ASSERT_EQ(2, fields1.size());
  EXPECT_EQ("hello", fields1.Get(0));
  EXPECT_EQ("world and text to avoid SSO", fields2.Get(1));
}

TEST(RepeatedField, CopyConstructIntegersWithArena) {
  auto token = internal::InternalVisibilityForTesting{};
  using RepeatedType = RepeatedField<int>;
  RepeatedType original;
  original.Add(1);
  original.Add(2);

  Arena arena;
  alignas(RepeatedType) char mem[sizeof(RepeatedType)];
  RepeatedType& fields1 = *new (mem) RepeatedType(token, &arena, original);
  ASSERT_EQ(2, fields1.size());
  EXPECT_EQ(1, fields1.Get(0));
  EXPECT_EQ(2, fields1.Get(1));
}

TEST(RepeatedField, CopyConstructCordsWithArena) {
  auto token = internal::InternalVisibilityForTesting{};
  using RepeatedType = RepeatedField<absl::Cord>;
  RepeatedType original;
  original.Add(absl::Cord("hello"));
  original.Add(absl::Cord("world and text to avoid SSO"));

  Arena arena;
  alignas(RepeatedType) char mem[sizeof(RepeatedType)];
  RepeatedType& fields1 = *new (mem) RepeatedType(token, &arena, original);
  ASSERT_EQ(2, fields1.size());
  EXPECT_EQ("hello", fields1.Get(0));
  EXPECT_EQ("world and text to avoid SSO", fields1.Get(1));

  // Contract requires dtor to be invoked for absl::Cord
  fields1.~RepeatedType();
}

TEST(RepeatedField, IteratorConstruct) {
  std::vector<int> values;
  RepeatedField<int> empty(values.begin(), values.end());
  ASSERT_EQ(values.size(), empty.size());

  values.push_back(1);
  values.push_back(2);

  RepeatedField<int> field(values.begin(), values.end());
  ASSERT_EQ(values.size(), field.size());
  EXPECT_EQ(values[0], field.Get(0));
  EXPECT_EQ(values[1], field.Get(1));

  RepeatedField<int> other(field.begin(), field.end());
  ASSERT_EQ(values.size(), other.size());
  EXPECT_EQ(values[0], other.Get(0));
  EXPECT_EQ(values[1], other.Get(1));
}

TEST(RepeatedField, CopyAssign) {
  RepeatedField<int> source, destination;
  source.Add(4);
  source.Add(5);
  destination.Add(1);
  destination.Add(2);
  destination.Add(3);

  destination = source;

  ASSERT_EQ(2, destination.size());
  EXPECT_EQ(4, destination.Get(0));
  EXPECT_EQ(5, destination.Get(1));
}

TEST(RepeatedField, SelfAssign) {
  // Verify that assignment to self does not destroy data.
  RepeatedField<int> source, *p;
  p = &source;
  source.Add(7);
  source.Add(8);

  *p = source;

  ASSERT_EQ(2, source.size());
  EXPECT_EQ(7, source.Get(0));
  EXPECT_EQ(8, source.Get(1));
}

TEST(RepeatedField, MoveConstruct) {
  {
    RepeatedField<int> source;
    source.Add(1);
    source.Add(2);
    const int* data = source.data();
    RepeatedField<int> destination = std::move(source);
    EXPECT_EQ(data, destination.data());
    EXPECT_THAT(destination, ElementsAre(1, 2));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_TRUE(source.empty());
  }
  {
    Arena arena;
    RepeatedField<int>* source =
        Arena::CreateMessage<RepeatedField<int>>(&arena);
    source->Add(1);
    source->Add(2);
    RepeatedField<int> destination = std::move(*source);
    EXPECT_EQ(nullptr, destination.GetArena());
    EXPECT_THAT(destination, ElementsAre(1, 2));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_THAT(*source, ElementsAre(1, 2));
  }
}

TEST(RepeatedField, MoveAssign) {
  {
    RepeatedField<int> source;
    source.Add(1);
    source.Add(2);
    RepeatedField<int> destination;
    destination.Add(3);
    const int* source_data = source.data();
    const int* destination_data = destination.data();
    destination = std::move(source);
    EXPECT_EQ(source_data, destination.data());
    EXPECT_THAT(destination, ElementsAre(1, 2));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_EQ(destination_data, source.data());
    EXPECT_THAT(source, ElementsAre(3));
  }
  {
    Arena arena;
    RepeatedField<int>* source =
        Arena::CreateMessage<RepeatedField<int>>(&arena);
    source->Add(1);
    source->Add(2);
    RepeatedField<int>* destination =
        Arena::CreateMessage<RepeatedField<int>>(&arena);
    destination->Add(3);
    const int* source_data = source->data();
    *destination = std::move(*source);
    EXPECT_EQ(source_data, destination->data());
    EXPECT_THAT(*destination, ElementsAre(1, 2));
    EXPECT_THAT(*source, ElementsAre(3));
  }
  {
    Arena source_arena;
    RepeatedField<int>* source =
        Arena::CreateMessage<RepeatedField<int>>(&source_arena);
    source->Add(1);
    source->Add(2);
    Arena destination_arena;
    RepeatedField<int>* destination =
        Arena::CreateMessage<RepeatedField<int>>(&destination_arena);
    destination->Add(3);
    *destination = std::move(*source);
    EXPECT_THAT(*destination, ElementsAre(1, 2));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_THAT(*source, ElementsAre(1, 2));
  }
  {
    Arena arena;
    RepeatedField<int>* source =
        Arena::CreateMessage<RepeatedField<int>>(&arena);
    source->Add(1);
    source->Add(2);
    RepeatedField<int> destination;
    destination.Add(3);
    destination = std::move(*source);
    EXPECT_THAT(destination, ElementsAre(1, 2));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_THAT(*source, ElementsAre(1, 2));
  }
  {
    RepeatedField<int> source;
    source.Add(1);
    source.Add(2);
    Arena arena;
    RepeatedField<int>* destination =
        Arena::CreateMessage<RepeatedField<int>>(&arena);
    destination->Add(3);
    *destination = std::move(source);
    EXPECT_THAT(*destination, ElementsAre(1, 2));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_THAT(source, ElementsAre(1, 2));
  }
  {
    RepeatedField<int> field;
    // An alias to defeat -Wself-move.
    RepeatedField<int>& alias = field;
    field.Add(1);
    field.Add(2);
    const int* data = field.data();
    field = std::move(alias);
    EXPECT_EQ(data, field.data());
    EXPECT_THAT(field, ElementsAre(1, 2));
  }
  {
    Arena arena;
    RepeatedField<int>* field =
        Arena::CreateMessage<RepeatedField<int>>(&arena);
    field->Add(1);
    field->Add(2);
    const int* data = field->data();
    *field = std::move(*field);
    EXPECT_EQ(data, field->data());
    EXPECT_THAT(*field, ElementsAre(1, 2));
  }
}

TEST(Movable, Works) {
  class NonMoveConstructible {
   public:
    NonMoveConstructible(NonMoveConstructible&&) = delete;
    NonMoveConstructible& operator=(NonMoveConstructible&&) { return *this; }
  };
  class NonMoveAssignable {
   public:
    NonMoveAssignable(NonMoveAssignable&&) {}
    NonMoveAssignable& operator=(NonMoveConstructible&&) = delete;
  };
  class NonMovable {
   public:
    NonMovable(NonMovable&&) = delete;
    NonMovable& operator=(NonMovable&&) = delete;
  };

  EXPECT_TRUE(internal::IsMovable<std::string>::value);

  EXPECT_FALSE(std::is_move_constructible<NonMoveConstructible>::value);
  EXPECT_TRUE(std::is_move_assignable<NonMoveConstructible>::value);
  EXPECT_FALSE(internal::IsMovable<NonMoveConstructible>::value);

  EXPECT_TRUE(std::is_move_constructible<NonMoveAssignable>::value);
  EXPECT_FALSE(std::is_move_assignable<NonMoveAssignable>::value);
  EXPECT_FALSE(internal::IsMovable<NonMoveAssignable>::value);

  EXPECT_FALSE(internal::IsMovable<NonMovable>::value);
}

TEST(RepeatedField, MoveAdd) {
  RepeatedPtrField<TestAllTypes> field;
  TestAllTypes test_all_types;
  auto* optional_nested_message =
      test_all_types.mutable_optional_nested_message();
  optional_nested_message->set_bb(42);
  field.Add(std::move(test_all_types));

  EXPECT_EQ(optional_nested_message,
            field.Mutable(0)->mutable_optional_nested_message());
}

TEST(RepeatedField, MutableDataIsMutable) {
  RepeatedField<int> field;
  field.Add(1);
  EXPECT_EQ(1, field.Get(0));
  // The fact that this line compiles would be enough, but we'll check the
  // value anyway.
  *field.mutable_data() = 2;
  EXPECT_EQ(2, field.Get(0));
}

TEST(RepeatedField, SubscriptOperators) {
  RepeatedField<int> field;
  field.Add(1);
  EXPECT_EQ(1, field.Get(0));
  EXPECT_EQ(1, field[0]);
  EXPECT_EQ(field.Mutable(0), &field[0]);
  const RepeatedField<int>& const_field = field;
  EXPECT_EQ(field.data(), &const_field[0]);
}

TEST(RepeatedField, Truncate) {
  RepeatedField<int> field;

  field.Add(12);
  field.Add(34);
  field.Add(56);
  field.Add(78);
  EXPECT_EQ(4, field.size());

  field.Truncate(3);
  EXPECT_EQ(3, field.size());

  field.Add(90);
  EXPECT_EQ(4, field.size());
  EXPECT_EQ(90, field.Get(3));

  // Truncations that don't change the size are allowed, but growing is not
  // allowed.
  field.Truncate(field.size());
#if GTEST_HAS_DEATH_TEST
  EXPECT_DEBUG_DEATH(field.Truncate(field.size() + 1), "new_size");
#endif
}

TEST(RepeatedCordField, AddRemoveLast) {
  RepeatedField<absl::Cord> field;
  field.Add(absl::Cord("foo"));
  field.RemoveLast();
}

TEST(RepeatedCordField, AddClear) {
  RepeatedField<absl::Cord> field;
  field.Add(absl::Cord("foo"));
  field.Clear();
}

TEST(RepeatedCordField, Resize) {
  RepeatedField<absl::Cord> field;
  field.Resize(10, absl::Cord("foo"));
}

TEST(RepeatedField, Cords) {
  RepeatedField<absl::Cord> field;

  field.Add(absl::Cord("foo"));
  field.Add(absl::Cord("bar"));
  field.Add(absl::Cord("baz"));
  field.Add(absl::Cord("moo"));
  field.Add(absl::Cord("corge"));

  EXPECT_EQ("foo", std::string(field.Get(0)));
  EXPECT_EQ("corge", std::string(field.Get(4)));

  // Test swap.  Note:  One of the swapped objects is using internal storage,
  //   the other is not.
  RepeatedField<absl::Cord> field2;
  field2.Add(absl::Cord("grault"));
  field.Swap(&field2);
  EXPECT_EQ(1, field.size());
  EXPECT_EQ("grault", std::string(field.Get(0)));
  EXPECT_EQ(5, field2.size());
  EXPECT_EQ("foo", std::string(field2.Get(0)));
  EXPECT_EQ("corge", std::string(field2.Get(4)));

  // Test SwapElements().
  field2.SwapElements(1, 3);
  EXPECT_EQ("moo", std::string(field2.Get(1)));
  EXPECT_EQ("bar", std::string(field2.Get(3)));

  // Make sure cords are cleared correctly.
  field2.RemoveLast();
  EXPECT_TRUE(field2.Add()->empty());
  field2.Clear();
  EXPECT_TRUE(field2.Add()->empty());
}

TEST(RepeatedField, TruncateCords) {
  RepeatedField<absl::Cord> field;

  field.Add(absl::Cord("foo"));
  field.Add(absl::Cord("bar"));
  field.Add(absl::Cord("baz"));
  field.Add(absl::Cord("moo"));
  EXPECT_EQ(4, field.size());

  field.Truncate(3);
  EXPECT_EQ(3, field.size());

  field.Add(absl::Cord("corge"));
  EXPECT_EQ(4, field.size());
  EXPECT_EQ("corge", std::string(field.Get(3)));

  // Truncating to the current size should be fine (no-op), but truncating
  // to a larger size should crash.
  field.Truncate(field.size());
#if defined(GTEST_HAS_DEATH_TEST) && !defined(NDEBUG)
  EXPECT_DEATH(field.Truncate(field.size() + 1), "new_size");
#endif
}

TEST(RepeatedField, ResizeCords) {
  RepeatedField<absl::Cord> field;
  field.Resize(2, absl::Cord("foo"));
  EXPECT_EQ(2, field.size());
  field.Resize(5, absl::Cord("bar"));
  EXPECT_EQ(5, field.size());
  field.Resize(4, absl::Cord("baz"));
  ASSERT_EQ(4, field.size());
  EXPECT_EQ("foo", std::string(field.Get(0)));
  EXPECT_EQ("foo", std::string(field.Get(1)));
  EXPECT_EQ("bar", std::string(field.Get(2)));
  EXPECT_EQ("bar", std::string(field.Get(3)));
  field.Resize(0, absl::Cord("moo"));
  EXPECT_TRUE(field.empty());
}

TEST(RepeatedField, ExtractSubrange) {
  // Exhaustively test every subrange in arrays of all sizes from 0 through 9.
  for (int sz = 0; sz < 10; ++sz) {
    for (int num = 0; num <= sz; ++num) {
      for (int start = 0; start < sz - num; ++start) {
        // Create RepeatedField with sz elements having values 0 through sz-1.
        RepeatedField<int32_t> field;
        for (int i = 0; i < sz; ++i) field.Add(i);
        EXPECT_EQ(field.size(), sz);

        // Create a catcher array and call ExtractSubrange.
        int32_t catcher[10];
        for (int i = 0; i < 10; ++i) catcher[i] = -1;
        field.ExtractSubrange(start, num, catcher);

        // Does the resulting array have the right size?
        EXPECT_EQ(field.size(), sz - num);

        // Were the removed elements extracted into the catcher array?
        for (int i = 0; i < num; ++i) EXPECT_EQ(catcher[i], start + i);
        EXPECT_EQ(catcher[num], -1);

        // Does the resulting array contain the right values?
        for (int i = 0; i < start; ++i) EXPECT_EQ(field.Get(i), i);
        for (int i = start; i < field.size(); ++i)
          EXPECT_EQ(field.Get(i), i + num);
      }
    }
  }
}

TEST(RepeatedField, TestSAddFromSelf) {
  RepeatedField<int> field;
  field.Add(0);
  for (int i = 0; i < 1000; i++) {
    field.Add(field[0]);
  }
}

// We have, or at least had bad callers that never triggered our DCHECKS
// Here we check we DO fail on bad Truncate calls under debug, and do nothing
// under opt compiles.
TEST(RepeatedField, HardenAgainstBadTruncate) {
  RepeatedField<int> field;
  for (int size = 0; size < 10; ++size) {
    field.Truncate(size);
#if GTEST_HAS_DEATH_TEST
    EXPECT_DEBUG_DEATH(field.Truncate(size + 1), "new_size <= current_size_");
    EXPECT_DEBUG_DEATH(field.Truncate(size + 2), "new_size <= current_size_");
#elif defined(NDEBUG)
    field.Truncate(size + 1);
    field.Truncate(size + 1);
#endif
    EXPECT_EQ(field.size(), size);
    field.Add(1);
  }
}

#if defined(GTEST_HAS_DEATH_TEST) && (defined(ABSL_HAVE_ADDRESS_SANITIZER) || \
                                      defined(ABSL_HAVE_MEMORY_SANITIZER))

// This function verifies that the code dies under ASAN or MSAN trying to both
// read and write the reserved element directly beyond the last element.
void VerifyDeathOnWriteAndReadAccessBeyondEnd(RepeatedField<int64_t>& field) {
  auto* end = field.Mutable(field.size() - 1) + 1;
#if defined(ABSL_HAVE_ADDRESS_SANITIZER)
  EXPECT_DEATH(*end = 1, "container-overflow");
  EXPECT_DEATH(EXPECT_NE(*end, 1), "container-overflow");
#elif defined(ABSL_HAVE_MEMORY_SANITIZER)
  EXPECT_DEATH(EXPECT_NE(*end, 1), "use-of-uninitialized-value");
#endif

  // Confirm we died a death of *SAN
  EXPECT_EQ(field.AddAlreadyReserved(), end);
  *end = 1;
  EXPECT_EQ(*end, 1);
}

TEST(RepeatedField, PoisonsMemoryOnAdd) {
  RepeatedField<int64_t> field;
  do {
    field.Add(0);
  } while (field.size() == field.Capacity());
  VerifyDeathOnWriteAndReadAccessBeyondEnd(field);
}

TEST(RepeatedField, PoisonsMemoryOnAddAlreadyReserved) {
  RepeatedField<int64_t> field;
  field.Reserve(2);
  field.AddAlreadyReserved();
  VerifyDeathOnWriteAndReadAccessBeyondEnd(field);
}

TEST(RepeatedField, PoisonsMemoryOnAddNAlreadyReserved) {
  RepeatedField<int64_t> field;
  field.Reserve(10);
  field.AddNAlreadyReserved(8);
  VerifyDeathOnWriteAndReadAccessBeyondEnd(field);
}

TEST(RepeatedField, PoisonsMemoryOnResize) {
  RepeatedField<int64_t> field;
  field.Add(0);
  do {
    field.Resize(field.size() + 1, 1);
  } while (field.size() == field.Capacity());
  VerifyDeathOnWriteAndReadAccessBeyondEnd(field);

  // Shrink size
  field.Resize(field.size() - 1, 1);
  VerifyDeathOnWriteAndReadAccessBeyondEnd(field);
}

TEST(RepeatedField, PoisonsMemoryOnTruncate) {
  RepeatedField<int64_t> field;
  field.Add(0);
  field.Add(1);
  field.Truncate(1);
  VerifyDeathOnWriteAndReadAccessBeyondEnd(field);
}

TEST(RepeatedField, PoisonsMemoryOnReserve) {
  RepeatedField<int64_t> field;
  field.Add(1);
  field.Reserve(field.Capacity() + 1);
  VerifyDeathOnWriteAndReadAccessBeyondEnd(field);
}

TEST(RepeatedField, PoisonsMemoryOnAssign) {
  RepeatedField<int64_t> src;
  RepeatedField<int64_t> field;
  src.Add(1);
  src.Add(2);
  field.Reserve(3);
  field = src;
  VerifyDeathOnWriteAndReadAccessBeyondEnd(field);
}

#endif

TEST(RepeatedField, Cleanups) {
  Arena arena;
  auto growth = internal::CleanupGrowth(
      arena, [&] { Arena::CreateMessage<RepeatedField<int>>(&arena); });
  EXPECT_THAT(growth.cleanups, testing::IsEmpty());

  void* ptr;
  growth = internal::CleanupGrowth(arena, [&] {
    ptr = Arena::CreateMessage<RepeatedField<absl::Cord>>(&arena);
  });
  EXPECT_THAT(growth.cleanups, testing::UnorderedElementsAre(ptr));
}

// ===================================================================
// RepeatedPtrField tests.  These pretty much just mirror the RepeatedField
// tests above.

TEST(RepeatedPtrField, ConstInit) {
  PROTOBUF_CONSTINIT static RepeatedPtrField<std::string> field{};  // NOLINT
  EXPECT_TRUE(field.empty());
}

TEST(RepeatedPtrField, ClearThenReserveMore) {
  // Test that Reserve properly destroys the old internal array when it's forced
  // to allocate a new one, even when cleared-but-not-deleted objects are
  // present. Use a 'string' and > 16 bytes length so that the elements are
  // non-POD and allocate -- the leak checker will catch any skipped destructor
  // calls here.
  RepeatedPtrField<std::string> field;
  for (int i = 0; i < 32; i++) {
    *field.Add() = std::string("abcdefghijklmnopqrstuvwxyz0123456789");
  }
  EXPECT_EQ(32, field.size());
  field.Clear();
  EXPECT_EQ(0, field.size());
  EXPECT_LE(32, field.Capacity());

  field.Reserve(1024);
  EXPECT_EQ(0, field.size());
  EXPECT_LE(1024, field.Capacity());
  // Finish test -- |field| should destroy the cleared-but-not-yet-destroyed
  // strings.
}

// This helper overload set tests whether X::f can be called with a braced pair,
// X::f({a, b}) of std::string iterators (specifically, pointers: That call is
// ambiguous if and only if the call to ValidResolutionPointerRange is not.
template <typename X>
auto ValidResolutionPointerRange(const std::string* p)
    -> decltype(X::f({p, p + 2}), std::true_type{});
template <typename X>
std::false_type ValidResolutionPointerRange(void*);

TEST(RepeatedPtrField, UnambiguousConstructor) {
  struct X {
    static bool f(std::vector<std::string>) { return false; }
    static bool f(google::protobuf::RepeatedPtrField<std::string>) { return true; }

    static bool g(std::vector<int>) { return false; }
    static bool g(google::protobuf::RepeatedPtrField<std::string>) { return true; }
  };

  // RepeatedPtrField has no initializer-list constructor, and a constructor
  // from to const char* values is excluded by its constraints.
  EXPECT_FALSE(X::f({"abc", "xyz"}));

  // Construction from a pair of int* is also not ambiguous.
  int a[5] = {};
  EXPECT_FALSE(X::g({a, a + 5}));

  // Construction from string iterators for the unique string overload "g"
  // works.
  // Disabling this for now, this is actually ambiguous with libstdc++.
  // std::string b[2] = {"abc", "xyz"};
  // EXPECT_TRUE(X::g({b, b + 2}));

  // Construction from string iterators for "f" is ambiguous, since both
  // containers are equally good.
  //
  // X::f({b, b + 2});  // error => ValidResolutionPointerRange is unambiguous.
  EXPECT_FALSE(decltype(ValidResolutionPointerRange<X>(nullptr))::value);
}

TEST(RepeatedPtrField, Small) {
  RepeatedPtrField<std::string> field;

  EXPECT_TRUE(field.empty());
  EXPECT_EQ(field.size(), 0);

  field.Add()->assign("foo");

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 1);
  EXPECT_EQ(field.Get(0), "foo");
  EXPECT_EQ(field.at(0), "foo");

  field.Add()->assign("bar");

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 2);
  EXPECT_EQ(field.Get(0), "foo");
  EXPECT_EQ(field.at(0), "foo");
  EXPECT_EQ(field.Get(1), "bar");
  EXPECT_EQ(field.at(1), "bar");

  field.Mutable(1)->assign("baz");

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 2);
  EXPECT_EQ(field.Get(0), "foo");
  EXPECT_EQ(field.at(0), "foo");
  EXPECT_EQ(field.Get(1), "baz");
  EXPECT_EQ(field.at(1), "baz");

  field.RemoveLast();

  EXPECT_FALSE(field.empty());
  EXPECT_EQ(field.size(), 1);
  EXPECT_EQ(field.Get(0), "foo");
  EXPECT_EQ(field.at(0), "foo");

  field.Clear();

  EXPECT_TRUE(field.empty());
  EXPECT_EQ(field.size(), 0);
}

TEST(RepeatedPtrField, Large) {
  RepeatedPtrField<std::string> field;

  for (int i = 0; i < 16; i++) {
    *field.Add() += 'a' + i;
  }

  EXPECT_EQ(field.size(), 16);

  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(field.Get(i).size(), 1);
    EXPECT_EQ(field.Get(i)[0], 'a' + i);
  }

  int min_expected_usage = 16 * sizeof(std::string);
  EXPECT_GE(field.SpaceUsedExcludingSelf(), min_expected_usage);
}

TEST(RepeatedPtrField, ArenaAllocationSizesMatchExpectedValues) {
  CheckAllocationSizes<RepeatedPtrField<std::string>>(true);
}

TEST(RepeatedPtrField, NaturalGrowthOnArenasReuseBlocks) {
  using Rep = RepeatedPtrField<std::string>;
  Arena arena;
  std::vector<Rep*> values;

  static constexpr int kNumFields = 100;
  static constexpr int kNumElems = 1000;
  for (int i = 0; i < kNumFields; ++i) {
    values.push_back(Arena::CreateMessage<Rep>(&arena));
    auto& field = *values.back();
    for (int j = 0; j < kNumElems; ++j) {
      field.Add("");
    }
  }

  size_t expected =
      values.size() * values[0]->Capacity() * sizeof(std::string*) +
      sizeof(std::string) * kNumElems * kNumFields;
  // Use a 2% slack for other overhead.
  // If we were not reusing the blocks, the actual value would be ~2x the
  // expected.
  EXPECT_THAT(arena.SpaceUsed(), AllOf(Ge(expected), Le(1.02 * expected)));
}

TEST(RepeatedPtrField, AddAndAssignRanges) {
  RepeatedPtrField<std::string> field;

  const char* vals[] = {"abc", "x", "yz", "xyzzy"};
  field.Assign(std::begin(vals), std::end(vals));

  ASSERT_EQ(field.size(), 4);
  EXPECT_EQ(field.Get(0), "abc");
  EXPECT_EQ(field.Get(1), "x");
  EXPECT_EQ(field.Get(2), "yz");
  EXPECT_EQ(field.Get(3), "xyzzy");

  field.Add(std::begin(vals), std::end(vals));
  ASSERT_EQ(field.size(), 8);
  EXPECT_EQ(field.Get(0), "abc");
  EXPECT_EQ(field.Get(1), "x");
  EXPECT_EQ(field.Get(2), "yz");
  EXPECT_EQ(field.Get(3), "xyzzy");
  EXPECT_EQ(field.Get(4), "abc");
  EXPECT_EQ(field.Get(5), "x");
  EXPECT_EQ(field.Get(6), "yz");
  EXPECT_EQ(field.Get(7), "xyzzy");
}

TEST(RepeatedPtrField, SwapSmallSmall) {
  RepeatedPtrField<std::string> field1;
  RepeatedPtrField<std::string> field2;

  EXPECT_TRUE(field1.empty());
  EXPECT_EQ(field1.size(), 0);
  EXPECT_TRUE(field2.empty());
  EXPECT_EQ(field2.size(), 0);

  field1.Add()->assign("foo");
  field1.Add()->assign("bar");

  EXPECT_FALSE(field1.empty());
  EXPECT_EQ(field1.size(), 2);
  EXPECT_EQ(field1.Get(0), "foo");
  EXPECT_EQ(field1.Get(1), "bar");

  EXPECT_TRUE(field2.empty());
  EXPECT_EQ(field2.size(), 0);

  field1.Swap(&field2);

  EXPECT_TRUE(field1.empty());
  EXPECT_EQ(field1.size(), 0);

  EXPECT_EQ(field2.size(), 2);
  EXPECT_EQ(field2.Get(0), "foo");
  EXPECT_EQ(field2.Get(1), "bar");
}

TEST(RepeatedPtrField, SwapLargeSmall) {
  RepeatedPtrField<std::string> field1;
  RepeatedPtrField<std::string> field2;

  field2.Add()->assign("foo");
  field2.Add()->assign("bar");
  for (int i = 0; i < 16; i++) {
    *field1.Add() += 'a' + i;
  }
  field1.Swap(&field2);

  EXPECT_EQ(field1.size(), 2);
  EXPECT_EQ(field1.Get(0), "foo");
  EXPECT_EQ(field1.Get(1), "bar");
  EXPECT_EQ(field2.size(), 16);
  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(field2.Get(i).size(), 1);
    EXPECT_EQ(field2.Get(i)[0], 'a' + i);
  }
}

TEST(RepeatedPtrField, SwapLargeLarge) {
  RepeatedPtrField<std::string> field1;
  RepeatedPtrField<std::string> field2;

  field1.Add()->assign("foo");
  field1.Add()->assign("bar");
  for (int i = 0; i < 16; i++) {
    *field1.Add() += 'A' + i;
    *field2.Add() += 'a' + i;
  }
  field2.Swap(&field1);

  EXPECT_EQ(field1.size(), 16);
  for (int i = 0; i < 16; i++) {
    EXPECT_EQ(field1.Get(i).size(), 1);
    EXPECT_EQ(field1.Get(i)[0], 'a' + i);
  }
  EXPECT_EQ(field2.size(), 18);
  EXPECT_EQ(field2.Get(0), "foo");
  EXPECT_EQ(field2.Get(1), "bar");
  for (int i = 2; i < 18; i++) {
    EXPECT_EQ(field2.Get(i).size(), 1);
    EXPECT_EQ(field2.Get(i)[0], 'A' + i - 2);
  }
}

static int ReservedSpace(RepeatedPtrField<std::string>* field) {
  const std::string* const* ptr = field->data();
  do {
    field->Add();
  } while (field->data() == ptr);

  return field->size() - 1;
}

TEST(RepeatedPtrField, ReserveMoreThanDouble) {
  RepeatedPtrField<std::string> field;
  field.Reserve(20);

  EXPECT_LE(20, ReservedSpace(&field));
}

TEST(RepeatedPtrField, ReserveLessThanDouble) {
  RepeatedPtrField<std::string> field;
  field.Reserve(20);

  int capacity = field.Capacity();
  // Grow by 1.5x
  field.Reserve(capacity + (capacity >> 2));

  EXPECT_LE(2 * capacity, ReservedSpace(&field));
}

TEST(RepeatedPtrField, ReserveLessThanExisting) {
  RepeatedPtrField<std::string> field;
  field.Reserve(20);
  const std::string* const* previous_ptr = field.data();
  field.Reserve(10);

  EXPECT_EQ(previous_ptr, field.data());
  EXPECT_LE(20, ReservedSpace(&field));
}

TEST(RepeatedPtrField, ReserveDoesntLoseAllocated) {
  // Check that a bug is fixed:  An earlier implementation of Reserve()
  // failed to copy pointers to allocated-but-cleared objects, possibly
  // leading to segfaults.
  RepeatedPtrField<std::string> field;
  std::string* first = field.Add();
  field.RemoveLast();

  field.Reserve(20);
  EXPECT_EQ(first, field.Add());
}

// Clearing elements is tricky with RepeatedPtrFields since the memory for
// the elements is retained and reused.
TEST(RepeatedPtrField, ClearedElements) {
  PROTOBUF_IGNORE_DEPRECATION_START
  RepeatedPtrField<std::string> field;

  std::string* original = field.Add();
  *original = "foo";

  EXPECT_EQ(field.ClearedCount(), 0);

  field.RemoveLast();
  EXPECT_TRUE(original->empty());
  EXPECT_EQ(field.ClearedCount(), 1);

  EXPECT_EQ(field.Add(),
            original);  // Should return same string for reuse.
  EXPECT_EQ(field.UnsafeArenaReleaseLast(), original);  // We take ownership.
  EXPECT_EQ(field.ClearedCount(), 0);

  EXPECT_NE(field.Add(), original);  // Should NOT return the same string.
  EXPECT_EQ(field.ClearedCount(), 0);

  field.UnsafeArenaAddAllocated(original);  // Give ownership back.
  EXPECT_EQ(field.ClearedCount(), 0);
  EXPECT_EQ(field.Mutable(1), original);

  field.Clear();
  EXPECT_EQ(field.ClearedCount(), 2);
#ifndef PROTOBUF_FUTURE_REMOVE_CLEARED_API
  EXPECT_EQ(field.ReleaseCleared(), original);  // Take ownership again.
  EXPECT_EQ(field.ClearedCount(), 1);
  EXPECT_NE(field.Add(), original);
  EXPECT_EQ(field.ClearedCount(), 0);
  EXPECT_NE(field.Add(), original);
  EXPECT_EQ(field.ClearedCount(), 0);

  field.AddCleared(original);  // Give ownership back, but as a cleared object.
  EXPECT_EQ(field.ClearedCount(), 1);
  EXPECT_EQ(field.Add(), original);
  EXPECT_EQ(field.ClearedCount(), 0);
#endif  // !PROTOBUF_FUTURE_REMOVE_CLEARED_API
  PROTOBUF_IGNORE_DEPRECATION_STOP
}

// Test all code paths in AddAllocated().
TEST(RepeatedPtrField, AddAllocated) {
  RepeatedPtrField<std::string> field;
  while (field.size() < field.Capacity()) {
    field.Add()->assign("filler");
  }

  const auto ensure_at_capacity = [&] {
    while (field.size() < field.Capacity()) {
      field.Add()->assign("filler");
    }
  };
  const auto ensure_not_at_capacity = [&] { field.Reserve(field.size() + 1); };

  ensure_at_capacity();
  int index = field.size();

  // First branch:  Field is at capacity with no cleared objects.
  ASSERT_EQ(field.size(), field.Capacity());
  std::string* foo = new std::string("foo");
  field.AddAllocated(foo);
  EXPECT_EQ(index + 1, field.size());
  EXPECT_EQ(0, field.ClearedCount());
  EXPECT_EQ(foo, &field.Get(index));

  // Last branch:  Field is not at capacity and there are no cleared objects.
  ensure_not_at_capacity();
  std::string* bar = new std::string("bar");
  field.AddAllocated(bar);
  ++index;
  EXPECT_EQ(index + 1, field.size());
  EXPECT_EQ(0, field.ClearedCount());
  EXPECT_EQ(bar, &field.Get(index));

  // Third branch:  Field is not at capacity and there are no cleared objects.
  ensure_not_at_capacity();
  field.RemoveLast();
  std::string* baz = new std::string("baz");
  field.AddAllocated(baz);
  EXPECT_EQ(index + 1, field.size());
  EXPECT_EQ(1, field.ClearedCount());
  EXPECT_EQ(baz, &field.Get(index));

  // Second branch:  Field is at capacity but has some cleared objects.
  ensure_at_capacity();
  field.RemoveLast();
  index = field.size();
  std::string* moo = new std::string("moo");
  field.AddAllocated(moo);
  EXPECT_EQ(index + 1, field.size());
  // We should have discarded the cleared object.
  EXPECT_EQ(0, field.ClearedCount());
  EXPECT_EQ(moo, &field.Get(index));
}

TEST(RepeatedPtrField, AddAllocatedDifferentArena) {
  RepeatedPtrField<TestAllTypes> field;
  Arena arena;
  auto* msg = Arena::CreateMessage<TestAllTypes>(&arena);
  field.AddAllocated(msg);
}

TEST(RepeatedPtrField, MergeFrom) {
  RepeatedPtrField<std::string> source, destination;
  source.Add()->assign("4");
  source.Add()->assign("5");
  destination.Add()->assign("1");
  destination.Add()->assign("2");
  destination.Add()->assign("3");

  destination.MergeFrom(source);

  ASSERT_EQ(5, destination.size());
  EXPECT_EQ("1", destination.Get(0));
  EXPECT_EQ("2", destination.Get(1));
  EXPECT_EQ("3", destination.Get(2));
  EXPECT_EQ("4", destination.Get(3));
  EXPECT_EQ("5", destination.Get(4));
}


TEST(RepeatedPtrField, CopyFrom) {
  RepeatedPtrField<std::string> source, destination;
  source.Add()->assign("4");
  source.Add()->assign("5");
  destination.Add()->assign("1");
  destination.Add()->assign("2");
  destination.Add()->assign("3");

  destination.CopyFrom(source);

  ASSERT_EQ(2, destination.size());
  EXPECT_EQ("4", destination.Get(0));
  EXPECT_EQ("5", destination.Get(1));
}

TEST(RepeatedPtrField, CopyFromSelf) {
  RepeatedPtrField<std::string> me;
  me.Add()->assign("1");
  me.CopyFrom(me);
  ASSERT_EQ(1, me.size());
  EXPECT_EQ("1", me.Get(0));
}

TEST(RepeatedPtrField, Erase) {
  RepeatedPtrField<std::string> me;
  RepeatedPtrField<std::string>::iterator it = me.erase(me.begin(), me.end());
  EXPECT_TRUE(me.begin() == it);
  EXPECT_EQ(0, me.size());

  *me.Add() = "1";
  *me.Add() = "2";
  *me.Add() = "3";
  it = me.erase(me.begin(), me.end());
  EXPECT_TRUE(me.begin() == it);
  EXPECT_EQ(0, me.size());

  *me.Add() = "4";
  *me.Add() = "5";
  *me.Add() = "6";
  it = me.erase(me.begin() + 2, me.end());
  EXPECT_TRUE(me.begin() + 2 == it);
  EXPECT_EQ(2, me.size());
  EXPECT_EQ("4", me.Get(0));
  EXPECT_EQ("5", me.Get(1));

  *me.Add() = "6";
  *me.Add() = "7";
  *me.Add() = "8";
  it = me.erase(me.begin() + 1, me.begin() + 3);
  EXPECT_TRUE(me.begin() + 1 == it);
  EXPECT_EQ(3, me.size());
  EXPECT_EQ("4", me.Get(0));
  EXPECT_EQ("7", me.Get(1));
  EXPECT_EQ("8", me.Get(2));
}

TEST(RepeatedPtrField, CopyConstruct) {
  auto token = internal::InternalVisibilityForTesting{};
  RepeatedPtrField<std::string> source;
  source.Add()->assign("1");
  source.Add()->assign("2");

  RepeatedPtrField<std::string> destination1(source);
  ASSERT_EQ(2, destination1.size());
  EXPECT_EQ("1", destination1.Get(0));
  EXPECT_EQ("2", destination1.Get(1));

  RepeatedPtrField<std::string> destination2(token, nullptr, source);
  ASSERT_EQ(2, destination2.size());
  EXPECT_EQ("1", destination2.Get(0));
  EXPECT_EQ("2", destination2.Get(1));
}

TEST(RepeatedPtrField, CopyConstructWithArena) {
  auto token = internal::InternalVisibilityForTesting{};
  RepeatedPtrField<std::string> source;
  source.Add()->assign("1");
  source.Add()->assign("2");

  Arena arena;
  RepeatedPtrField<std::string> destination(token, &arena, source);
  ASSERT_EQ(2, destination.size());
  EXPECT_EQ("1", destination.Get(0));
  EXPECT_EQ("2", destination.Get(1));
}

TEST(RepeatedPtrField, IteratorConstruct_String) {
  std::vector<std::string> values;
  values.push_back("1");
  values.push_back("2");

  RepeatedPtrField<std::string> field(values.begin(), values.end());
  ASSERT_EQ(values.size(), field.size());
  EXPECT_EQ(values[0], field.Get(0));
  EXPECT_EQ(values[1], field.Get(1));

  RepeatedPtrField<std::string> other(field.begin(), field.end());
  ASSERT_EQ(values.size(), other.size());
  EXPECT_EQ(values[0], other.Get(0));
  EXPECT_EQ(values[1], other.Get(1));
}

TEST(RepeatedPtrField, IteratorConstruct_Proto) {
  typedef TestAllTypes::NestedMessage Nested;
  std::vector<Nested> values;
  values.push_back(Nested());
  values.back().set_bb(1);
  values.push_back(Nested());
  values.back().set_bb(2);

  RepeatedPtrField<Nested> field(values.begin(), values.end());
  ASSERT_EQ(values.size(), field.size());
  EXPECT_EQ(values[0].bb(), field.Get(0).bb());
  EXPECT_EQ(values[1].bb(), field.Get(1).bb());

  RepeatedPtrField<Nested> other(field.begin(), field.end());
  ASSERT_EQ(values.size(), other.size());
  EXPECT_EQ(values[0].bb(), other.Get(0).bb());
  EXPECT_EQ(values[1].bb(), other.Get(1).bb());
}

TEST(RepeatedPtrField, SmallOptimization) {
  // Properties checked here are not part of the contract of RepeatedPtrField,
  // but we test them to verify that SSO is working as expected by the
  // implementation.

  // We use an arena to easily measure memory usage, but not needed.
  Arena arena;
  auto* array = Arena::CreateMessage<RepeatedPtrField<std::string>>(&arena);
  EXPECT_EQ(array->Capacity(), 1);
  EXPECT_EQ(array->SpaceUsedExcludingSelf(), 0);
  std::string str;
  auto usage_before = arena.SpaceUsed();
  // We use UnsafeArenaAddAllocated just to grow the array without creating
  // objects or causing extra cleanup costs in the arena to make the
  // measurements simpler.
  array->UnsafeArenaAddAllocated(&str);
  // No backing array, just the string.
  EXPECT_EQ(array->SpaceUsedExcludingSelf(), sizeof(str));
  // We have not used any arena space.
  EXPECT_EQ(usage_before, arena.SpaceUsed());
  // Verify the string is where we think it is.
  EXPECT_EQ(&*array->begin(), &str);
  EXPECT_EQ(array->pointer_begin()[0], &str);
  // The T** in pointer_begin points into the sso in the object.
  EXPECT_TRUE(std::less_equal<void*>{}(array, &*array->pointer_begin()));
  EXPECT_TRUE(std::less_equal<void*>{}(&*array->pointer_begin(), array + 1));

  // Adding a second object stops sso.
  std::string str2;
  array->UnsafeArenaAddAllocated(&str2);
  EXPECT_EQ(array->Capacity(), 3);
  // Backing array and the strings.
  EXPECT_EQ(array->SpaceUsedExcludingSelf(),
            (1 + array->Capacity()) * sizeof(void*) + 2 * sizeof(str));
  // We used some arena space now.
  EXPECT_LT(usage_before, arena.SpaceUsed());
  // And the pointer_begin is not in the sso anymore.
  EXPECT_FALSE(std::less_equal<void*>{}(array, &*array->pointer_begin()) &&
               std::less_equal<void*>{}(&*array->pointer_begin(), array + 1));
}

TEST(RepeatedPtrField, CopyAssign) {
  RepeatedPtrField<std::string> source, destination;
  source.Add()->assign("4");
  source.Add()->assign("5");
  destination.Add()->assign("1");
  destination.Add()->assign("2");
  destination.Add()->assign("3");

  destination = source;

  ASSERT_EQ(2, destination.size());
  EXPECT_EQ("4", destination.Get(0));
  EXPECT_EQ("5", destination.Get(1));
}

TEST(RepeatedPtrField, SelfAssign) {
  // Verify that assignment to self does not destroy data.
  RepeatedPtrField<std::string> source, *p;
  p = &source;
  source.Add()->assign("7");
  source.Add()->assign("8");

  *p = source;

  ASSERT_EQ(2, source.size());
  EXPECT_EQ("7", source.Get(0));
  EXPECT_EQ("8", source.Get(1));
}

TEST(RepeatedPtrField, MoveConstruct) {
  {
    RepeatedPtrField<std::string> source;
    *source.Add() = "1";
    *source.Add() = "2";
    const std::string* const* data = source.data();
    RepeatedPtrField<std::string> destination = std::move(source);
    EXPECT_EQ(data, destination.data());
    EXPECT_THAT(destination, ElementsAre("1", "2"));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_TRUE(source.empty());
  }
  {
    Arena arena;
    RepeatedPtrField<std::string>* source =
        Arena::CreateMessage<RepeatedPtrField<std::string>>(&arena);
    *source->Add() = "1";
    *source->Add() = "2";
    RepeatedPtrField<std::string> destination = std::move(*source);
    EXPECT_EQ(nullptr, destination.GetArena());
    EXPECT_THAT(destination, ElementsAre("1", "2"));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_THAT(*source, ElementsAre("1", "2"));
  }
}

TEST(RepeatedPtrField, MoveAssign) {
  {
    RepeatedPtrField<std::string> source;
    *source.Add() = "1";
    *source.Add() = "2";
    RepeatedPtrField<std::string> destination;
    *destination.Add() = "3";
    const std::string* const* source_data = source.data();
    destination = std::move(source);
    EXPECT_EQ(source_data, destination.data());
    EXPECT_THAT(destination, ElementsAre("1", "2"));
    EXPECT_THAT(source, ElementsAre("3"));
  }
  {
    Arena arena;
    RepeatedPtrField<std::string>* source =
        Arena::CreateMessage<RepeatedPtrField<std::string>>(&arena);
    *source->Add() = "1";
    *source->Add() = "2";
    RepeatedPtrField<std::string>* destination =
        Arena::CreateMessage<RepeatedPtrField<std::string>>(&arena);
    *destination->Add() = "3";
    const std::string* const* source_data = source->data();
    *destination = std::move(*source);
    EXPECT_EQ(source_data, destination->data());
    EXPECT_THAT(*destination, ElementsAre("1", "2"));
    EXPECT_THAT(*source, ElementsAre("3"));
  }
  {
    Arena source_arena;
    RepeatedPtrField<std::string>* source =
        Arena::CreateMessage<RepeatedPtrField<std::string>>(&source_arena);
    *source->Add() = "1";
    *source->Add() = "2";
    Arena destination_arena;
    RepeatedPtrField<std::string>* destination =
        Arena::CreateMessage<RepeatedPtrField<std::string>>(&destination_arena);
    *destination->Add() = "3";
    *destination = std::move(*source);
    EXPECT_THAT(*destination, ElementsAre("1", "2"));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_THAT(*source, ElementsAre("1", "2"));
  }
  {
    Arena arena;
    RepeatedPtrField<std::string>* source =
        Arena::CreateMessage<RepeatedPtrField<std::string>>(&arena);
    *source->Add() = "1";
    *source->Add() = "2";
    RepeatedPtrField<std::string> destination;
    *destination.Add() = "3";
    destination = std::move(*source);
    EXPECT_THAT(destination, ElementsAre("1", "2"));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_THAT(*source, ElementsAre("1", "2"));
  }
  {
    RepeatedPtrField<std::string> source;
    *source.Add() = "1";
    *source.Add() = "2";
    Arena arena;
    RepeatedPtrField<std::string>* destination =
        Arena::CreateMessage<RepeatedPtrField<std::string>>(&arena);
    *destination->Add() = "3";
    *destination = std::move(source);
    EXPECT_THAT(*destination, ElementsAre("1", "2"));
    // This property isn't guaranteed but it's useful to have a test that would
    // catch changes in this area.
    EXPECT_THAT(source, ElementsAre("1", "2"));
  }
  {
    RepeatedPtrField<std::string> field;
    // An alias to defeat -Wself-move.
    RepeatedPtrField<std::string>& alias = field;
    *field.Add() = "1";
    *field.Add() = "2";
    const std::string* const* data = field.data();
    field = std::move(alias);
    EXPECT_EQ(data, field.data());
    EXPECT_THAT(field, ElementsAre("1", "2"));
  }
  {
    Arena arena;
    RepeatedPtrField<std::string>* field =
        Arena::CreateMessage<RepeatedPtrField<std::string>>(&arena);
    *field->Add() = "1";
    *field->Add() = "2";
    const std::string* const* data = field->data();
    *field = std::move(*field);
    EXPECT_EQ(data, field->data());
    EXPECT_THAT(*field, ElementsAre("1", "2"));
  }
}

TEST(RepeatedPtrField, MutableDataIsMutable) {
  RepeatedPtrField<std::string> field;
  *field.Add() = "1";
  EXPECT_EQ("1", field.Get(0));
  // The fact that this line compiles would be enough, but we'll check the
  // value anyway.
  std::string** data = field.mutable_data();
  **data = "2";
  EXPECT_EQ("2", field.Get(0));
}

TEST(RepeatedPtrField, SubscriptOperators) {
  RepeatedPtrField<std::string> field;
  *field.Add() = "1";
  EXPECT_EQ("1", field.Get(0));
  EXPECT_EQ("1", field[0]);
  EXPECT_EQ(field.Mutable(0), &field[0]);
  const RepeatedPtrField<std::string>& const_field = field;
  EXPECT_EQ(*field.data(), &const_field[0]);
}

TEST(RepeatedPtrField, ExtractSubrange) {
  // Exhaustively test every subrange in arrays of all sizes from 0 through 9
  // with 0 through 3 cleared elements at the end.
  for (int sz = 0; sz < 10; ++sz) {
    for (int num = 0; num <= sz; ++num) {
      for (int start = 0; start < sz - num; ++start) {
        for (int extra = 0; extra < 4; ++extra) {
          std::vector<std::string*> subject;

          // Create an array with "sz" elements and "extra" cleared elements.
          // Use an arena to avoid copies from debug-build stability checks.
          Arena arena;
          auto& field =
              *Arena::CreateMessage<RepeatedPtrField<std::string>>(&arena);
          for (int i = 0; i < sz + extra; ++i) {
            subject.push_back(new std::string());
            field.AddAllocated(subject[i]);
          }
          EXPECT_EQ(field.size(), sz + extra);
          for (int i = 0; i < extra; ++i) field.RemoveLast();
          EXPECT_EQ(field.size(), sz);
          EXPECT_EQ(field.ClearedCount(), extra);

          // Create a catcher array and call ExtractSubrange.
          std::string* catcher[10];
          for (int i = 0; i < 10; ++i) catcher[i] = nullptr;
          field.ExtractSubrange(start, num, catcher);

          // Does the resulting array have the right size?
          EXPECT_EQ(field.size(), sz - num);

          // Were the removed elements extracted into the catcher array?
          for (int i = 0; i < num; ++i)
            EXPECT_EQ(*catcher[i], *subject[start + i]);
          EXPECT_EQ(nullptr, catcher[num]);

          // Does the resulting array contain the right values?
          for (int i = 0; i < start; ++i)
            EXPECT_EQ(field.Mutable(i), subject[i]);
          for (int i = start; i < field.size(); ++i)
            EXPECT_EQ(field.Mutable(i), subject[i + num]);

          // Reinstate the cleared elements.
          EXPECT_EQ(field.ClearedCount(), extra);
          for (int i = 0; i < extra; ++i) field.Add();
          EXPECT_EQ(field.ClearedCount(), 0);
          EXPECT_EQ(field.size(), sz - num + extra);

          // Make sure the extra elements are all there (in some order).
          for (int i = sz; i < sz + extra; ++i) {
            int count = 0;
            for (int j = sz; j < sz + extra; ++j) {
              if (field.Mutable(j - num) == subject[i]) count += 1;
            }
            EXPECT_EQ(count, 1);
          }

          // Release the caught elements.
          for (int i = 0; i < num; ++i) delete catcher[i];
        }
      }
    }
  }
}

TEST(RepeatedPtrField, DeleteSubrange) {
  // DeleteSubrange is a trivial extension of ExtendSubrange.
}

TEST(RepeatedPtrField, Cleanups) {
  Arena arena;
  auto growth = internal::CleanupGrowth(arena, [&] {
    Arena::CreateMessage<RepeatedPtrField<std::string>>(&arena);
  });
  EXPECT_THAT(growth.cleanups, testing::IsEmpty());

  growth = internal::CleanupGrowth(arena, [&] {
    Arena::CreateMessage<RepeatedPtrField<TestAllTypes>>(&arena);
  });
  EXPECT_THAT(growth.cleanups, testing::IsEmpty());
}


// ===================================================================

// Iterator tests stolen from net/proto/proto-array_unittest.
class RepeatedFieldIteratorTest : public testing::Test {
 protected:
  void SetUp() override {
    for (int i = 0; i < 3; ++i) {
      proto_array_.Add(i);
    }
  }

  RepeatedField<int> proto_array_;
};

TEST_F(RepeatedFieldIteratorTest, Convertible) {
  RepeatedField<int>::iterator iter = proto_array_.begin();
  RepeatedField<int>::const_iterator c_iter = iter;
  RepeatedField<int>::value_type value = *c_iter;
  EXPECT_EQ(0, value);
}

TEST_F(RepeatedFieldIteratorTest, MutableIteration) {
  RepeatedField<int>::iterator iter = proto_array_.begin();
  EXPECT_EQ(0, *iter);
  ++iter;
  EXPECT_EQ(1, *iter++);
  EXPECT_EQ(2, *iter);
  ++iter;
  EXPECT_TRUE(proto_array_.end() == iter);

  EXPECT_EQ(2, *(proto_array_.end() - 1));
}

TEST_F(RepeatedFieldIteratorTest, ConstIteration) {
  const RepeatedField<int>& const_proto_array = proto_array_;
  RepeatedField<int>::const_iterator iter = const_proto_array.begin();
  EXPECT_EQ(0, *iter);
  ++iter;
  EXPECT_EQ(1, *iter++);
  EXPECT_EQ(2, *iter);
  ++iter;
  EXPECT_TRUE(const_proto_array.end() == iter);
  EXPECT_EQ(2, *(const_proto_array.end() - 1));
}

TEST_F(RepeatedFieldIteratorTest, Mutation) {
  RepeatedField<int>::iterator iter = proto_array_.begin();
  *iter = 7;
  EXPECT_EQ(7, proto_array_.Get(0));
}

// -------------------------------------------------------------------

class RepeatedPtrFieldIteratorTest : public testing::Test {
 protected:
  void SetUp() override {
    proto_array_.Add()->assign("foo");
    proto_array_.Add()->assign("bar");
    proto_array_.Add()->assign("baz");
  }

  RepeatedPtrField<std::string> proto_array_;
};

TEST_F(RepeatedPtrFieldIteratorTest, Convertible) {
  RepeatedPtrField<std::string>::iterator iter = proto_array_.begin();
  RepeatedPtrField<std::string>::const_iterator c_iter = iter;
  RepeatedPtrField<std::string>::value_type value = *c_iter;
  EXPECT_EQ("foo", value);
}

TEST_F(RepeatedPtrFieldIteratorTest, MutableIteration) {
  RepeatedPtrField<std::string>::iterator iter = proto_array_.begin();
  EXPECT_EQ("foo", *iter);
  ++iter;
  EXPECT_EQ("bar", *(iter++));
  EXPECT_EQ("baz", *iter);
  ++iter;
  EXPECT_TRUE(proto_array_.end() == iter);
  EXPECT_EQ("baz", *(--proto_array_.end()));
}

TEST_F(RepeatedPtrFieldIteratorTest, ConstIteration) {
  const RepeatedPtrField<std::string>& const_proto_array = proto_array_;
  RepeatedPtrField<std::string>::const_iterator iter =
      const_proto_array.begin();
  iter - const_proto_array.cbegin();
  EXPECT_EQ("foo", *iter);
  ++iter;
  EXPECT_EQ("bar", *(iter++));
  EXPECT_EQ("baz", *iter);
  ++iter;
  EXPECT_TRUE(const_proto_array.end() == iter);
  EXPECT_EQ("baz", *(--const_proto_array.end()));
}

TEST_F(RepeatedPtrFieldIteratorTest, MutableReverseIteration) {
  RepeatedPtrField<std::string>::reverse_iterator iter = proto_array_.rbegin();
  EXPECT_EQ("baz", *iter);
  ++iter;
  EXPECT_EQ("bar", *(iter++));
  EXPECT_EQ("foo", *iter);
  ++iter;
  EXPECT_TRUE(proto_array_.rend() == iter);
  EXPECT_EQ("foo", *(--proto_array_.rend()));
}

TEST_F(RepeatedPtrFieldIteratorTest, ConstReverseIteration) {
  const RepeatedPtrField<std::string>& const_proto_array = proto_array_;
  RepeatedPtrField<std::string>::const_reverse_iterator iter =
      const_proto_array.rbegin();
  EXPECT_EQ("baz", *iter);
  ++iter;
  EXPECT_EQ("bar", *(iter++));
  EXPECT_EQ("foo", *iter);
  ++iter;
  EXPECT_TRUE(const_proto_array.rend() == iter);
  EXPECT_EQ("foo", *(--const_proto_array.rend()));
}

TEST_F(RepeatedPtrFieldIteratorTest, RandomAccess) {
  RepeatedPtrField<std::string>::iterator iter = proto_array_.begin();
  RepeatedPtrField<std::string>::iterator iter2 = iter;
  ++iter2;
  ++iter2;
  EXPECT_TRUE(iter + 2 == iter2);
  EXPECT_TRUE(iter == iter2 - 2);
  EXPECT_EQ("baz", iter[2]);
  EXPECT_EQ("baz", *(iter + 2));
  EXPECT_EQ(3, proto_array_.end() - proto_array_.begin());
}

TEST_F(RepeatedPtrFieldIteratorTest, RandomAccessConst) {
  RepeatedPtrField<std::string>::const_iterator iter = proto_array_.cbegin();
  RepeatedPtrField<std::string>::const_iterator iter2 = iter;
  ++iter2;
  ++iter2;
  EXPECT_TRUE(iter + 2 == iter2);
  EXPECT_TRUE(iter == iter2 - 2);
  EXPECT_EQ("baz", iter[2]);
  EXPECT_EQ("baz", *(iter + 2));
  EXPECT_EQ(3, proto_array_.cend() - proto_array_.cbegin());
}

TEST_F(RepeatedPtrFieldIteratorTest, DifferenceConstConversion) {
  EXPECT_EQ(3, proto_array_.end() - proto_array_.cbegin());
  EXPECT_EQ(3, proto_array_.cend() - proto_array_.begin());
}

TEST_F(RepeatedPtrFieldIteratorTest, Comparable) {
  RepeatedPtrField<std::string>::const_iterator iter = proto_array_.begin();
  RepeatedPtrField<std::string>::const_iterator iter2 = iter + 1;
  EXPECT_TRUE(iter == iter);
  EXPECT_TRUE(iter != iter2);
  EXPECT_TRUE(iter < iter2);
  EXPECT_TRUE(iter <= iter2);
  EXPECT_TRUE(iter <= iter);
  EXPECT_TRUE(iter2 > iter);
  EXPECT_TRUE(iter2 >= iter);
  EXPECT_TRUE(iter >= iter);
}

TEST_F(RepeatedPtrFieldIteratorTest, ComparableConstConversion) {
  RepeatedPtrField<std::string>::iterator iter = proto_array_.begin();
  RepeatedPtrField<std::string>::const_iterator iter2 = iter + 1;
  EXPECT_TRUE(iter == iter);
  EXPECT_TRUE(iter == proto_array_.cbegin());
  EXPECT_TRUE(proto_array_.cbegin() == iter);
  EXPECT_TRUE(iter != iter2);
  EXPECT_TRUE(iter2 != iter);
  EXPECT_TRUE(iter < iter2);
  EXPECT_TRUE(iter <= iter2);
  EXPECT_TRUE(iter <= iter);
  EXPECT_TRUE(iter2 > iter);
  EXPECT_TRUE(iter2 >= iter);
  EXPECT_TRUE(iter >= iter);
}

// Uninitialized iterator does not point to any of the RepeatedPtrField.
TEST_F(RepeatedPtrFieldIteratorTest, UninitializedIterator) {
  RepeatedPtrField<std::string>::iterator iter;
  EXPECT_TRUE(iter != proto_array_.begin());
  EXPECT_TRUE(iter != proto_array_.begin() + 1);
  EXPECT_TRUE(iter != proto_array_.begin() + 2);
  EXPECT_TRUE(iter != proto_array_.begin() + 3);
  EXPECT_TRUE(iter != proto_array_.end());
}

TEST_F(RepeatedPtrFieldIteratorTest, STLAlgorithms_lower_bound) {
  proto_array_.Clear();
  proto_array_.Add()->assign("a");
  proto_array_.Add()->assign("c");
  proto_array_.Add()->assign("d");
  proto_array_.Add()->assign("n");
  proto_array_.Add()->assign("p");
  proto_array_.Add()->assign("x");
  proto_array_.Add()->assign("y");

  std::string v = "f";
  RepeatedPtrField<std::string>::const_iterator it =
      std::lower_bound(proto_array_.begin(), proto_array_.end(), v);

  EXPECT_EQ(*it, "n");
  EXPECT_TRUE(it == proto_array_.begin() + 3);
}

TEST_F(RepeatedPtrFieldIteratorTest, Mutation) {
  RepeatedPtrField<std::string>::iterator iter = proto_array_.begin();
  *iter = "moo";
  EXPECT_EQ("moo", proto_array_.Get(0));
}

// -------------------------------------------------------------------

class RepeatedPtrFieldPtrsIteratorTest : public testing::Test {
 protected:
  void SetUp() override {
    proto_array_.Add()->assign("foo");
    proto_array_.Add()->assign("bar");
    proto_array_.Add()->assign("baz");
    const_proto_array_ = &proto_array_;
  }

  RepeatedPtrField<std::string> proto_array_;
  const RepeatedPtrField<std::string>* const_proto_array_;
};

TEST_F(RepeatedPtrFieldPtrsIteratorTest, ConvertiblePtr) {
  RepeatedPtrField<std::string>::pointer_iterator iter =
      proto_array_.pointer_begin();
  static_cast<void>(iter);
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, ConvertibleConstPtr) {
  RepeatedPtrField<std::string>::const_pointer_iterator iter =
      const_proto_array_->pointer_begin();
  static_cast<void>(iter);
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, MutablePtrIteration) {
  RepeatedPtrField<std::string>::pointer_iterator iter =
      proto_array_.pointer_begin();
  EXPECT_EQ("foo", **iter);
  ++iter;
  EXPECT_EQ("bar", **(iter++));
  EXPECT_EQ("baz", **iter);
  ++iter;
  EXPECT_TRUE(proto_array_.pointer_end() == iter);
  EXPECT_EQ("baz", **(--proto_array_.pointer_end()));
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, MutableConstPtrIteration) {
  RepeatedPtrField<std::string>::const_pointer_iterator iter =
      const_proto_array_->pointer_begin();
  EXPECT_EQ("foo", **iter);
  ++iter;
  EXPECT_EQ("bar", **(iter++));
  EXPECT_EQ("baz", **iter);
  ++iter;
  EXPECT_TRUE(const_proto_array_->pointer_end() == iter);
  EXPECT_EQ("baz", **(--const_proto_array_->pointer_end()));
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, RandomPtrAccess) {
  RepeatedPtrField<std::string>::pointer_iterator iter =
      proto_array_.pointer_begin();
  RepeatedPtrField<std::string>::pointer_iterator iter2 = iter;
  ++iter2;
  ++iter2;
  EXPECT_TRUE(iter + 2 == iter2);
  EXPECT_TRUE(iter == iter2 - 2);
  EXPECT_EQ("baz", *iter[2]);
  EXPECT_EQ("baz", **(iter + 2));
  EXPECT_EQ(3, proto_array_.end() - proto_array_.begin());
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, RandomConstPtrAccess) {
  RepeatedPtrField<std::string>::const_pointer_iterator iter =
      const_proto_array_->pointer_begin();
  RepeatedPtrField<std::string>::const_pointer_iterator iter2 = iter;
  ++iter2;
  ++iter2;
  EXPECT_TRUE(iter + 2 == iter2);
  EXPECT_TRUE(iter == iter2 - 2);
  EXPECT_EQ("baz", *iter[2]);
  EXPECT_EQ("baz", **(iter + 2));
  EXPECT_EQ(3, const_proto_array_->end() - const_proto_array_->begin());
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, DifferenceConstConversion) {
  EXPECT_EQ(3,
            proto_array_.pointer_end() - const_proto_array_->pointer_begin());
  EXPECT_EQ(3,
            const_proto_array_->pointer_end() - proto_array_.pointer_begin());
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, ComparablePtr) {
  RepeatedPtrField<std::string>::pointer_iterator iter =
      proto_array_.pointer_begin();
  RepeatedPtrField<std::string>::pointer_iterator iter2 = iter + 1;
  EXPECT_TRUE(iter == iter);
  EXPECT_TRUE(iter != iter2);
  EXPECT_TRUE(iter < iter2);
  EXPECT_TRUE(iter <= iter2);
  EXPECT_TRUE(iter <= iter);
  EXPECT_TRUE(iter2 > iter);
  EXPECT_TRUE(iter2 >= iter);
  EXPECT_TRUE(iter >= iter);
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, ComparableConstPtr) {
  RepeatedPtrField<std::string>::const_pointer_iterator iter =
      const_proto_array_->pointer_begin();
  RepeatedPtrField<std::string>::const_pointer_iterator iter2 = iter + 1;
  EXPECT_TRUE(iter == iter);
  EXPECT_TRUE(iter != iter2);
  EXPECT_TRUE(iter < iter2);
  EXPECT_TRUE(iter <= iter2);
  EXPECT_TRUE(iter <= iter);
  EXPECT_TRUE(iter2 > iter);
  EXPECT_TRUE(iter2 >= iter);
  EXPECT_TRUE(iter >= iter);
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, ComparableConstConversion) {
  RepeatedPtrField<std::string>::pointer_iterator iter =
      proto_array_.pointer_begin();
  RepeatedPtrField<std::string>::const_pointer_iterator iter2 = iter + 1;
  EXPECT_TRUE(iter == iter);
  EXPECT_TRUE(iter == const_proto_array_->pointer_begin());
  EXPECT_TRUE(const_proto_array_->pointer_begin() == iter);
  EXPECT_TRUE(iter != iter2);
  EXPECT_TRUE(iter2 != iter);
  EXPECT_TRUE(iter < iter2);
  EXPECT_TRUE(iter <= iter2);
  EXPECT_TRUE(iter <= iter);
  EXPECT_TRUE(iter2 > iter);
  EXPECT_TRUE(iter2 >= iter);
  EXPECT_TRUE(iter >= iter);
}

// Uninitialized iterator does not point to any of the RepeatedPtrOverPtrs.
// Dereferencing an uninitialized iterator crashes the process.
TEST_F(RepeatedPtrFieldPtrsIteratorTest, UninitializedPtrIterator) {
  RepeatedPtrField<std::string>::pointer_iterator iter;
  EXPECT_TRUE(iter != proto_array_.pointer_begin());
  EXPECT_TRUE(iter != proto_array_.pointer_begin() + 1);
  EXPECT_TRUE(iter != proto_array_.pointer_begin() + 2);
  EXPECT_TRUE(iter != proto_array_.pointer_begin() + 3);
  EXPECT_TRUE(iter != proto_array_.pointer_end());
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, UninitializedConstPtrIterator) {
  RepeatedPtrField<std::string>::const_pointer_iterator iter;
  EXPECT_TRUE(iter != const_proto_array_->pointer_begin());
  EXPECT_TRUE(iter != const_proto_array_->pointer_begin() + 1);
  EXPECT_TRUE(iter != const_proto_array_->pointer_begin() + 2);
  EXPECT_TRUE(iter != const_proto_array_->pointer_begin() + 3);
  EXPECT_TRUE(iter != const_proto_array_->pointer_end());
}

// This comparison functor is required by the tests for RepeatedPtrOverPtrs.
// They operate on strings and need to compare strings as strings in
// any stl algorithm, even though the iterator returns a pointer to a
// string
// - i.e. *iter has type std::string*.
struct StringLessThan {
  bool operator()(const std::string* z, const std::string* y) const {
    return *z < *y;
  }
};

TEST_F(RepeatedPtrFieldPtrsIteratorTest, PtrSTLAlgorithms_lower_bound) {
  proto_array_.Clear();
  proto_array_.Add()->assign("a");
  proto_array_.Add()->assign("c");
  proto_array_.Add()->assign("d");
  proto_array_.Add()->assign("n");
  proto_array_.Add()->assign("p");
  proto_array_.Add()->assign("x");
  proto_array_.Add()->assign("y");

  {
    std::string v = "f";
    RepeatedPtrField<std::string>::pointer_iterator it =
        std::lower_bound(proto_array_.pointer_begin(),
                         proto_array_.pointer_end(), &v, StringLessThan());

    ABSL_CHECK(*it != nullptr);

    EXPECT_EQ(**it, "n");
    EXPECT_TRUE(it == proto_array_.pointer_begin() + 3);
  }
  {
    std::string v = "f";
    RepeatedPtrField<std::string>::const_pointer_iterator it = std::lower_bound(
        const_proto_array_->pointer_begin(), const_proto_array_->pointer_end(),
        &v, StringLessThan());

    ABSL_CHECK(*it != nullptr);

    EXPECT_EQ(**it, "n");
    EXPECT_TRUE(it == const_proto_array_->pointer_begin() + 3);
  }
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, PtrMutation) {
  RepeatedPtrField<std::string>::pointer_iterator iter =
      proto_array_.pointer_begin();
  **iter = "moo";
  EXPECT_EQ("moo", proto_array_.Get(0));

  EXPECT_EQ("bar", proto_array_.Get(1));
  EXPECT_EQ("baz", proto_array_.Get(2));
  ++iter;
  delete *iter;
  *iter = new std::string("a");
  ++iter;
  delete *iter;
  *iter = new std::string("b");
  EXPECT_EQ("a", proto_array_.Get(1));
  EXPECT_EQ("b", proto_array_.Get(2));
}

TEST_F(RepeatedPtrFieldPtrsIteratorTest, Sort) {
  proto_array_.Add()->assign("c");
  proto_array_.Add()->assign("d");
  proto_array_.Add()->assign("n");
  proto_array_.Add()->assign("p");
  proto_array_.Add()->assign("a");
  proto_array_.Add()->assign("y");
  proto_array_.Add()->assign("x");
  EXPECT_EQ("foo", proto_array_.Get(0));
  EXPECT_EQ("n", proto_array_.Get(5));
  EXPECT_EQ("x", proto_array_.Get(9));
  std::sort(proto_array_.pointer_begin(), proto_array_.pointer_end(),
            StringLessThan());
  EXPECT_EQ("a", proto_array_.Get(0));
  EXPECT_EQ("baz", proto_array_.Get(2));
  EXPECT_EQ("y", proto_array_.Get(9));
}

// -----------------------------------------------------------------------------
// Unit-tests for the insert iterators
// google::protobuf::RepeatedFieldBackInserter,
// google::protobuf::AllocatedRepeatedPtrFieldBackInserter
// Ported from util/gtl/proto-array-iterators_unittest.

class RepeatedFieldInsertionIteratorsTest : public testing::Test {
 protected:
  std::list<double> halves;
  std::list<int> fibonacci;
  std::vector<std::string> words;
  typedef TestAllTypes::NestedMessage Nested;
  Nested nesteds[2];
  std::vector<Nested*> nested_ptrs;
  TestAllTypes protobuffer;

  void SetUp() override {
    fibonacci.push_back(1);
    fibonacci.push_back(1);
    fibonacci.push_back(2);
    fibonacci.push_back(3);
    fibonacci.push_back(5);
    fibonacci.push_back(8);
    std::copy(fibonacci.begin(), fibonacci.end(),
              RepeatedFieldBackInserter(protobuffer.mutable_repeated_int32()));

    halves.push_back(1.0);
    halves.push_back(0.5);
    halves.push_back(0.25);
    halves.push_back(0.125);
    halves.push_back(0.0625);
    std::copy(halves.begin(), halves.end(),
              RepeatedFieldBackInserter(protobuffer.mutable_repeated_double()));

    words.push_back("Able");
    words.push_back("was");
    words.push_back("I");
    words.push_back("ere");
    words.push_back("I");
    words.push_back("saw");
    words.push_back("Elba");
    std::copy(words.begin(), words.end(),
              RepeatedFieldBackInserter(protobuffer.mutable_repeated_string()));

    nesteds[0].set_bb(17);
    nesteds[1].set_bb(4711);
    std::copy(&nesteds[0], &nesteds[2],
              RepeatedFieldBackInserter(
                  protobuffer.mutable_repeated_nested_message()));

    nested_ptrs.push_back(new Nested);
    nested_ptrs.back()->set_bb(170);
    nested_ptrs.push_back(new Nested);
    nested_ptrs.back()->set_bb(47110);
    std::copy(nested_ptrs.begin(), nested_ptrs.end(),
              RepeatedFieldBackInserter(
                  protobuffer.mutable_repeated_nested_message()));
  }

  void TearDown() override {
    for (auto ptr : nested_ptrs) {
      delete ptr;
    }
  }
};

TEST_F(RepeatedFieldInsertionIteratorsTest, Fibonacci) {
  EXPECT_TRUE(std::equal(fibonacci.begin(), fibonacci.end(),
                         protobuffer.repeated_int32().begin()));
  EXPECT_TRUE(std::equal(protobuffer.repeated_int32().begin(),
                         protobuffer.repeated_int32().end(),
                         fibonacci.begin()));
}

TEST_F(RepeatedFieldInsertionIteratorsTest, Halves) {
  EXPECT_TRUE(std::equal(halves.begin(), halves.end(),
                         protobuffer.repeated_double().begin()));
  EXPECT_TRUE(std::equal(protobuffer.repeated_double().begin(),
                         protobuffer.repeated_double().end(), halves.begin()));
}

TEST_F(RepeatedFieldInsertionIteratorsTest, Words) {
  ASSERT_EQ(words.size(), protobuffer.repeated_string_size());
  for (int i = 0; i < words.size(); ++i)
    EXPECT_EQ(words.at(i), protobuffer.repeated_string(i));
}

TEST_F(RepeatedFieldInsertionIteratorsTest, Words2) {
  words.clear();
  words.push_back("sing");
  words.push_back("a");
  words.push_back("song");
  words.push_back("of");
  words.push_back("six");
  words.push_back("pence");
  protobuffer.mutable_repeated_string()->Clear();
  std::copy(
      words.begin(), words.end(),
      RepeatedPtrFieldBackInserter(protobuffer.mutable_repeated_string()));
  ASSERT_EQ(words.size(), protobuffer.repeated_string_size());
  for (int i = 0; i < words.size(); ++i)
    EXPECT_EQ(words.at(i), protobuffer.repeated_string(i));
}

TEST_F(RepeatedFieldInsertionIteratorsTest, Nesteds) {
  ASSERT_EQ(protobuffer.repeated_nested_message_size(), 4);
  EXPECT_EQ(protobuffer.repeated_nested_message(0).bb(), 17);
  EXPECT_EQ(protobuffer.repeated_nested_message(1).bb(), 4711);
  EXPECT_EQ(protobuffer.repeated_nested_message(2).bb(), 170);
  EXPECT_EQ(protobuffer.repeated_nested_message(3).bb(), 47110);
}

TEST_F(RepeatedFieldInsertionIteratorsTest,
       AllocatedRepeatedPtrFieldWithStringIntData) {
  std::vector<Nested*> data;
  TestAllTypes goldenproto;
  for (int i = 0; i < 10; ++i) {
    Nested* new_data = new Nested;
    new_data->set_bb(i);
    data.push_back(new_data);

    new_data = goldenproto.add_repeated_nested_message();
    new_data->set_bb(i);
  }
  TestAllTypes testproto;
  std::copy(data.begin(), data.end(),
            AllocatedRepeatedPtrFieldBackInserter(
                testproto.mutable_repeated_nested_message()));
  EXPECT_EQ(testproto.DebugString(), goldenproto.DebugString());
}

TEST_F(RepeatedFieldInsertionIteratorsTest,
       AllocatedRepeatedPtrFieldWithString) {
  std::vector<std::string*> data;
  TestAllTypes goldenproto;
  for (int i = 0; i < 10; ++i) {
    std::string* new_data = new std::string;
    *new_data = absl::StrCat("name-", i);
    data.push_back(new_data);

    new_data = goldenproto.add_repeated_string();
    *new_data = absl::StrCat("name-", i);
  }
  TestAllTypes testproto;
  std::copy(data.begin(), data.end(),
            AllocatedRepeatedPtrFieldBackInserter(
                testproto.mutable_repeated_string()));
  EXPECT_EQ(testproto.DebugString(), goldenproto.DebugString());
}

TEST_F(RepeatedFieldInsertionIteratorsTest,
       UnsafeArenaAllocatedRepeatedPtrFieldWithStringIntData) {
  std::vector<Nested*> data;
  Arena arena;
  auto* goldenproto = Arena::CreateMessage<TestAllTypes>(&arena);
  for (int i = 0; i < 10; ++i) {
    auto* new_data = goldenproto->add_repeated_nested_message();
    new_data->set_bb(i);
    data.push_back(new_data);
  }
  auto* testproto = Arena::CreateMessage<TestAllTypes>(&arena);
  std::copy(data.begin(), data.end(),
            UnsafeArenaAllocatedRepeatedPtrFieldBackInserter(
                testproto->mutable_repeated_nested_message()));
  EXPECT_EQ(testproto->DebugString(), goldenproto->DebugString());
}

TEST_F(RepeatedFieldInsertionIteratorsTest,
       UnsafeArenaAllocatedRepeatedPtrFieldWithString) {
  std::vector<std::string*> data;
  Arena arena;
  auto* goldenproto = Arena::CreateMessage<TestAllTypes>(&arena);
  for (int i = 0; i < 10; ++i) {
    auto* new_data = goldenproto->add_repeated_string();
    *new_data = absl::StrCat("name-", i);
    data.push_back(new_data);
  }
  auto* testproto = Arena::CreateMessage<TestAllTypes>(&arena);
  std::copy(data.begin(), data.end(),
            UnsafeArenaAllocatedRepeatedPtrFieldBackInserter(
                testproto->mutable_repeated_string()));
  EXPECT_EQ(testproto->DebugString(), goldenproto->DebugString());
}

TEST_F(RepeatedFieldInsertionIteratorsTest, MoveStrings) {
  std::vector<std::string> src = {"a", "b", "c", "d"};
  std::vector<std::string> copy =
      src;  // copy since move leaves in undefined state
  TestAllTypes testproto;
  std::move(copy.begin(), copy.end(),
            RepeatedFieldBackInserter(testproto.mutable_repeated_string()));

  ASSERT_THAT(testproto.repeated_string(), testing::ElementsAreArray(src));
}

TEST_F(RepeatedFieldInsertionIteratorsTest, MoveProtos) {
  auto make_nested = [](int32_t x) {
    Nested ret;
    ret.set_bb(x);
    return ret;
  };
  std::vector<Nested> src = {make_nested(3), make_nested(5), make_nested(7)};
  std::vector<Nested> copy = src;  // copy since move leaves in undefined state
  TestAllTypes testproto;
  std::move(
      copy.begin(), copy.end(),
      RepeatedFieldBackInserter(testproto.mutable_repeated_nested_message()));

  ASSERT_EQ(src.size(), testproto.repeated_nested_message_size());
  for (int i = 0; i < src.size(); ++i) {
    EXPECT_EQ(src[i].DebugString(),
              testproto.repeated_nested_message(i).DebugString());
  }
}

}  // namespace

}  // namespace protobuf
}  // namespace google

#include "google/protobuf/port_undef.inc"

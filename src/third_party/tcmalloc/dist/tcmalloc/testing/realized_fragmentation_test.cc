// Copyright 2022 The TCMalloc Authors
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

#include <new>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/random/random.h"
#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using tcmalloc_internal::SLL_Pop;
using tcmalloc_internal::SLL_Push;

// Since application data is sampled, allow wider error bars.
constexpr double kBackingTolerance = 0.20;
constexpr double kApplicationTolerance = 0.25;

struct PeakStats {
  size_t backing;
  size_t application;
};

PeakStats GetPeakStats() {
  // TODO(ckennelly): Parse this with protobuf directly
  PeakStats ret;
  const std::string buf = GetStatsInPbTxt();
  constexpr absl::string_view backing_needle = "peak_backed: ";
  constexpr absl::string_view application_needle = "peak_application_demand: ";

  auto parse = [](absl::string_view buf, absl::string_view needle) {
    auto pos = buf.find(needle);
    EXPECT_NE(pos, absl::string_view::npos);
    pos += needle.size();
    auto stop = buf.find(' ', pos);
    if (stop != absl::string_view::npos) {
      stop -= pos;
    }
    size_t ret;
    EXPECT_TRUE(absl::SimpleAtoi(buf.substr(pos, stop), &ret))
        << buf.substr(pos, stop);
    return ret;
  };

  ret.backing = parse(buf, backing_needle);
  ret.application = parse(buf, application_needle);

  return ret;
}

size_t RealizedFragmentation() {
  const auto fragmentation = tcmalloc::MallocExtension::GetNumericProperty(
      "generic.realized_fragmentation");
  CHECK(fragmentation.has_value());
  return fragmentation.value();
}

size_t ExpectedFragmentation(const size_t backing, const size_t application) {
  return static_cast<uint64_t>(
      100. * tcmalloc_internal::safe_div(backing - application, application));
}

size_t ExpectedFragmentation(const PeakStats& expected) {
  return ExpectedFragmentation(expected.backing, expected.application);
}

PeakStats UpperBound(const PeakStats& stats) {
  PeakStats upper;
  upper.backing = stats.backing + stats.backing * kBackingTolerance / 2;
  upper.application =
      stats.application + stats.application * kApplicationTolerance / 2;
  return upper;
}

PeakStats LowerBound(const PeakStats& stats) {
  PeakStats lower;
  lower.backing = stats.backing - stats.backing * kBackingTolerance / 2;
  lower.application =
      stats.application - stats.application * kApplicationTolerance / 2;
  return lower;
}

size_t FragmentationTolerance(const PeakStats& expected) {
  const PeakStats upper = UpperBound(expected);
  const PeakStats lower = LowerBound(expected);
  const size_t upper_bound =
      ExpectedFragmentation(upper.backing, lower.application);
  const size_t lower_bound =
      ExpectedFragmentation(lower.backing, upper.application);
  return upper_bound - lower_bound;
}

TEST(RealizedFragmentation, Accuracy) {
#ifndef NDEBUG
  GTEST_SKIP() << "Skipping test under debug build for performance";
#endif

  const PeakStats starting = GetPeakStats();
  // We have allocated at least once up to this point.
  ASSERT_GT(starting.backing, 0);

  absl::BitGen rng;

  // Allocate many 2MB allocations, as to trigger a new high water mark, then
  // deallocate.
  constexpr size_t kLargeTarget = 1 << 29;
  constexpr size_t kLargeSize = 2 << 20;
  void* large_list = nullptr;

  for (size_t total = 0; total < kLargeTarget; total += kLargeSize) {
    SLL_Push(&large_list, ::operator new(kLargeSize));
  }

  const PeakStats peak0 = GetPeakStats();
  PeakStats expected0;
  expected0.backing = starting.backing + kLargeTarget;
  expected0.application = starting.application + kLargeTarget;
  EXPECT_NEAR(peak0.backing, expected0.backing,
              expected0.backing * kBackingTolerance);
  EXPECT_NEAR(peak0.application, expected0.application,
              expected0.application * kApplicationTolerance);
  EXPECT_NEAR(RealizedFragmentation(), ExpectedFragmentation(expected0),
              FragmentationTolerance(expected0));

  while (large_list != nullptr) {
    void* object = SLL_Pop(&large_list);
    sized_delete(object, kLargeSize);
  }

  // Allocate many small alocations, as to trigger another high water mark.
  // Deallocate half of these allocations, but fragmentation should remain high.
  constexpr size_t kSmallTarget = kLargeTarget * 2;
  constexpr size_t kSmallSize = 1024;
  void* small_list_keep = nullptr;
  int kept = 0;
  void* small_list_free = nullptr;
  int freed = 0;

  for (size_t total = 0; total < kSmallTarget; total += kSmallSize) {
    void* object = ::operator new(kSmallSize);
    if (absl::Bernoulli(rng, 0.5)) {
      SLL_Push(&small_list_keep, object);
      kept++;
    } else {
      SLL_Push(&small_list_free, object);
      freed++;
    }
  }

  const PeakStats peak1 = GetPeakStats();
  PeakStats expected1;
  expected1.backing = starting.backing + kSmallTarget;
  expected1.application = starting.application + kSmallTarget;
  EXPECT_NEAR(peak1.backing, expected1.backing,
              expected1.backing * kBackingTolerance);
  EXPECT_NEAR(peak1.application, expected1.application,
              expected1.application * kApplicationTolerance);
  EXPECT_NEAR(RealizedFragmentation(), ExpectedFragmentation(expected1),
              FragmentationTolerance(expected1));

  while (small_list_free != nullptr) {
    void* object = SLL_Pop(&small_list_free);
    sized_delete(object, kSmallSize);
  }

  // Allocate many 2MB allocations, as to trigger another high water mark.
  // Fragmentation should continue to be high due to partial spans from the
  // previous round.
  for (size_t total = 0; total < 2 * kLargeTarget; total += kLargeSize) {
    SLL_Push(&large_list, ::operator new(kLargeSize));
  }

  const PeakStats peak2 = GetPeakStats();
  PeakStats expected2;
  expected2.backing = starting.backing + kSmallTarget + 2 * kLargeTarget;
  expected2.application =
      starting.backing + kSmallSize * kept + 2 * kLargeTarget;
  EXPECT_NEAR(peak2.backing, expected2.backing,
              expected2.backing * kBackingTolerance);
  EXPECT_NEAR(peak2.application, expected2.application,
              expected2.application * kApplicationTolerance);
  EXPECT_NEAR(RealizedFragmentation(), ExpectedFragmentation(expected2),
              FragmentationTolerance(expected2));

  while (large_list != nullptr) {
    void* object = SLL_Pop(&large_list);
    sized_delete(object, kLargeSize);
  }

  while (small_list_keep != nullptr) {
    void* object = SLL_Pop(&small_list_keep);
    sized_delete(object, kSmallSize);
  }
}

}  // namespace
}  // namespace tcmalloc

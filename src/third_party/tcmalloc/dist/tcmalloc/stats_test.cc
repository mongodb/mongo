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

#include "tcmalloc/stats.h"

#include <stddef.h>
#include <stdint.h>

#include <limits>
#include <string>

#include "gtest/gtest.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class PrintTest : public ::testing::Test {
 protected:
  static constexpr size_t kBufferSize = 256 * 1024;
  char buf_[kBufferSize];

  void ExpectStats(const BackingStats& back, const SmallSpanStats& small,
                   const LargeSpanStats& large, const std::string& expected) {
    Printer out(&buf_[0], kBufferSize);
    PrintStats("PrintTest", &out, back, small, large, true);
    EXPECT_EQ(expected, buf_);
  }

  BackingStats Backing(size_t system, size_t free, size_t unmapped) {
    BackingStats stat;
    stat.system_bytes = system;
    stat.free_bytes = free;
    stat.unmapped_bytes = unmapped;

    return stat;
  }
};

TEST_F(PrintTest, Empty) {
  ExpectStats(Backing(0, 0, 0), {{}, {}},  // small
              {0, Length(0), Length(0)},   // large
                                           // clang-format off
R"LIT(------------------------------------------------
PrintTest: 0 sizes;    0.0 MiB free;    0.0 MiB unmapped
------------------------------------------------
 >=128 large *      0 spans ~    0.0 MiB;    0.0 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
)LIT");
                                           // clang-format on
}

TEST_F(PrintTest, ManySizes) {
  ExpectStats(Backing(987654321, 1900 * 1000, 67 * 1000 * 1000),
              {{0, 100, 0, 250, 0, 0, 0, 0, 0, 51},
               {0, 0, 300, 400, 0, 0, 0, 0, 0, 27}},  // small
              {2, Length(100000), Length(2000)},      // large
                                                      // clang-format off
R"LIT(------------------------------------------------
PrintTest: 4 sizes;    1.8 MiB free;   63.9 MiB unmapped
------------------------------------------------
     1 pages *    100 spans ~    0.8 MiB;    0.8 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
     2 pages *    300 spans ~    4.7 MiB;    5.5 MiB cum; unmapped:    4.7 MiB;    4.7 MiB cum
     3 pages *    650 spans ~   15.2 MiB;   20.7 MiB cum; unmapped:    9.4 MiB;   14.1 MiB cum
     9 pages *     78 spans ~    5.5 MiB;   26.2 MiB cum; unmapped:    1.9 MiB;   16.0 MiB cum
 >=128 large *      2 spans ~  796.9 MiB;  823.1 MiB cum; unmapped:   15.6 MiB;   31.6 MiB cum
)LIT");
                                                      // clang-format on
}

TEST(PageAllocInfo, Small) {
  PageAllocInfo info("");
  static_assert(kMaxPages >= Length(4), "odd config");

  info.RecordAlloc(PageId{0}, Length(2));
  info.RecordAlloc(PageId{0}, Length(2));
  info.RecordAlloc(PageId{0}, Length(2));

  info.RecordAlloc(PageId{0}, Length(3));
  info.RecordAlloc(PageId{0}, Length(3));

  info.RecordFree(PageId{0}, Length(3));

  auto c2 = info.counts_for(Length(2));
  EXPECT_EQ(3, c2.nalloc);
  EXPECT_EQ(0, c2.nfree);
  EXPECT_EQ(Length(6), c2.alloc_size);
  EXPECT_EQ(Length(0), c2.free_size);

  auto c3 = info.counts_for(Length(3));
  EXPECT_EQ(2, c3.nalloc);
  EXPECT_EQ(1, c3.nfree);
  EXPECT_EQ(Length(6), c3.alloc_size);
  EXPECT_EQ(Length(3), c3.free_size);

  EXPECT_EQ(Length(3 * 2 + (2 - 1) * 3), info.small());
  EXPECT_EQ(Length(0), info.slack());
}

TEST(PageAllocInfo, Large) {
  PageAllocInfo info("");
  static_assert(kPagesPerHugePage > kMaxPages, "odd config");

  // These three should be aggregated
  Length slack;
  info.RecordAlloc(PageId{0}, kMaxPages + Length(1));
  slack += kPagesPerHugePage - kMaxPages - Length(1);
  info.RecordAlloc(PageId{0}, kMaxPages * 3 / 2);
  slack += kPagesPerHugePage - kMaxPages * 3 / 2;
  info.RecordAlloc(PageId{0}, kMaxPages * 2);
  slack += kPagesPerHugePage - kMaxPages * 2;

  // This shouldn't
  const Length larger = kMaxPages * 2 + Length(1);
  info.RecordAlloc(PageId{0}, larger);
  slack +=
      (kPagesPerHugePage - (larger % kPagesPerHugePage)) % kPagesPerHugePage;

  auto c1 = info.counts_for(kMaxPages + Length(1));
  EXPECT_EQ(3, c1.nalloc);
  EXPECT_EQ(0, c1.nfree);
  EXPECT_EQ(kMaxPages * 9 / 2 + Length(1), c1.alloc_size);
  EXPECT_EQ(Length(0), c1.free_size);

  auto c2 = info.counts_for(kMaxPages * 2 + Length(1));
  EXPECT_EQ(1, c2.nalloc);
  EXPECT_EQ(0, c2.nfree);
  EXPECT_EQ(kMaxPages * 2 + Length(1), c2.alloc_size);
  EXPECT_EQ(Length(0), c2.free_size);

  EXPECT_EQ(Length(0), info.small());
  EXPECT_EQ(slack, info.slack());
}

TEST(ClockTest, ClockTicks) {
  // It's a bit ironic to test this clock against other clocks since
  // this exists because we don't trust other clocks.  But hopefully
  // no one is using libfaketime on this binary, and of course we
  // don't care about signal safety, just ticking.
  const absl::Time before = absl::Now();
  const double b = absl::base_internal::CycleClock::Now() /
                   absl::base_internal::CycleClock::Frequency();
  static const absl::Duration kDur = absl::Milliseconds(500);
  absl::SleepFor(kDur);
  const double a = absl::base_internal::CycleClock::Now() /
                   absl::base_internal::CycleClock::Frequency();
  const absl::Time after = absl::Now();

  const absl::Duration actual = (after - before);
  const absl::Duration measured = absl::Seconds(a - b);
  EXPECT_LE(actual * 0.99, measured) << actual;
  EXPECT_GE(actual * 1.01, measured) << actual;
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

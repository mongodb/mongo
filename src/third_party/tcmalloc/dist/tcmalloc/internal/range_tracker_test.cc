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

#include "tcmalloc/internal/range_tracker.h"

#include <stddef.h>
#include <sys/types.h>

#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/fixed_array.h"
#include "absl/random/random.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

using testing::ElementsAre;
using testing::Pair;

class BitmapTest : public testing::Test {
 protected:
  template <size_t N>
  std::vector<size_t> FindSetResults(const Bitmap<N>& map) {
    return FindResults<N, true>(map);
  }

  template <size_t N>
  std::vector<size_t> FindClearResults(const Bitmap<N>& map) {
    return FindResults<N, false>(map);
  }

  template <size_t N, bool Value>
  std::vector<size_t> FindResults(const Bitmap<N>& map) {
    std::vector<size_t> results;
    ssize_t last = -1;
    for (size_t i = 0; i < N; ++i) {
      ssize_t j = Value ? map.FindSet(i) : map.FindClear(i);
      EXPECT_LE(last, j) << i;
      EXPECT_LE(i, j) << i;
      EXPECT_GE(N, j) << i;
      if (last != j) {
        results.push_back(j);
        last = j;
      }
    }

    return results;
  }

  template <size_t N>
  std::vector<size_t> FindSetResultsBackwards(const Bitmap<N>& map) {
    return FindResultsBackwards<N, true>(map);
  }

  template <size_t N>
  std::vector<size_t> FindClearResultsBackwards(const Bitmap<N>& map) {
    return FindResultsBackwards<N, false>(map);
  }

  template <size_t N, bool Value>
  std::vector<size_t> FindResultsBackwards(const Bitmap<N>& map) {
    std::vector<size_t> results;
    ssize_t last = N;
    for (ssize_t i = N - 1; i >= 0; --i) {
      ssize_t j = Value ? map.FindSetBackwards(i) : map.FindClearBackwards(i);
      EXPECT_GE(last, j) << i;
      EXPECT_GE(i, j) << i;
      EXPECT_LE(-1, j) << i;
      if (last != j) {
        results.push_back(j);
        last = j;
      }
    }

    return results;
  }
};

TEST_F(BitmapTest, GetBitEmpty) {
  Bitmap<253> map;
  for (size_t i = 0; i < map.size(); ++i) {
    EXPECT_EQ(map.GetBit(i), 0);
  }
}

TEST_F(BitmapTest, CheckIsZero) {
  Bitmap<253> map;
  EXPECT_EQ(map.IsZero(), true);
  for (size_t i = 0; i < map.size(); ++i) {
    map.Clear();
    EXPECT_EQ(map.IsZero(), true);
    map.SetBit(i);
    EXPECT_EQ(map.IsZero(), false);
  }
}

TEST_F(BitmapTest, CheckClearLowestBit) {
  Bitmap<253> map;
  for (size_t i = 0; i < map.size(); ++i) {
    map.SetBit(i);
  }
  for (size_t i = 0; i < map.size(); ++i) {
    size_t index = map.FindSet(0);
    EXPECT_EQ(index, i);
    map.ClearLowestBit();
  }
}

TEST_F(BitmapTest, GetBitOneSet) {
  const size_t N = 251;
  for (size_t s = 0; s < N; s++) {
    Bitmap<N> map;
    map.SetBit(s);
    for (size_t i = 0; i < map.size(); ++i) {
      EXPECT_EQ(map.GetBit(i), i == s ? 1 : 0);
    }
  }
}

TEST_F(BitmapTest, FindSet) {
  Bitmap<253> map;
  EXPECT_THAT(FindSetResults(map), ElementsAre(253));
  EXPECT_THAT(FindSetResultsBackwards(map), ElementsAre(-1));
  map.SetBit(7);
  map.SetBit(14);
  map.SetBit(15);
  map.SetBit(63);
  map.SetBit(128);
  EXPECT_THAT(FindSetResults(map), ElementsAre(7, 14, 15, 63, 128, 253));
  EXPECT_THAT(FindSetResultsBackwards(map),
              ElementsAre(128, 63, 15, 14, 7, -1));
  map.SetBit(195);
  map.SetBit(196);
  map.SetBit(251);
  map.SetBit(252);
  EXPECT_THAT(FindSetResults(map),
              ElementsAre(7, 14, 15, 63, 128, 195, 196, 251, 252));
  EXPECT_THAT(FindSetResultsBackwards(map),
              ElementsAre(252, 251, 196, 195, 128, 63, 15, 14, 7, -1));
  map.SetBit(0);
  EXPECT_THAT(FindSetResultsBackwards(map),
              ElementsAre(252, 251, 196, 195, 128, 63, 15, 14, 7, 0));
}

TEST_F(BitmapTest, FindClear) {
  Bitmap<253> map;
  map.SetRange(0, 253);
  EXPECT_THAT(FindClearResults(map), ElementsAre(253));
  EXPECT_THAT(FindClearResultsBackwards(map), ElementsAre(-1));

  map.ClearBit(7);
  map.ClearBit(14);
  map.ClearBit(15);
  map.ClearBit(63);
  map.ClearBit(128);
  EXPECT_THAT(FindClearResults(map), ElementsAre(7, 14, 15, 63, 128, 253));
  EXPECT_THAT(FindClearResultsBackwards(map),
              ElementsAre(128, 63, 15, 14, 7, -1));
  map.ClearBit(195);
  map.ClearBit(196);
  map.ClearBit(251);
  map.ClearBit(252);
  EXPECT_THAT(FindClearResults(map),
              ElementsAre(7, 14, 15, 63, 128, 195, 196, 251, 252));
  EXPECT_THAT(FindClearResultsBackwards(map),
              ElementsAre(252, 251, 196, 195, 128, 63, 15, 14, 7, -1));
  map.ClearBit(0);
  EXPECT_THAT(FindClearResultsBackwards(map),
              ElementsAre(252, 251, 196, 195, 128, 63, 15, 14, 7, 0));
}

TEST_F(BitmapTest, CountBits) {
  Bitmap<253> map;
  map.SetRange(0, 253);
  EXPECT_EQ(map.CountBits(0, 253), 253);
  EXPECT_EQ(map.CountBits(8, 245), 245);
  EXPECT_EQ(map.CountBits(0, 250), 250);

  map.ClearBit(7);
  map.ClearBit(14);
  map.ClearBit(15);
  map.ClearBit(63);
  map.ClearBit(128);

  EXPECT_EQ(map.CountBits(0, 253), 248);
  EXPECT_EQ(map.CountBits(8, 245), 241);
  EXPECT_EQ(map.CountBits(0, 250), 245);

  map.ClearBit(195);
  map.ClearBit(196);
  map.ClearBit(251);
  map.ClearBit(252);

  EXPECT_EQ(map.CountBits(0, 253), 244);
  EXPECT_EQ(map.CountBits(8, 245), 237);
  EXPECT_EQ(map.CountBits(0, 250), 243);

  map.ClearBit(0);

  EXPECT_EQ(map.CountBits(0, 253), 243);
  EXPECT_EQ(map.CountBits(8, 245), 237);
  EXPECT_EQ(map.CountBits(0, 250), 242);
}

TEST_F(BitmapTest, CountBitsFuzz) {
  static constexpr size_t kBits = 253;
  absl::FixedArray<bool> truth(kBits);
  Bitmap<kBits> map;

  absl::BitGen rng;
  for (int i = 0; i < kBits; i++) {
    bool v = absl::Bernoulli(rng, 0.3);
    truth[i] = v;
    if (v) {
      map.SetBit(i);
    }
  }

  for (int i = 0; i < 100; i++) {
    SCOPED_TRACE(i);

    // Pick a random starting point and a length, use a naive loop against truth
    // to calculate the expected bit count.
    size_t start = absl::Uniform(rng, 0u, kBits);
    size_t length = absl::Uniform(rng, 0u, kBits - start);

    size_t expected = 0;
    for (int j = 0; j < length; j++) {
      if (truth[start + j]) {
        expected++;
      }
    }

    EXPECT_EQ(expected, map.CountBits(start, length));
  }
}

class RangeTrackerTest : public ::testing::Test {
 protected:
  std::vector<std::pair<size_t, size_t>> FreeRanges() {
    std::vector<std::pair<size_t, size_t>> ret;
    size_t index = 0, len;
    while (range_.NextFreeRange(index, &index, &len)) {
      ret.push_back({index, len});
      index += len;
    }
    return ret;
  }
  static constexpr size_t kBits = 1017;
  RangeTracker<kBits> range_;
};

TEST_F(RangeTrackerTest, Trivial) {
  EXPECT_EQ(kBits, range_.size());
  EXPECT_EQ(0, range_.used());
  EXPECT_EQ(kBits, range_.longest_free());
  EXPECT_THAT(FreeRanges(), ElementsAre(Pair(0, kBits)));
  ASSERT_EQ(0, range_.FindAndMark(kBits));
  EXPECT_EQ(0, range_.longest_free());
  EXPECT_EQ(kBits, range_.used());
  EXPECT_THAT(FreeRanges(), ElementsAre());
  range_.Unmark(0, 100);
  EXPECT_EQ(100, range_.longest_free());
  EXPECT_EQ(kBits - 100, range_.used());
  EXPECT_THAT(FreeRanges(), ElementsAre(Pair(0, 100)));
  // non-contiguous - shouldn't increase longest
  range_.Unmark(200, 100);
  EXPECT_EQ(100, range_.longest_free());
  EXPECT_EQ(kBits - 200, range_.used());
  EXPECT_THAT(FreeRanges(), ElementsAre(Pair(0, 100), Pair(200, 100)));
  range_.Unmark(100, 100);
  EXPECT_EQ(300, range_.longest_free());
  EXPECT_EQ(kBits - 300, range_.used());
  EXPECT_THAT(FreeRanges(), ElementsAre(Pair(0, 300)));
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

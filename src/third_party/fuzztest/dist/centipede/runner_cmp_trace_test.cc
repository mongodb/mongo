// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/runner_cmp_trace.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/nullability.h"

namespace fuzztest::internal {
namespace {

template <typename T>
std::vector<uint8_t> IntPairToByteVector(T a, T b) {
  std::vector<uint8_t> res;
  uint8_t buff[sizeof(T)];
  memcpy(buff, &a, sizeof(T));
  res.insert(res.begin(), buff, buff + sizeof(T));
  memcpy(buff, &b, sizeof(T));
  res.insert(res.begin(), buff, buff + sizeof(T));
  return res;
}

std::vector<uint8_t> TwoArraysToByteVector(const uint8_t *absl_nonnull a,
                                           const uint8_t *absl_nonnull b,
                                           size_t size) {
  std::vector<uint8_t> res;
  res.insert(res.begin(), a, a + size);
  res.insert(res.begin(), b, b + size);
  return res;
}

TEST(CmpTrace, T1) {
  std::vector<std::vector<uint8_t>> observed_pairs;

  auto callback = [&observed_pairs](uint8_t size, const uint8_t *v0,
                                    const uint8_t *v1) {
    std::vector<uint8_t> cmp_pair;
    cmp_pair.insert(cmp_pair.begin(), v0, v0 + size);
    cmp_pair.insert(cmp_pair.begin(), v1, v1 + size);
    observed_pairs.push_back(cmp_pair);
  };

  CmpTrace<2, 10> trace2;
  CmpTrace<4, 11> trace4;
  CmpTrace<8, 12> trace8;
  CmpTrace<0, 13> traceN;
  trace2.Clear();
  trace4.Clear();
  trace8.Clear();
  traceN.Clear();

  uint16_t small_short_value0 = 10;
  uint16_t small_short_value1 = 20;
  uint16_t short_value0 = 310;
  uint16_t short_value1 = 320;
  uint32_t int_value0 = 500;
  uint32_t int_value1 = 600;
  uint64_t long_value0 = 1000;
  uint64_t long_value1 = 2000;
  uint64_t long_value2 = 4000;
  uint64_t long_value3 = 8000;

  trace2.Capture(small_short_value0, small_short_value1);  // will be ignored.
  trace2.Capture(short_value0, short_value1);
  observed_pairs.clear();
  trace2.ForEachNonZero(callback);
  EXPECT_THAT(observed_pairs, testing::UnorderedElementsAre(IntPairToByteVector(
                                  short_value0, short_value1)));

  trace4.Capture(30, 40);  // small values, will be ignored.
  trace4.Capture(int_value0, int_value1);
  observed_pairs.clear();
  trace4.ForEachNonZero(callback);
  EXPECT_THAT(observed_pairs, testing::UnorderedElementsAre(
                                  IntPairToByteVector(int_value0, int_value1)));

  trace8.Capture(200LL, 255LL);  // small values, will be ignored.
  trace8.Capture(long_value0, long_value1);
  trace8.Capture(long_value2, long_value3);
  observed_pairs.clear();
  trace8.ForEachNonZero(callback);
  EXPECT_THAT(observed_pairs,
              testing::UnorderedElementsAre(
                  IntPairToByteVector(long_value0, long_value1),
                  IntPairToByteVector(long_value2, long_value3)));

  constexpr uint8_t value0[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
  constexpr uint8_t value1[10] = {0, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  constexpr uint8_t long_array[20] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                                      10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  traceN.Capture(7, value0, value1);
  traceN.Capture(3, value0, value1);
  traceN.Capture(10, value0, value1);
  traceN.Capture(20, long_array, long_array);  // will be trimmed to 16.
  observed_pairs.clear();
  traceN.ForEachNonZero(callback);
  EXPECT_THAT(observed_pairs,
              testing::UnorderedElementsAre(
                  TwoArraysToByteVector(value0, value1, 10),
                  TwoArraysToByteVector(value0, value1, 7),
                  TwoArraysToByteVector(value0, value1, 3),
                  TwoArraysToByteVector(long_array, long_array, 16)));
}

}  // namespace
}  // namespace fuzztest::internal

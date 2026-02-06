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

#include "./centipede/concurrent_byteset.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/const_init.h"
#include "./centipede/thread_pool.h"

namespace fuzztest::internal {
namespace {

TEST(ConcurrentByteSetTest, Basic) {
  static ConcurrentByteSet<1024> bs(absl::kConstInit);
  const std::vector<std::pair<size_t, uint8_t>> in = {
      {0, 1}, {1, 42}, {2, 33}, {100, 15}, {102, 1}, {800, 66}};

  for (const auto &idx_value : in) {
    bs.Set(idx_value.first, idx_value.second);
  }

  // Test ForEachNonZeroByte.
  std::vector<std::pair<size_t, uint8_t>> out;
  bs.ForEachNonZeroByte(
      [&](size_t idx, uint8_t value) { out.emplace_back(idx, value); });
  EXPECT_EQ(out, in);

  // Now bs should be empty.
  out.clear();
  bs.ForEachNonZeroByte(
      [&](size_t idx, uint8_t value) { out.emplace_back(idx, value); });
  EXPECT_TRUE(out.empty());

  // Test SaturatedIncrement.
  for (const auto &idx_value : in) {
    for (auto iter = 0; iter < idx_value.second; ++iter) {
      bs.SaturatedIncrement(idx_value.first);
    }
  }
  bs.ForEachNonZeroByte(
      [&](size_t idx, uint8_t value) { out.emplace_back(idx, value); });
  EXPECT_EQ(out, in);
}

// Test a thread_local object.
static thread_local TwoLayerConcurrentByteSet<(1 << 17)> two_layer_byte_set(
    absl::kConstInit);

TEST(ConcurrentByteSetTest, TwoLayer) {
  auto &bs = two_layer_byte_set;
  const std::vector<std::pair<size_t, uint8_t>> in = {
      {0, 1}, {1, 42}, {2, 33}, {100, 15}, {102, 1}, {800, 66}};

  for (const auto &idx_value : in) {
    bs.Set(idx_value.first, idx_value.second);
  }

  // Test ForEachNonZeroByte.
  std::vector<std::pair<size_t, uint8_t>> out;
  bs.ForEachNonZeroByte(
      [&](size_t idx, uint8_t value) { out.emplace_back(idx, value); });
  EXPECT_EQ(out, in);

  // Now bs should be empty.
  out.clear();
  bs.ForEachNonZeroByte(
      [&](size_t idx, uint8_t value) { out.emplace_back(idx, value); });
  EXPECT_TRUE(out.empty());

  // Test SaturatedIncrement.
  for (const auto &idx_value : in) {
    for (auto iter = 0; iter < idx_value.second; ++iter) {
      bs.SaturatedIncrement(idx_value.first);
    }
  }
  bs.ForEachNonZeroByte(
      [&](size_t idx, uint8_t value) { out.emplace_back(idx, value); });
  EXPECT_EQ(out, in);
}

// Tests TwoLayerConcurrentByteSet from multiple threads.
TEST(ConcurrentByteSetTest, TwoLayerConcurrentThreads) {
  static TwoLayerConcurrentByteSet<(1 << 16)> bs(absl::kConstInit);
  // 3 threads will each increment one specific byte in a long loop.
  // 4th thread will increment another byte, just once.
  static auto cb = [](size_t idx) {
    for (size_t i = 0; i < 10000000; i++) {
      bs.SaturatedIncrement(idx);
    }
  };
  {
    ThreadPool threads{4};
    threads.Schedule([]() { cb(10); });
    threads.Schedule([]() { cb(11); });
    threads.Schedule([]() { cb(14); });
    threads.Schedule([]() { bs.SaturatedIncrement(15); });
  }  // The threads join here.
  const std::vector<std::pair<size_t, uint8_t>> expected = {
      {10, 255}, {11, 255}, {14, 255}, {15, 1}};
  std::vector<std::pair<size_t, uint8_t>> out;
  bs.ForEachNonZeroByte(
      [&](size_t idx, uint8_t value) { out.emplace_back(idx, value); });
  EXPECT_EQ(out, expected);
}

}  // namespace
}  // namespace fuzztest::internal

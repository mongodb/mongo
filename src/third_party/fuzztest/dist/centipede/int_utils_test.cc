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

#include "./centipede/int_utils.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"

namespace fuzztest::internal {
namespace {

TEST(IntUtilsTest, Hash64Bits) {
  // Run a large sample of small integers and verify that lower X bits
  // of Hash64Bits(), for X in 64, 48, 32, and 20, are unique.
  absl::flat_hash_set<uint64_t> set64;
  absl::flat_hash_set<uint64_t> set48;
  absl::flat_hash_set<uint64_t> set32;
  absl::flat_hash_set<uint64_t> set20;
  size_t num_values = 0;
  constexpr uint64_t kMaxIntToCheck = 1ULL << 28;
  constexpr uint64_t kMask48 = (1ULL << 48) - 1;
  constexpr uint64_t kMask32 = (1ULL << 32) - 1;
  constexpr uint64_t kMask20 = (1ULL << 20) - 1;
  for (uint64_t i = 0; i < kMaxIntToCheck; i += 101, ++num_values) {
    set64.insert(Hash64Bits(i));
    set48.insert(Hash64Bits(i) & kMask48);
    set32.insert(Hash64Bits(i) & kMask32);
    set20.insert(Hash64Bits(i) & kMask20);
  }
  EXPECT_EQ(set64.size(), num_values);
  EXPECT_EQ(set48.size(), num_values);
  EXPECT_EQ(set32.size(), num_values);
  EXPECT_EQ(set20.size(), 1 << 20);  // all possible 20-bit numbers.

  // For a large number of pairs of small integers {i, j} verify that
  // values of Hash64Bits(i) ^ (j) are unique.
  set64.clear();
  num_values = 0;
  for (uint64_t i = 0; i < kMaxIntToCheck; i += 100000) {
    for (uint64_t j = 1; j < kMaxIntToCheck; j += 100000) {
      set64.insert(Hash64Bits(i) ^ (j));
      ++num_values;
    }
  }
  EXPECT_EQ(set64.size(), num_values);
}

}  // namespace
}  // namespace fuzztest::internal

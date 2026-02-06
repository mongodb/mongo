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

#include "./centipede/hashed_ring_buffer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <numeric>
#include <vector>

#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"

namespace fuzztest::internal {
namespace {

TEST(Feature, HashedRingBuffer) {
  HashedRingBuffer<32> rb16;  // used with ring_buffer_size == 16
  HashedRingBuffer<32> rb32;  // used with ring_buffer_size == 32
  rb16.Reset(16);
  rb32.Reset(32);
  absl::flat_hash_set<size_t> hashes16, hashes32;
  size_t kNumIter = 10000000;
  // push a large number of different numbers into rb, ensure that most of the
  // resulting hashes are different.
  for (size_t i = 0; i < kNumIter; i++) {
    hashes16.insert(rb16.push(i));
    hashes32.insert(rb32.push(i));
  }
  // No collisions.
  EXPECT_EQ(hashes16.size(), kNumIter);
  EXPECT_EQ(hashes32.size(), kNumIter);

  // Try all permutations of {0, 1, 2, ... 9}, ensure we have at least half
  // this many different hashes.
  std::vector<size_t> numbers(10);
  std::iota(numbers.begin(), numbers.end(), 0);
  hashes32.clear();
  size_t num_permutations = 0;
  while (std::next_permutation(numbers.begin(), numbers.end())) {
    ++num_permutations;
    rb32.Reset(32);
    for (const auto number : numbers) {
      rb32.push(number);
    }
    hashes32.insert(rb32.hash());
  }
  EXPECT_GT(hashes32.size(), num_permutations / 2);
}

}  // namespace
}  // namespace fuzztest::internal

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

#include "./centipede/concurrent_bitset.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/const_init.h"
#include "./centipede/thread_pool.h"

namespace fuzztest::internal {
namespace {

TEST(ConcurrentBitSetTest, Set) {
  constexpr size_t kSize = 1 << 18;
  static ConcurrentBitSet<kSize> bs(absl::kConstInit);
  std::vector<size_t> in_bits = {0, 1, 2, 100, 102, 1000000};
  std::vector<size_t> expected_out_bits = {0, 1, 2, 100, 102, 1000000 % kSize};
  std::vector<size_t> out_bits;
  for (auto idx : in_bits) {
    bs.set(idx);
  }
  bs.ForEachNonZeroBit([&](size_t idx) { out_bits.push_back(idx); });
  EXPECT_EQ(out_bits, expected_out_bits);

  bs.clear();
  out_bits.clear();
  bs.ForEachNonZeroBit([&](size_t idx) { out_bits.push_back(idx); });
  EXPECT_TRUE(out_bits.empty());
  bs.set(42);
  bs.ForEachNonZeroBit([&](size_t idx) { out_bits.push_back(idx); });
  expected_out_bits = {42};
  EXPECT_EQ(out_bits, expected_out_bits);
  // Check that all bits are now clear.
  out_bits.clear();
  bs.ForEachNonZeroBit([&](size_t idx) { out_bits.push_back(idx); });
  EXPECT_TRUE(out_bits.empty());
}

TEST(ConcurrentBitSetTest, Get) {
  constexpr size_t kSize = 1 << 18;
  static ConcurrentBitSet<kSize> bs(absl::kConstInit);
  constexpr size_t kInBit1 = 134217728;
  constexpr size_t kInBit2 = 134217732;
  ASSERT_EQ(bs.get(kInBit1), 0);
  ASSERT_EQ(bs.get(kInBit2), 0);
  bs.set(kInBit1);
  EXPECT_EQ(bs.get(kInBit1), 1);
  EXPECT_EQ(bs.get(kInBit2), 0);
}

// Tests `ConcurrentBitSet` from multiple threads.
TEST(ConcurrentBitSetTest, SetInConcurrentThreads) {
  // 3 threads will each set one specific bit in a long loop.
  // 4th thread will set another bit, just once.
  // The set() function is lossy, i.e. it may fail to set the bit.
  // If the value is set in a long loop, it will be set with a probability
  // indistinguishable from one (at least this is my theory :).
  // But the 4th thread that sets its bit once, may actually fail to do it.
  // So, this test allows two outcomes (possible_bits3/possible_bits4).
  // WARNING: `bs` must be static (see the class comment).
  static ConcurrentBitSet<(1 << 18)> bs(absl::kConstInit);
  static auto cb = [](size_t idx) {
    for (size_t i = 0; i < 10000000; i++) {
      bs.set(idx);
    }
  };
  {
    ThreadPool pool{4};
    pool.Schedule([]() { cb(10); });
    pool.Schedule([]() { cb(11); });
    pool.Schedule([]() { cb(14); });
    pool.Schedule([]() { bs.set(15); });
  }
  std::vector<size_t> bits;
  std::vector<size_t> possible_bits3 = {10, 11, 14};
  std::vector<size_t> possible_bits4 = {10, 11, 14, 15};
  bs.ForEachNonZeroBit([&bits](size_t idx) { bits.push_back(idx); });
  if (bits.size() == 3) {
    EXPECT_EQ(bits, possible_bits3);
  } else {
    EXPECT_EQ(bits, possible_bits4);
  }
}

// Global ConcurrentBitSet with a absl::kConstInit CTOR.
static ConcurrentBitSet<(1 << 20)> large_concurrent_bitset(absl::kConstInit);
// Test a thread-local object.
static thread_local ConcurrentBitSet<(1 << 20)> large_tls_concurrent_bitset(
    absl::kConstInit);

TEST(ConcurrentBitSetTest, Large) {
  for (auto *bs : {&large_concurrent_bitset, &large_tls_concurrent_bitset}) {
    const std::vector<size_t> in_bits = {
        0, 1, 2, 100, 102, 800, 10000, 20000, 30000, 500000,
    };

    for (size_t iter = 0; iter < 100000; ++iter) {
      for (auto idx : in_bits) {
        bs->set(idx);
      }
      std::vector<size_t> out_bits;
      bs->ForEachNonZeroBit([&](size_t idx) { out_bits.push_back(idx); });
      EXPECT_EQ(out_bits, in_bits);
    }
  }
}

}  // namespace
}  // namespace fuzztest::internal

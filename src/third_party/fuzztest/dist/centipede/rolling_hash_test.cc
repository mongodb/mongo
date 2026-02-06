// Copyright 2023 The Centipede Authors.
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

#include "./centipede/rolling_hash.h"

#include <stdlib.h>
#include <sys/types.h>

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <numeric>
#include <vector>

#include "gtest/gtest.h"
#include "./centipede/feature.h"

namespace fuzztest::internal {
namespace {

// Reference implementation for RollingHash.
// Maintains the entire window of hash-ed ints in memory.
// Otherwise, equivalent to RollingHash.
class TestOnlyRollingHash {
 public:
  void Reset(size_t window_size) {
    window_size_ = window_size;
    deq_.clear();
  }

  void Update(uint32_t add, uint32_t remove) {
    deq_.push_back(add);
    if (deq_.size() > window_size_) deq_.pop_front();
  }

  uint32_t Hash() const {
    uint64_t hash = 0;
    for (const auto &value : deq_) {
      hash = RollingHash::TestOnlyUpdate(hash, value);
    }
    return hash;
  }

 protected:
  size_t window_size_ = 0;
  std::deque<uint32_t> deq_;
};

// Tests RollingHashType, compares the results with TestOnlyRollingHashType.
template <typename RollingHashType, typename TestOnlyRollingHashType>
void TestRollingHash() {
  static RollingHashType hasher;  // must be static.
  TestOnlyRollingHashType test_hasher;

  // Tests on this many ints.
  constexpr size_t kDataSize = 1 << 23;
  // We test collisions for the full 32-bit hash, and also
  // for the hash masked-off by kMask, to ensure that it remains
  // a good hash if we only take a subset of bits.
  // Our main use case is using the number of bits required for kDomainSize.
  constexpr uint32_t kMask = feature_domains::Domain::kDomainSize - 1;
  // Allow this many collisions for the masked hash.
  constexpr size_t kMaxNumMaskCollisions = 3;
  constexpr size_t kNumWindowsSizes = 20;

  // kDataSize ints: 0, 1, 2, ...
  std::vector<uint32_t> data(kDataSize);
  std::iota(data.begin(), data.end(), 0);

  // Bitset is a bit faster for this test than a hash set.
  using BS = std::bitset<(1ULL << 32)>;
  // Allocate BS objects on heap, because they are too large for stack.
  std::unique_ptr<BS> collisions_full(new BS());
  std::unique_ptr<BS> collisions_mask(new BS());

  for (size_t window_size = 1; window_size <= kNumWindowsSizes; ++window_size) {
    hasher.Reset(window_size);
    test_hasher.Reset(window_size);
    // Clear all bitset bits (->reset(), not .reset()).
    collisions_full->reset();
    collisions_mask->reset();
    // pipe all ints in `data` through the hasher, maintaining the window
    // of `window_size` elements. Count hash collisions.
    size_t num_collisions_full = 0, num_collisions_mask = 0;
    for (size_t idx = 0; idx < data.size(); ++idx) {
      uint32_t remove = idx >= window_size ? data[idx - window_size] : 0;
      hasher.Update(data[idx], remove);
      uint32_t hash = hasher.Hash();
      num_collisions_full += collisions_full->test(hash);
      num_collisions_mask += collisions_mask->test(hash & kMask);
      collisions_full->set(hash);
      collisions_mask->set(hash & kMask);
      if (idx < 100000) {
        // test_hasher is much more expensive, test only first few iterations.
        test_hasher.Update(data[idx], remove);
        EXPECT_EQ(hash, test_hasher.Hash());
      }
    }
    EXPECT_EQ(num_collisions_full, 0);
    EXPECT_LE(num_collisions_mask, kMaxNumMaskCollisions);
  }
}

TEST(RollingHash, RollingHash) {
  TestRollingHash<RollingHash, TestOnlyRollingHash>();
}

}  // namespace
}  // namespace fuzztest::internal

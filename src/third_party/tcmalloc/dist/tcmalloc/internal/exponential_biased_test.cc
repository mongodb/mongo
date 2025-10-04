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

#include "tcmalloc/internal/exponential_biased.h"

#include <cstdint>

#include "gtest/gtest.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// Testing that NextRandom generates uniform
// random numbers.
// Applies the Anderson-Darling test for uniformity
void TestNextRandom(int n) {
  uint64_t x = 1;
  // This assumes that the prng returns 48 bit numbers
  uint64_t max_prng_value = static_cast<uint64_t>(1) << 48;
  // Initialize
  for (int i = 1; i <= 20; i++) {  // 20 mimics sampler.Init()
    x = ExponentialBiased::NextRandom(x);
  }
  std::vector<uint64_t> int_random_sample(n);
  // Collect samples
  for (int i = 0; i < n; i++) {
    int_random_sample[i] = x;
    x = ExponentialBiased::NextRandom(x);
  }
  // First sort them...
  std::sort(int_random_sample.begin(), int_random_sample.end());
  std::vector<double> random_sample(n);
  // Convert them to uniform randoms (in the range [0,1])
  for (int i = 0; i < n; i++) {
    random_sample[i] =
        static_cast<double>(int_random_sample[i]) / max_prng_value;
  }
  // Now compute the Anderson-Darling statistic
  double ad_pvalue = AndersonDarlingTest(random_sample);
  EXPECT_GT(std::min(ad_pvalue, 1 - ad_pvalue), 0.0001)
      << "prng is not uniform: n = " << n << " p = " << ad_pvalue;
}

TEST(ExponentialBiased, TestNextRandom_MultipleValues) {
  TestNextRandom(10);  // Check short-range correlation
  TestNextRandom(100);
  TestNextRandom(1000);
  TestNextRandom(10000);  // Make sure there's no systematic error
}

// Test that NextRand is in the right range.  Unfortunately, this is a
// stochastic test which could miss problems.
TEST(ExponentialBiased, NextRand_range) {
  uint64_t one = 1;
  // The next number should be (one << 48) - 1
  uint64_t max_value = (one << 48) - 1;
  uint64_t x = (one << 55);
  int n = 22;                            // 27;
  for (int i = 1; i <= (1 << n); i++) {  // 20 mimics sampler.Init()
    x = ExponentialBiased::NextRandom(x);
    ASSERT_LE(x, max_value);
  }
}

// Tests certain arithmetic operations to make sure they compute what we
// expect them too (for testing across different platforms)
TEST(ExponentialBiased, arithmetic_1) {
  uint64_t rnd;  // our 48 bit random number, which we don't trust
  const uint64_t prng_mod_power = 48;
  uint64_t one = 1;
  rnd = one;
  uint64_t max_value = (one << 48) - 1;
  for (int i = 1; i <= (1 << 27); i++) {  // 20 mimics sampler.Init()
    rnd = ExponentialBiased::NextRandom(rnd);
    ASSERT_LE(rnd, max_value);
    double q = (rnd >> (prng_mod_power - 26)) + 1.0;
    ASSERT_GE(q, 0) << rnd << "  " << prng_mod_power;
  }
  // Test some potentially out of bounds value for rnd
  for (int i = 1; i < 64; i++) {
    rnd = one << i;
    double q = (rnd >> (prng_mod_power - 26)) + 1.0;
    ASSERT_GE(q, 0) << " rnd=" << rnd << "  i=" << i << " prng_mod_power"
                    << prng_mod_power;
  }
}

TEST(ExponentialBiased, CoinFlip) {
  // Ensure that the low bits contain good randomness and can be as a coin flip.
  for (uint64_t seed = 0; seed < 100; seed++) {
    uint64_t rnd = seed;
    int even = 0;
    constexpr int kIters = 100;
    for (int i = 0; i < 2 * kIters; i++) {
      rnd = ExponentialBiased::NextRandom(rnd);
      // Check that it works even if we look at every second value
      // (i.e. if the rand is used twice per some operation).
      // This fails without GetRandom, which caused issues for guarded page
      // allocator sampling (left-right-alignment decision).
      if (i % 2) {
        even += ExponentialBiased::GetRandom(rnd) % 2;
      }
    }
    EXPECT_GT(even, kIters / 10) << seed;
    EXPECT_LT(even, kIters / 10 * 9) << seed;
  }
}
}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

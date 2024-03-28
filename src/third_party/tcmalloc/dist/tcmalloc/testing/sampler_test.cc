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
//
// Checks basic properties of the sampler

#include "tcmalloc/sampler.h"

#include <stddef.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

// Back-door so we can access Sampler internals.
namespace tcmalloc {
namespace tcmalloc_internal {

class SamplerTest {
 public:
  static void Init(Sampler* s, uint64_t seed) { s->Init(seed); }
};

namespace {

// Note that these tests are stochastic. This means that the chance of correct
// code passing the test is ~(1 - 10 ^ -kSigmas).
static const double kSigmas = 6;
static const size_t kSamplingInterval =
    MallocExtension::GetProfileSamplingRate();
static const size_t kGuardedSamplingInterval = 100 * kSamplingInterval;

// Tests that GetSamplePeriod returns the expected value
// which is 1<<19
TEST(Sampler, TestGetSamplePeriod) {
  Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  uint64_t sample_period;
  sample_period = sampler.GetSamplePeriod();
  EXPECT_GT(sample_period, 0);
}

void TestSampleAndersonDarling(int sample_period,
                               std::vector<uint64_t>* sample) {
  // First sort them...
  std::sort(sample->begin(), sample->end());
  int n = sample->size();
  std::vector<double> random_sample(n);
  // Convert them to uniform random numbers
  // by applying the geometric CDF
  for (int i = 0; i < n; i++) {
    random_sample[i] =
        1 - exp(-static_cast<double>((*sample)[i]) / sample_period);
  }
  // Now compute the Anderson-Darling statistic
  double geom_ad_pvalue = AndersonDarlingTest(random_sample);
  EXPECT_GT(std::min(geom_ad_pvalue, 1 - geom_ad_pvalue), 0.0001)
      << "PickNextSamplingPoint does not produce good "
         "geometric/exponential random numbers "
         "n = "
      << n << " p = " << geom_ad_pvalue;
}

// Tests that PickNextSamplePeriod generates
// geometrically distributed random numbers.
// First converts to uniforms then applied the
// Anderson-Darling test for uniformity.
void TestPickNextSample(int n) {
  Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  std::vector<uint64_t> int_random_sample(n);
  int sample_period = sampler.GetSamplePeriod();
  int ones_count = 0;
  for (int i = 0; i < n; i++) {
    int_random_sample[i] = sampler.PickNextSamplingPoint();
    EXPECT_GE(int_random_sample[i], 1);
    if (int_random_sample[i] == 1) {
      ones_count += 1;
    }
    EXPECT_LT(ones_count, 4) << " out of " << i << " samples.";
  }
  TestSampleAndersonDarling(sample_period, &int_random_sample);
}

TEST(Sampler, TestPickNextSample_MultipleValues) {
  TestPickNextSample(10);  // Make sure the first few are good (enough)
  TestPickNextSample(100);
  TestPickNextSample(1000);
  TestPickNextSample(10000);  // Make sure there's no systematic error
}

// Further tests

double StandardDeviationsErrorInSample(int total_samples, int picked_samples,
                                       int alloc_size, int sampling_interval) {
  double p = 1 - exp(-(static_cast<double>(alloc_size) / sampling_interval));
  double expected_samples = total_samples * p;
  double sd = pow(p * (1 - p) * total_samples, 0.5);
  return ((picked_samples - expected_samples) / sd);
}

TEST(Sampler, LargeAndSmallAllocs_CombinedTest) {
  Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  int counter_big = 0;
  int counter_small = 0;
  int size_big = 129 * 8 * 1024 + 1;
  int size_small = 1024 * 8;
  int num_iters = 128 * 4 * 8;
  // Allocate in mixed chunks
  for (int i = 0; i < num_iters; i++) {
    if (sampler.RecordAllocation(size_big)) {
      counter_big += 1;
    }
    for (int i = 0; i < 129; i++) {
      if (sampler.RecordAllocation(size_small)) {
        counter_small += 1;
      }
    }
  }
  // Now test that there are the right number of each
  double large_allocs_sds = StandardDeviationsErrorInSample(
      num_iters, counter_big, size_big, kSamplingInterval);
  double small_allocs_sds = StandardDeviationsErrorInSample(
      num_iters * 129, counter_small, size_small, kSamplingInterval);
  ASSERT_LE(fabs(large_allocs_sds), kSigmas) << large_allocs_sds;
  ASSERT_LE(fabs(small_allocs_sds), kSigmas) << small_allocs_sds;
}

template <typename Body>
void DoCheckMean(size_t mean, int num_samples, Body next_sampling_point) {
  size_t total = 0;
  for (int i = 0; i < num_samples; i++) {
    total += next_sampling_point();
  }
  double empirical_mean = total / static_cast<double>(num_samples);
  double expected_sd = mean / pow(num_samples * 1.0, 0.5);
  EXPECT_LT(fabs(mean - empirical_mean), expected_sd * kSigmas);
}

// Tests whether the mean is about right over 1000 samples
TEST(Sampler, IsMeanRight) {
  Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  DoCheckMean(kSamplingInterval, 1000,
              [&sampler]() { return sampler.PickNextSamplingPoint(); });
}

// This checks that the stated maximum value for the sampling rate never
// overflows bytes_until_sample_
TEST(Sampler, bytes_until_sample_Overflow_Underflow) {
  Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  uint64_t one = 1;
  // sample_rate = 0;  // To test the edge case
  uint64_t sample_rate_array[4] = {0, 1, one << 19, one << 58};
  for (int i = 0; i < 4; i++) {
    uint64_t sample_rate = sample_rate_array[i];
    SCOPED_TRACE(sample_rate);

    double sample_scaling = -std::log(2.0) * sample_rate;
    // Take the top 26 bits as the random number
    // (This plus the 1<<26 sampling bound give a max step possible of
    // 1209424308 bytes.)
    const uint64_t prng_mod_power = 48;  // Number of bits in prng

    // First, check the largest_prng value
    uint64_t largest_prng_value = (static_cast<uint64_t>(1) << 48) - 1;
    double q = (largest_prng_value >> (prng_mod_power - 26)) + 1.0;
    uint64_t smallest_sample_step =
        1 + static_cast<uint64_t>((std::log2(q) - 26) * sample_scaling);
    uint64_t cutoff =
        static_cast<uint64_t>(10) * (sample_rate / (one << 24) + 1);
    // This checks that the answer is "small" and positive
    ASSERT_LE(smallest_sample_step, cutoff);

    // Next, check with the smallest prng value
    uint64_t smallest_prng_value = 0;
    q = (smallest_prng_value >> (prng_mod_power - 26)) + 1.0;
    uint64_t largest_sample_step =
        1 + static_cast<uint64_t>((std::log2(q) - 26) * sample_scaling);
    ASSERT_LE(largest_sample_step, one << 63);
    ASSERT_GE(largest_sample_step, smallest_sample_step);
  }
}

// Tests certain arithmetic operations to make sure they compute what we
// expect them too (for testing across different platforms)
// know bad values under with -c dbg --cpu piii for _some_ binaries:
// rnd=227453640600554
// shifted_rnd=54229173
// (hard to reproduce)
TEST(Sampler, arithmetic_2) {
  uint64_t rnd{227453640600554};

  const uint64_t prng_mod_power = 48;  // Number of bits in prng
  uint64_t shifted_rnd = rnd >> (prng_mod_power - 26);
  ASSERT_LT(shifted_rnd, (1 << 26));
  ASSERT_GE(static_cast<double>(static_cast<uint32_t>(shifted_rnd)), 0)
      << " rnd=" << rnd << "  srnd=" << shifted_rnd;
  ASSERT_GE(static_cast<double>(shifted_rnd), 0)
      << " rnd=" << rnd << "  srnd=" << shifted_rnd;
  double q = static_cast<double>(shifted_rnd) + 1.0;
  ASSERT_GT(q, 0);
}

// It's not really a test, but it's good to know
TEST(Sampler, size_of_class) {
  Sampler sampler;
  SamplerTest::Init(&sampler, 1);
  EXPECT_LE(sizeof(sampler), 48);
}

TEST(Sampler, stirring) {
  // Lets test that we get somewhat random values from sampler even when we're
  // dealing with Samplers that have same addresses, as we see when thread's TLS
  // areas are reused. b/117296263

  alignas(Sampler) char place[sizeof(Sampler)];

  DoCheckMean(kSamplingInterval, 1000, [&place]() {
    Sampler* sampler = new (place) Sampler;
    // Sampler constructor just 0-initializes
    // everything. RecordAllocation really makes sampler initialize
    // itself.
    sampler->RecordAllocation(1);
    // And then we probe sampler's (second) value.
    size_t retval = sampler->PickNextSamplingPoint();
    sampler->Sampler::~Sampler();
    return retval;
  });
}

// Tests that the weights returned by RecordAllocation match the sampling rate.
TEST(Sampler, weight_distribution) {
  static constexpr size_t sizes[] = {
      0, 1, 8, 198, 1024, 1152, 3712, 1 << 16, 1 << 25, 50 << 20, 1 << 30};

  for (auto size : sizes) {
    SCOPED_TRACE(size);

    Sampler s;
    SamplerTest::Init(&s, 1);

    static constexpr int kSamples = 10000;
    double expected =
        (size + 1) / (1.0 - exp(-1.0 * (size + 1) / s.GetSamplePeriod()));
    // Since each sample requires ~2MiB / size iterations, using fewer samples
    // for the small sizes makes this test run in ~2s vs. ~90s on Forge in 2019.
    DoCheckMean(expected, size < 256 ? 100 : kSamples, [size, &s]() {
      size_t weight = 0;
      while (!(weight = s.RecordAllocation(size))) {
      }
      return weight;
    });
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

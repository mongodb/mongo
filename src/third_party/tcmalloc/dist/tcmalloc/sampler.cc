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

#include "tcmalloc/sampler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// We used to select an initial value for the sampling counter and then sample
// an allocation when the counter becomes less or equal to zero.
// Now we sample when the counter becomes less than zero, so that we can use
// __builtin_usubl_overflow, which is the only way to get good machine code
// for both x86 and arm. To account for that, we subtract 1 from the initial
// counter value and then add it back when we calculate the sample weight.
constexpr ssize_t kIntervalOffset = 1;

ssize_t Sampler::GetSamplePeriod() {
  return Parameters::profile_sampling_rate();
}

// Run this before using your sampler
ABSL_ATTRIBUTE_NOINLINE void Sampler::Init(uint64_t seed) {
  TC_ASSERT_NE(seed, 0);

  // do_malloc comes here without having initialized statics, and
  // PickNextSamplingPoint uses data initialized in static vars.
  tc_globals.InitIfNecessary();

  // Initialize PRNG
  rnd_ = seed;
  // Step it forward 20 times for good measure
  for (int i = 0; i < 20; i++) {
    rnd_ = ExponentialBiased::NextRandom(rnd_);
  }
  // Initialize counters
  bytes_until_sample_ = PickNextSamplingPoint();
}

ssize_t Sampler::PickNextSamplingPoint() {
  sample_period_ = GetSamplePeriod();
  if (sample_period_ <= 0) {
    // In this case, we don't want to sample ever, and the larger a
    // value we put here, the longer until we hit the slow path
    // again. However, we have to support the flag changing at
    // runtime, so pick something reasonably large (to keep overhead
    // low) but small enough that we'll eventually start to sample
    // again.
    return 128 << 20;
  }
  if (ABSL_PREDICT_FALSE(sample_period_ == 1)) {
    // A sample period of 1, generally used only in tests due to its exorbitant
    // cost, is a request for *every* allocation to be sampled.
    return 0;
  }
  return std::max<ssize_t>(
      0, GetGeometricVariable(sample_period_) - kIntervalOffset);
}

// Generates a geometric variable with the specified mean.
// This is done by generating a random number between 0 and 1 and applying
// the inverse cumulative distribution function for an exponential.
// Specifically: Let m be the inverse of the sample period, then
// the probability distribution function is m*exp(-mx) so the CDF is
// p = 1 - exp(-mx), so
// q = 1 - p = exp(-mx)
// log_e(q) = -mx
// -log_e(q)/m = x
// log_2(q) * (-log_e(2) * 1/m) = x
// In the code, q is actually in the range 1 to 2**26, hence the -26 below
ssize_t Sampler::GetGeometricVariable(ssize_t mean) {
  rnd_ = ExponentialBiased::NextRandom(rnd_);
  // Take the top 26 bits as the random number
  // (This plus the 1<<58 sampling bound give a max possible step of
  // 5194297183973780480 bytes.)
  const uint64_t prng_mod_power = 48;  // Number of bits in prng
  // The uint32_t cast is to prevent a (hard-to-reproduce) NAN
  // under piii debug for some binaries.
  double q = static_cast<uint32_t>(rnd_ >> (prng_mod_power - 26)) + 1.0;
  // Put the computed p-value through the CDF of a geometric.
  double interval = (std::log2(q) - 26) * (-std::log(2.0) * mean);

  // Very large values of interval overflow ssize_t. If we happen to hit this
  // improbable condition, we simply cheat and clamp interval to the largest
  // supported value.  This is slightly tricky, since casting the maximum
  // ssize_t value to a double rounds it up, and casting that rounded value
  // back to an ssize_t will still overflow.  Thus, we specifically need to
  // use a ">=" condition here, rather than simply ">" as would be appropriate
  // if the arithmetic were exact.
  if (interval >= static_cast<double>(std::numeric_limits<ssize_t>::max()))
    return std::numeric_limits<ssize_t>::max();
  else
    return static_cast<ssize_t>(interval);
}

size_t Sampler::RecordAllocationSlow(size_t k) {
  static std::atomic<uint64_t> global_randomness;

  if (ABSL_PREDICT_FALSE(!initialized_)) {
    initialized_ = true;
    uint64_t global_seed =
        global_randomness.fetch_add(1, std::memory_order_relaxed);
    Init(reinterpret_cast<uintptr_t>(this) ^ global_seed);
    // Avoid missampling 0.
    bytes_until_sample_ -= k + 1;
    if (ABSL_PREDICT_TRUE(bytes_until_sample_ >= 0)) {
      return 0;
    }
  }

  // Compute sampling weight (i.e. the number of bytes represented by this
  // sample in expectation).
  //
  // Let k be the size of the allocation, T be the sample period
  // (sample_period_), and f the number of bytes after which we decided to
  // sample (bytes_until_sample_). On average, if we were to continue taking
  // samples every T bytes, we would take (k - f) / T additional samples in
  // this allocation, plus the one we are taking now, for 1 + (k - f) / T
  // total samples. Multiplying by T, the mean number of bytes between samples,
  // gives us a weight of T + k - f.
  //
  size_t weight = sample_period_ - bytes_until_sample_ - kIntervalOffset;
  bytes_until_sample_ = PickNextSamplingPoint();
  return GetSamplePeriod() <= 0 ? 0 : weight;
}

double AllocatedBytes(const StackTrace& stack) {
  return static_cast<double>(stack.weight) * stack.allocated_size /
         (stack.requested_size + 1);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

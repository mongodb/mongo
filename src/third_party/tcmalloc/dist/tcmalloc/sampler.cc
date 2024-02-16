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
#include <limits>

#include "tcmalloc/common.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

ssize_t Sampler::GetSamplePeriod() {
  return Parameters::profile_sampling_rate();
}

// Run this before using your sampler
ABSL_ATTRIBUTE_NOINLINE void Sampler::Init(uint64_t seed) {
  ASSERT(seed != 0);

  // do_malloc comes here without having initialized statics, and
  // PickNextSamplingPoint uses data initialized in static vars.
  tc_globals.InitIfNecessary();

  // Initialize PRNG
  rnd_ = seed;
  // Step it forward 20 times for good measure
  for (int i = 0; i < 20; i++) {
    rnd_ = NextRandom(rnd_);
  }
  // Initialize counters
  true_bytes_until_sample_ = PickNextSamplingPoint();
  if (tc_globals.IsOnFastPath()) {
    bytes_until_sample_ = true_bytes_until_sample_;
    was_on_fast_path_ = true;
  } else {
    // Force the next allocation to hit the slow path.
    ASSERT(bytes_until_sample_ == 0);
    was_on_fast_path_ = false;
  }
  allocs_until_guarded_sample_ = PickNextGuardedSamplingPoint();
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
    return 1;
  }
  return GetGeometricVariable(sample_period_);
}

ssize_t Sampler::PickNextGuardedSamplingPoint() {
  double guarded_sample_rate = Parameters::guarded_sampling_rate();
  double profile_sample_rate = Parameters::profile_sampling_rate();
  if (guarded_sample_rate < 0 || profile_sample_rate <= 0) {
    // Guarded sampling is disabled but could be turned on at run time.  So we
    // return a sampling point (default mean=100) in case guarded sampling is
    // later enabled.  Since the flag is also checked in
    // ShouldSampleGuardedAllocation(), guarded sampling is still guaranteed
    // not to run until it is enabled.
    return GetGeometricVariable(/*mean=*/100);
  }
  return GetGeometricVariable(
      std::ceil(guarded_sample_rate / profile_sample_rate));
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
  rnd_ = NextRandom(rnd_);
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
    if (static_cast<size_t>(true_bytes_until_sample_) > k) {
      true_bytes_until_sample_ -= k;
      if (tc_globals.IsOnFastPath()) {
        bytes_until_sample_ -= k;
        was_on_fast_path_ = true;
      }
      return 0;
    }
  }

  if (ABSL_PREDICT_FALSE(true_bytes_until_sample_ > k)) {
    // The last time we picked a sampling point, we were on the slow path.  We
    // don't want to sample yet since true_bytes_until_sample_ >= k.
    true_bytes_until_sample_ -= k;

    if (ABSL_PREDICT_TRUE(tc_globals.IsOnFastPath())) {
      // We've moved from the slow path to the fast path since the last sampling
      // point was picked.
      bytes_until_sample_ = true_bytes_until_sample_;
      true_bytes_until_sample_ = 0;
      was_on_fast_path_ = true;
    } else {
      bytes_until_sample_ = 0;
      was_on_fast_path_ = false;
    }

    return 0;
  }

  // Compute sampling weight (i.e. the number of bytes represented by this
  // sample in expectation).
  //
  // Let k be the size of the allocation, p be the sample period
  // (sample_period_), and f the number of bytes after which we decided to
  // sample (either bytes_until_sample_ or true_bytes_until_sample_). On
  // average, if we were to continue taking samples every p bytes, we would take
  // (k - f) / p additional samples in this allocation, plus the one we are
  // taking now, for 1 + (k - f) / p total samples. Multiplying by p, the mean
  // number of bytes between samples, gives us a weight of p + k - f.
  //
  size_t weight =
      sample_period_ + k -
      (was_on_fast_path_ ? bytes_until_sample_ : true_bytes_until_sample_);
  const auto point = PickNextSamplingPoint();
  if (ABSL_PREDICT_TRUE(tc_globals.IsOnFastPath())) {
    bytes_until_sample_ = point;
    true_bytes_until_sample_ = 0;
    was_on_fast_path_ = true;
  } else {
    bytes_until_sample_ = 0;
    true_bytes_until_sample_ = point;
    was_on_fast_path_ = false;
  }
  return GetSamplePeriod() <= 0 ? 0 : weight;
}

double AllocatedBytes(const StackTrace& stack) {
  return static_cast<double>(stack.weight) * stack.allocated_size /
         (stack.requested_size + 1);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

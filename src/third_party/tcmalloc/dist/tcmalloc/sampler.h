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

#ifndef TCMALLOC_SAMPLER_H_
#define TCMALLOC_SAMPLER_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

//-------------------------------------------------------------------
// Sampler to decide when to create a sample trace for an allocation
// Not thread safe: Each thread should have its own sampler object.
// Caller must use external synchronization if used
// from multiple threads.
//
// With 512K average sample step (the default):
//  the probability of sampling a 4K allocation is about 0.00778
//  the probability of sampling a 1MB allocation is about 0.865
//  the probability of sampling a 1GB allocation is about 1.00000
// In general, the probability of sampling is an allocation of size X
// given a flag value of Y (default 1M) is:
//  1 - e^(-X/Y)
//
// With 128K average sample step:
//  the probability of sampling a 1MB allocation is about 0.99966
//  the probability of sampling a 1GB allocation is about 1.0
//  (about 1 - 2**(-26))
// With 1M average sample step:
//  the probability of sampling a 4K allocation is about 0.00390
//  the probability of sampling a 1MB allocation is about 0.632
//  the probability of sampling a 1GB allocation is about 1.0
//
// The sampler works by representing memory as a long stream from
// which allocations are taken. Some of the bytes in this stream are
// marked and if an allocation includes a marked byte then it is
// sampled. Bytes are marked according to a Poisson point process
// with each byte being marked independently with probability
// p = 1/profile_sampling_rate.  This makes the probability
// of sampling an allocation of X bytes equal to the CDF of
// a geometric with mean profile_sampling_rate. (ie. the
// probability that at least one byte in the range is marked). This
// is accurately given by the CDF of the corresponding exponential
// distribution : 1 - e^(-X/profile_sampling_rate)
// Independence of the byte marking ensures independence of
// the sampling of each allocation.
//
// This scheme is implemented by noting that, starting from any
// fixed place, the number of bytes until the next marked byte
// is geometrically distributed. This number is recorded as
// bytes_until_sample_.  Every allocation subtracts from this
// number until it is less than 0. When this happens the current
// allocation is sampled.
//
// When an allocation occurs, bytes_until_sample_ is reset to
// a new independtly sampled geometric number of bytes. The
// memoryless property of the point process means that this may
// be taken as the number of bytes after the end of the current
// allocation until the next marked byte. This ensures that
// very large allocations which would intersect many marked bytes
// only result in a single call to PickNextSamplingPoint.
//-------------------------------------------------------------------

class SamplerTest;

class Sampler {
 public:
  // Record allocation of "k" bytes. If the allocation needs to be sampled,
  // return its sampling weight (i.e., the expected number of allocations of
  // this size represented by this sample); otherwise return 0.
  size_t RecordAllocation(size_t k);

  // Same as above (but faster), except:
  // a) REQUIRES(k < std::numeric_limits<ssize_t>::max())
  // b) if this returns false, you must call RecordAllocation
  //    to confirm if sampling truly needed.
  //
  // The point of this function is to only deal with common case of no
  // sampling and let caller (which is in malloc fast-path) to
  // "escalate" to fuller and slower logic only if necessary.
  bool TryRecordAllocationFast(size_t k);

  // If the guarded sampling point has been reached, selects a new sampling
  // point and returns true.  Otherwise returns false.
  Profile::Sample::GuardedStatus ShouldSampleGuardedAllocation();

  // Returns the Sampler's cached tc_globals.IsOnFastPath state.  This may
  // differ from a fresh computation due to activating per-CPU mode or the
  // addition/removal of hooks.
  bool IsOnFastPath() const;
  void UpdateFastPathState();

  // Generate a geometric with mean profile_sampling_rate.
  //
  // Remembers the value of sample_rate for use in reweighing the sample
  // later (so that if the flag value changes before the next sample is taken,
  // the next sample is still weighed properly).
  ssize_t PickNextSamplingPoint();

  // Generates a geometric with mean guarded_sample_rate.
  ssize_t PickNextGuardedSamplingPoint();

  // Returns the current sample period
  static ssize_t GetSamplePeriod();

  // The following are public for the purposes of testing
  static uint64_t NextRandom(uint64_t rnd_);  // Returns the next prng value

  constexpr Sampler()
      : bytes_until_sample_(0),
        sample_period_(0),
        true_bytes_until_sample_(0),
        allocs_until_guarded_sample_(0),
        rnd_(0),
        initialized_(false),
        was_on_fast_path_(false) {}

 private:
  // Bytes until we sample next.
  //
  // More specifically when bytes_until_sample_ is X, we can allocate
  // X bytes without triggering sampling; on the (X+1)th allocated
  // byte, the containing allocation will be sampled.
  //
  // Always non-negative with only very brief exceptions (see
  // DecrementFast{,Finish}, so casting to size_t is ok.
  ssize_t bytes_until_sample_;

  // Saved copy of the sampling period from when we actually set
  // (true_)bytes_until_sample_. This allows us to properly calculate the sample
  // weight of the first sample after the sampling period is changed.
  ssize_t sample_period_;

  // true_bytes_until_sample_ tracks the sampling point when we are on the slow
  // path when picking sampling points (!tc_globals.IsOnFastPath()) up until we
  // notice (due to another allocation) that this state has changed.
  ssize_t true_bytes_until_sample_;

  // Number of sampled allocations until we do a guarded allocation.
  ssize_t allocs_until_guarded_sample_;

  uint64_t rnd_;  // Cheap random number generator
  bool initialized_;
  bool was_on_fast_path_;

 private:
  friend class SamplerTest;
  // Initialize this sampler.
  void Init(uint64_t seed);
  size_t RecordAllocationSlow(size_t k);
  ssize_t GetGeometricVariable(ssize_t mean);
};

inline size_t Sampler::RecordAllocation(size_t k) {
  // The first time we enter this function we expect bytes_until_sample_
  // to be zero, and we must call SampleAllocationSlow() to ensure
  // proper initialization of static vars.
  ASSERT(tc_globals.IsInited() || bytes_until_sample_ == 0);

  // Avoid missampling 0.
  k++;

  // Note that we have to deal with arbitrarily large values of k
  // here. Thus we're upcasting bytes_until_sample_ to unsigned rather
  // than the other way around. And this is why this code cannot be
  // merged with DecrementFast code below.
  if (static_cast<size_t>(bytes_until_sample_) <= k) {
    size_t result = RecordAllocationSlow(k);
    ASSERT(tc_globals.IsInited());
    return result;
  } else {
    bytes_until_sample_ -= k;
    ASSERT(tc_globals.IsInited());
    return 0;
  }
}

inline bool ABSL_ATTRIBUTE_ALWAYS_INLINE
Sampler::TryRecordAllocationFast(size_t k) {
  // Avoid missampling 0.  Callers pass in requested size (which based on the
  // assertion below k>=0 at this point).  Since subtracting 0 from
  // bytes_until_sample_ is a no-op, we increment k by one and resolve the
  // effect on the distribution in Sampler::Unsample.
  k++;

  // For efficiency reason, we're testing bytes_until_sample_ after
  // decrementing it by k. This allows compiler to do sub <reg>, <mem>
  // followed by conditional jump on sign. But it is correct only if k
  // is actually smaller than largest ssize_t value. Otherwise
  // converting k to signed value overflows.
  //
  // It would be great for generated code to be sub <reg>, <mem>
  // followed by conditional jump on 'carry', which would work for
  // arbitrary values of k, but there seem to be no way to express
  // that in C++.
  //
  // Our API contract explicitly states that only small values of k
  // are permitted. And thus it makes sense to assert on that.
  ASSERT(static_cast<ssize_t>(k) > 0);

  bytes_until_sample_ -= static_cast<ssize_t>(k);
  if (ABSL_PREDICT_FALSE(bytes_until_sample_ <= 0)) {
    // Note, we undo sampling counter update, since we're not actually
    // handling slow path in the "needs sampling" case (calling
    // RecordAllocationSlow to reset counter). And we do that in order
    // to avoid non-tail calls in malloc fast-path. See also comments
    // on declaration inside Sampler class.
    //
    // volatile is used here to improve compiler's choice of
    // instructions. We know that this path is very rare and that there
    // is no need to keep previous value of bytes_until_sample_ in
    // register. This helps compiler generate slightly more efficient
    // sub <reg>, <mem> instruction for subtraction above.
    volatile ssize_t* ptr = const_cast<volatile ssize_t*>(&bytes_until_sample_);
    *ptr = *ptr + k;
    return false;
  }
  return true;
}

inline Profile::Sample::GuardedStatus ABSL_ATTRIBUTE_ALWAYS_INLINE
Sampler::ShouldSampleGuardedAllocation() {
  if (Parameters::guarded_sampling_rate() < 0) {
    return Profile::Sample::GuardedStatus::Disabled;
  }
  allocs_until_guarded_sample_--;
  if (ABSL_PREDICT_FALSE(allocs_until_guarded_sample_ < 0)) {
    allocs_until_guarded_sample_ = PickNextGuardedSamplingPoint();
    return Profile::Sample::GuardedStatus::Required;
  }
  return Profile::Sample::GuardedStatus::RateLimited;
}

// Inline functions which are public for testing purposes

// Returns the next prng value.
// pRNG is: aX+b mod c with a = 0x5DEECE66D, b =  0xB, c = 1<<48
// This is the lrand64 generator.
inline uint64_t Sampler::NextRandom(uint64_t rnd) {
  const uint64_t prng_mult = UINT64_C(0x5DEECE66D);
  const uint64_t prng_add = 0xB;
  const uint64_t prng_mod_power = 48;
  const uint64_t prng_mod_mask =
      ~((~static_cast<uint64_t>(0)) << prng_mod_power);
  return (prng_mult * rnd + prng_add) & prng_mod_mask;
}

inline bool Sampler::IsOnFastPath() const { return was_on_fast_path_; }

inline void Sampler::UpdateFastPathState() {
  const bool is_on_fast_path = tc_globals.IsOnFastPath();
  if (ABSL_PREDICT_TRUE(was_on_fast_path_ == is_on_fast_path)) {
    return;
  }

  was_on_fast_path_ = is_on_fast_path;

  if (is_on_fast_path) {
    bytes_until_sample_ = true_bytes_until_sample_;
    true_bytes_until_sample_ = 0;
  } else {
    true_bytes_until_sample_ = bytes_until_sample_;
    bytes_until_sample_ = 0;
  }
}

// Returns the approximate number of bytes that would have been allocated to
// obtain this sample.
double AllocatedBytes(const StackTrace& stack);

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SAMPLER_H_

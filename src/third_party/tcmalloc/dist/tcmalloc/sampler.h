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
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
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
  // b) if this returns false, you must call RecordAllocation
  //    to confirm if sampling truly needed.
  //
  // The point of this function is to only deal with common case of no
  // sampling and let caller (which is in malloc fast-path) to
  // "escalate" to fuller and slower logic only if necessary.
  bool TryRecordAllocationFast(size_t k);

  // Counterpart of TryRecordAllocationFast that needs to be called
  // on the slow path when TryRecordAllocationFast returns false.
  size_t RecordedAllocationFast(size_t k);

  // Check if the next allocation of size "k" will be sampled
  // without changing the internal state.
  bool WillRecordAllocation(size_t k);

  // Generate a geometric with mean profile_sampling_rate.
  //
  // Remembers the value of sample_rate for use in reweighing the sample
  // later (so that if the flag value changes before the next sample is taken,
  // the next sample is still weighed properly).
  ssize_t PickNextSamplingPoint();

  // Returns the current sample period
  static ssize_t GetSamplePeriod();

  // The following are public for the purposes of testing

  // Used to ensure that the hot fields are collocated in the same cache line
  // as __rseq_abi.
  static constexpr size_t HotDataOffset() {
    return offsetof(Sampler, bytes_until_sample_);
  }

  constexpr Sampler()
      : sample_period_(0),
        rnd_(0),
        initialized_(false),
        bytes_until_sample_(0) {}

 private:
  // Saved copy of the sampling period from when we actually set
  // bytes_until_sample_. This allows us to properly calculate the sample
  // weight of the first sample after the sampling period is changed.
  ssize_t sample_period_;

  uint64_t rnd_;  // Cheap random number generator
  bool initialized_;

  // Bytes until we sample next.
  //
  // More specifically when bytes_until_sample_ is X, we can allocate
  // X bytes without triggering sampling; on the (X+1)th allocated
  // byte, the containing allocation will be sampled.
  //
  // Always non-negative with only very brief exceptions (see
  // DecrementFast{,Finish}, so casting to size_t is ok.
  ssize_t bytes_until_sample_;

 private:
  friend class SamplerTest;
  // Initialize this sampler.
  void Init(uint64_t seed);
  size_t RecordAllocationSlow(size_t k);
  ssize_t GetGeometricVariable(ssize_t mean);
};

inline size_t Sampler::RecordAllocation(size_t k) {
  if (!TryRecordAllocationFast(k)) {
    return RecordAllocationSlow(k);
  }
  return 0;
}

inline bool ABSL_ATTRIBUTE_ALWAYS_INLINE
Sampler::TryRecordAllocationFast(size_t k) {
  TC_ASSERT_GE(bytes_until_sample_, 0);

  // Avoid missampling 0.  Callers pass in requested size (which based on the
  // assertion below k>=0 at this point).  Since subtracting 0 from
  // bytes_until_sample_ is a no-op, we increment k by one.
  k++;

  return ABSL_PREDICT_TRUE(!__builtin_usubl_overflow(
      bytes_until_sample_, k, reinterpret_cast<size_t*>(&bytes_until_sample_)));
}

inline size_t Sampler::RecordedAllocationFast(size_t k) {
  // TryRecordAllocationFast already decremented the counter.
  if (ABSL_PREDICT_FALSE(bytes_until_sample_ < 0)) {
    return RecordAllocationSlow(k);
  }
  return 0;
}

inline bool Sampler::WillRecordAllocation(size_t k) {
  return ABSL_PREDICT_FALSE(bytes_until_sample_ < (k + 1));
}

// Returns the approximate number of bytes that would have been allocated to
// obtain this sample.
double AllocatedBytes(const StackTrace& stack);

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SAMPLER_H_

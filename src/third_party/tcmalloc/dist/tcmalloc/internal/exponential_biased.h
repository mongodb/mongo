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

#ifndef TCMALLOC_INTERNAL_EXPONENTIAL_BIASED_H_
#define TCMALLOC_INTERNAL_EXPONENTIAL_BIASED_H_

#include <cstdint>

namespace tcmalloc {
namespace tcmalloc_internal {

class ExponentialBiased {
 public:
  static uint64_t NextRandom(uint64_t rnd);
  static uint32_t GetRandom(uint64_t rnd);
};

// Returns the next prng value.
// pRNG is: aX+b mod c with a = 0x5DEECE66D, b =  0xB, c = 1<<48
// This is the lrand64 generator.
inline uint64_t ExponentialBiased::NextRandom(uint64_t rnd) {
  const uint64_t prng_mult = UINT64_C(0x5DEECE66D);
  const uint64_t prng_add = 0xB;
  const uint64_t prng_mod_power = 48;
  const uint64_t prng_mod_mask =
      ~((~static_cast<uint64_t>(0)) << prng_mod_power);
  return (prng_mult * rnd + prng_add) & prng_mod_mask;
}

// Extracts higher-quality random bits.
// The raw value returned from NextRandom has poor randomness low bits
// and is not directly suitable for things like 'if (rnd % 2)'.
inline uint32_t ExponentialBiased::GetRandom(uint64_t rnd) { return rnd >> 16; }

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_EXPONENTIAL_BIASED_H_

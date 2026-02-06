// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./fuzztest/fuzzing_bit_gen.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "absl/base/fast_type_id.h"
#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "./fuzztest/internal/register_fuzzing_mocks.h"

namespace fuzztest {

FuzzingBitGen::FuzzingBitGen(absl::Span<const uint8_t> data_stream)
    : data_stream_(data_stream) {
  // Seed the internal URBG with the first 8 bytes of the data stream.
  uint64_t stream_seed = 0x6C7FD535EDC7A62D;
  if (!data_stream_.empty()) {
    size_t num_bytes = std::min(sizeof(stream_seed), data_stream_.size());
    std::memcpy(&stream_seed, data_stream_.data(), num_bytes);
    data_stream_.remove_prefix(num_bytes);
  }
  seed(stream_seed);
}

FuzzingBitGen::result_type FuzzingBitGen::operator()() {
  // The non-mockable calls will consume the next 8 bytes from the data
  // stream until it is exhausted, then they will return a value from the
  // internal URBG.
  if (!data_stream_.empty()) {
    result_type x = 0;
    size_t num_bytes = std::min(sizeof(x), data_stream_.size());
    std::memcpy(&x, data_stream_.data(), num_bytes);
    data_stream_.remove_prefix(num_bytes);
    return x;
  }

  // Fallback to the internal URBG.
  state_ = lcg(state_);
  return mix(state_);
}

bool FuzzingBitGen::InvokeMock(absl::FastTypeIdType key_id, void* args_tuple,
                               void* result) {
  using FuzzMapT = absl::flat_hash_map<absl::FastTypeIdType,
                                       internal::TypeErasedFuzzFunctionT>;
  static const absl::NoDestructor<FuzzMapT> fuzzing_map([]() {
    FuzzMapT map;
    auto register_fn = [&map](absl::FastTypeIdType key, auto fn) {
      map[key] = fn;
    };
    internal::RegisterAbslRandomFuzzingMocks(register_fn);
    return map;
  }());

  auto it = fuzzing_map->find(key_id);
  if (it == fuzzing_map->end()) {
    return false;
  }
  it->second(data_stream_, args_tuple, result);
  return true;
}

}  // namespace fuzztest

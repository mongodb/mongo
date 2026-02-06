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

#include "./common/hash.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "./common/defs.h"
#include "./common/sha1.h"

namespace fuzztest::internal {

std::string Hash(ByteSpan span) {
  // Compute SHA1.
  uint8_t sha1[kShaDigestLength];
  SHA1(span.data(), span.size(), sha1);
  // Convert SHA1 to text.
  static_assert(kHashLen == 2 * kShaDigestLength);
  char sha1_hex_text[kHashLen];
  static const char hex[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                               '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  for (size_t i = 0; i < kShaDigestLength; i++) {
    sha1_hex_text[i * 2 + 0] = hex[sha1[i] / 16];
    sha1_hex_text[i * 2 + 1] = hex[sha1[i] % 16];
  }
  return {sha1_hex_text, sha1_hex_text + kHashLen};
}

std::string Hash(std::string_view str) {
  static_assert(sizeof(decltype(str)::value_type) == sizeof(uint8_t));
  return Hash(AsByteSpan(str));
}

}  // namespace fuzztest::internal

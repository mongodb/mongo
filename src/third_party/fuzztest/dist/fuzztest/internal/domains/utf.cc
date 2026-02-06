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

#include "./fuzztest/internal/domains/utf.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "./fuzztest/internal/domains/rune.h"

namespace fuzztest::internal {

std::string EncodeAsUTF8(const std::vector<int>& code_points) {
  std::string out;
  out.reserve(code_points.size());
  for (int c : code_points) {
    if ((static_cast<uint32_t>(c) < 0xD800) || (c >= 0xE000 && c <= 0x10FFFF)) {
      char buf[4];
      out.append(std::string(buf, runetochar(buf, &c)));
    } else {
      static constexpr char ReplacementChars[] = {'\xEF', '\xBF', '\xBD'};
      out.append(ReplacementChars, sizeof(ReplacementChars));
    }
  }
  return out;
}

std::optional<std::vector<int>> DecodeFromUTF8(const std::string& utf8) {
  std::vector<int> out;
  absl::string_view in(utf8);
  out.reserve(in.size());
  while (!in.empty()) {
    Rune r;
    int len = chartorune(&r, in.data());
    out.push_back(r);
    if (r == Runeerror && len != 3) return std::nullopt;
    in.remove_prefix(len);
  }
  return out;
}

}  // namespace fuzztest::internal

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

#ifndef FUZZTEST_INTERNAL_DOMAINS_UTF_H_
#define FUZZTEST_INTERNAL_DOMAINS_UTF_H_

#include <optional>
#include <string>
#include <vector>

namespace fuzztest::internal {

// Encode a sequence of code points as UTF-8 string.
std::string EncodeAsUTF8(const std::vector<int>& code_points);

// Decode a UTF-8 string into a sequence of code points. Returns nullopt if the
// string is not valid UTF-8.
std::optional<std::vector<int>> DecodeFromUTF8(const std::string& utf8);

}  // namespace fuzztest::internal

#endif  // FUZZTEST_INTERNAL_DOMAINS_UTF_H_

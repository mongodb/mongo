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

#ifndef FUZZTEST_FUZZTEST_INTERNAL_ESCAPING_H_
#define FUZZTEST_FUZZTEST_INTERNAL_ESCAPING_H_

#include <string>

#include "absl/strings/string_view.h"

namespace fuzztest::internal {

// Returns a Bourne shell string literal to be expanded into `s`
std::string ShellEscape(absl::string_view s);

}  // namespace fuzztest::internal

#endif  // FUZZTEST_FUZZTEST_INTERNAL_ESCAPING_H_

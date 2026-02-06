// Copyright 2023 The Centipede Authors.
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

#include "./centipede/runner_flags.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace fuzztest::internal {

RunnerFlags::RunnerFlags(const std::string& runner_flags_string) {
  size_t pos = 0;
  while (pos != std::string::npos) {
    // Extract substring from pos up to next ':'.
    size_t colon_pos = runner_flags_string.find_first_of(':', pos);
    std::string flag;
    if (colon_pos == std::string::npos) {
      flag = runner_flags_string.substr(pos);
      pos = std::string::npos;
    } else {
      flag = runner_flags_string.substr(pos, colon_pos - pos);
      pos = runner_flags_string.find_first_not_of(':', colon_pos);
    }

    if (!flag.empty()) {
      std::string value;
      // Check to see if flag has a value.
      size_t assignment_pos = flag.find_first_of('=');
      if (assignment_pos != std::string::npos) {
        value = flag.substr(assignment_pos + 1);
        flag.resize(assignment_pos);
      }

      // We do not check for duplicate flags in input. Multiple instances of
      // a flags are inserted in the order they appear input.
      if (!flag.empty()) {  // ignore malformed flag "=<value>"
        flags_.push_back(std::make_pair(flag, value));
      }
    }
  }
}

std::string RunnerFlags::ToString() const {
  if (flags_.empty()) return "";

  std::vector<std::string> output_fragments;
  output_fragments.reserve(flags_.size() + 1);
  size_t output_size = 0;
  for (const auto& [flag, value] : flags_) {
    std::string s = ":" + flag;
    if (!value.empty()) {
      s += "=" + value;
    }
    output_size += s.size();
    output_fragments.push_back(s);
  }

  // Add a trailing ':' so that output starts and ends with ':'s.
  output_fragments.push_back(":");
  output_size++;

  // Join fragments to form output.  Reserve output size before joining to
  // avoid a quadratic behavior.
  std::string output;
  output.reserve(output_size);
  for (const auto& fragment : output_fragments) {
    output.append(fragment);
  }
  return output;
}

}  // namespace fuzztest::internal

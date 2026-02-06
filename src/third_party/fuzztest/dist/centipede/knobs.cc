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

#include "./centipede/knobs.h"

#include <cstdio>
#include <string_view>

namespace fuzztest::internal {
size_t Knobs::next_id_ = 0;
std::string_view Knobs::knob_names_[kNumKnobs];

KnobId Knobs::NewId(std::string_view knob_name) {
  if (next_id_ >= kNumKnobs) {
    // If we've run out of IDs, log using stderr (don't use extra deps).
    fprintf(stderr, "Knobs::NewId: no more IDs left, aborting\n");
    __builtin_trap();
  }
  knob_names_[next_id_] = knob_name;
  return next_id_++;
}
}  // namespace fuzztest::internal

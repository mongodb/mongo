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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "absl/strings/string_view.h"
#include "tcmalloc/experiment.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t size) {
  const char* data = reinterpret_cast<const char*>(d);

  bool buffer[tcmalloc::tcmalloc_internal::kNumExperiments];
  absl::string_view active, disabled;

  const char* split = static_cast<const char*>(memchr(data, ';', size));
  if (split == nullptr) {
    active = absl::string_view(data, size);
  } else {
    active = absl::string_view(data, split - data);
    disabled = absl::string_view(split + 1, size - (split - data + 1));
  }

  tcmalloc::tcmalloc_internal::SelectExperiments(buffer, active, disabled);
  return 0;
}

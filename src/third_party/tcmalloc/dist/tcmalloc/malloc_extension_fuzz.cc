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

#include <stdint.h>

#include <map>
#include <string>

#include "absl/types/optional.h"
#include "tcmalloc/malloc_extension.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t size) {
  using tcmalloc::MallocExtension;

  const std::string property(reinterpret_cast<const char*>(d), size);
  absl::optional<size_t> val = MallocExtension::GetNumericProperty(property);
  if (!val.has_value()) {
    // Rather than inspect the result of MallocExtension::GetProperties, we
    // defer to the test in //tcmalloc/malloc_extension_test.cc to
    // ensure that every key in GetProperties has a value returned by
    // GetNumericProperty.
    return 0;
  }

  std::map<std::string, MallocExtension::Property> properties =
      MallocExtension::GetProperties();
  if (properties.find(property) == properties.end()) {
    __builtin_trap();
  }
  return 0;
}

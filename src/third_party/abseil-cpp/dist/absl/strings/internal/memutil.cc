// Copyright 2017 The Abseil Authors.
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

#include "absl/strings/internal/memutil.h"

#include <cstdlib>

#include "absl/strings/ascii.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace strings_internal {

int memcasecmp(const char* s1, const char* s2, size_t len) {
  const unsigned char* us1 = reinterpret_cast<const unsigned char*>(s1);
  const unsigned char* us2 = reinterpret_cast<const unsigned char*>(s2);

  for (size_t i = 0; i < len; i++) {
    const int diff =
        int{static_cast<unsigned char>(absl::ascii_tolower(us1[i]))} -
        int{static_cast<unsigned char>(absl::ascii_tolower(us2[i]))};
    if (diff != 0) return diff;
  }
  return 0;
}

}  // namespace strings_internal
ABSL_NAMESPACE_END
}  // namespace absl

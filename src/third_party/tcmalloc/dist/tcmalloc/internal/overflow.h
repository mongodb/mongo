// Copyright 2022 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_OVERFLOW_H_
#define TCMALLOC_INTERNAL_OVERFLOW_H_

#include <cstddef>

#include "absl/base/config.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

inline bool MultiplyOverflow(size_t a, size_t b, size_t* out) {
#if ABSL_HAVE_BUILTIN(__builtin_mul_overflow)
  return __builtin_mul_overflow(a, b, out);
#else
  *out = a * b;
  return b != 0 && *out / b != a;
#endif
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_OVERFLOW_H_

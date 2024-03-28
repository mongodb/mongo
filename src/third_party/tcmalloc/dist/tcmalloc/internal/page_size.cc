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

#include "tcmalloc/internal/page_size.h"

#include <unistd.h>

#include <cstddef>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

size_t GetPageSize() {
  ABSL_CONST_INIT static size_t page_size;
  ABSL_CONST_INIT static absl::once_flag flag;

  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
#if defined(__wasm__) || defined(__asmjs__)
    page_size = static_cast<size_t>(getpagesize());
#else
      page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
  });
  return page_size;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

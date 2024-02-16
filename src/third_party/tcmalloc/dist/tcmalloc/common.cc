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

#include "tcmalloc/common.h"

#include <algorithm>

#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/span.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

absl::string_view MemoryTagToLabel(MemoryTag tag) {
  switch (tag) {
    case MemoryTag::kNormal:
      return "NORMAL";
    case MemoryTag::kNormalP1:
      return "NORMAL_P1";
    case MemoryTag::kSampled:
      return "SAMPLED";
    case MemoryTag::kCold:
      return "COLD";
    default:
      ASSUME(false);
  }
}

// This only provides correct answer for TCMalloc-allocated memory,
// and may give a false positive for non-allocated block.
extern "C" bool TCMalloc_Internal_PossiblyCold(const void* ptr) {
  return IsColdMemory(ptr);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

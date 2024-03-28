// Copyright 2021 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_CACHE_TOPOLOGY_H_
#define TCMALLOC_INTERNAL_CACHE_TOPOLOGY_H_

#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class CacheTopology {
 public:
  static CacheTopology& Instance() {
    ABSL_CONST_INIT static CacheTopology instance;
    return instance;
  }

  constexpr CacheTopology() = default;

  void Init();

  unsigned l3_count() const { return l3_count_; }

  unsigned GetL3FromCpuId(int cpu) const {
    TC_ASSERT_GE(cpu, 0);
    TC_ASSERT_LT(cpu, cpu_count_);
    return l3_cache_index_[cpu];
  }

 private:
  unsigned cpu_count_ = 0;
  unsigned l3_count_ = 0;
  uint8_t l3_cache_index_[CPU_SETSIZE] = {};
};

// Helper function exposed to permit testing it.
int BuildCpuToL3CacheMap_FindFirstNumberInBuf(absl::string_view current);

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_CACHE_TOPOLOGY_H_

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
//
// Provides skeleton RSEQ functions which raise a hard error in the case of
// being erroneously called on an unsupported platform.

#include "tcmalloc/internal/percpu.h"

#if !TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace subtle {
namespace percpu {

static void Unsupported() {
  TC_BUG("RSEQ function called on unsupported platform.");
}

int TcmallocSlab_Internal_PerCpuCmpxchg64(int target_cpu, intptr_t* p,
                                          intptr_t old_val, intptr_t new_val,
                                          size_t virtual_cpu_id_offset) {
  Unsupported();
  return -1;
}

size_t TcmallocSlab_Internal_PushBatch(size_t size_class, void** batch,
                                       size_t len, uintptr_t slabs_and_shift,
                                       size_t virtual_cpu_id_offset) {
  Unsupported();
  return 0;
}

size_t TcmallocSlab_Internal_PopBatch(size_t size_class, void** batch,
                                      size_t len, uintptr_t slabs_and_shift,
                                      size_t virtual_cpu_id_offset) {
  Unsupported();
  return 0;
}

int PerCpuReadCycleCounter(int64_t* cycles) {
  Unsupported();
  return -1;
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // !TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM

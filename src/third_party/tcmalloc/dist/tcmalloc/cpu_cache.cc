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

#include "tcmalloc/cpu_cache.h"

#include <stdlib.h>

#include <cstdint>
#include <new>

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

static void ActivatePerCpuCaches() {
  if (tcmalloc::tcmalloc_internal::tc_globals.CpuCacheActive()) {
    // Already active.
    return;
  }

  if (Parameters::per_cpu_caches() && subtle::percpu::IsFast()) {
    tc_globals.InitIfNecessary();
    tc_globals.cpu_cache().Activate();
    tc_globals.ActivateCpuCache();
    // no need for this thread cache anymore, I guess.
    ThreadCache::BecomeIdle();
    // If there's a problem with this code, let's notice it right away:
    ::operator delete(::operator new(1));
  }
}

class PerCPUInitializer {
 public:
  PerCPUInitializer() {
   ActivatePerCpuCaches();
  }
};
static PerCPUInitializer module_enter_exit;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

extern "C" void TCMalloc_Internal_ForceCpuCacheActivation() {
  tcmalloc::tcmalloc_internal::ActivatePerCpuCaches();
}

extern "C" bool MallocExtension_Internal_GetPerCpuCachesActive() {
  return tcmalloc::tcmalloc_internal::tc_globals.CpuCacheActive();
}

extern "C" int32_t MallocExtension_Internal_GetMaxPerCpuCacheSize() {
  return tcmalloc::tcmalloc_internal::Parameters::max_per_cpu_cache_size();
}

extern "C" void MallocExtension_Internal_SetMaxPerCpuCacheSize(int32_t value) {
  tcmalloc::tcmalloc_internal::Parameters::set_max_per_cpu_cache_size(value);
}

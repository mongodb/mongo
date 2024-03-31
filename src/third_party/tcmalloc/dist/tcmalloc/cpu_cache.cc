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
#include <unistd.h>

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

////////////////////////////////////////////////////////////
//////// START MONGO HACK
// Code is copied from processinfo_linux.cpp and modified to not depend on MongoDB C++ code.
namespace {
std::string parseLineFromFile(const char* fname) {
    FILE* f;
    char fstr[1024] = {0};

    f = fopen(fname, "r");
    if (f != nullptr) {
        if (fgets(fstr, 1023, f) != nullptr)
            fstr[strlen(fstr) < 1 ? 0 : strlen(fstr) - 1] = '\0';
        fclose(f);
    }

    return fstr;
}

unsigned long long getSystemMemorySizeBytes() {
    return sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
}

/**
 * Get memory limit for the process.
 * If memory is being limited by the applied control group and it's less
 * than the OS system memory (default cgroup limit is ulonglong max) let's
 * return the actual memory we'll have available to the process.
 */
unsigned long long getMemorySizeLimitInBytes() {
    const unsigned long long systemMemBytes = getSystemMemorySizeBytes();
    for (const char* file : {
              "/sys/fs/cgroup/memory.max",                   // cgroups v2
              "/sys/fs/cgroup/memory/memory.limit_in_bytes"  // cgroups v1
          }) {

        unsigned long long groupMemBytes = 0;
        std::string groupLimit = parseLineFromFile(file);

        if (!groupLimit.empty() ) {
            return std::min(systemMemBytes, (unsigned long long)atoll(groupLimit.c_str()));
        }
    }
    return systemMemBytes;
}

}

//////// END  MONGO HACK
////////////////////////////////////////////////////////////

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

////////////////////////////////////////////////////////////
//////// START MONGO HACK

unsigned long long getMongoMaxCpuCacheSize(size_t numCpus) {

    char* userCacheSizeBytes = getenv("MONGO_TCMALLOC_PER_CPU_CACHE_SIZE_BYTES");
    if (userCacheSizeBytes != nullptr) {
        auto value = atoll(userCacheSizeBytes);
        if (value != 0) {
            return value;
        }
    }

    // 1024MB in bytes spread across cores.
    size_t systemMemorySizeMB = getMemorySizeLimitInBytes() / (1024 * 1024);
    size_t defaultTcMallocPerCPUCacheSize = (1024 * 1024 * 1024) / numCpus;
    size_t derivedTcMallocPerCPUCacheSize =
        ((systemMemorySizeMB / 4) * 1024 * 1024) / numCpus;  // 1/4 of system memory in bytes

    size_t perCPUCacheSize =
        std::min(defaultTcMallocPerCPUCacheSize, derivedTcMallocPerCPUCacheSize);
    return perCPUCacheSize;
}

//////// END MONGO HACK
////////////////////////////////////////////////////////////

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

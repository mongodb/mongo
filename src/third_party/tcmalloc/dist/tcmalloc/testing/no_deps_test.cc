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
// Simple malloc test that does not depend on //base.
// Useful as the first test during tcmalloc development: first, it builds fast;
// second, it does not trigger rebuild of host binaries and regeneration of
// auto-generated files as the result a crash happens in this test rather than
// in a host binary executed from blaze.

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>

#include "absl/base/macros.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"

const size_t kMem = 10 << 20;
const size_t kMin = 8;
void* blocks[kMem / kMin];

int main() {
  if (&TCMalloc_Internal_ForceCpuCacheActivation != nullptr) {
    TCMalloc_Internal_ForceCpuCacheActivation();
  }

  size_t step = 16;
  // malloc-free
  for (size_t size = kMin; size <= kMem; size += step) {
    const size_t count = kMem / size;
    for (size_t i = 0; i < count; i++) {
      blocks[i] = malloc(size);
    }
    for (size_t i = 0; i < count; i++) {
      free(blocks[i]);
    }
    step = std::max(step, size / 32);
  }

  // new-delete
  step = 16;
  for (size_t size = kMin; size <= kMem; size += step) {
    const size_t count = kMem / size;
    for (size_t i = 0; i < count; i++) {
      blocks[i] = ::operator new(size);
    }
    for (size_t i = 0; i < count; i++) {
#ifdef __cpp_sized_deallocation
      if (i % 2 == 0) {
        ::operator delete(blocks[i], size);
        continue;
      }
#endif
      ::operator delete(blocks[i]);
    }
    step = std::max(step, size / 32);
  }

  constexpr const char* kProperties[] = {
      // TODO(b/329837900):  Source all properties from GetProperties, rather
      // than a hard-coded list.
      //
      // go/keep-sorted start
      "generic.bytes_in_use_by_app", "generic.current_allocated_bytes",
      "generic.heap_size", "generic.peak_memory_usage",
      "generic.physical_memory_used", "generic.virtual_memory_used",
      "tcmalloc.central_cache_free", "tcmalloc.cpu_free",
      "tcmalloc.current_total_thread_cache_bytes",
      // TODO(b/329837900): Add tcmalloc.metadata_bytes to this list.
      "tcmalloc.pageheap_free_bytes", "tcmalloc.pageheap_unmapped_bytes",
      "tcmalloc.per_cpu_caches_active", "tcmalloc.sharded_transfer_cache_free",
      "tcmalloc.transfer_cache_free",
      // go/keep-sorted end
  };
  int scalar_values[ABSL_ARRAYSIZE(kProperties)];

  if (&MallocExtension_Internal_GetProperties == nullptr ||
      &MallocExtension_Internal_GetNumericProperty == nullptr) {
    // Skip rest of test since we don't link against TCMalloc.  This should only
    // happen under sanitizers.
#if !defined(ABSL_HAVE_ADDRESS_SANITIZER) && \
    !defined(ABSL_HAVE_MEMORY_SANITIZER) &&  \
    !defined(ABSL_HAVE_THREAD_SANITIZER)
    assert(false &&
           "Not linked against TCMalloc, but not built with sanitizers.");
#endif

    return 0;
  }

  std::map<std::string, tcmalloc::MallocExtension::Property> properties;
  // Run GetProperties twice.  Once to use some RAM, which will skew our stats,
  // and a second time for the actual read-out.
  MallocExtension_Internal_GetProperties(&properties);

  for (int i = 0; i < ABSL_ARRAYSIZE(kProperties); ++i) {
    const char* property = kProperties[i];
    size_t value;
    bool ret = MallocExtension_Internal_GetNumericProperty(
        property, strlen(property), &value);

    scalar_values[i] = ret ? value : 0;
  }

  MallocExtension_Internal_GetProperties(&properties);
  for (int i = 0; i < ABSL_ARRAYSIZE(kProperties); ++i) {
    const char* property = kProperties[i];
    const size_t scalar_value = scalar_values[i];
    const size_t set_value = properties[property].value;

    const size_t tolerance = set_value * 0.01;

    fprintf(stderr, "property '%s': scalar %zu versus set %zu\n", property,
            scalar_value, set_value);
    if (scalar_value < set_value - tolerance &&
        scalar_value > set_value + tolerance) {
      abort();
    }
  }
}

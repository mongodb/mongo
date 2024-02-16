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
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "tcmalloc/internal_malloc_extension.h"

const size_t kMem = 10 << 20;
const size_t kMin = 8;
void* blocks[kMem / kMin];

int main() {
  const char* env = getenv("FORCE_CPU_CACHES");
  if (env != nullptr && strcmp(env, "1") == 0) {
    if (&TCMalloc_Internal_ForceCpuCacheActivation != nullptr) {
      TCMalloc_Internal_ForceCpuCacheActivation();
    }
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
}

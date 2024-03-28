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
// This tests the accounting done by tcmalloc.  When we allocate and
// free a small buffer, the number of bytes used by the application
// before the alloc+free should match the number of bytes used after.
// However, the internal data structures used by tcmalloc will be
// quite different -- new spans will have been allocated, etc.  This
// is, thus, a simple test that we account properly for the internal
// data structures, so that we report the actual application-used
// bytes properly.

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>

#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

int main() {
  const char kCurrent[] = "generic.current_allocated_bytes";
  size_t before_bytes =
      *tcmalloc::MallocExtension::GetNumericProperty(kCurrent);

  free(malloc(200));

  size_t after_bytes = *tcmalloc::MallocExtension::GetNumericProperty(kCurrent);
  TC_CHECK_EQ(before_bytes, after_bytes);

  // Do a lot of different allocs in a lot of different size classes,
  // then free them all, to make sure that the logic is correct.
  void* ptrs[1000];  // how many pointers to allocate in one run
  for (int size = 1; size < 1000000;
       size = std::max(size + 1, size * 2 - 100)) {
    for (int cycles = 0; cycles < 2; ++cycles) {
      for (int repeat = 0; repeat < sizeof(ptrs) / sizeof(*ptrs); ++repeat) {
        ptrs[repeat] = malloc(size);
      }
      for (int repeat = 0; repeat < sizeof(ptrs) / sizeof(*ptrs); ++repeat) {
        free(ptrs[repeat]);
      }
    }
  }

  after_bytes = *tcmalloc::MallocExtension::GetNumericProperty(kCurrent);
  TC_CHECK_EQ(before_bytes, after_bytes);

  printf("PASS\n");
  return 0;
}

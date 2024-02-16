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

#include <stddef.h>
#include <stdlib.h>

#include <map>
#include <string>

#include "gtest/gtest.h"
#include "tcmalloc/common.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace {

// Regression test for b/31102171.
// Ensure that when we allocate lots of kMinSystemAlloc + epsilon blocks,
// tcmalloc does not double memory consumption.
TEST(LargeAllocSizeTest, Basic) {
  typedef std::map<std::string, MallocExtension::Property> PropertyMap;
  PropertyMap map = MallocExtension::GetProperties();
  const size_t start_mem = map["generic.physical_memory_used"].value;
  const size_t kTotalToAllocate = 1024 << 20;
  const size_t kAllocSize =
      tcmalloc_internal::kMinSystemAlloc + tcmalloc_internal::kPageSize;
  const size_t kBlocks = kTotalToAllocate / kAllocSize;
  void* volatile blocks[kBlocks];
  for (size_t i = 0; i < kBlocks; ++i) {
    blocks[i] = malloc(kAllocSize);
  }
  map = MallocExtension::GetProperties();
  const size_t end_mem = map["generic.physical_memory_used"].value;
  for (size_t i = 0; i < kBlocks; ++i) {
    free(blocks[i]);
  }
  EXPECT_LE(end_mem - start_mem, kTotalToAllocate * 1.3)
      << "start: " << (start_mem >> 20) << "MB -> "
      << "end: " << (end_mem >> 20) << "MB "
      << "(+" << ((end_mem - start_mem) >> 20) << "MB)";
}

}  // namespace
}  // namespace tcmalloc

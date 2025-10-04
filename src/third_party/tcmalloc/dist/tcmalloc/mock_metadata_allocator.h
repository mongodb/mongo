// Copyright 2023 The TCMalloc Authors
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

#ifndef TCMALLOC_MOCK_METADATA_ALLOCATOR_H_
#define TCMALLOC_MOCK_METADATA_ALLOCATOR_H_

#include <cstdlib>
#include <vector>

#include "absl/base/attributes.h"
#include "tcmalloc/metadata_allocator.h"

namespace tcmalloc::tcmalloc_internal {

class FakeMetadataAllocator final : public MetadataAllocator {
 public:
  ~FakeMetadataAllocator() override {
    for (void* p : metadata_allocs_) {
      free(p);
    }
  }

  ABSL_MUST_USE_RESULT void* operator()(size_t size) override {
    void* ptr = malloc(size);
    metadata_allocs_.push_back(ptr);
    return ptr;
  }

 private:
  std::vector<void*> metadata_allocs_;
};

}  // namespace tcmalloc::tcmalloc_internal

#endif  // TCMALLOC_MOCK_METADATA_ALLOCATOR_H_

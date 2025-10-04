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

#ifndef TCMALLOC_METADATA_ALLOCATOR_H_
#define TCMALLOC_METADATA_ALLOCATOR_H_

#include <cstddef>

#include "absl/base/attributes.h"

namespace tcmalloc::tcmalloc_internal {

class MetadataAllocator {
 public:
  MetadataAllocator() = default;
  virtual ~MetadataAllocator() = default;

  MetadataAllocator(const MetadataAllocator&) = delete;
  MetadataAllocator(MetadataAllocator&&) = delete;
  MetadataAllocator& operator=(const MetadataAllocator&) = delete;
  MetadataAllocator& operator=(MetadataAllocator&&) = delete;

  // Allocates bytes suitable for metadata.
  ABSL_MUST_USE_RESULT virtual void* operator()(size_t bytes) = 0;
};

}  // namespace tcmalloc::tcmalloc_internal

#endif  // TCMALLOC_METADATA_ALLOCATOR_H_

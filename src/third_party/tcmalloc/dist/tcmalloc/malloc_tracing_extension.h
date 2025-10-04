// Copyright 2022 The TCMalloc Authors
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

// Extra extensions exported by some malloc implementations.  These
// extensions are accessed through a virtual base class so an
// application can link against a malloc that does not implement these
// extensions, and it will get default versions that do nothing.

#ifndef TCMALLOC_MALLOC_TRACING_EXTENSION_H_
#define TCMALLOC_MALLOC_TRACING_EXTENSION_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/status/statusor.h"

namespace tcmalloc {
namespace malloc_tracing_extension {

// Type used by GetAllocatedAddressRanges. Contains details of address ranges
// that have a corresponding Span in TCMalloc.
struct AllocatedAddressRanges {
  struct SpanDetails {
    uintptr_t start_addr;
    size_t size;
    // For Spans with objects that fit into some size-class, object_size is
    // actually the size-class bytes, not the exact object size bytes.
    // This is zero for non-size-class objects that are objects larger than
    // kMaxSize.
    size_t object_size;
  };
  // Note that any subset of size-class-sized objects may be currently
  // allocated from each Span.
  std::vector<SpanDetails> spans;
};
// Returns the address ranges currently allocated by TCMalloc.
absl::StatusOr<AllocatedAddressRanges> GetAllocatedAddressRanges();

}  // namespace malloc_tracing_extension
}  // namespace tcmalloc

#endif  // TCMALLOC_MALLOC_TRACING_EXTENSION_H_

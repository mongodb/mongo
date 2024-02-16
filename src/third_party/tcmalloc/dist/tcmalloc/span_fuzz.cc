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

#include <algorithm>
#include <vector>

#include "tcmalloc/common.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/span.h"

using tcmalloc::tcmalloc_internal::kMaxObjectsToMove;
using tcmalloc::tcmalloc_internal::kPageSize;
using tcmalloc::tcmalloc_internal::Length;
using tcmalloc::tcmalloc_internal::PageIdContaining;
using tcmalloc::tcmalloc_internal::SizeMap;
using tcmalloc::tcmalloc_internal::Span;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Extract fuzz input into 3 integers.
  size_t state[3];
  if (size != sizeof(state)) {
    return 0;
  }
  memcpy(state, data, size);

  const size_t object_size = state[0];
  const size_t num_pages = state[1];
  const size_t num_to_move = state[2];

  if (!SizeMap::IsValidSizeClass(object_size, num_pages, num_to_move)) {
    // Invalid size class configuration, but ValidSizeClass detected that.
    return 0;
  }

  const auto pages = Length(num_pages);
  const size_t objects_per_span = pages.in_bytes() / object_size;

  void* mem;
  int res = posix_memalign(&mem, kPageSize, pages.in_bytes());
  CHECK_CONDITION(res == 0);
  Span span;
  span.Init(PageIdContaining(mem), pages);
  span.BuildFreelist(object_size, objects_per_span, nullptr, 0);

  CHECK_CONDITION(span.Allocated() == 0);

  std::vector<void*> ptrs;
  ptrs.reserve(objects_per_span);

  while (ptrs.size() < objects_per_span) {
    size_t want = std::min(num_to_move, objects_per_span - ptrs.size());
    CHECK_CONDITION(want > 0);
    void* batch[kMaxObjectsToMove];
    CHECK_CONDITION(!span.FreelistEmpty(object_size));
    size_t n = span.FreelistPopBatch(batch, want, object_size);

    CHECK_CONDITION(n > 0);
    CHECK_CONDITION(n <= want);
    CHECK_CONDITION(n <= kMaxObjectsToMove);
    ptrs.insert(ptrs.end(), batch, batch + n);
  }

  CHECK_CONDITION(span.FreelistEmpty(object_size));
  CHECK_CONDITION(ptrs.size() == objects_per_span);
  CHECK_CONDITION(ptrs.size() == span.Allocated());

  for (size_t i = 0, popped = ptrs.size(); i < popped; ++i) {
    bool ok = span.FreelistPush(ptrs[i], object_size);
    CHECK_CONDITION(ok == (i != popped - 1));
    // If the freelist becomes full, then the span does not actually push the
    // element onto the freelist.
    //
    // For single object spans, the freelist always stays "empty" as a result.
    CHECK_CONDITION(popped == 1 || !span.FreelistEmpty(object_size));
  }

  free(mem);

  return 0;
}

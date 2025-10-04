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

#ifndef TCMALLOC_MOCK_VIRTUAL_ALLOCATOR_H_
#define TCMALLOC_MOCK_VIRTUAL_ALLOCATOR_H_

#include <cstddef>
#include <vector>

#include "absl/base/attributes.h"
#include "tcmalloc/huge_allocator.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/system-alloc.h"

namespace tcmalloc::tcmalloc_internal {

class FakeVirtualAllocator final : public VirtualAllocator {
 public:
  ABSL_MUST_USE_RESULT AddressRange operator()(size_t bytes,
                                               size_t align) override;

  static constexpr size_t kMaxBacking = 1024 * 1024;
  // This isn't super good form but we'll never have more than one HAT
  // extant at once.
  std::vector<size_t> backing_;

  bool should_overallocate_ = false;
  HugeLength huge_pages_requested_;
  HugeLength huge_pages_received_;
};

// Use a tiny fraction of actual size so we can test aggressively.
inline AddressRange FakeVirtualAllocator::operator()(size_t bytes,
                                                     size_t align) {
  TC_CHECK_EQ(bytes % kHugePageSize, 0);
  TC_CHECK_EQ(align % kHugePageSize, 0);
  HugeLength req = HLFromBytes(bytes);
  huge_pages_requested_ += req;
  // Test the case where our sys allocator provides too much.
  if (should_overallocate_) ++req;
  huge_pages_received_ += req;
  // we'll actually provide hidden backing, one word per hugepage.
  bytes = req / NHugePages(1);
  align /= kHugePageSize;
  size_t index = backing_.size();
  if (index % align != 0) {
    index += (align - (index & align));
  }
  if (index + bytes > kMaxBacking) return {nullptr, 0};
  backing_.resize(index + bytes);
  void* ptr = reinterpret_cast<void*>(index * kHugePageSize);
  return {ptr, req.in_bytes()};
}

}  // namespace tcmalloc::tcmalloc_internal

#endif  // TCMALLOC_MOCK_VIRTUAL_ALLOCATOR_H_

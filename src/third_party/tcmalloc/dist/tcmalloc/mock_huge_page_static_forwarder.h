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

#ifndef TCMALLOC_MOCK_HUGE_PAGE_STATIC_FORWARDER_H_
#define TCMALLOC_MOCK_HUGE_PAGE_STATIC_FORWARDER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"
#include "absl/numeric/bits.h"
#include "absl/time/time.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace huge_page_allocator_internal {

class FakeStaticForwarder {
 public:
  // Runtime parameters.  This can change between calls.
  absl::Duration filler_skip_subrelease_interval() {
    return subrelease_interval_;
  }
  absl::Duration filler_skip_subrelease_short_interval() {
    return short_interval_;
  }
  absl::Duration filler_skip_subrelease_long_interval() {
    return long_interval_;
  }
  bool release_partial_alloc_pages() { return release_partial_alloc_pages_; }
  bool hpaa_subrelease() { return hpaa_subrelease_; }

  void set_filler_skip_subrelease_interval(absl::Duration v) {
    subrelease_interval_ = v;
  }
  void set_filler_skip_subrelease_short_interval(absl::Duration v) {
    short_interval_ = v;
  }
  void set_filler_skip_subrelease_long_interval(absl::Duration v) {
    long_interval_ = v;
  }
  void set_release_partial_alloc_pages(bool v) {
    release_partial_alloc_pages_ = v;
  }
  void set_hpaa_subrelease(bool v) { hpaa_subrelease_ = v; }
  bool release_succeeds() const { return release_succeeds_; }
  void set_release_succeeds(bool v) { release_succeeds_ = v; }

  // Arena state.
  Arena& arena() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) { return arena_; }

  // PageAllocator state.

  // Check page heap memory limit.  `n` indicates the size of the allocation
  // currently being made, which will not be included in the sampled memory heap
  // for realized fragmentation estimation.
  void ShrinkToUsageLimit(Length n)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {}

  // PageMap state.
  void* GetHugepage(HugePage p) {
    auto it = trackers_.find(p);
    if (it == trackers_.end()) {
      return nullptr;
    }
    return it->second;
  }
  bool Ensure(PageId page, Length length)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return true;
  }
  void Set(PageId page, Span* span) {}
  void SetHugepage(HugePage p, void* pt) { trackers_[p] = pt; }

  // SpanAllocator state.
  Span* NewSpan(PageId page, Length length) ABSL_EXCLUSIVE_LOCKS_REQUIRED(
      pageheap_lock) ABSL_ATTRIBUTE_RETURNS_NONNULL {
    Span* span;
    void* result = absl::base_internal::LowLevelAlloc::AllocWithArena(
        sizeof(*span) + alignof(Span) + sizeof(void*), ll_arena());
    span = new (reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(result) + alignof(Span) - 1u) &
        ~(alignof(Span) - 1u))) Span();
    *(reinterpret_cast<uintptr_t*>(span + 1)) =
        reinterpret_cast<uintptr_t>(result);
    span->Init(page, length);
    return span;
  }
  void DeleteSpan(Span* span) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock)
      ABSL_ATTRIBUTE_NONNULL() {
    absl::base_internal::LowLevelAlloc::Free(
        reinterpret_cast<void*>(*(reinterpret_cast<uintptr_t*>(span + 1))));
  }

  // SystemAlloc state.
  AddressRange AllocatePages(size_t bytes, size_t align, MemoryTag tag) {
    TC_CHECK(absl::has_single_bit(align), "align=%v", align);
    fake_allocation_ = (fake_allocation_ + align - 1u) & ~(align - 1u);

    AddressRange ret{
        reinterpret_cast<void*>(fake_allocation_ |
                                (static_cast<uintptr_t>(tag) << kTagShift)),
        bytes};
    fake_allocation_ += bytes;
    return ret;
  }
  // TODO(ckennelly): Accept PageId/Length.
  bool ReleasePages(void* ptr, size_t size) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~kTagMask;
    const uintptr_t end = start + size;
    TC_CHECK_LE(end, fake_allocation_);

    return release_succeeds_;
  }

 private:
  static absl::base_internal::LowLevelAlloc::Arena* ll_arena() {
    ABSL_CONST_INIT static absl::base_internal::LowLevelAlloc::Arena* a;
    ABSL_CONST_INIT static absl::once_flag flag;
    absl::base_internal::LowLevelCallOnce(&flag, [&]() {
      a = absl::base_internal::LowLevelAlloc::NewArena(
          absl::base_internal::LowLevelAlloc::kAsyncSignalSafe);
    });
    return a;
  }

  absl::Duration subrelease_interval_, short_interval_, long_interval_;
  bool release_partial_alloc_pages_ = false;
  bool hpaa_subrelease_ = true;
  bool release_succeeds_ = true;
  Arena arena_;

  uintptr_t fake_allocation_ = 0x1000;

  template <typename T>
  class AllocAdaptor final {
   public:
    using value_type = T;

    AllocAdaptor() = default;
    AllocAdaptor(const AllocAdaptor&) = default;

    template <class T1>
    using rebind = AllocAdaptor<T1>;

    template <class T1>
    explicit AllocAdaptor(const AllocAdaptor<T1>&) {}

    T* allocate(size_t n) {
      // Check if n is too big to allocate.
      TC_ASSERT_EQ((n * sizeof(T)) / sizeof(T), n);
      return static_cast<T*>(absl::base_internal::LowLevelAlloc::AllocWithArena(
          n * sizeof(T), ll_arena()));
    }
    void deallocate(T* p, size_t n) {
      absl::base_internal::LowLevelAlloc::Free(p);
    }
  };

  absl::flat_hash_map<HugePage, void*, absl::Hash<HugePage>,
                      std::equal_to<HugePage>,
                      AllocAdaptor<std::pair<HugePage, void*>>>
      trackers_;
};

}  // namespace huge_page_allocator_internal
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_MOCK_HUGE_PAGE_STATIC_FORWARDER_H_

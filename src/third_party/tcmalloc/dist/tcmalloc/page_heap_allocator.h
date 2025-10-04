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

#ifndef TCMALLOC_PAGE_HEAP_ALLOCATOR_H_
#define TCMALLOC_PAGE_HEAP_ALLOCATOR_H_

#include <stddef.h>

#include <new>

#include "absl/base/attributes.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"

#ifdef ABSL_HAVE_ADDRESS_SANITIZER
#include <sanitizer/asan_interface.h>
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct AllocatorStats {
  // Number of allocated but unfreed objects
  size_t in_use;
  // Number of objects created (both free and allocated)
  size_t total;
};

// Simple allocator for objects of a specified type.  External locking
// is required before accessing one of these objects.
template <class T>
class PageHeapAllocator {
 public:
  constexpr PageHeapAllocator()
      : arena_(nullptr), free_list_(nullptr), stats_{0, 0} {}

  // We use an explicit Init function because these variables are statically
  // allocated and their constructors might not have run by the time some
  // other static variable tries to allocate memory.
  void Init(Arena* arena) ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    arena_ = arena;
    // Reserve some space at the beginning to avoid fragmentation.
    Delete(New());
  }

  ABSL_ATTRIBUTE_RETURNS_NONNULL T* New()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return NewWithSize(sizeof(T), static_cast<std::align_val_t>(alignof(T)));
  }

  ABSL_ATTRIBUTE_RETURNS_NONNULL T* NewWithSize(size_t size,
                                                std::align_val_t align)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    TC_ASSERT_GE(static_cast<size_t>(align), alignof(T));
    // Consult free list
    T* result = free_list_;
    stats_.in_use++;
    if (ABSL_PREDICT_FALSE(result == nullptr)) {
      stats_.total++;
      result = reinterpret_cast<T*>(arena_->Alloc(size, align));
      ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(result, size);
      return result;
    } else {
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
      // Unpoison the object on the freelist.
      ASAN_UNPOISON_MEMORY_REGION(result, size);
#endif
    }
    free_list_ = *(reinterpret_cast<T**>(free_list_));
    ABSL_ANNOTATE_MEMORY_IS_UNINITIALIZED(result, size);
    return result;
  }

  void Delete(T* p) ABSL_ATTRIBUTE_NONNULL()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    *(reinterpret_cast<void**>(p)) = free_list_;
#ifdef ABSL_HAVE_ADDRESS_SANITIZER
    // Poison the object on the freelist.  We do not dereference it after this
    // point.
    ASAN_POISON_MEMORY_REGION(p, sizeof(*p));
#endif
    free_list_ = p;
    stats_.in_use--;
  }

  AllocatorStats stats() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return stats_;
  }

 private:
  // Arena from which to allocate memory
  Arena* arena_;

  // Free list of already carved objects
  T* free_list_ ABSL_GUARDED_BY(pageheap_lock);

  AllocatorStats stats_ ABSL_GUARDED_BY(pageheap_lock);
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PAGE_HEAP_ALLOCATOR_H_

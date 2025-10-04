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

#ifndef TCMALLOC_THREAD_CACHE_H_
#define TCMALLOC_THREAD_CACHE_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

//-------------------------------------------------------------------
// Data kept per thread
//-------------------------------------------------------------------

class ThreadCache {
 public:
  explicit ThreadCache(pthread_t tid)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);
  void Cleanup();

  // Allocate an object of the given size class.
  // Returns nullptr when allocation fails.
  void* Allocate(size_t size_class);

  void Deallocate(void* ptr, size_t size_class);

  static void InitTSD();
  static ThreadCache* GetCache();
  static ThreadCache* GetCacheIfPresent();
  static void BecomeIdle();

  // Adds to *total_bytes the total number of bytes used by all thread heaps.
  // Also, if class_count is not NULL, it must be an array of size kNumClasses,
  // and this function will increment each element of class_count by the number
  // of items in all thread-local freelists of the corresponding size class.
  static AllocatorStats GetStats(uint64_t* total_bytes, uint64_t* class_count)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Sets the total thread cache size to new_size, recomputing the
  // individual thread cache sizes as necessary.
  static void set_overall_thread_cache_size(size_t new_size)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  static size_t overall_thread_cache_size()
      ABSL_SHARED_LOCKS_REQUIRED(pageheap_lock) {
    return overall_thread_cache_size_;
  }

 private:
  // We inherit rather than include the list as a data structure to reduce
  // compiler padding.  Without inheritance, the compiler pads the list
  // structure and then adds it as a member, even though we could fit everything
  // without padding.
  class FreeList : public LinkedList {
   private:
    uint32_t lowater_;     // Low water mark for list length.
    uint32_t max_length_;  // Dynamic max list length based on usage.
    // Tracks the number of times a deallocation has caused
    // length_ > max_length_.  After the kMaxOverages'th time, max_length_
    // shrinks and length_overages_ is reset to zero.
    uint32_t length_overages_;

   public:
    void Init() {
      lowater_ = 0;
      max_length_ = 1;
      length_overages_ = 0;
    }

    // Return the maximum length of the list.
    size_t max_length() const { return max_length_; }

    // Set the maximum length of the list.  If 'new_max' > length(), the
    // client is responsible for removing objects from the list.
    void set_max_length(size_t new_max) { max_length_ = new_max; }

    // Return the number of times that length() has gone over max_length().
    size_t length_overages() const { return length_overages_; }

    void set_length_overages(size_t new_count) { length_overages_ = new_count; }

    // Low-water mark management
    int lowwatermark() const { return lowater_; }
    void clear_lowwatermark() { lowater_ = length(); }

    ABSL_ATTRIBUTE_ALWAYS_INLINE bool TryPop(void** ret) {
      bool out = LinkedList::TryPop(ret);
      if (ABSL_PREDICT_TRUE(out) && ABSL_PREDICT_FALSE(length() < lowater_)) {
        lowater_ = length();
      }
      return out;
    }

    void PopBatch(int N, void** batch) {
      LinkedList::PopBatch(N, batch);
      if (length() < lowater_) lowater_ = length();
    }
  };

  // Gets and returns an object from the transfer cache, and, if possible,
  // also adds some objects of that size class to this thread cache.
  void* FetchFromTransferCache(size_t size_class, size_t byte_size);

  // Releases some number of items from src.  Adjusts the list's max_length
  // to eventually converge on num_objects_to_move(size_class).
  void ListTooLong(FreeList* list, size_t size_class);

  void DeallocateSlow(void* ptr, FreeList* list, size_t size_class);

  // Releases N items from this thread cache.
  void ReleaseToTransferCache(FreeList* src, size_t size_class, int N);

  // Increase max_size_ by reducing unclaimed_cache_space_ or by
  // reducing the max_size_ of some other thread.  In both cases,
  // the delta is kStealAmount.
  void IncreaseCacheLimit();

  // Same as above but called with pageheap_lock held.
  void IncreaseCacheLimitLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  void Scavenge();
  static ThreadCache* CreateCacheIfNecessary();

  // If TLS is available, we also store a copy of the per-thread object
  // in a __thread variable since __thread variables are faster to read
  // than pthread_getspecific().  We still need pthread_setspecific()
  // because __thread variables provide no way to run cleanup code when
  // a thread is destroyed.
  //
  // We also give a hint to the compiler to use the "initial exec" TLS
  // model.  This is faster than the default TLS model, at the cost that
  // you cannot dlopen this library.  (To see the difference, look at
  // the CPU use of __tls_get_addr with and without this attribute.)
  //
  // Since using dlopen on a malloc replacement is asking for trouble in any
  // case, that's a good tradeoff for us.
  ABSL_CONST_INIT static thread_local ThreadCache* thread_local_data_
      ABSL_ATTRIBUTE_INITIAL_EXEC;

  // Thread-specific key.  Initialization here is somewhat tricky
  // because some Linux startup code invokes malloc() before it
  // is in a good enough state to handle pthread_keycreate().
  // Therefore, we use TSD keys only after tsd_inited is set to true.
  // Until then, we use a slow path to get the heap object.
  static bool tsd_inited_;
  static pthread_key_t heap_key_;

  // Linked list of heap objects.
  static ThreadCache* thread_heaps_ ABSL_GUARDED_BY(pageheap_lock);
  static int thread_heap_count_ ABSL_GUARDED_BY(pageheap_lock);

  // A pointer to one of the objects in thread_heaps_.  Represents
  // the next ThreadCache from which a thread over its max_size_ should
  // steal memory limit.  Round-robin through all of the objects in
  // thread_heaps_.
  static ThreadCache* next_memory_steal_ ABSL_GUARDED_BY(pageheap_lock);

  // Overall thread cache size.
  static size_t overall_thread_cache_size_ ABSL_GUARDED_BY(pageheap_lock);

  // Global per-thread cache size.
  static size_t per_thread_cache_size_ ABSL_GUARDED_BY(pageheap_lock);

  // Represents overall_thread_cache_size_ minus the sum of max_size_
  // across all ThreadCaches.
  static int64_t unclaimed_cache_space_ ABSL_GUARDED_BY(pageheap_lock);

  // This class is laid out with the most frequently used fields
  // first so that hot elements are placed on the same cache line.

  FreeList list_[kNumClasses];  // Array indexed by size-class

  size_t size_;      // Combined size of data
  size_t max_size_;  // size_ > max_size_ --> Scavenge()

  pthread_t tid_;
  bool in_setspecific_;

  // Allocate a new heap.
  static ThreadCache* NewHeap(pthread_t tid)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Use only as pthread thread-specific destructor function.
  static void DestroyThreadCache(void* ptr);

  static void DeleteCache(ThreadCache* heap);
  static void RecomputePerThreadCacheSize()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // All ThreadCache objects are kept in a linked list (for stats collection)
  ThreadCache* next_;
  ThreadCache* prev_;

  // Ensure that two instances of this class are never on the same cache line.
  // This is critical for performance, as false sharing would negate many of
  // the benefits of a per-thread cache.
  char padding_[ABSL_CACHELINE_SIZE];
};

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* ThreadCache::Allocate(
    size_t size_class) {
  const size_t allocated_size = tc_globals.sizemap().class_to_size(size_class);

  FreeList* list = &list_[size_class];
  void* ret;
  if (ABSL_PREDICT_TRUE(list->TryPop(&ret))) {
    size_ -= allocated_size;
    return ret;
  }

  return FetchFromTransferCache(size_class, allocated_size);
}

inline void ABSL_ATTRIBUTE_ALWAYS_INLINE
ThreadCache::Deallocate(void* ptr, size_t size_class) {
  FreeList* list = &list_[size_class];
  size_ += tc_globals.sizemap().class_to_size(size_class);
  ssize_t size_headroom = max_size_ - size_ - 1;

  list->Push(ptr);
  ssize_t list_headroom =
      static_cast<ssize_t>(list->max_length()) - list->length();

  // There are two relatively uncommon things that require further work.
  // In the common case we're done, and in that case we need a single branch
  // because of the bitwise-or trick that follows.
  if ((list_headroom | size_headroom) < 0) {
    DeallocateSlow(ptr, list, size_class);
  }
}

inline ThreadCache* ABSL_ATTRIBUTE_ALWAYS_INLINE
ThreadCache::GetCacheIfPresent() {
  return thread_local_data_;
}

inline ThreadCache* ThreadCache::GetCache() {
  ThreadCache* tc = GetCacheIfPresent();
  return (ABSL_PREDICT_TRUE(tc != nullptr)) ? tc : CreateCacheIfNecessary();
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_THREAD_CACHE_H_

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

#ifndef TCMALLOC_GUARDED_PAGE_ALLOCATOR_H_
#define TCMALLOC_GUARDED_PAGE_ALLOCATOR_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/internal/atomic_stats_counter.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/stacktrace_filter.h"
#include "tcmalloc/pages.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// An allocator that gives each allocation a new region, with guard pages on
// either side of the allocated region.  If a buffer is overflowed to the next
// guard page or underflowed to the previous guard page, a segfault occurs.
// After an allocation is freed, the underlying page is marked as inaccessible,
// and any future accesses to it will also cause segfaults until the page is
// reallocated.
//
// Is safe to use with static storage duration and is thread safe with the
// exception of calls to Init() and Destroy() (see corresponding function
// comments).
//
// Example:
//   ABSL_CONST_INIT GuardedPageAllocator gpa;
//
//   void foo() {
//     char *buf = reinterpret_cast<char *>(gpa.Allocate(8000, 1));
//     buf[0] = 'A';            // OK. No segfault occurs.
//     memset(buf, 'A', 8000);  // OK. No segfault occurs.
//     buf[-300] = 'A';         // Segfault!
//     buf[9000] = 'A';         // Segfault!
//     gpa.Deallocate(buf);
//     buf[0] = 'B';            // Segfault!
//   }
//
//   int main() {
//     // Call Init() only once.
//     gpa.Init(64, GuardedPageAllocator::kGpaMaxPages);
//     gpa.AllowAllocations();
//     for (int i = 0; i < 1000; i++) foo();
//     return 0;
//   }
class GuardedPageAllocator {
 public:
  // Maximum number of pages this class can allocate.
  static constexpr size_t kGpaMaxPages = 512;

  constexpr GuardedPageAllocator()
      : guarded_page_lock_(absl::kConstInit,
                           absl::base_internal::SCHEDULE_KERNEL_ONLY),
        free_pages_{},
        num_alloced_pages_(0),
        num_alloced_pages_max_(0),
        data_(nullptr),
        pages_base_addr_(0),
        pages_end_addr_(0),
        first_page_addr_(0),
        max_alloced_pages_(0),
        total_pages_(0),
        total_pages_used_(0),
        alloced_page_count_when_all_used_once_(0),
        page_size_(0),
        rand_(0),
        initialized_(false),
        allow_allocations_(false),
        double_free_detected_(false),
        write_overflow_detected_(false) {}

  GuardedPageAllocator(const GuardedPageAllocator&) = delete;
  GuardedPageAllocator& operator=(const GuardedPageAllocator&) = delete;

  ~GuardedPageAllocator() = default;

  // Configures this allocator to allocate up to max_alloced_pages pages at a
  // time from a pool of total_pages pages, where:
  //   1 <= max_alloced_pages <= total_pages <= kGpaMaxPages
  //
  // This method should be called non-concurrently and only once to complete
  // initialization.  Dynamic initialization is deliberately done here and not
  // in the constructor, thereby allowing the constructor to be constexpr and
  // avoiding static initialization order issues.
  void Init(size_t max_alloced_pages, size_t total_pages)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Unmaps memory allocated by this class.
  //
  // This method should be called non-concurrently and only once to complete
  // destruction.  Destruction is deliberately done here and not in the
  // destructor, thereby allowing the destructor to be trivial (i.e. a no-op)
  // and avoiding use-after-destruction issues for static/global instances.
  void Destroy();

  // If this allocation can be guarded, and if it's time to do a guarded sample,
  // returns an instance of GuardedAllocWithStatus, that includes guarded
  // allocation Span and guarded status. Otherwise, returns nullptr and the
  // status indicating why the allocation may not be guarded.
  GuardedAllocWithStatus TrySample(size_t size, size_t alignment,
                                   Length num_pages,
                                   const StackTrace& stack_trace);

  // On success, returns an instance of GuardedAllocWithStatus which includes a
  // pointer to size bytes of page-guarded memory, aligned to alignment.  The
  // member 'alloc' is a pointer that is guaranteed to be tagged.  The 'status'
  // member is set to GuardedStatus::Guarded.  On failure, returns an instance
  // of GuardedAllocWithStatus (the 'alloc' member is set to 'nullptr').
  // Failure can occur if memory could not be mapped or protected, if all
  // guarded pages are already allocated, or if size is 0.  These conditions are
  // reflected in the 'status' member of the GuardedAllocWithStatus return
  // value.
  //
  // Precondition:  size and alignment <= page_size_
  // Precondition:  alignment is 0 or a power of 2
  GuardedAllocWithStatus Allocate(size_t size, size_t alignment)
      ABSL_LOCKS_EXCLUDED(guarded_page_lock_);

  // Deallocates memory pointed to by ptr.  ptr must have been previously
  // returned by a call to Allocate.
  void Deallocate(void* ptr) ABSL_LOCKS_EXCLUDED(guarded_page_lock_);

  // Returns the size requested when ptr was allocated.  ptr must have been
  // previously returned by a call to Allocate.
  size_t GetRequestedSize(const void* ptr) const;

  // Returns ptr's offset from the beginning of its allocation along with the
  // allocation's size.
  std::pair<off_t, size_t> GetAllocationOffsetAndSize(const void* ptr) const;

  // Records stack traces in alloc_trace and dealloc_trace for the page nearest
  // to ptr.  alloc_trace is the trace at the time the page was allocated.  If
  // the page is still allocated, dealloc_trace->depth will be 0. If the page
  // has been deallocated, dealloc_trace is the trace at the time the page was
  // deallocated.
  //
  // Returns the likely error type for an access at ptr.
  //
  // Requires that ptr points to memory mapped by this class.
  GuardedAllocationsErrorType GetStackTraces(
      const void* ptr, GuardedAllocationsStackTrace** alloc_trace,
      GuardedAllocationsStackTrace** dealloc_trace) const;

  // Writes a human-readable summary of GuardedPageAllocator's internal state to
  // *out.
  void Print(Printer* out) ABSL_LOCKS_EXCLUDED(guarded_page_lock_);
  void PrintInPbtxt(PbtxtRegion* gwp_asan)
      ABSL_LOCKS_EXCLUDED(guarded_page_lock_);

  // Returns true if ptr points to memory managed by this class.
  inline bool ABSL_ATTRIBUTE_ALWAYS_INLINE
  PointerIsMine(const void* ptr) const {
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    return pages_base_addr_ <= addr && addr < pages_end_addr_;
  }

  // Allows Allocate() to start returning allocations.
  void AllowAllocations() ABSL_LOCKS_EXCLUDED(guarded_page_lock_) {
    AllocationGuardSpinLockHolder h(&guarded_page_lock_);
    allow_allocations_ = true;
  }

  // Returns the number of pages available for allocation, based on how many are
  // currently in use.  (Should only be used in testing.)
  size_t GetNumAvailablePages() ABSL_LOCKS_EXCLUDED(guarded_page_lock_) {
    AllocationGuardSpinLockHolder h(&guarded_page_lock_);
    return max_alloced_pages_ - num_alloced_pages_;
  }

  size_t SuccessfulAllocations() { return num_successful_allocations_.value(); }

  // Resets sampling state.
  void Reset();

  size_t page_size() const { return page_size_; }

 private:
  // Structure for storing data about a slot.
  struct SlotMetadata {
    GuardedAllocationsStackTrace alloc_trace;
    GuardedAllocationsStackTrace dealloc_trace;
    size_t requested_size = 0;
    uintptr_t allocation_start = 0;
  };

  // Max number of magic bytes we use to detect write-overflows at deallocation.
  static constexpr size_t kMagicSize = 32;

  // Maps pages into memory.
  void MapPages() ABSL_LOCKS_EXCLUDED(guarded_page_lock_)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock);

  // Reserves and returns a slot randomly selected from the free slots in
  // free_pages_.  Returns -1 if no slots available, or if AllowAllocations()
  // hasn't been called yet.
  ssize_t ReserveFreeSlot() ABSL_LOCKS_EXCLUDED(guarded_page_lock_);

  // Returns the i-th free slot of free_pages_.  i must be in the range [0,
  // total_pages_ - num_alloced_pages_).
  size_t GetIthFreeSlot(size_t i)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(guarded_page_lock_);

  // Marks the specified slot as unreserved.
  void FreeSlot(size_t slot) ABSL_EXCLUSIVE_LOCKS_REQUIRED(guarded_page_lock_);

  // Returns the address of the page that addr resides on.
  uintptr_t GetPageAddr(uintptr_t addr) const;

  // Returns an address somewhere on the valid page nearest to addr.
  uintptr_t GetNearestValidPage(uintptr_t addr) const;

  // Returns the slot number for the page nearest to addr.
  size_t GetNearestSlot(uintptr_t addr) const;

  // Returns true if the specified slot has already been freed.
  bool IsFreed(size_t slot) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(guarded_page_lock_);

  // Returns true if magic bytes for slot were overwritten.
  bool WriteOverflowOccurred(size_t slot) const;

  // Returns the likely error type for the given access address and metadata
  // associated with the nearest slot.
  GuardedAllocationsErrorType GetErrorType(uintptr_t addr,
                                           const SlotMetadata& d) const;

  // Magic constant used for detecting write-overflows at deallocation time.
  static uint8_t GetWriteOverflowMagic(size_t slot) {
    // Only even slots get magic bytes, so use slot / 2 for more unique magics.
    return uint8_t{0xcd} * static_cast<uint8_t>(slot / 2);
  }

  // Returns true if slot should be right aligned.
  static bool ShouldRightAlign(size_t slot) { return slot % 2 == 0; }

  // If slot is marked for right alignment, moves the allocation in *ptr to the
  // right end of the slot, maintaining the specified size and alignment.  Magic
  // bytes are written in any alignment padding.
  void MaybeRightAlign(size_t slot, size_t size, size_t alignment, void** ptr);

  uintptr_t SlotToAddr(size_t slot) const;
  size_t AddrToSlot(uintptr_t addr) const;

  // Returns a random number in range [0, max).
  size_t Rand(size_t max);

  StackTraceFilter stacktrace_filter_;

  absl::base_internal::SpinLock guarded_page_lock_;

  // Maps each bool to one page.
  // true: Free.  false: Reserved.
  bool free_pages_[kGpaMaxPages] ABSL_GUARDED_BY(guarded_page_lock_);

  // Number of currently-allocated pages.
  size_t num_alloced_pages_ ABSL_GUARDED_BY(guarded_page_lock_);

  // The high-water mark for num_alloced_pages_.
  size_t num_alloced_pages_max_ ABSL_GUARDED_BY(guarded_page_lock_);

  // Number of successful allocations (calls to Allocate - failed).
  tcmalloc_internal::StatsCounter num_successful_allocations_;

  // Number of times Allocate has failed.
  tcmalloc_internal::StatsCounter num_failed_allocations_;

  // A dynamically-allocated array of stack trace data captured when each page
  // is allocated/deallocated.  Printed by the SEGV handler when a memory error
  // is detected.
  SlotMetadata* data_;

  uintptr_t pages_base_addr_;  // Points to start of mapped region.
  uintptr_t pages_end_addr_;   // Points to the end of mapped region.
  uintptr_t first_page_addr_;  // Points to first page returnable by Allocate.
  size_t max_alloced_pages_;   // Max number of pages to allocate at once.
  size_t total_pages_;         // Size of the page pool to allocate from.
  // Number of pages allocated at least once from page pool.
  size_t total_pages_used_ ABSL_GUARDED_BY(guarded_page_lock_);
  // The count of allocs when all the pages had been used at least once (i.e.
  // when total_pages_used_ == total_pages_).
  size_t alloced_page_count_when_all_used_once_
      ABSL_GUARDED_BY(guarded_page_lock_);
  size_t page_size_;           // Size of pages we allocate.
  std::atomic<uint64_t> rand_;  // RNG seed.

  // True if this object has been fully initialized.
  bool initialized_ ABSL_GUARDED_BY(guarded_page_lock_);

  // Flag to control whether we can return allocations or not.
  bool allow_allocations_ ABSL_GUARDED_BY(guarded_page_lock_);

  // Set to true if a double free has occurred.
  bool double_free_detected_;

  // Set to true if a write overflow was detected on deallocation.
  bool write_overflow_detected_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_GUARDED_PAGE_ALLOCATOR_H_

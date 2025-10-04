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

#include "tcmalloc/guarded_page_allocator.h"

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/debugging/stacktrace.h"
#include "absl/numeric/bits.h"
#include "tcmalloc/common.h"
#include "tcmalloc/global_stats.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/exponential_biased.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

const size_t GuardedPageAllocator::kMagicSize;  // NOLINT

void GuardedPageAllocator::Init(size_t max_alloced_pages, size_t total_pages) {
  TC_CHECK(!initialized_);
  TC_CHECK_GT(max_alloced_pages, 0);
  TC_CHECK_LE(max_alloced_pages, total_pages);
  TC_CHECK_LE(total_pages, kGpaMaxPages);
  max_alloced_pages_ = max_alloced_pages;
  total_pages_ = total_pages;

  // If the system page size is larger than kPageSize, we need to use the
  // system page size for this allocator since mprotect operates on full pages
  // only.  This case happens on PPC.
  page_size_ = std::max(kPageSize, static_cast<size_t>(GetPageSize()));
  TC_ASSERT_EQ(page_size_ % kPageSize, 0);

  rand_ = reinterpret_cast<uint64_t>(this);  // Initialize RNG seed.
  MapPages();
}

void GuardedPageAllocator::Destroy() {
  AllocationGuardSpinLockHolder h(&guarded_page_lock_);
  if (initialized_) {
    size_t len = pages_end_addr_ - pages_base_addr_;
    int err = munmap(reinterpret_cast<void*>(pages_base_addr_), len);
    TC_ASSERT_NE(err, -1);
    (void)err;
    initialized_ = false;
  }
}

void GuardedPageAllocator::Reset() {
  // Reset is used by tests to ensure that subsequent allocations will be
  // sampled. Reset sampled/guarded counters so that that we don't skip
  // guarded sampling for a prolonged time due to accumulated stats.
  tc_globals.total_sampled_count_.Add(-tc_globals.total_sampled_count_.value());
  num_successful_allocations_.Add(-num_successful_allocations_.value());
  stacktrace_filter_.Reset();
}

GuardedAllocWithStatus GuardedPageAllocator::TrySample(
    size_t size, size_t alignment, Length num_pages,
    const StackTrace& stack_trace) {
  if (num_pages != Length(1)) {
    return {nullptr, Profile::Sample::GuardedStatus::LargerThanOnePage};
  }
  const int64_t guarded_sampling_rate =
      tcmalloc::tcmalloc_internal::Parameters::guarded_sampling_rate();
  if (guarded_sampling_rate < 0) {
    return {nullptr, Profile::Sample::GuardedStatus::Disabled};
  }
  // TODO(b/273954799): Possible optimization: only calculate this when
  // guarded_sampling_rate or profile_sampling_rate change.  Likewise for
  // margin_multiplier below.
  const int64_t profile_sampling_rate =
      tcmalloc::tcmalloc_internal::Parameters::profile_sampling_rate();
  // If guarded_sampling_rate == 0, then attempt to guard, as usual.
  if (guarded_sampling_rate > 0) {
    const double target_ratio =
        profile_sampling_rate > 0
            ? std::ceil(guarded_sampling_rate / profile_sampling_rate)
            : 1.0;
    const double current_ratio = 1.0 * tc_globals.total_sampled_count_.value() /
                                 (std::max(SuccessfulAllocations(), 1UL));
    if (current_ratio <= target_ratio) {
      return {nullptr, Profile::Sample::GuardedStatus::RateLimited};
    }

    switch (stacktrace_filter_.Count(stack_trace)) {
      case 0:
        // Fall through to allocation below.
        break;
      case 1:
        /* 25% */
        if (Rand(/*max=*/4) != 0) {
          return {nullptr, Profile::Sample::GuardedStatus::Filtered};
        }
        break;
      case 2:
        /* 12.5% */
        if (Rand(/*max=*/8) != 0) {
          return {nullptr, Profile::Sample::GuardedStatus::Filtered};
        }
        break;
      case 3:
        /* ~1% */
        if (Rand(/*max=*/128) != 0) {
          return {nullptr, Profile::Sample::GuardedStatus::Filtered};
        }
        break;
      default:
        // Reached max per stack.
        return {nullptr, Profile::Sample::GuardedStatus::Filtered};
    }
  }
  // The num_pages == 1 constraint ensures that size <= kPageSize.  And
  // since alignments above kPageSize cause size_class == 0, we're also
  // guaranteed alignment <= kPageSize
  //
  // In all cases kPageSize <= GPA::page_size_, so Allocate's preconditions
  // are met.
  auto alloc_with_status = Allocate(size, alignment);
  if (alloc_with_status.status == Profile::Sample::GuardedStatus::Guarded) {
    stacktrace_filter_.Add(stack_trace);
  }
  return alloc_with_status;
}

GuardedAllocWithStatus GuardedPageAllocator::Allocate(size_t size,
                                                      size_t alignment) {
  if (size == 0) {
    return {nullptr, Profile::Sample::GuardedStatus::TooSmall};
  }
  ssize_t free_slot = ReserveFreeSlot();
  // All slots are reserved.
  if (free_slot == -1) {
    return {nullptr, Profile::Sample::GuardedStatus::NoAvailableSlots};
  }

  TC_ASSERT_LE(size, page_size_);
  TC_ASSERT_LE(alignment, page_size_);
  TC_ASSERT(alignment == 0 || absl::has_single_bit(alignment));
  void* result = reinterpret_cast<void*>(SlotToAddr(free_slot));
  if (mprotect(result, page_size_, PROT_READ | PROT_WRITE) == -1) {
    TC_ASSERT(false && "mprotect failed");
    AllocationGuardSpinLockHolder h(&guarded_page_lock_);
    num_failed_allocations_.LossyAdd(1);
    num_successful_allocations_.LossyAdd(-1);
    FreeSlot(free_slot);
    return {nullptr, Profile::Sample::GuardedStatus::MProtectFailed};
  }

  // Place some allocations at end of page for better overflow detection.
  MaybeRightAlign(free_slot, size, alignment, &result);

  // Record stack trace.
  SlotMetadata& d = data_[free_slot];
  // Count the number of pages that have been used at least once.
  if (d.allocation_start == 0) {
    AllocationGuardSpinLockHolder h(&guarded_page_lock_);
    ++total_pages_used_;
    if (total_pages_used_ == total_pages_) {
      alloced_page_count_when_all_used_once_ =
          num_successful_allocations_.value();
    }
  }
  d.dealloc_trace.depth = 0;
  d.alloc_trace.depth = absl::GetStackTrace(d.alloc_trace.stack, kMaxStackDepth,
                                            /*skip_count=*/3);
  d.alloc_trace.thread_id = absl::base_internal::GetTID();
  d.requested_size = size;
  d.allocation_start = reinterpret_cast<uintptr_t>(result);

  TC_ASSERT(!alignment || d.allocation_start % alignment == 0);
  return {result, Profile::Sample::GuardedStatus::Guarded};
}

void GuardedPageAllocator::Deallocate(void* ptr) {
  TC_ASSERT(PointerIsMine(ptr));
  const uintptr_t page_addr = GetPageAddr(reinterpret_cast<uintptr_t>(ptr));
  size_t slot = AddrToSlot(page_addr);

  {
    AllocationGuardSpinLockHolder h(&guarded_page_lock_);
    if (IsFreed(slot)) {
      double_free_detected_ = true;
    } else if (WriteOverflowOccurred(slot)) {
      write_overflow_detected_ = true;
    }

    TC_CHECK_EQ(
        0, mprotect(reinterpret_cast<void*>(page_addr), page_size_, PROT_NONE));
  }

  if (write_overflow_detected_ || double_free_detected_) {
    {
      TC_LOG("Detected an error in the GuardedPageAllocator-- dumping stats and then exiting");
      const int kBufferSize = 64 << 10;
      char* buffer = new char[kBufferSize];
      Printer printer(buffer, kBufferSize);
      DumpStats(&printer, 2);
      (void)write(STDERR_FILENO, buffer, strlen(buffer));
      delete[] buffer;
    }

    *reinterpret_cast<char*>(ptr) = 'X';  // Trigger SEGV handler.
    TC_BUG("unreachable");
  }

  // Record stack trace.
  AllocationGuardSpinLockHolder h(&guarded_page_lock_);
  GuardedAllocationsStackTrace& trace = data_[slot].dealloc_trace;
  trace.depth = absl::GetStackTrace(trace.stack, kMaxStackDepth,
                                    /*skip_count=*/2);
  trace.thread_id = absl::base_internal::GetTID();

  FreeSlot(slot);
}

size_t GuardedPageAllocator::GetRequestedSize(const void* ptr) const {
  TC_ASSERT(PointerIsMine(ptr));
  size_t slot = AddrToSlot(GetPageAddr(reinterpret_cast<uintptr_t>(ptr)));
  return data_[slot].requested_size;
}

std::pair<off_t, size_t> GuardedPageAllocator::GetAllocationOffsetAndSize(
    const void* ptr) const {
  TC_ASSERT(PointerIsMine(ptr));
  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  const size_t slot = GetNearestSlot(addr);
  return {addr - data_[slot].allocation_start, data_[slot].requested_size};
}

GuardedAllocationsErrorType GuardedPageAllocator::GetStackTraces(
    const void* ptr, GuardedAllocationsStackTrace** alloc_trace,
    GuardedAllocationsStackTrace** dealloc_trace) const {
  TC_ASSERT(PointerIsMine(ptr));
  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  size_t slot = GetNearestSlot(addr);
  *alloc_trace = &data_[slot].alloc_trace;
  *dealloc_trace = &data_[slot].dealloc_trace;
  return GetErrorType(addr, data_[slot]);
}

// We take guarded samples during periodic profiling samples.  Computes the
// mean number of profiled samples made for every guarded sample.
static int GetChainedRate() {
  auto guarded_rate = Parameters::guarded_sampling_rate();
  auto sample_rate = Parameters::profile_sampling_rate();
  if (guarded_rate < 0 || sample_rate <= 0) {
    return guarded_rate;
  } else {
    return std::ceil(static_cast<double>(guarded_rate) /
                     static_cast<double>(sample_rate));
  }
}

void GuardedPageAllocator::Print(Printer* out) {
  AllocationGuardSpinLockHolder h(&guarded_page_lock_);
  out->printf(
      "\n"
      "------------------------------------------------\n"
      "GWP-ASan Status\n"
      "------------------------------------------------\n"
      "Successful Allocations: %zu\n"
      "Failed Allocations: %zu\n"
      "Slots Currently Allocated: %zu\n"
      "Slots Currently Quarantined: %zu\n"
      "Maximum Slots Allocated: %zu / %zu\n"
      "StackTraceFilter Max Slots Used: %zu\n"
      "StackTraceFilter Replacement Inserts: %zu\n"
      "Total Slots Used Once: %zu / %zu\n"
      "Allocation Count When All Slots Used Once: %zu\n"
      "PARAMETER tcmalloc_guarded_sample_parameter %d\n",
      num_successful_allocations_.value(), num_failed_allocations_.value(),
      num_alloced_pages_, total_pages_ - num_alloced_pages_,
      num_alloced_pages_max_, max_alloced_pages_,
      stacktrace_filter_.max_slots_used(),
      stacktrace_filter_.replacement_inserts(), total_pages_used_, total_pages_,
      alloced_page_count_when_all_used_once_, GetChainedRate());
}

void GuardedPageAllocator::PrintInPbtxt(PbtxtRegion* gwp_asan) {
  AllocationGuardSpinLockHolder h(&guarded_page_lock_);
  gwp_asan->PrintI64("successful_allocations",
                     num_successful_allocations_.value());
  gwp_asan->PrintI64("failed_allocations", num_failed_allocations_.value());
  gwp_asan->PrintI64("current_slots_allocated", num_alloced_pages_);
  gwp_asan->PrintI64("current_slots_quarantined",
                     total_pages_ - num_alloced_pages_);
  gwp_asan->PrintI64("max_slots_allocated", num_alloced_pages_max_);
  gwp_asan->PrintI64("allocated_slot_limit", max_alloced_pages_);
  gwp_asan->PrintI64("stack_trace_filter_max_slots_used",
                     stacktrace_filter_.max_slots_used());
  gwp_asan->PrintI64("stack_trace_filter_replacement_inserts",
                     stacktrace_filter_.replacement_inserts());
  gwp_asan->PrintI64("total_pages_used", total_pages_used_);
  gwp_asan->PrintI64("total_pages", total_pages_);
  gwp_asan->PrintI64("alloced_page_count_when_all_used_once",
                     alloced_page_count_when_all_used_once_);
  gwp_asan->PrintI64("tcmalloc_guarded_sample_parameter", GetChainedRate());
}

// Maps 2 * total_pages_ + 1 pages so that there are total_pages_ unique pages
// we can return from Allocate with guard pages before and after them.
void GuardedPageAllocator::MapPages() {
  AllocationGuardSpinLockHolder h(&guarded_page_lock_);
  TC_ASSERT(!first_page_addr_);
  TC_ASSERT_EQ(page_size_ % GetPageSize(), 0);
  size_t len = (2 * total_pages_ + 1) * page_size_;
  auto base_addr = reinterpret_cast<uintptr_t>(
      MmapAligned(len, page_size_, MemoryTag::kSampled));
  TC_ASSERT(base_addr);
  if (!base_addr) return;

  // Tell TCMalloc's PageMap about the memory we own.
  const PageId page = PageIdContaining(reinterpret_cast<void*>(base_addr));
  const Length page_len = BytesToLengthFloor(len);
  if (!tc_globals.pagemap().Ensure(page, page_len)) {
    TC_ASSERT(false && "Failed to notify page map of page-guarded memory.");
    return;
  }

  // Allocate memory for slot metadata.
  data_ = reinterpret_cast<SlotMetadata*>(
      tc_globals.arena().Alloc(sizeof(*data_) * total_pages_));
  for (size_t i = 0; i < total_pages_; ++i) {
    new (&data_[i]) SlotMetadata;
  }

  pages_base_addr_ = base_addr;
  pages_end_addr_ = pages_base_addr_ + len;

  // Align first page to page_size_.
  first_page_addr_ = GetPageAddr(pages_base_addr_ + page_size_);

  std::fill_n(free_pages_, total_pages_, true);
  initialized_ = true;
}

// Selects a random slot in O(total_pages_) time.
ssize_t GuardedPageAllocator::ReserveFreeSlot() {
  AllocationGuardSpinLockHolder h(&guarded_page_lock_);
  if (!initialized_ || !allow_allocations_) return -1;
  if (num_alloced_pages_ == max_alloced_pages_) {
    num_failed_allocations_.LossyAdd(1);
    return -1;
  }
  num_successful_allocations_.LossyAdd(1);

  size_t num_free_pages = total_pages_ - num_alloced_pages_;
  size_t slot = GetIthFreeSlot(Rand(num_free_pages));
  TC_ASSERT(free_pages_[slot]);
  free_pages_[slot] = false;
  num_alloced_pages_++;
  num_alloced_pages_max_ = std::max(num_alloced_pages_, num_alloced_pages_max_);
  return slot;
}

size_t GuardedPageAllocator::Rand(size_t max) {
  auto x = ExponentialBiased::NextRandom(rand_.load(std::memory_order_relaxed));
  rand_.store(x, std::memory_order_relaxed);
  return ExponentialBiased::GetRandom(x) % max;
}

size_t GuardedPageAllocator::GetIthFreeSlot(size_t ith_free_slot) {
  TC_ASSERT_LT(ith_free_slot, total_pages_ - num_alloced_pages_);
  for (size_t free_slot_count = 0, j = 0;; j++) {
    if (free_pages_[j]) {
      if (free_slot_count == ith_free_slot) return j;
      free_slot_count++;
    }
  }
}

void GuardedPageAllocator::FreeSlot(size_t slot) {
  TC_ASSERT_LT(slot, total_pages_);
  TC_ASSERT(!free_pages_[slot]);
  free_pages_[slot] = true;
  num_alloced_pages_--;
}

uintptr_t GuardedPageAllocator::GetPageAddr(uintptr_t addr) const {
  const uintptr_t addr_mask = ~(page_size_ - 1ULL);
  return addr & addr_mask;
}

uintptr_t GuardedPageAllocator::GetNearestValidPage(uintptr_t addr) const {
  if (addr < first_page_addr_) return first_page_addr_;
  const uintptr_t last_page_addr =
      first_page_addr_ + 2 * (total_pages_ - 1) * page_size_;
  if (addr > last_page_addr) return last_page_addr;
  uintptr_t offset = addr - first_page_addr_;

  // If addr is already on a valid page, just return addr.
  if ((offset / page_size_) % 2 == 0) return addr;

  // ptr points to a guard page, so get nearest valid page.
  const size_t kHalfPageSize = page_size_ / 2;
  if ((offset / kHalfPageSize) % 2 == 0) {
    return addr - kHalfPageSize;  // Round down.
  }
  return addr + kHalfPageSize;  // Round up.
}

size_t GuardedPageAllocator::GetNearestSlot(uintptr_t addr) const {
  return AddrToSlot(GetPageAddr(GetNearestValidPage(addr)));
}

bool GuardedPageAllocator::IsFreed(size_t slot) const {
  return free_pages_[slot];
}

bool GuardedPageAllocator::WriteOverflowOccurred(size_t slot) const {
  if (!ShouldRightAlign(slot)) return false;
  uint8_t magic = GetWriteOverflowMagic(slot);
  uintptr_t alloc_end =
      data_[slot].allocation_start + data_[slot].requested_size;
  uintptr_t page_end = SlotToAddr(slot) + page_size_;
  uintptr_t magic_end = std::min(page_end, alloc_end + kMagicSize);
  for (uintptr_t p = alloc_end; p < magic_end; ++p) {
    if (*reinterpret_cast<uint8_t*>(p) != magic) return true;
  }
  return false;
}

GuardedAllocationsErrorType GuardedPageAllocator::GetErrorType(
    uintptr_t addr, const SlotMetadata& d) const {
  if (!d.allocation_start) return GuardedAllocationsErrorType::kUnknown;
  if (double_free_detected_) return GuardedAllocationsErrorType::kDoubleFree;
  if (write_overflow_detected_)
    return GuardedAllocationsErrorType::kBufferOverflowOnDealloc;
  if (d.dealloc_trace.depth > 0) {
    return GuardedAllocationsErrorType::kUseAfterFree;
  }
  if (addr < d.allocation_start) {
    return GuardedAllocationsErrorType::kBufferUnderflow;
  }
  if (addr >= d.allocation_start + d.requested_size) {
    return GuardedAllocationsErrorType::kBufferOverflow;
  }
  return GuardedAllocationsErrorType::kUnknown;
}

uintptr_t GuardedPageAllocator::SlotToAddr(size_t slot) const {
  TC_ASSERT_LT(slot, total_pages_);
  return first_page_addr_ + 2 * slot * page_size_;
}

size_t GuardedPageAllocator::AddrToSlot(uintptr_t addr) const {
  uintptr_t offset = addr - first_page_addr_;
  TC_ASSERT_EQ(offset % page_size_, 0);
  TC_ASSERT_EQ((offset / page_size_) % 2, 0);
  int slot = offset / page_size_ / 2;
  TC_ASSERT(slot >= 0 && slot < total_pages_);
  return slot;
}

void GuardedPageAllocator::MaybeRightAlign(size_t slot, size_t size,
                                           size_t alignment, void** ptr) {
  if (!ShouldRightAlign(slot)) return;
  uintptr_t adjusted_ptr =
      reinterpret_cast<uintptr_t>(*ptr) + page_size_ - size;

  // If alignment == 0, the necessary alignment is never larger than the size
  // rounded up to the next power of 2.  We use this fact to minimize alignment
  // padding between the end of small allocations and their guard pages.
  //
  // For allocations larger than the greater of kAlignment and
  // __STDCPP_DEFAULT_NEW_ALIGNMENT__, we're safe aligning to that value.
  size_t default_alignment =
      std::min(absl::bit_ceil(size),
               std::max(static_cast<size_t>(kAlignment),
                        static_cast<size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__)));

  // Ensure valid alignment.
  alignment = std::max(alignment, default_alignment);
  uintptr_t alignment_padding = adjusted_ptr & (alignment - 1);
  adjusted_ptr -= alignment_padding;

  // Write magic bytes in alignment padding to detect small overflow writes.
  size_t magic_size = std::min(alignment_padding, kMagicSize);
  memset(reinterpret_cast<void*>(adjusted_ptr + size),
         GetWriteOverflowMagic(slot), magic_size);
  *ptr = reinterpret_cast<void*>(adjusted_ptr);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

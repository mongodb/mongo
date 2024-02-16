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

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <tuple>
#include <utility>

#include "absl/base/call_once.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/debugging/stacktrace.h"
#include "absl/numeric/bits.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

const size_t GuardedPageAllocator::kMagicSize;  // NOLINT

void GuardedPageAllocator::Init(size_t max_alloced_pages, size_t total_pages) {
  CHECK_CONDITION(max_alloced_pages > 0);
  CHECK_CONDITION(max_alloced_pages <= total_pages);
  CHECK_CONDITION(total_pages <= kGpaMaxPages);
  max_alloced_pages_ = max_alloced_pages;
  total_pages_ = total_pages;

  // If the system page size is larger than kPageSize, we need to use the
  // system page size for this allocator since mprotect operates on full pages
  // only.  This case happens on PPC.
  page_size_ = std::max(kPageSize, static_cast<size_t>(GetPageSize()));
  ASSERT(page_size_ % kPageSize == 0);

  rand_ = reinterpret_cast<uint64_t>(this);  // Initialize RNG seed.
  MapPages();
}

void GuardedPageAllocator::Destroy() {
  absl::base_internal::SpinLockHolder h(&guarded_page_lock_);
  if (initialized_) {
    size_t len = pages_end_addr_ - pages_base_addr_;
    int err = munmap(reinterpret_cast<void*>(pages_base_addr_), len);
    ASSERT(err != -1);
    (void)err;
    initialized_ = false;
  }
}

GuardedPageAllocator::AllocWithStatus GuardedPageAllocator::Allocate(
    size_t size, size_t alignment) {
  if (size == 0) {
    return {nullptr, Profile::Sample::GuardedStatus::TooSmall};
  }
  ssize_t free_slot = ReserveFreeSlot();
  // All slots are reserved.
  if (free_slot == -1) {
    return {nullptr, Profile::Sample::GuardedStatus::NoAvailableSlots};
  }

  ASSERT(size <= page_size_);
  ASSERT(alignment <= page_size_);
  ASSERT(alignment == 0 || absl::has_single_bit(alignment));
  void* result = reinterpret_cast<void*>(SlotToAddr(free_slot));
  if (mprotect(result, page_size_, PROT_READ | PROT_WRITE) == -1) {
    ASSERT(false && "mprotect failed");
    absl::base_internal::SpinLockHolder h(&guarded_page_lock_);
    num_failed_allocations_++;
    FreeSlot(free_slot);
    return {nullptr, Profile::Sample::GuardedStatus::MProtectFailed};
  }

  // Place some allocations at end of page for better overflow detection.
  MaybeRightAlign(free_slot, size, alignment, &result);

  // Record stack trace.
  SlotMetadata& d = data_[free_slot];
  d.dealloc_trace.depth = 0;
  d.alloc_trace.depth = absl::GetStackTrace(d.alloc_trace.stack, kMaxStackDepth,
                                            /*skip_count=*/3);
  d.alloc_trace.tid = absl::base_internal::GetTID();
  d.requested_size = size;
  d.allocation_start = reinterpret_cast<uintptr_t>(result);

  ASSERT(!alignment || d.allocation_start % alignment == 0);
  return {result, Profile::Sample::GuardedStatus::Guarded};
}

void GuardedPageAllocator::Deallocate(void* ptr) {
  ASSERT(PointerIsMine(ptr));
  const uintptr_t page_addr = GetPageAddr(reinterpret_cast<uintptr_t>(ptr));
  size_t slot = AddrToSlot(page_addr);

  absl::base_internal::SpinLockHolder h(&guarded_page_lock_);
  if (IsFreed(slot)) {
    double_free_detected_ = true;
  } else if (WriteOverflowOccurred(slot)) {
    write_overflow_detected_ = true;
  }

  CHECK_CONDITION(mprotect(reinterpret_cast<void*>(page_addr), page_size_,
                           PROT_NONE) != -1);

  if (write_overflow_detected_ || double_free_detected_) {
    *reinterpret_cast<char*>(ptr) = 'X';  // Trigger SEGV handler.
    CHECK_CONDITION(false);               // Unreachable.
  }

  // Record stack trace.
  GpaStackTrace& trace = data_[slot].dealloc_trace;
  trace.depth = absl::GetStackTrace(trace.stack, kMaxStackDepth,
                                    /*skip_count=*/2);
  trace.tid = absl::base_internal::GetTID();

  FreeSlot(slot);
}

size_t GuardedPageAllocator::GetRequestedSize(const void* ptr) const {
  ASSERT(PointerIsMine(ptr));
  size_t slot = AddrToSlot(GetPageAddr(reinterpret_cast<uintptr_t>(ptr)));
  return data_[slot].requested_size;
}

std::pair<off_t, size_t> GuardedPageAllocator::GetAllocationOffsetAndSize(
    const void* ptr) const {
  ASSERT(PointerIsMine(ptr));
  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  const size_t slot = GetNearestSlot(addr);
  return {addr - data_[slot].allocation_start, data_[slot].requested_size};
}

GuardedPageAllocator::ErrorType GuardedPageAllocator::GetStackTraces(
    const void* ptr, GpaStackTrace* alloc_trace,
    GpaStackTrace* dealloc_trace) const {
  ASSERT(PointerIsMine(ptr));
  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  size_t slot = GetNearestSlot(addr);
  *alloc_trace = data_[slot].alloc_trace;
  *dealloc_trace = data_[slot].dealloc_trace;
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
  absl::base_internal::SpinLockHolder h(&guarded_page_lock_);
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
      "PARAMETER tcmalloc_guarded_sample_parameter %d\n",
      num_allocation_requests_ - num_failed_allocations_,
      num_failed_allocations_, num_alloced_pages_,
      total_pages_ - num_alloced_pages_, num_alloced_pages_max_,
      max_alloced_pages_, GetChainedRate());
}

void GuardedPageAllocator::PrintInPbtxt(PbtxtRegion* gwp_asan) {
  absl::base_internal::SpinLockHolder h(&guarded_page_lock_);
  gwp_asan->PrintI64("successful_allocations",
                     num_allocation_requests_ - num_failed_allocations_);
  gwp_asan->PrintI64("failed_allocations", num_failed_allocations_);
  gwp_asan->PrintI64("current_slots_allocated", num_alloced_pages_);
  gwp_asan->PrintI64("current_slots_quarantined",
                     total_pages_ - num_alloced_pages_);
  gwp_asan->PrintI64("max_slots_allocated", num_alloced_pages_max_);
  gwp_asan->PrintI64("allocated_slot_limit", max_alloced_pages_);
  gwp_asan->PrintI64("tcmalloc_guarded_sample_parameter", GetChainedRate());
}

// Maps 2 * total_pages_ + 1 pages so that there are total_pages_ unique pages
// we can return from Allocate with guard pages before and after them.
void GuardedPageAllocator::MapPages() {
  absl::base_internal::SpinLockHolder h(&guarded_page_lock_);
  ASSERT(!first_page_addr_);
  ASSERT(page_size_ % GetPageSize() == 0);
  size_t len = (2 * total_pages_ + 1) * page_size_;
  auto base_addr = reinterpret_cast<uintptr_t>(
      MmapAligned(len, page_size_, MemoryTag::kSampled));
  ASSERT(base_addr);
  if (!base_addr) return;

  // Tell TCMalloc's PageMap about the memory we own.
  const PageId page = PageIdContaining(reinterpret_cast<void*>(base_addr));
  const Length page_len = BytesToLengthFloor(len);
  if (!tc_globals.pagemap().Ensure(page, page_len)) {
    ASSERT(false && "Failed to notify page map of page-guarded memory.");
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
  absl::base_internal::SpinLockHolder h(&guarded_page_lock_);
  if (!initialized_ || !allow_allocations_) return -1;
  num_allocation_requests_++;
  if (num_alloced_pages_ == max_alloced_pages_) {
    num_failed_allocations_++;
    return -1;
  }

  rand_ = Sampler::NextRandom(rand_);
  size_t num_free_pages = total_pages_ - num_alloced_pages_;
  size_t slot = GetIthFreeSlot(rand_ % num_free_pages);
  ASSERT(free_pages_[slot]);
  free_pages_[slot] = false;
  num_alloced_pages_++;
  num_alloced_pages_max_ = std::max(num_alloced_pages_, num_alloced_pages_max_);
  return slot;
}

size_t GuardedPageAllocator::GetIthFreeSlot(size_t ith_free_slot) {
  ASSERT(ith_free_slot < total_pages_ - num_alloced_pages_);
  for (size_t free_slot_count = 0, j = 0;; j++) {
    if (free_pages_[j]) {
      if (free_slot_count == ith_free_slot) return j;
      free_slot_count++;
    }
  }
}

void GuardedPageAllocator::FreeSlot(size_t slot) {
  ASSERT(slot < total_pages_);
  ASSERT(!free_pages_[slot]);
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

GuardedPageAllocator::ErrorType GuardedPageAllocator::GetErrorType(
    uintptr_t addr, const SlotMetadata& d) const {
  if (!d.allocation_start) return ErrorType::kUnknown;
  if (double_free_detected_) return ErrorType::kDoubleFree;
  if (write_overflow_detected_) return ErrorType::kBufferOverflowOnDealloc;
  if (d.dealloc_trace.depth) return ErrorType::kUseAfterFree;
  if (addr < d.allocation_start) return ErrorType::kBufferUnderflow;
  if (addr >= d.allocation_start + d.requested_size) {
    return ErrorType::kBufferOverflow;
  }
  return ErrorType::kUnknown;
}

uintptr_t GuardedPageAllocator::SlotToAddr(size_t slot) const {
  ASSERT(slot < total_pages_);
  return first_page_addr_ + 2 * slot * page_size_;
}

size_t GuardedPageAllocator::AddrToSlot(uintptr_t addr) const {
  uintptr_t offset = addr - first_page_addr_;
  ASSERT(offset % page_size_ == 0);
  ASSERT((offset / page_size_) % 2 == 0);
  int slot = offset / page_size_ / 2;
  ASSERT(slot >= 0 && slot < total_pages_);
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

// If this failure occurs during "bazel test", writes a warning for Bazel to
// display.
static void RecordBazelWarning(absl::string_view error) {
  const char* warning_file = thread_safe_getenv("TEST_WARNINGS_OUTPUT_FILE");
  if (!warning_file) return;  // Not a bazel test.

  constexpr char warning[] = "GWP-ASan error detected: ";
  int fd = open(warning_file, O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd == -1) return;
  (void)write(fd, warning, sizeof(warning) - 1);
  (void)write(fd, error.data(), error.size());
  (void)write(fd, "\n", 1);
  close(fd);
}

// If this failure occurs during a gUnit test, writes an XML file describing the
// error type.  Note that we cannot use ::testing::Test::RecordProperty()
// because it doesn't write the XML file if a test crashes (which we're about to
// do here).  So we write directly to the XML file instead.
//
static void RecordTestFailure(absl::string_view error) {
  const char* xml_file = thread_safe_getenv("XML_OUTPUT_FILE");
  if (!xml_file) return;  // Not a gUnit test.

  // Record test failure for Sponge.
  constexpr char xml_text_header[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<testsuites><testsuite><testcase>"
      "  <properties>"
      "    <property name=\"gwp-asan-report\" value=\"";
  constexpr char xml_text_footer[] =
      "\"/>"
      "  </properties>"
      "  <failure message=\"MemoryError\">"
      "    GWP-ASan detected a memory error.  See the test log for full report."
      "  </failure>"
      "</testcase></testsuite></testsuites>";

  int fd = open(xml_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd == -1) return;
  (void)write(fd, xml_text_header, sizeof(xml_text_header) - 1);
  (void)write(fd, error.data(), error.size());
  (void)write(fd, xml_text_footer, sizeof(xml_text_footer) - 1);
  close(fd);
}
//
// If this crash occurs in a test, records test failure summaries.
//
// error contains the type of error to record.
static void RecordCrash(absl::string_view error) {

  RecordBazelWarning(error);
  RecordTestFailure(error);
}

static void PrintStackTrace(void** stack_frames, size_t depth) {
  for (size_t i = 0; i < depth; ++i) {
    Log(kLog, __FILE__, __LINE__, "  @  ", stack_frames[i]);
  }
}

static void PrintStackTraceFromSignalHandler(void* context) {
  void* stack_frames[kMaxStackDepth];
  size_t depth = absl::GetStackTraceWithContext(stack_frames, kMaxStackDepth, 1,
                                                context, nullptr);
  PrintStackTrace(stack_frames, depth);
}

// A SEGV handler that prints stack traces for the allocation and deallocation
// of relevant memory as well as the location of the memory error.
static void SegvHandler(int signo, siginfo_t* info, void* context) {
  if (signo != SIGSEGV) return;
  void* fault = info->si_addr;
  if (!tc_globals.guardedpage_allocator().PointerIsMine(fault)) return;
  GuardedPageAllocator::GpaStackTrace alloc_trace, dealloc_trace;
  GuardedPageAllocator::ErrorType error =
      tc_globals.guardedpage_allocator().GetStackTraces(fault, &alloc_trace,
                                                        &dealloc_trace);
  if (error == GuardedPageAllocator::ErrorType::kUnknown) return;
  pid_t current_thread = absl::base_internal::GetTID();
  off_t offset;
  size_t size;
  std::tie(offset, size) =
      tc_globals.guardedpage_allocator().GetAllocationOffsetAndSize(fault);

  Log(kLog, __FILE__, __LINE__,
      "*** GWP-ASan "
      "(https://google.github.io/tcmalloc/gwp-asan.html)  "
      "has detected a memory error ***");
  Log(kLog, __FILE__, __LINE__, ">>> Access at offset", offset,
      "into buffer of length", size);
  Log(kLog, __FILE__, __LINE__,
      "Error originates from memory allocated in thread", alloc_trace.tid,
      "at:");
  PrintStackTrace(alloc_trace.stack, alloc_trace.depth);

  switch (error) {
    case GuardedPageAllocator::ErrorType::kUseAfterFree:
      Log(kLog, __FILE__, __LINE__, "The memory was freed in thread",
          dealloc_trace.tid, "at:");
      PrintStackTrace(dealloc_trace.stack, dealloc_trace.depth);
      Log(kLog, __FILE__, __LINE__, "Use-after-free occurs in thread",
          current_thread, "at:");
      RecordCrash("use-after-free");
      break;
    case GuardedPageAllocator::ErrorType::kBufferUnderflow:
      Log(kLog, __FILE__, __LINE__, "Buffer underflow occurs in thread",
          current_thread, "at:");
      RecordCrash("buffer-underflow");
      break;
    case GuardedPageAllocator::ErrorType::kBufferOverflow:
      Log(kLog, __FILE__, __LINE__, "Buffer overflow occurs in thread",
          current_thread, "at:");
      RecordCrash("buffer-overflow");
      break;
    case GuardedPageAllocator::ErrorType::kDoubleFree:
      Log(kLog, __FILE__, __LINE__, "The memory was freed in thread",
          dealloc_trace.tid, "at:");
      PrintStackTrace(dealloc_trace.stack, dealloc_trace.depth);
      Log(kLog, __FILE__, __LINE__, "Double free occurs in thread",
          current_thread, "at:");
      RecordCrash("double-free");
      break;
    case GuardedPageAllocator::ErrorType::kBufferOverflowOnDealloc:
      Log(kLog, __FILE__, __LINE__,
          "Buffer overflow (write) detected in thread", current_thread,
          "at free:");
      RecordCrash("buffer-overflow-detected-at-free");
      break;
    case GuardedPageAllocator::ErrorType::kUnknown:
      Crash(kCrash, __FILE__, __LINE__, "Unexpected ErrorType::kUnknown");
  }
  PrintStackTraceFromSignalHandler(context);
  if (error == GuardedPageAllocator::ErrorType::kBufferOverflowOnDealloc) {
    Log(kLog, __FILE__, __LINE__,
        "*** Try rerunning with --config=asan to get stack trace of overflow "
        "***");
  }
}

static struct sigaction old_sa;

static void ForwardSignal(int signo, siginfo_t* info, void* context) {
  if (old_sa.sa_flags & SA_SIGINFO) {
    old_sa.sa_sigaction(signo, info, context);
  } else if (old_sa.sa_handler == SIG_DFL) {
    // No previous handler registered.  Re-raise signal for core dump.
    int err = sigaction(signo, &old_sa, nullptr);
    if (err == -1) {
      Log(kLog, __FILE__, __LINE__, "Couldn't restore previous sigaction!");
    }
    raise(signo);
  } else if (old_sa.sa_handler == SIG_IGN) {
    return;  // Previous sigaction ignored signal, so do the same.
  } else {
    old_sa.sa_handler(signo);
  }
}

static void HandleSegvAndForward(int signo, siginfo_t* info, void* context) {
  SegvHandler(signo, info, context);
  ForwardSignal(signo, info, context);
}

extern "C" void MallocExtension_Internal_ActivateGuardedSampling() {
  static absl::once_flag flag;
  absl::call_once(flag, []() {
    struct sigaction action = {};
    action.sa_sigaction = HandleSegvAndForward;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &old_sa);
    tc_globals.guardedpage_allocator().AllowAllocations();
  });
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

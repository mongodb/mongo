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

#include "tcmalloc/stats.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <limits>

#include "absl/base/internal/cycleclock.h"
#include "absl/base/macros.h"
#include "absl/numeric/bits.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

static double BytesToMiB(size_t bytes) {
  const double MiB = 1048576.0;
  return bytes / MiB;
}

// For example, PrintRightAdjustedWithPrefix(out, ">=", 42, 6) prints "  >=42".
static void PrintRightAdjustedWithPrefix(Printer* out, const char* prefix,
                                         Length num, int width) {
  width -= strlen(prefix);
  int num_tmp = num.raw_num();
  for (int i = 0; i < width - 1; i++) {
    num_tmp /= 10;
    if (num_tmp == 0) {
      out->printf(" ");
    }
  }
  out->printf("%s%zu", prefix, num.raw_num());
}

void PrintStats(const char* label, Printer* out, const BackingStats& backing,
                const SmallSpanStats& small, const LargeSpanStats& large,
                bool everything) {
  size_t nonempty_sizes = 0;
  for (int i = 0; i < kMaxPages.raw_num(); ++i) {
    const size_t norm = small.normal_length[i];
    const size_t ret = small.returned_length[i];
    if (norm + ret > 0) nonempty_sizes++;
  }

  out->printf("------------------------------------------------\n");
  out->printf("%s: %zu sizes; %6.1f MiB free; %6.1f MiB unmapped\n", label,
              nonempty_sizes, BytesToMiB(backing.free_bytes),
              BytesToMiB(backing.unmapped_bytes));
  out->printf("------------------------------------------------\n");

  Length cum_normal_pages, cum_returned_pages, cum_total_pages;
  if (!everything) return;

  for (size_t i = 0; i < kMaxPages.raw_num(); ++i) {
    const size_t norm = small.normal_length[i];
    const size_t ret = small.returned_length[i];
    const size_t total = norm + ret;
    if (total == 0) continue;
    const Length norm_pages = Length(norm * i);
    const Length ret_pages = Length(ret * i);
    const Length total_pages = norm_pages + ret_pages;
    cum_normal_pages += norm_pages;
    cum_returned_pages += ret_pages;
    cum_total_pages += total_pages;
    out->printf(
        "%6zu pages * %6zu spans ~ %6.1f MiB; %6.1f MiB cum"
        "; unmapped: %6.1f MiB; %6.1f MiB cum\n",
        i, total, total_pages.in_mib(), cum_total_pages.in_mib(),
        ret_pages.in_mib(), cum_returned_pages.in_mib());
  }

  cum_normal_pages += large.normal_pages;
  cum_returned_pages += large.returned_pages;
  const Length large_total_pages = large.normal_pages + large.returned_pages;
  cum_total_pages += large_total_pages;
  PrintRightAdjustedWithPrefix(out, ">=", kMaxPages, 6);
  out->printf(
      " large * %6zu spans ~ %6.1f MiB; %6.1f MiB cum"
      "; unmapped: %6.1f MiB; %6.1f MiB cum\n",
      static_cast<size_t>(large.spans), large_total_pages.in_mib(),
      cum_total_pages.in_mib(), large.returned_pages.in_mib(),
      cum_returned_pages.in_mib());
}

struct HistBucket {};

struct PageHeapEntry {
  int64_t span_size;  // bytes
  int64_t present;    // bytes
  int64_t released;   // bytes
  int64_t num_spans;

  void PrintInPbtxt(PbtxtRegion* parent,
                    absl::string_view sub_region_name) const;
};

void PageHeapEntry::PrintInPbtxt(PbtxtRegion* parent,
                                 absl::string_view sub_region_name) const {
  auto page_heap = parent->CreateSubRegion(sub_region_name);
  page_heap.PrintI64("span_size", span_size);
  page_heap.PrintI64("present", present);
  page_heap.PrintI64("released", released);
  page_heap.PrintI64("num_spans", num_spans);
}

void PrintStatsInPbtxt(PbtxtRegion* region, const SmallSpanStats& small,
                       const LargeSpanStats& large) {
  // Print for small pages.
  for (auto i = Length(0); i < kMaxPages; ++i) {
    const size_t norm = small.normal_length[i.raw_num()];
    const size_t ret = small.returned_length[i.raw_num()];
    const size_t total = norm + ret;
    if (total == 0) continue;
    const Length norm_pages = norm * i;
    const Length ret_pages = ret * i;
    PageHeapEntry entry;
    entry.span_size = i.in_bytes();
    entry.present = norm_pages.in_bytes();
    entry.released = ret_pages.in_bytes();
    entry.num_spans = total;

    entry.PrintInPbtxt(region, "page_heap");
  }

  // Print for large page.
  {
    PageHeapEntry entry;
    entry.span_size = -1;
    entry.num_spans = large.spans;
    entry.present = large.normal_pages.in_bytes();
    entry.released = large.returned_pages.in_bytes();
    entry.PrintInPbtxt(region, "page_heap");
  }

  region->PrintI64("min_large_span_size", kMaxPages.raw_num());
}

void PageAllocInfo::Print(Printer* out) const {
  int64_t ticks = TimeTicks();
  double hz = freq_ / ticks;
  out->printf("%s: stats on allocation sizes\n", label_);
  out->printf("%s: %zu pages live small allocation\n", label_,
              total_small_.raw_num());
  out->printf("%s: %zu pages of slack on large allocations\n", label_,
              total_slack_.raw_num());
  out->printf("%s: largest seen allocation %zu pages\n", label_,
              largest_seen_.raw_num());
  out->printf("%s: per-size information:\n", label_);

  auto print_counts = [this, hz, out](const Counts& c, Length nmin,
                                      Length nmax) {
    const size_t a = c.nalloc;
    const size_t f = c.nfree;
    const Length a_pages = c.alloc_size;
    const Length f_pages = c.free_size;
    if (a == 0) return;
    const size_t live = a - f;
    const double live_mib = (a_pages - f_pages).in_mib();
    const double rate_hz = a * hz;
    const double mib_hz = a_pages.in_mib() * hz;
    if (nmin == nmax) {
      out->printf("%s: %21zu page info: ", label_, nmin.raw_num());
    } else {
      out->printf("%s: [ %7zu , %7zu ] page info: ", label_, nmin.raw_num(),
                  nmax.raw_num());
    }
    out->printf(
        "%10zu / %10zu a/f, %8zu (%6.1f MiB) live, "
        "%8.3f allocs/s (%6.1f MiB/s)\n",
        a, f, live, live_mib, rate_hz, mib_hz);
  };

  for (auto i = Length(0); i < kMaxPages; ++i) {
    const Length n = i + Length(1);
    print_counts(small_[i.raw_num()], n, n);
  }

  for (int i = 0; i < kAddressBits - kPageShift; ++i) {
    const Length nmax = Length(uintptr_t{1} << i);
    const Length nmin = nmax / 2 + Length(1);
    print_counts(large_[i], nmin, nmax);
  }
}

void PageAllocInfo::PrintInPbtxt(PbtxtRegion* region,
                                 absl::string_view stat_name) const {
  int64_t ticks = TimeTicks();
  double hz = freq_ / ticks;
  region->PrintI64("num_small_allocation_pages", total_small_.raw_num());
  region->PrintI64("num_slack_pages", total_slack_.raw_num());
  region->PrintI64("largest_allocation_pages", largest_seen_.raw_num());

  auto print_counts = [hz, region, &stat_name](const Counts& c, Length nmin,
                                               Length nmax) {
    const size_t a = c.nalloc;
    const size_t f = c.nfree;
    const Length a_pages = c.alloc_size;
    const Length f_pages = c.free_size;
    if (a == 0) return;
    const int64_t live_bytes = (a_pages - f_pages).in_bytes();
    const double rate_hz = a * hz;
    const double bytes_hz = static_cast<double>(a_pages.in_bytes()) * hz;
    auto stat = region->CreateSubRegion(stat_name);
    stat.PrintI64("min_span_pages", nmin.raw_num());
    stat.PrintI64("max_span_pages", nmax.raw_num());
    stat.PrintI64("num_spans_allocated", a);
    stat.PrintI64("num_spans_freed", f);
    stat.PrintI64("live_bytes", live_bytes);
    stat.PrintDouble("spans_allocated_per_second", rate_hz);
    stat.PrintI64("bytes_allocated_per_second", static_cast<int64_t>(bytes_hz));
  };

  for (auto i = Length(0); i < kMaxPages; ++i) {
    const Length n = i + Length(1);
    print_counts(small_[i.raw_num()], n, n);
  }

  for (int i = 0; i < kAddressBits - kPageShift; ++i) {
    const Length nmax = Length(uintptr_t(1) << i);
    const Length nmin = nmax / 2 + Length(1);
    print_counts(large_[i], nmin, nmax);
  }
}

static Length RoundUp(Length value, Length alignment) {
  return Length((value.raw_num() + alignment.raw_num() - 1) &
                ~(alignment.raw_num() - 1));
}

void PageAllocInfo::RecordAlloc(PageId p, Length n) {
  static_assert(kMaxPages.in_bytes() == 1024 * 1024, "threshold changed?");
  static_assert(kMaxPages < kPagesPerHugePage, "there should be slack");
  largest_seen_ = std::max(largest_seen_, n);
  if (n <= kMaxPages) {
    total_small_ += n;
    small_[(n - Length(1)).raw_num()].Alloc(n);
  } else {
    Length slack = RoundUp(n, kPagesPerHugePage) - n;
    total_slack_ += slack;
    size_t i = absl::bit_width(n.raw_num() - 1);
    large_[i].Alloc(n);
  }
}

void PageAllocInfo::RecordFree(PageId p, Length n) {
  if (n <= kMaxPages) {
    total_small_ -= n;
    small_[n.raw_num() - 1].Free(n);
  } else {
    Length slack = RoundUp(n, kPagesPerHugePage) - n;
    total_slack_ -= slack;
    size_t i = absl::bit_width(n.raw_num() - 1);
    large_[i].Free(n);
  }
}

void PageAllocInfo::RecordRelease(Length n, Length got) {}

const PageAllocInfo::Counts& PageAllocInfo::counts_for(Length n) const {
  if (n <= kMaxPages) {
    return small_[n.raw_num() - 1];
  }
  size_t i = absl::bit_width(n.raw_num() - 1);
  return large_[i];
}

PageAllocInfo::PageAllocInfo(const char* label) : label_(label) {}

int64_t PageAllocInfo::TimeTicks() const {
  return absl::base_internal::CycleClock::Now() - baseline_ticks_;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

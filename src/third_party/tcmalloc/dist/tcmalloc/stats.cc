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

#include <inttypes.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cstdint>
#include <limits>

#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/macros.h"
#include "absl/numeric/bits.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"
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

struct HistBucket {
  uint64_t min_sec;
  const char* label;
};

static const HistBucket kSpanAgeHistBuckets[] = {
    // clang-format off
    {0, "<1s"},
    {1, "1s"},
    {30, "30s"},
    {1 * 60, "1m"},
    {30 * 60, "30m"},
    {1 * 60 * 60, "1h"},
    {8 * 60 * 60, "8+h"},
    // clang-format on
};

struct PageHeapEntry {
  int64_t span_size;  // bytes
  int64_t present;    // bytes
  int64_t released;   // bytes
  int64_t num_spans;
  double avg_live_age_secs;
  double avg_released_age_secs;
  int64_t live_age_hist_bytes[PageAgeHistograms::kNumBuckets] = {0, 0, 0, 0,
                                                                 0, 0, 0};
  int64_t released_age_hist_bytes[PageAgeHistograms::kNumBuckets] = {0, 0, 0, 0,
                                                                     0, 0, 0};

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
  page_heap.PrintDouble("avg_live_age_secs", avg_live_age_secs);
  page_heap.PrintDouble("avg_released_age_secs", avg_released_age_secs);

  for (int j = 0; j < PageAgeHistograms::kNumBuckets; j++) {
    uint64_t min_age_secs = kSpanAgeHistBuckets[j].min_sec;
    uint64_t max_age_secs = j != PageAgeHistograms::kNumBuckets - 1
                                ? kSpanAgeHistBuckets[j + 1].min_sec
                                : INT_MAX;
    if (live_age_hist_bytes[j] != 0) {
      auto live_age_hist = page_heap.CreateSubRegion("live_age_hist");
      live_age_hist.PrintI64("bytes", live_age_hist_bytes[j]);
      live_age_hist.PrintI64("min_age_secs", min_age_secs);
      live_age_hist.PrintI64("max_age_secs", max_age_secs);
    }
    if (released_age_hist_bytes[j] != 0) {
      auto released_age_hist = page_heap.CreateSubRegion("released_age_hist");
      released_age_hist.PrintI64("bytes", released_age_hist_bytes[j]);
      released_age_hist.PrintI64("min_age_secs", min_age_secs);
      released_age_hist.PrintI64("max_age_secs", max_age_secs);
    }
  }
}

void PrintStatsInPbtxt(PbtxtRegion* region, const SmallSpanStats& small,
                       const LargeSpanStats& large,
                       const PageAgeHistograms& ages) {
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

    // Histogram is only collected for pages < ages.kNumSize.
    if (i < Length(PageAgeHistograms::kNumSizes)) {
      entry.avg_live_age_secs =
          ages.GetSmallHistogram(/*released=*/false, i)->avg_age();
      entry.avg_released_age_secs =
          ages.GetSmallHistogram(/*released=*/true, i)->avg_age();
      for (int j = 0; j < ages.kNumBuckets; j++) {
        entry.live_age_hist_bytes[j] =
            ages.GetSmallHistogram(/*released=*/false, i)->pages_in_bucket(j) *
            kPageSize;
        entry.released_age_hist_bytes[j] =
            ages.GetSmallHistogram(/*released=*/true, i)->pages_in_bucket(j) *
            kPageSize;
      }
    }
    entry.PrintInPbtxt(region, "page_heap");
  }

  // Print for large page.
  {
    PageHeapEntry entry;
    entry.span_size = -1;
    entry.num_spans = large.spans;
    entry.present = large.normal_pages.in_bytes();
    entry.released = large.returned_pages.in_bytes();
    entry.avg_live_age_secs =
        ages.GetLargeHistogram(/*released=*/false)->avg_age();
    entry.avg_released_age_secs =
        ages.GetLargeHistogram(/*released=*/true)->avg_age();
    for (int j = 0; j < ages.kNumBuckets; j++) {
      entry.live_age_hist_bytes[j] =
          ages.GetLargeHistogram(/*released=*/false)->pages_in_bucket(j) *
          kPageSize;
      entry.released_age_hist_bytes[j] =
          ages.GetLargeHistogram(/*released=*/true)->pages_in_bucket(j) *
          kPageSize;
    }
    entry.PrintInPbtxt(region, "page_heap");
  }

  region->PrintI64("min_large_span_size", kMaxPages.raw_num());
}

static int HistBucketIndex(double age_exact) {
  uint64_t age_secs = age_exact;  // truncate to seconds
  for (int i = 0; i < ABSL_ARRAYSIZE(kSpanAgeHistBuckets) - 1; i++) {
    if (age_secs < kSpanAgeHistBuckets[i + 1].min_sec) {
      return i;
    }
  }
  return ABSL_ARRAYSIZE(kSpanAgeHistBuckets) - 1;
}

PageAgeHistograms::PageAgeHistograms(int64_t now)
    : now_(now), freq_(absl::base_internal::CycleClock::Frequency()) {
  static_assert(
      PageAgeHistograms::kNumBuckets == ABSL_ARRAYSIZE(kSpanAgeHistBuckets),
      "buckets don't match constant in header");
}

void PageAgeHistograms::RecordRange(Length pages, bool released, int64_t when) {
  double age = std::max(0.0, (now_ - when) / freq_);
  (released ? returned_ : live_).Record(pages, age);
}

void PageAgeHistograms::PerSizeHistograms::Record(Length pages, double age) {
  (pages < kLargeSize ? GetSmall(pages) : GetLarge())->Record(pages, age);
  total.Record(pages, age);
}

static uint32_t SaturatingAdd(uint32_t x, uint32_t y) {
  uint32_t z = x + y;
  if (z < x) z = std::numeric_limits<uint32_t>::max();
  return z;
}

void PageAgeHistograms::Histogram::Record(Length pages, double age) {
  size_t bucket = HistBucketIndex(age);
  buckets_[bucket] = SaturatingAdd(buckets_[bucket], pages.raw_num());
  total_pages_ += pages;
  total_age_ += pages.raw_num() * age;
}

void PageAgeHistograms::Print(const char* label, Printer* out) const {
  out->printf("------------------------------------------------\n");
  out->printf(
      "%s cache entry age (count of pages in spans of "
      "a given size that have been idle for up to the given period of time)\n",
      label);
  out->printf("------------------------------------------------\n");
  out->printf("                             ");
  // Print out the table header.  All columns have width 8 chars.
  out->printf("    mean");
  for (int b = 0; b < kNumBuckets; b++) {
    out->printf("%8s", kSpanAgeHistBuckets[b].label);
  }
  out->printf("\n");

  live_.Print("Live span", out);
  out->printf("\n");
  returned_.Print("Unmapped span", out);
}

static void PrintLineHeader(Printer* out, const char* kind, const char* prefix,
                            Length num) {
  // Print the beginning of the line, e.g. "Live span,   >=128 pages: ".  The
  // span size ("128" in the example) is padded such that it plus the span
  // prefix ("Live") plus the span size prefix (">=") is kHeaderExtraChars wide.
  const int kHeaderExtraChars = 19;
  const int span_size_width =
      std::max<int>(0, kHeaderExtraChars - strlen(kind));
  out->printf("%s, ", kind);
  PrintRightAdjustedWithPrefix(out, prefix, num, span_size_width);
  out->printf(" pages: ");
}

void PageAgeHistograms::PerSizeHistograms::Print(const char* kind,
                                                 Printer* out) const {
  out->printf("%-15s TOTAL PAGES: ", kind);
  total.Print(out);

  for (auto l = Length(1); l < Length(kNumSizes); ++l) {
    const Histogram* here = &small[l.raw_num() - 1];
    if (here->empty()) continue;
    PrintLineHeader(out, kind, "", l);
    here->Print(out);
  }

  if (!large.empty()) {
    PrintLineHeader(out, kind, ">=", Length(kNumSizes));
    large.Print(out);
  }
}

void PageAgeHistograms::Histogram::Print(Printer* out) const {
  const double mean = avg_age();
  out->printf(" %7.1f", mean);
  for (int b = 0; b < kNumBuckets; ++b) {
    out->printf(" %7u", buckets_[b]);
  }

  out->printf("\n");
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

void PageAllocInfo::RecordAlloc(PageId p, Length n, size_t num_objects) {
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

void PageAllocInfo::RecordFree(PageId p, Length n, size_t num_objects) {
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

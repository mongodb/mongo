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

#ifndef TCMALLOC_STATS_H_
#define TCMALLOC_STATS_H_

#include <stddef.h>
#include <stdint.h>

#include "absl/base/internal/cycleclock.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct BackingStats {
  BackingStats() : system_bytes(0), free_bytes(0), unmapped_bytes(0) {}
  uint64_t system_bytes;    // Total bytes allocated from system
  uint64_t free_bytes;      // Total bytes on normal freelists
  uint64_t unmapped_bytes;  // Total bytes on returned freelists

  BackingStats& operator+=(BackingStats rhs) {
    system_bytes += rhs.system_bytes;
    free_bytes += rhs.free_bytes;
    unmapped_bytes += rhs.unmapped_bytes;
    return *this;
  }
};

inline BackingStats operator+(BackingStats lhs, BackingStats rhs) {
  return lhs += rhs;
}

struct SmallSpanStats {
  // For each free list of small spans, the length (in spans) of the
  // normal and returned free lists for that size.
  int64_t normal_length[kMaxPages.raw_num()] = {0};
  int64_t returned_length[kMaxPages.raw_num()] = {0};

  SmallSpanStats& operator+=(SmallSpanStats rhs) {
    for (size_t i = 0; i < kMaxPages.raw_num(); ++i) {
      normal_length[i] += rhs.normal_length[i];
      returned_length[i] += rhs.returned_length[i];
    }
    return *this;
  }
};

inline SmallSpanStats operator+(SmallSpanStats lhs, SmallSpanStats rhs) {
  return lhs += rhs;
}

// Stats for free large spans (i.e., spans with more than kMaxPages pages).
struct LargeSpanStats {
  size_t spans = 0;       // Number of such spans
  Length normal_pages;    // Combined page length of normal large spans
  Length returned_pages;  // Combined page length of unmapped spans

  LargeSpanStats& operator+=(LargeSpanStats rhs) {
    spans += rhs.spans;
    normal_pages += rhs.normal_pages;
    returned_pages += rhs.returned_pages;
    return *this;
  }
};

inline LargeSpanStats operator+(LargeSpanStats lhs, LargeSpanStats rhs) {
  return lhs += rhs;
}

void PrintStats(const char* label, Printer* out, const BackingStats& backing,
                const SmallSpanStats& small, const LargeSpanStats& large,
                bool everything);

void PrintStatsInPbtxt(PbtxtRegion* region, const SmallSpanStats& small,
                       const LargeSpanStats& large);

class PageAllocInfo {
 private:
  struct Counts;

 public:
  PageAllocInfo(const char* label);

  // Subclasses are responsible for calling these methods when
  // the relevant actions occur
  void RecordAlloc(PageId p, Length n);
  void RecordFree(PageId p, Length n);
  void RecordRelease(Length n, Length got);
  // And invoking this in their Print() implementation.
  void Print(Printer* out) const;
  void PrintInPbtxt(PbtxtRegion* region, absl::string_view stat_name) const;

  // Total size of allocations < 1 MiB
  Length small() const { return total_small_; }
  // We define the "slack" of an allocation as the difference
  // between its size and the nearest hugepage multiple (i.e. how
  // much would go unused if we allocated it as an aligned hugepage
  // and didn't use the rest.)
  // Return the total slack of all non-small allocations.
  Length slack() const { return total_slack_; }

  const Counts& counts_for(Length n) const;

  // Returns (approximate) CycleClock ticks since class instantiation.
  int64_t TimeTicks() const;

 private:
  Length total_small_;
  Length total_slack_;

  Length largest_seen_;

  // How many alloc/frees have we seen (of some size range?)
  struct Counts {
    // raw counts
    size_t nalloc{0}, nfree{0};
    // and total sizes (needed if this struct tracks a nontrivial range
    Length alloc_size;
    Length free_size;

    void Alloc(Length n) {
      nalloc++;
      alloc_size += n;
    }
    void Free(Length n) {
      nfree++;
      free_size += n;
    }
  };

  // Indexed by exact length
  Counts small_[kMaxPages.raw_num()];
  // Indexed by power-of-two-buckets
  Counts large_[kAddressBits - kPageShift];
  const char* label_;

  const int64_t baseline_ticks_{absl::base_internal::CycleClock::Now()};
  const double freq_{absl::base_internal::CycleClock::Frequency()};
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_STATS_H_

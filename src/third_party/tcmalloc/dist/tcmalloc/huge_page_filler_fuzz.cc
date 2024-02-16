// Copyright 2022 The TCMalloc Authors
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

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "tcmalloc/huge_page_filler.h"
#include "tcmalloc/internal/clock.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"

namespace {

using tcmalloc::tcmalloc_internal::Clock;
using tcmalloc::tcmalloc_internal::FillerPartialRerelease;
using tcmalloc::tcmalloc_internal::HugePage;
using tcmalloc::tcmalloc_internal::HugePageFiller;
using tcmalloc::tcmalloc_internal::kPagesPerHugePage;
using tcmalloc::tcmalloc_internal::Length;
using tcmalloc::tcmalloc_internal::LengthFromBytes;
using tcmalloc::tcmalloc_internal::MemoryModifyFunction;
using tcmalloc::tcmalloc_internal::NHugePages;
using tcmalloc::tcmalloc_internal::pageheap_lock;
using tcmalloc::tcmalloc_internal::PageId;
using tcmalloc::tcmalloc_internal::PageIdContaining;
using tcmalloc::tcmalloc_internal::PageTracker;
using tcmalloc::tcmalloc_internal::Printer;
using tcmalloc::tcmalloc_internal::SkipSubreleaseIntervals;

// As we read the fuzzer input, we update these variables to control global
// state.
int64_t fake_clock = 0;
bool unback_success = true;

int64_t mock_clock() { return fake_clock; }

double freq() { return 1 << 10; }

absl::flat_hash_set<PageId>& ReleasedPages() {
  static auto* set = new absl::flat_hash_set<PageId>();
  return *set;
}

bool MockUnback(void* start, size_t len) {
  if (!unback_success) {
    return false;
  }

  absl::flat_hash_set<PageId>& released_set = ReleasedPages();

  PageId p = PageIdContaining(start);
  Length l = LengthFromBytes(len);
  PageId end = p + l;
  for (; p != end; ++p) {
    released_set.insert(p);
  }

  return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 3 || size > 100000) {
    // size < 3 for needing some entropy to initialize the filler with.
    //
    // size > 100000 for avoiding overly large inputs given we do extra
    // checking.
    return 0;
  }

  // Reset global state.
  fake_clock = 0;
  unback_success = true;
  absl::flat_hash_set<PageId>& released_set = ReleasedPages();
  released_set.clear();
  // To avoid reentrancy during unback, reserve space in released_set.
  // We have at most size/5 allocations, for at most kPagesPerHugePage pages
  // each, that we can track the released status of.
  //
  // TODO(b/73749855): Releasing the pageheap_lock during ReleaseFree will
  // eliminate the need for this.
  released_set.reserve(kPagesPerHugePage.raw_num() * size / 5);

  // We interpret data as a small DSL for exploring the state space of
  // HugePageFiller.
  //
  // [0] - We choose the partial release parameter.
  // [1] - We choose the separate_allocs_for_few_and_many_objects_spans
  // parameter.
  //
  // Afterwards, we read 5 bytes at a time until the buffer is exhausted.
  // [i + 0]        - Specifies an operation to perform on the filler (allocate,
  //                  deallocate, release memory, gather stats, etc.)
  // [i + 1, i + 4] - Specifies an integer.  We use this as a source of
  //                  deterministic entropy to allow inputs to be replayed.
  //                  For example, this input can provide a Length to
  //                  allocate, or the index of the previous allocation to
  //                  deallocate.

  const FillerPartialRerelease partial_rerelease =
      data[0] ? FillerPartialRerelease::Retain : FillerPartialRerelease::Return;
  const bool separate_allocs_for_few_and_many_objects_spans =
      data[1] >= 128 ? true : false;
  data += 2;
  size -= 2;

  HugePageFiller<PageTracker> filler(
      partial_rerelease, Clock{.now = mock_clock, .freq = freq},
      separate_allocs_for_few_and_many_objects_spans,
      MemoryModifyFunction(MockUnback));

  struct Alloc {
    PageId page;
    Length length;
    size_t num_objects;
  };

  std::vector<PageTracker*> trackers;
  absl::flat_hash_map<PageTracker*, std::vector<Alloc>> allocs;

  // Running counter to allocate psuedo-random addresses
  size_t next_hugepage = 1;

  for (size_t i = 0; i + 5 <= size; i += 5) {
    const uint8_t op = data[i];
    uint32_t value;
    memcpy(&value, &data[i + 1], sizeof(value));

    switch (op & 0x7) {
      case 0: {
        // Allocate.  We divide up our random value by:
        //
        // value[0:15]  - We choose a Length to allocate.
        // value[16:31] - We select num_to_objects.
        const Length n(std::clamp<size_t>(value & 0xFFFF, 1,
                                          kPagesPerHugePage.raw_num() - 1));
        size_t num_objects;
        const uint32_t lval = (value >> 16);
        // Choose many objects if the last bit is 1.
        if (lval & 1) {
          num_objects = std::max<size_t>(
              lval >> 1,
              tcmalloc::tcmalloc_internal::kFewObjectsAllocMaxLimit + 1);
        } else {
          // We need to choose few objects, so only top four bits are used.
          num_objects = std::max<size_t>(lval >> 12, 1);
        }
        absl::flat_hash_set<PageId>& released_set = ReleasedPages();

        CHECK_EQ(filler.size().raw_num(), trackers.size());
        CHECK_EQ(filler.unmapped_pages().raw_num(), released_set.size());

        HugePageFiller<PageTracker>::TryGetResult result;
        {
          absl::base_internal::SpinLockHolder l(&pageheap_lock);
          result = filler.TryGet(n, num_objects);
        }

        if (result.pt == nullptr) {
          // Failed to allocate.  Create a new huge page.
          //
          // Donated pages do not necessarily have to have a particular size,
          // since this may be (kPagesPerHugePage/2,kPagesPerHugePage) in size
          // *or* the tail of an allocation >PagesPerHugePage.
          //
          // Since small objects are likely to be found, we model those tail
          // donations separately.
          const bool donated = n > kPagesPerHugePage / 2;
          result.pt = new PageTracker(HugePage{.pn = next_hugepage},
                                      mock_clock(), donated);
          next_hugepage++;
          {
            absl::base_internal::SpinLockHolder l(&pageheap_lock);

            result.page = result.pt->Get(n).page;
            filler.Contribute(result.pt, donated, num_objects);
          }

          trackers.push_back(result.pt);
        }

        // We have now successfully allocated.  Record the alloc and clear any
        // released bits.
        for (PageId p = result.page, end = p + n; p != end; ++p) {
          released_set.erase(p);
        }

        allocs[result.pt].push_back({result.page, n, num_objects});

        CHECK_EQ(filler.size().raw_num(), trackers.size());
        CHECK_EQ(filler.unmapped_pages().raw_num(), released_set.size());

        break;
      }
      case 1: {
        // Deallocate.
        //
        // value[0:15]  - Index of the huge page (from trackers) to select
        // value[16:31] - Index of the allocation (on pt) to select
        if (trackers.empty()) {
          break;
        }

        const size_t lo = std::min<size_t>(value & 0xFFFF, trackers.size() - 1);
        PageTracker* pt = trackers[lo];

        CHECK(!allocs[pt].empty());
        const size_t hi = std::min<size_t>(value >> 16, allocs[pt].size() - 1);
        Alloc alloc = allocs[pt][hi];

        // Remove the allocation.
        std::swap(allocs[pt][hi], allocs[pt].back());
        allocs[pt].resize(allocs[pt].size() - 1);
        bool last_alloc = allocs[pt].empty();
        if (last_alloc) {
          allocs.erase(pt);
          std::swap(trackers[lo], trackers.back());
          trackers.resize(trackers.size() - 1);
        }

        PageTracker* ret;
        {
          absl::base_internal::SpinLockHolder l(&pageheap_lock);
          ret = filler.Put(pt, alloc.page, alloc.length, alloc.num_objects);
        }
        CHECK_EQ(ret != nullptr, last_alloc);
        absl::flat_hash_set<PageId>& released_set = ReleasedPages();
        if (ret) {
          // Clear released_set, since the page has become free.
          HugePage hp = ret->location();
          for (PageId p = hp.first_page(),
                      end = hp.first_page() + kPagesPerHugePage;
               p != end; ++p) {
            released_set.erase(p);
          }
          delete ret;
        }

        CHECK_EQ(filler.size().raw_num(), trackers.size());
        CHECK_EQ(filler.unmapped_pages().raw_num(), released_set.size());

        break;
      }
      case 2: {
        // Release
        //
        // value[0]    - Whether are trying to apply TCMalloc's memory limits
        // value[1]    - Whether using peak interval for skip subrelease
        // If using peak interval:
        // value[2:9]  - Peak interval for skip subrelease
        // value[10:31]- Number of pages to try to release
        // If not using peak interval:
        // value[2:9]  - Short interval for skip subrelease
        // value[10:17]- Long interval for skip subrelease
        // value[18:31]- Number of pages to try to release
        bool hit_limit = value & 0x1;
        bool use_peak_interval = (value >> 1) & 0x1;
        SkipSubreleaseIntervals skip_subrelease_intervals;
        if (use_peak_interval) {
          const uint32_t peak_interval_s = (value >> 2) & 0xFF;
          skip_subrelease_intervals.peak_interval =
              absl::Seconds(peak_interval_s);
          value >>= 10;
        } else {
          uint32_t short_interval_s = (value >> 2) & 0xFF;
          uint32_t long_interval_s = (value >> 10) & 0xFF;
          if (short_interval_s > long_interval_s) {
            std::swap(short_interval_s, long_interval_s);
          }
          skip_subrelease_intervals.short_interval =
              absl::Seconds(short_interval_s);
          skip_subrelease_intervals.long_interval =
              absl::Seconds(long_interval_s);
          value >>= 18;
        }
        Length desired(value);

        Length released;
        {
          absl::base_internal::SpinLockHolder l(&pageheap_lock);
          released = filler.ReleasePages(desired, skip_subrelease_intervals,
                                         hit_limit);
        }
        break;
      }
      case 3: {
        // Advance clock
        //
        // value[0:31] - Advances clock by this amount in arbitrary units.
        fake_clock += value;
        break;
      }
      case 4: {
        // Toggle unback, simulating madvise potentially failing or succeeding.
        //
        // value is unused.
        unback_success = !unback_success;
        break;
      }
      case 5: {
        // Gather stats
        //
        // value is unused.
        std::string s;
        s.resize(1 << 20);
        Printer p(&s[0], s.size());
        absl::base_internal::SpinLockHolder l(&pageheap_lock);
        filler.Print(&p, true);
        break;
      }
      case 6: {
        // Model a tail from a larger allocation.  The tail can have any size
        // [1,kPagesPerHugePage).
        //
        // value[0:15]  - We choose a Length to allocate.
        // value[16:31] - Unused.
        const Length n(std::clamp<size_t>(value & 0xFFFF, 1,
                                          kPagesPerHugePage.raw_num() - 1));
        absl::flat_hash_set<PageId>& released_set = ReleasedPages();

        auto* pt = new PageTracker(HugePage{.pn = next_hugepage}, mock_clock(),
                                   /*was_donated=*/true);
        next_hugepage++;
        PageId start;
        {
          absl::base_internal::SpinLockHolder l(&pageheap_lock);

          start = pt->Get(n).page;
          filler.Contribute(pt, /*donated=*/true, /*num_objects=*/1);
        }

        trackers.push_back(pt);

        // We have now successfully allocated.  Record the alloc and clear any
        // released bits.
        for (PageId p = start, end = p + n; p != end; ++p) {
          released_set.erase(p);
        }

        allocs[pt].push_back({start, n, 1});

        CHECK_EQ(filler.size().raw_num(), trackers.size());
        CHECK_EQ(filler.unmapped_pages().raw_num(), released_set.size());
        break;
      }
    }
  }

  // Shut down, confirm filler is empty.
  CHECK_EQ(ReleasedPages().size(), filler.unmapped_pages().raw_num());
  for (auto& [pt, v] : allocs) {
    for (size_t i = 0, n = v.size(); i < n; ++i) {
      auto alloc = v[i];
      PageTracker* ret;
      {
        absl::base_internal::SpinLockHolder l(&pageheap_lock);
        ret = filler.Put(pt, alloc.page, alloc.length, alloc.num_objects);
      }
      CHECK_EQ(ret != nullptr, i + 1 == n);
    }

    delete pt;
  }

  CHECK(filler.size() == NHugePages(0));
  return 0;
}

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

#include "tcmalloc/internal/pageflags.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kernel-page-flags.h>
#include <stddef.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>

#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {
// From include/uapi/linux/kernel-page-flags.h
#define KPF_COMPOUND_HEAD 15
#define KPF_COMPOUND_TAIL 16
#define KPF_THP 22
#ifndef KPF_MLOCKED
#define KPF_MLOCKED 33
#endif
#define KPF_STALE 44

// If a page is Head or Tail it is a compound page. It cannot be both, but it
// can be neither, in which case it's just a native page and no special handling
// needs to be done.
constexpr bool PageHead(uint64_t flags) {
  constexpr uint64_t kPageHead = (1UL << KPF_COMPOUND_HEAD);
  return (flags & kPageHead) == kPageHead;
}
constexpr bool PageTail(uint64_t flags) {
  constexpr uint64_t kPageTail = (1UL << KPF_COMPOUND_TAIL);
  return (flags & kPageTail) == kPageTail;
}
constexpr bool PageThp(uint64_t flags) {
  constexpr uint64_t kPageThp = (1UL << KPF_THP);
  return (flags & kPageThp) == kPageThp;
}
constexpr bool PageStale(uint64_t flags) {
  constexpr uint64_t kPageStale = (1UL << KPF_STALE);
  return (flags & kPageStale) == kPageStale;
}
constexpr bool PageLocked(uint64_t flags) {
  constexpr uint64_t kPageMlocked = (1UL << KPF_MLOCKED);
  // Locked pages are often "unevictable."  KPF_LOCKED has a different meaning.
  constexpr uint64_t kPageUnevictable = (1UL << KPF_UNEVICTABLE);
  return (flags & (kPageMlocked | kPageUnevictable)) != 0;
}
void MaybeAddToStats(PageFlags::PageStats& stats, const uint64_t flags,
                     const size_t delta) {
  if (PageStale(flags)) stats.bytes_stale += delta;
  if (PageLocked(flags)) stats.bytes_locked += delta;
}

}  // namespace

PageFlags::PageFlags()
    : fd_(signal_safe_open("/proc/self/pageflags", O_RDONLY)) {}

PageFlags::PageFlags(const char* const alternate_filename)
    : fd_(signal_safe_open(alternate_filename, O_RDONLY)) {
  if (fd_ == -1) {
    TC_LOG("Could not open %s (errno %d)", alternate_filename, errno);
  }
}

PageFlags::~PageFlags() {
  if (fd_ >= 0) {
    signal_safe_close(fd_);
  }
}

absl::StatusCode PageFlags::Seek(const uintptr_t vaddr) {
  size_t offset = vaddr / kPageSize * kPagemapEntrySize;
  // Note: lseek can't be interrupted.
  off_t status = ::lseek(fd_, offset, SEEK_SET);
  if (status != offset) {
    return absl::StatusCode::kUnavailable;
  }
  return absl::StatusCode::kOk;
}

absl::StatusCode PageFlags::MaybeReadOne(uintptr_t vaddr, uint64_t& flags,
                                         bool& is_huge) {
  if (auto res = Seek(vaddr); res != absl::StatusCode::kOk) return res;
  static_assert(sizeof(flags) == kPagemapEntrySize);
  auto status = signal_safe_read(fd_, reinterpret_cast<char*>(&flags),
                                 kPagemapEntrySize, nullptr);
  if (status != kPagemapEntrySize) {
    return absl::StatusCode::kUnavailable;
  }

  if (ABSL_PREDICT_FALSE((PageHead(flags) || PageTail(flags)) &&
                         !PageThp(flags))) {
    TC_LOG("PageFlags asked for information on non-THP hugepage??");
    return absl::StatusCode::kFailedPrecondition;
  }

  if (PageTail(flags)) {
    if (auto res = Seek(vaddr & kHugePageMask); res != absl::StatusCode::kOk) {
      return res;
    }
    auto status = signal_safe_read(fd_, reinterpret_cast<char*>(&flags),
                                   kPagemapEntrySize, nullptr);
    if (status != kPagemapEntrySize) {
      return absl::StatusCode::kUnavailable;
    }
    if (ABSL_PREDICT_FALSE(PageTail(flags))) {
      TC_LOG("Somehow still at tail page even after seeking?");
      return absl::StatusCode::kFailedPrecondition;
    }
    // NOMUTANTS--Efficiency improvement that's not visible
    is_huge = true;
  } else {
    // The current page is not a tail page, but it could still be the very first
    // page of a hugepage. If this is the case, also plumb the information
    // upward so we don't waste time re-reading the next 511 tail pages.
    // NOMUTANTS--Efficiency improvement that's not visible
    is_huge = PageHead(flags);
  }

  return absl::StatusCode::kOk;
}

absl::StatusCode PageFlags::ReadMany(int64_t num_pages, PageStats& output) {
  while (num_pages > 0) {
    const size_t batch_size = std::min<int64_t>(kEntriesInBuf, num_pages);
    const size_t to_read = kPagemapEntrySize * batch_size;

    // We read continuously. For the first read, this starts at wherever the
    // first ReadOne ended. See above note for the reinterpret_cast.
    auto status =
        signal_safe_read(fd_, reinterpret_cast<char*>(buf_), to_read, nullptr);
    if (status != to_read) {
      return absl::StatusCode::kUnavailable;
    }
    for (int i = 0; i < batch_size; ++i) {
      if (PageHead(buf_[i])) {
        last_head_read_ = buf_[i];
      }

      if (PageTail(buf_[i])) {
        if (ABSL_PREDICT_FALSE(last_head_read_ == -1)) {
          TC_LOG("Did not see head page before tail page (i=%v, buf=%v)", i,
                 buf_[i]);
          return absl::StatusCode::kFailedPrecondition;
        }
        auto last_read = last_head_read_;
        MaybeAddToStats(output, last_read, kPageSize);
      } else {
        MaybeAddToStats(output, buf_[i], kPageSize);
      }
    }
    num_pages -= batch_size;
  }
  return absl::StatusCode::kOk;
}

std::optional<PageFlags::PageStats> PageFlags::Get(const void* const addr,
                                                   const size_t size) {
  if (fd_ < 0) {
    return std::nullopt;
  }
  last_head_read_ = -1;

  PageStats ret;
  if (size == 0) return ret;
  uint64_t result_flags = 0;
  bool is_huge = false;

  uintptr_t uaddr = reinterpret_cast<uintptr_t>(addr);
  // Round address down to get the start of the first page that has any bytes
  // corresponding to the span [addr, addr+size).
  uintptr_t basePage = uaddr & ~(kPageSize - 1);
  // Round end address up to get the start of the first page that does not
  // have any bytes corresponding to the span [addr, addr+size).
  // The span is a subset of [basePage, endPage).
  uintptr_t endPage = (uaddr + size + kPageSize - 1) & ~(kPageSize - 1);

  int64_t remainingPages = (endPage - basePage) / kPageSize;

  if (remainingPages == 1) {
    if (auto res = MaybeReadOne(basePage, result_flags, is_huge);
        res != absl::StatusCode::kOk) {
      return std::nullopt;
    }
    MaybeAddToStats(ret, result_flags, size);
    return ret;
  }

  // Since the input address might not be page-aligned (it can possibly point
  // to an arbitrary object), we read staleness about the first page separately
  // with ReadOne, then read the complete pages with ReadMany, and then read the
  // last page with ReadOne again if needed.

  // Handle the first page.
  if (auto res = MaybeReadOne(basePage, result_flags, is_huge);
      res != absl::StatusCode::kOk) {
    return std::nullopt;
  }
  size_t firstPageSize = kPageSize - (uaddr - basePage);
  if (is_huge) {
    // The object starts in the middle of a native page, but the entire page
    // might be stale. So the situation looks like, simplifying to four native
    // pages per hugepage to make the diagram fit, an entire hugepage that looks
    // like (where X is the span of interest):
    //      .                basePage
    // [....|..XX|XXXX|XXXX]
    //  ^^^^^^^              some other stale object(s)
    //         ^^            firstPageSize
    //       ^^^^^^^^^^^^^^  `pages_represented` pages, each of kPageSize
    // The remainingPages <= 0 case covers the situation where the span ends
    // before the hugepage.
    const uint64_t base_page_offset = basePage & (kHugePageSize - 1);
    const uint64_t base_page_index = base_page_offset / kPageSize;
    const int64_t pages_represented = kPagesInHugePage - base_page_index;
    remainingPages -= pages_represented;
    if (remainingPages <= 0) {
      // This hugepage represents every single page that this object is on;
      // we're done.
      MaybeAddToStats(ret, result_flags, size);
      return ret;
    }

    // pages_represented - 1 is the number of full pages represented (see
    // diagram)
    MaybeAddToStats(ret, result_flags,
                    firstPageSize + (pages_represented - 1) * kPageSize);

    // We've read one uint64_t about a single page, but it represents 512 small
    // pages. So the next page that is of interest is one hugepage away -- seek
    // to make sure the next read doesn't double-count the native pages in
    // between the two head pages.
    if (auto res = Seek((basePage & kHugePageMask) + kHugePageSize);
        res != absl::StatusCode::kOk) {
      return std::nullopt;
    }
  } else {
    remainingPages--;
    MaybeAddToStats(ret, result_flags, firstPageSize);
  }

  // Handle all pages but the last page.
  if (auto res = ReadMany(remainingPages - 1, ret);
      res != absl::StatusCode::kOk) {
    return std::nullopt;
  }

  // Check final page. It doesn't really matter if is_huge; we just want the
  // statistics about the page that has the last byte of the object.
  size_t lastPageSize = kPageSize - (endPage - uaddr - size);
  if (auto res = MaybeReadOne(endPage - kPageSize, result_flags, is_huge);
      res != absl::StatusCode::kOk) {
    return std::nullopt;
  }
  MaybeAddToStats(ret, result_flags, lastPageSize);
  return ret;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

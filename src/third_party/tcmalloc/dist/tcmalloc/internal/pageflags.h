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

// /proc/self/pageflags is not available without kernel patches such as
// https://patchwork.kernel.org/project/linux-mm/patch/20211028205854.830200-1-almasrymina@google.com/
// The pageflags that we look at are subject to change.

#ifndef TCMALLOC_INTERNAL_PAGEFLAGS_H_
#define TCMALLOC_INTERNAL_PAGEFLAGS_H_

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <optional>

#include "absl/status/status.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/page_size.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// PageFlags offers a look at kernel page flags to identify pieces of memory as
// stale. This class is very similar to Residency but has some substantial
// differences to be hugepage aware.
//
// Specifically, if a page is huge, KPF_STALE is set only on the head native
// page of a hugepage and means that the entire hugepage is stale. Thus, when
// encountering tail pages, we must rewind to find the head page to get the
// information related to them. Native pages have KPF_STALE set as normal; no
// special handling needs to be done for them.
class PageFlags {
 public:
  // This class keeps an open file handle to procfs. Destroy the object to
  // reclaim it.
  PageFlags();
  ~PageFlags();

  struct PageStats {
    size_t bytes_stale = 0;
    size_t bytes_locked = 0;

    // This is currently used only by tests. It'll be good to convert this to
    // C++20 "= default" when we increase the baseline compiler requirement.
    bool operator==(const PageStats& rhs) const {
      return bytes_stale == rhs.bytes_stale && bytes_locked == rhs.bytes_locked;
    }

    bool operator!=(const PageStats& rhs) const { return !(*this == rhs); }
  };

  // Query a span of memory starting from `addr` for `size` bytes. The memory
  // span must consist of only native-size pages and THP hugepages; the behavior
  // is undefined if we encounter other hugepages (such as hugetlbfs). We try to
  // bail out if we find hugetlbfs immediately, but in esoteric cases like a
  // hugetlbfs in the middle of another mapping, this won't work.
  //
  // We use std::optional for return value as std::optional guarantees that no
  // dynamic memory allocation would happen.  In contrast, absl::StatusOr may
  // dynamically allocate memory when needed.  Using std::optional allows us to
  // use the function in places where memory allocation is prohibited.
  std::optional<PageStats> Get(const void* addr, size_t size);

 private:
  // This helper seeks the internal file to the correct location for the given
  // virtual address.
  [[nodiscard]] absl::StatusCode Seek(uintptr_t vaddr);

  // Tries to read staleness information about the page that contains vaddr.
  // Possibly seeks backwards in an effort to find head hugepages.
  absl::StatusCode MaybeReadOne(uintptr_t vaddr, uint64_t& flags,
                                bool& is_huge);
  // This helper reads staleness information for `num_pages` worth of _full_
  // pages and puts the results into `output`. It continues the read from the
  // last Seek() or last Read operation.
  absl::StatusCode ReadMany(int64_t num_pages, PageStats& output);

  // For testing.
  friend class PageFlagsFriend;
  explicit PageFlags(const char* alternate_filename);

  // Size of the buffer used to gather results.
  static constexpr int kBufferLength = 4096;
  static constexpr int kPagemapEntrySize = 8;
  static constexpr int kEntriesInBuf = kBufferLength / kPagemapEntrySize;

  const size_t kPageSize = GetPageSize();
  // You can technically not hard-code this but it would involve many more
  // queries to figure out the size of every page. It's a lot easier to just
  // assume any compound pages are 2 MB.
  static constexpr int kHugePageSize = (2 << 20);
  static constexpr uintptr_t kHugePageMask = ~(kHugePageSize - 1);
  const size_t kPagesInHugePage = kHugePageSize / kPageSize;

  uint64_t buf_[kEntriesInBuf];
  // Information about the previous head page. For any future-encountered tail
  // pages, we use the information from this page to determine staleness of the
  // tail page.
  uint64_t last_head_read_ = -1;
  const int fd_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PAGEFLAGS_H_

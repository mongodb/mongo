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

#include "tcmalloc/internal/mincore.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/page_size.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Class that implements the call into the OS provided mincore() function.
class OsMInCore final : public MInCoreInterface {
 public:
  int mincore(void* addr, size_t length, unsigned char* result) final {
    return ::mincore(addr, length, result);
  }

  ~OsMInCore() override = default;
};

// Returns the number of resident bytes for an range of memory of arbitrary
// alignment and size.
size_t MInCore::residence_impl(void* addr, size_t size,
                               MInCoreInterface* mincore) {
  if (size == 0) {
    return 0;
  }
  unsigned char res[kArrayLength];
  const size_t kPageSize = GetPageSize();

  uintptr_t uaddr = reinterpret_cast<uintptr_t>(addr);
  // Round address down to get the start of the page containing the data.
  uintptr_t basePage = uaddr & ~(kPageSize - 1);
  // Round end address up to get the end of the page containing the data.
  uintptr_t endPage = (uaddr + size + kPageSize - 1) & ~(kPageSize - 1);

  uintptr_t remainingPages = endPage - basePage;

  // We need to handle the first and last pages differently. Most pages
  // will contribute pagesize bytes to residence, but the first and last
  // pages will contribute fewer than that. Easiest way to do this is to
  // handle the special case where the entire object fits into a page,
  // then handle the case where the object spans more than one page.
  if (remainingPages == kPageSize) {
    // Find out whether the first page is resident.
    if (mincore->mincore(reinterpret_cast<void*>(basePage), remainingPages,
                         res) != 0) {
      return 0;
    }
    // Residence info is returned in LSB, other bits are undefined.
    if ((res[0] & 1) == 1) {
      return size;
    }
    return 0;
  }

  // We're calling this outside the loop so that we can get info for the
  // first page, deal with subsequent pages in the loop, and then handle
  // the last page after the loop.
  size_t scanLength = std::min(remainingPages, kPageSize * kArrayLength);
  if (mincore->mincore(reinterpret_cast<void*>(basePage), scanLength, res) !=
      0) {
    return 0;
  }

  size_t totalResident = 0;

  // Handle the first page.
  size_t firstPageSize = kPageSize - (uaddr - basePage);
  if ((res[0] & 1) == 1) {
    totalResident += firstPageSize;
  }
  basePage += kPageSize;
  remainingPages -= kPageSize;

  int resIndex = 1;

  // Handle all pages but the last page.
  while (remainingPages > kPageSize) {
    if ((res[resIndex] & 1) == 1) {
      totalResident += kPageSize;
    }
    resIndex++;
    basePage += kPageSize;
    remainingPages -= kPageSize;
    // Refresh the array if necessary.
    if (resIndex == kArrayLength) {
      resIndex = 0;
      scanLength = std::min(remainingPages, kPageSize * kArrayLength);
      if (mincore->mincore(reinterpret_cast<void*>(basePage), scanLength,
                           res) != 0) {
        return 0;
      }
    }
  }

  // Check final page
  size_t lastPageSize = kPageSize - (endPage - uaddr - size);
  if ((res[resIndex] & 1) == 1) {
    totalResident += lastPageSize;
  }

  return totalResident;
}

// Return residence info using call to OS provided mincore().
size_t MInCore::residence(void* addr, size_t size) {
  OsMInCore mc;
  return residence_impl(addr, size, &mc);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

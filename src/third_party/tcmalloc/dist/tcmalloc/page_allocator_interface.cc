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

#include "tcmalloc/page_allocator_interface.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

PageAllocatorInterface::PageAllocatorInterface(const char* label, MemoryTag tag)
    : PageAllocatorInterface(label, &tc_globals.pagemap(), tag) {}

PageAllocatorInterface::PageAllocatorInterface(const char* label, PageMap* map,
                                               MemoryTag tag)
    : info_(label), pagemap_(map), tag_(tag) {}

PageAllocatorInterface::~PageAllocatorInterface() {
  // This is part of tcmalloc statics - they must be immortal.
  TC_BUG("should never destroy this");
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

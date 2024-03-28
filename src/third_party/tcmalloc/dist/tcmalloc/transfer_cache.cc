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

#include "tcmalloc/transfer_cache.h"

#include <fcntl.h>
#include <string.h>

#include <new>

#include "absl/base/attributes.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

#ifndef TCMALLOC_INTERNAL_SMALL_BUT_SLOW

size_t StaticForwarder::class_to_size(int size_class) {
  return tc_globals.sizemap().class_to_size(size_class);
}
size_t StaticForwarder::num_objects_to_move(int size_class) {
  return tc_globals.sizemap().num_objects_to_move(size_class);
}
void *StaticForwarder::Alloc(size_t size, std::align_val_t alignment) {
  return tc_globals.arena().Alloc(size, alignment);
}

ABSL_CONST_INIT bool ShardedStaticForwarder::use_generic_cache_(false);
ABSL_CONST_INIT bool
    ShardedStaticForwarder::enable_cache_for_large_classes_only_(false);

void BackingTransferCache::InsertRange(absl::Span<void *> batch) const {
  tc_globals.transfer_cache().InsertRange(size_class_, batch);
}

ABSL_MUST_USE_RESULT int BackingTransferCache::RemoveRange(void **batch,
                                                           int n) const {
  return tc_globals.transfer_cache().RemoveRange(size_class_, batch, n);
}

#endif

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

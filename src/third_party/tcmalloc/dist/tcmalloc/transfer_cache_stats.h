// Copyright 2020 The TCMalloc Authors
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

#ifndef TCMALLOC_TRANSFER_CACHE_STATS_H_
#define TCMALLOC_TRANSFER_CACHE_STATS_H_

#include <stddef.h>

namespace tcmalloc {
namespace tcmalloc_internal {

struct TransferCacheStats {
  size_t insert_hits;
  size_t insert_misses;
  size_t insert_object_misses;
  size_t remove_hits;
  size_t remove_misses;
  size_t remove_object_misses;
  size_t used;
  size_t capacity;
  size_t max_capacity;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_TRANSFER_CACHE_STATS_H_

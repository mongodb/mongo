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

#include "tcmalloc/mock_transfer_cache.h"

namespace tcmalloc {
namespace tcmalloc_internal {

ABSL_CONST_INIT bool
    ArenaBasedFakeTransferCacheManager::partial_legacy_transfer_cache_(false);
ABSL_CONST_INIT bool FakeShardedTransferCacheManager::enable_generic_cache_(
    false);
ABSL_CONST_INIT bool
    FakeShardedTransferCacheManager::enable_cache_for_large_classes_only_(
        false);
}  // namespace tcmalloc_internal
}  // namespace tcmalloc

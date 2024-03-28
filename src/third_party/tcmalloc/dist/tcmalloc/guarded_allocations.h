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

#ifndef TCMALLOC_GUARDED_ALLOCATIONS_H_
#define TCMALLOC_GUARDED_ALLOCATIONS_H_

#include <cstddef>

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct GuardedAllocationsStackTrace {
  void* stack[kMaxStackDepth];
  size_t depth = 0;
  pid_t thread_id = 0;
};

enum class WriteFlag : int { Unknown, Read, Write };

enum class GuardedAllocationsErrorType {
  kUseAfterFree,
  kUseAfterFreeRead,
  kUseAfterFreeWrite,
  kBufferUnderflow,
  kBufferUnderflowRead,
  kBufferUnderflowWrite,
  kBufferOverflow,
  kBufferOverflowRead,
  kBufferOverflowWrite,
  kDoubleFree,
  kBufferOverflowOnDealloc,
  kUnknown,
};

struct GuardedAllocWithStatus {
  void* alloc = nullptr;
  Profile::Sample::GuardedStatus status =
      Profile::Sample::GuardedStatus::Unknown;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_GUARDED_ALLOCATIONS_H_

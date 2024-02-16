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

// Single file to include target specific implementations for percpu.

#include "tcmalloc/internal/percpu.h"

#if TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM
#if defined(__x86_64__)
#include "tcmalloc/internal/percpu_rseq_x86_64.S"
#elif defined(__aarch64__)
#include "tcmalloc/internal/percpu_rseq_aarch64.S"
#else
#error "RSEQ support expected, but not found."
#endif
#endif // TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM

// We do not need an executable stack. Put this outside the
// architecture-specific region above in order to suppress "missing
// .note.GNU-stack section implies executable stack" errors.
//
// Cf. http://en.chys.info/2010/12/note-gnu-stack/
#if defined(__arm__)
.section .note.GNU-stack, "", %progbits
#else
.section .note.GNU-stack, "", @progbits
#endif  // __arm__


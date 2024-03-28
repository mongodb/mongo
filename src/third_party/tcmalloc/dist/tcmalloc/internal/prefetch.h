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

#ifndef TCMALLOC_INTERNAL_PREFETCH_H_
#define TCMALLOC_INTERNAL_PREFETCH_H_

#include <stdint.h>

#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Move data into the cache before it is read, or "prefetch" it.
//
// The value of `addr` is the address of the memory to prefetch. If
// the target and compiler support it, data prefetch instructions are
// generated. If the prefetch is done some time before the memory is
// read, it may be in the cache by the time the read occurs.
//
// The function names specify the temporal locality heuristic applied,
// using the names of Intel prefetch instructions:
//
//   T0 - high degree of temporal locality; data should be left in as
//        many levels of the cache possible
//   T1 - moderate degree of temporal locality
//   T2 - low degree of temporal locality
//   Nta - no temporal locality, data need not be left in the cache
//         after the read
//   W - prefetch data in preparation for a write; may prefetch data
//       to the local CPU and invalidate other cached copies
//
// Incorrect or gratuitous use of these functions can degrade
// performance, so use them only when representative benchmarks show
// an improvement.
//
// Example usage:
//
//   tcmalloc_internal::PrefetchT0(addr);
//
#if defined(__GNUC__)
// See __builtin_prefetch:
// https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html.
inline void PrefetchT0(const void* addr) { __builtin_prefetch(addr, 0, 3); }
inline void PrefetchT1(const void* addr) { __builtin_prefetch(addr, 0, 2); }
inline void PrefetchT2(const void* addr) { __builtin_prefetch(addr, 0, 1); }
inline void PrefetchNta(const void* addr) { __builtin_prefetch(addr, 0, 0); }
// Wrappers for prefetch with intent to modify.
// [x86] gcc/clang don't generate PREFETCHW for __builtin_prefetch(.., /*rw=*/1)
// unless -march=broadwell or newer; this is not generally the default, so
// manually emit prefetchw. PREFETCHW is recognized as a no-op on older Intel
// processors and has been present on AMD processors since the K6-2.
inline void PrefetchW(const void* addr) {
#if defined(__x86_64__) && !defined(__PRFCHW__)
  asm("prefetchw %0" : : "m"(*static_cast<const char*>(addr)));
#else
  __builtin_prefetch(addr, /*rw=*/1, 0);
#endif
}
inline void PrefetchWT0(const void* addr) {
#if defined(__x86_64__) && !defined(__PRFCHW__)
  asm("prefetchw %0" : : "m"(*static_cast<const char*>(addr)));
#else
  __builtin_prefetch(addr, 1, 3);
#endif
}
inline void PrefetchWT1(const void* addr) {
#if defined(__x86_64__) && !defined(__PRFCHW__)
  asm("prefetchw %0" : : "m"(*static_cast<const char*>(addr)));
#else
  __builtin_prefetch(addr, 1, 2);
#endif
}
inline void PrefetchWT2(const void* addr) {
#if defined(__x86_64__) && !defined(__PRFCHW__)
  asm("prefetchw %0" : : "m"(*static_cast<const char*>(addr)));
#else
  __builtin_prefetch(addr, 1, 1);
#endif
}
inline void PrefetchWNta(const void* addr) {
#if defined(__x86_64__) && !defined(__PRFCHW__)
  asm("prefetchw %0" : : "m"(*static_cast<const char*>(addr)));
#else
  __builtin_prefetch(addr, 1, 0);
#endif
}
#else
inline void PrefetchT0(const void*) {}
inline void PrefetchT1(const void*) {}
inline void PrefetchT2(const void*) {}
inline void PrefetchNta(const void*) {}
inline void PrefetchWT0(const void*) {}
inline void PrefetchWT1(const void*) {}
inline void PrefetchWT2(const void*) {}
inline void PrefetchWT3(const void*) {}
#endif

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PREFETCH_H_

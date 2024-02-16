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

// The routines exported by this module are subtle and dangerous.

#ifndef TCMALLOC_INTERNAL_ATOMIC_DANGER_H_
#define TCMALLOC_INTERNAL_ATOMIC_DANGER_H_

#include <atomic>
#include <type_traits>

#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace atomic_danger {

// Casts the address of a std::atomic<IntType> to the address of an IntType.
//
// This is almost certainly not the function you are looking for! It is
// undefined behavior, as the object under a std::atomic<int> isn't
// fundamentally an int. This function is intended for passing the address of an
// atomic integer to syscalls or for assembly interpretation.
//
// Callers should be migrated if C++ standardizes a better way to do this:
// * http://wg21.link/n4013 (Atomic operations on non-atomic data)
// * http://wg21.link/p0019 (Atomic Ref, merged into C++20)
// * http://wg21.link/p1478 (Byte-wise atomic memcpy)
template <typename IntType>
IntType* CastToIntegral(std::atomic<IntType>* atomic_for_syscall) {
  static_assert(std::is_integral<IntType>::value,
                "CastToIntegral must be instantiated with an integral type.");
#if __cpp_lib_atomic_is_always_lock_free >= 201603
  static_assert(std::atomic<IntType>::is_always_lock_free,
                "CastToIntegral must be instantiated with a lock-free type.");
#else
  static_assert(__atomic_always_lock_free(sizeof(IntType),
                                          nullptr /* typical alignment */),
                "CastToIntegral must be instantiated with a lock-free type.");
#endif
  return reinterpret_cast<IntType*>(atomic_for_syscall);
}
}  // namespace atomic_danger
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_ATOMIC_DANGER_H_

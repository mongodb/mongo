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

#include "tcmalloc/new_extension.h"

#include <cstddef>
#include <new>

#include "absl/base/attributes.h"
#include "tcmalloc/malloc_extension.h"

ABSL_ATTRIBUTE_WEAK void* operator new(
    size_t size, tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  return ::operator new(size);
}

ABSL_ATTRIBUTE_WEAK void* operator new(size_t size, const std::nothrow_t&,
                                       tcmalloc::hot_cold_t hot_cold) noexcept {
  return ::operator new(size, std::nothrow);
}

ABSL_ATTRIBUTE_WEAK void* operator new[](
    size_t size, tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  return ::operator new[](size);
}

ABSL_ATTRIBUTE_WEAK void* operator new[](
    size_t size, const std::nothrow_t&,
    tcmalloc::hot_cold_t hot_cold) noexcept {
  return ::operator new[](size, std::nothrow);
}

#ifdef __cpp_aligned_new
ABSL_ATTRIBUTE_WEAK void* operator new(
    size_t size, std::align_val_t alignment,
    tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  return ::operator new(size, alignment);
}

ABSL_ATTRIBUTE_WEAK void* operator new(size_t size, std::align_val_t alignment,
                                       const std::nothrow_t&,
                                       tcmalloc::hot_cold_t hot_cold) noexcept {
  return ::operator new(size, alignment, std::nothrow);
}

ABSL_ATTRIBUTE_WEAK void* operator new[](
    size_t size, std::align_val_t alignment,
    tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  return ::operator new[](size, alignment);
}

ABSL_ATTRIBUTE_WEAK void* operator new[](
    size_t size, std::align_val_t alignment, const std::nothrow_t&,
    tcmalloc::hot_cold_t hot_cold) noexcept {
  return ::operator new[](size, alignment, std::nothrow);
}
#endif  // __cpp_aligned_new

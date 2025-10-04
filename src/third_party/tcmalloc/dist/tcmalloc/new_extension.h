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

#ifndef TCMALLOC_NEW_EXTENSION_H_
#define TCMALLOC_NEW_EXTENSION_H_

#include <cstddef>
#include <new>

#include "tcmalloc/malloc_extension.h"

void* operator new(size_t size, tcmalloc::hot_cold_t hot_cold) noexcept(false);
void* operator new(size_t size, const std::nothrow_t&,
                   tcmalloc::hot_cold_t hot_cold) noexcept;
void* operator new[](size_t size,
                     tcmalloc::hot_cold_t hot_cold) noexcept(false);
void* operator new[](size_t size, const std::nothrow_t&,
                     tcmalloc::hot_cold_t hot_cold) noexcept;

#ifdef __cpp_aligned_new
void* operator new(size_t size, std::align_val_t alignment,
                   tcmalloc::hot_cold_t hot_cold) noexcept(false);
void* operator new(size_t size, std::align_val_t alignment,
                   const std::nothrow_t&,
                   tcmalloc::hot_cold_t hot_cold) noexcept;
void* operator new[](size_t size, std::align_val_t alignment,
                     tcmalloc::hot_cold_t hot_cold) noexcept(false);
void* operator new[](size_t size, std::align_val_t alignment,
                     const std::nothrow_t&,
                     tcmalloc::hot_cold_t hot_cold) noexcept;
#endif  // __cpp_aligned_new

#endif  // TCMALLOC_NEW_EXTENSION_H_

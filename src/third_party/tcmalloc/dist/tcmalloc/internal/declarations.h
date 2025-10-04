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

// These declarations are for internal use, allowing us to have access to
// allocation functions whose declarations are not provided by the standard
// library.
#ifndef TCMALLOC_INTERNAL_DECLARATIONS_H_
#define TCMALLOC_INTERNAL_DECLARATIONS_H_


#if !defined(__cpp_sized_deallocation)

void operator delete(void*, std::size_t) noexcept;
void operator delete[](void*, std::size_t) noexcept;

#endif  // !defined(__cpp_sized_deallocation)

#if !defined(__cpp_aligned_new)

namespace std {
enum class align_val_t : size_t;
}  // namespace std

void* operator new(std::size_t, std::align_val_t);
void* operator new(std::size_t, std::align_val_t,
                   const std::nothrow_t&) noexcept;
void* operator new[](std::size_t, std::align_val_t);
void* operator new[](std::size_t, std::align_val_t,
                     const std::nothrow_t&) noexcept;

void operator delete(void*, std::align_val_t) noexcept;
void operator delete[](void*, std::align_val_t) noexcept;

#endif  // !defined(__cpp_aligned_new)

#if !defined(__cpp_sized_deallocation) || !defined(__cpp_aligned_new)

void operator delete(void*, std::size_t, std::align_val_t) noexcept;
void operator delete[](void*, std::size_t, std::align_val_t) noexcept;

#endif  // !defined(__cpp_sized_deallocation) || !defined(__cpp_aligned_new)

#endif  // TCMALLOC_INTERNAL_DECLARATIONS_H_

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

#ifndef TCMALLOC_INTERNAL_CONFIG_H_
#define TCMALLOC_INTERNAL_CONFIG_H_

#include <stddef.h>

#include "absl/base/attributes.h"
#include "absl/base/config.h"

// TCMALLOC_HAVE_SCHED_GETCPU is defined when the system implements
// sched_getcpu(3) as by glibc and it's imitators.
#if defined(__linux__) || defined(__ros__)
#define TCMALLOC_HAVE_SCHED_GETCPU 1
#else
#undef TCMALLOC_HAVE_SCHED_GETCPU
#endif

#if defined(__GLIBC__) && defined(__GLIBC_MINOR__)
#define TCMALLOC_GLIBC_PREREQ(major, minor) \
  ((__GLIBC__ * 100 + __GLIBC_MINOR__) >= ((major)*100 + (minor)))
#else
#define TCMALLOC_GLIBC_PREREQ(major, minor) 0
#endif

// TCMALLOC_HAVE_STRUCT_MALLINFO is defined when we know that the system has
// `struct mallinfo` available.
//
// We know that bionic, glibc (and variants), newlib, and uclibc do
// provide the `mallopt` interface.  The musl libc is known to not provide the
// interface, nor does it provide a macro for checking.  As a result, we
// conservatively state that `struct mallinfo` is only available on these
// environments.
#if defined(__BIONIC__) || defined(__GLIBC__) || defined(__NEWLIB__) || \
    defined(__UCLIBC__)
#define TCMALLOC_HAVE_STRUCT_MALLINFO 1
#else
#undef TCMALLOC_HAVE_STRUCT_MALLINFO
#endif

#if TCMALLOC_GLIBC_PREREQ(2, 33)
#define TCMALLOC_HAVE_STRUCT_MALLINFO2 1
#else
#undef TCMALLOC_HAVE_STRUCT_MALLINFO2
#endif

// When possible, name the text section as google_malloc.  This macro should not
// be added to header files as that may move unrelated code to google_malloc
// section.
#if defined(__clang__) && defined(__linux__)
#define GOOGLE_MALLOC_SECTION_BEGIN \
  _Pragma("clang section text = \"google_malloc\"")
#define GOOGLE_MALLOC_SECTION_END _Pragma("clang section text = \"\"")
#else
#define GOOGLE_MALLOC_SECTION_BEGIN
#define GOOGLE_MALLOC_SECTION_END
#endif

// TCMALLOC_ATTRIBUTE_NO_DESTROY is defined when clang::no_destroy attribute is
// present.
#if ABSL_HAVE_CPP_ATTRIBUTE(clang::no_destroy)
#define TCMALLOC_ATTRIBUTE_NO_DESTROY [[clang::no_destroy]]
#else
#define TCMALLOC_ATTRIBUTE_NO_DESTROY
#endif

#if defined(__GNUC__) && !defined(__clang__)
#if __GNUC__ < 9 || (__GNUC__ == 9 && __GNUC_MINOR__ < 2)
#error "GCC 9.2 or higher is required."
#endif
#endif

#if defined(__clang__)
#if __clang_major__ < 9
#error "Clang 9 or higher is required."
#endif
#endif

#if !defined(__i386__) && !defined(__x86_64__) && !defined(__ppc64__) && \
    !defined(__arm__) && !defined(__aarch64__) && !defined(__riscv)
#error "Unsupported architecture."
#endif

#ifndef ABSL_IS_LITTLE_ENDIAN
#error "TCMalloc only supports little endian architectures"
#endif

#ifndef __linux__
#error "TCMalloc is only supported on Linux."
#endif

#if !defined(__cplusplus) || __cplusplus < 201703L
#error "TCMalloc requires C++17 or later."
#else
// Also explicitly use some C++17 syntax, to prevent detect flags like
// `-Wc++14-compat`.
namespace tcmalloc::google3_requires_cpp17_or_later {}
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

#if defined __x86_64__
// x86_64 processors use lower 48 bits in virtual to physical address
// translation with 4-level page tables. The top 16 are thus unused.
// We don't support 5-level page tables yet.
// TODO(b/134686025): Under what operating systems can we increase it safely to
// 17? This lets us use smaller page maps.  On first allocation, a 36-bit page
// map uses only 96 KB instead of the 4.5 MB used by a 52-bit page map.
inline constexpr int kAddressBits = 48;
#elif defined __powerpc64__ && defined __linux__
// Linux(4.12 and above) on powerpc64 supports 128TB user virtual address space
// by default, and up to 512TB if user space opts in by specifying hint in mmap.
// See comments in arch/powerpc/include/asm/processor.h
// and arch/powerpc/mm/mmap.c.
inline constexpr int kAddressBits = 49;
#elif defined __aarch64__ && defined __linux__
// According to Documentation/arm64/memory.txt of kernel 3.16,
// AARCH64 kernel supports 48-bit virtual addresses for both user and kernel.
inline constexpr int kAddressBits = 48;
#elif defined __riscv && defined __linux__
inline constexpr int kAddressBits = 48;
#else
inline constexpr int kAddressBits = 8 * sizeof(void*);
#endif

#if defined(__x86_64__)
// x86 has 2 MiB huge pages
static constexpr size_t kHugePageShift = 21;
#elif defined(__PPC64__)
static constexpr size_t kHugePageShift = 24;
#elif defined __aarch64__ && defined __linux__
static constexpr size_t kHugePageShift = 21;
#elif defined __riscv && defined __linux__
static constexpr size_t kHugePageShift = 21;
#else
// ...whatever, guess something big-ish
static constexpr size_t kHugePageShift = 21;
#endif

static constexpr size_t kHugePageSize = static_cast<size_t>(1)
                                        << kHugePageShift;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_CONFIG_H_

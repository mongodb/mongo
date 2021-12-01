/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozmemory_wrap_h
#define mozmemory_wrap_h

// This header contains #defines which tweak the names of various memory
// allocation functions.
//
// There are several types of functions related to memory allocation
// that are meant to be used publicly by the Gecko codebase:
//
// - malloc implementation functions:
//   - malloc
//   - posix_memalign
//   - aligned_alloc
//   - calloc
//   - realloc
//   - free
//   - memalign
//   - valloc
//   - malloc_usable_size
//   - malloc_good_size
//   Some of these functions are specific to some systems, but for
//   convenience, they are treated as being cross-platform, and available
//   as such.
//
// - duplication functions:
//   - strndup
//   - strdup
//   - wcsdup (Windows only)
//
// - jemalloc specific functions:
//   - jemalloc_stats
//   - jemalloc_purge_freed_pages
//   - jemalloc_free_dirty_pages
//   - jemalloc_thread_local_arena
//   - jemalloc_ptr_info
//   (these functions are native to mozjemalloc)
//
// These functions are all exported as part of libmozglue (see
// $(topsrcdir)/mozglue/build/Makefile.in), with a few implementation
// peculiarities:
//
// - On Windows, the malloc implementation functions are all prefixed with
//   "je_", the duplication functions are prefixed with "wrap_", and jemalloc
//   specific functions are left unprefixed. All these functions are however
//   aliased when exporting them, such that the resulting mozglue.dll exports
//   them unprefixed (see $(topsrcdir)/mozglue/build/mozglue.def.in). The
//   prefixed malloc implementation and duplication functions are not
//   exported.
//
// - On MacOSX, the system libc has a zone allocator, which allows us to
//   hook custom malloc implementation functions without exporting them.
//   However, since we want things in Firefox to skip the system zone
//   allocator, the malloc implementation functions are all exported
//   unprefixed, as well as duplication functions.
//   Jemalloc-specific functions are also left unprefixed.
//
// - On Android all functions are left unprefixed. Additionally,
//   C++ allocation functions (operator new/delete) are also exported and
//   unprefixed.
//
// - On other systems (mostly Linux), all functions are left unprefixed.
//
// Only Android adds C++ allocation functions.
//
// Proper exporting of the various functions is done with the MOZ_MEMORY_API
// and MOZ_JEMALLOC_API macros. MOZ_MEMORY_API is meant to be used for malloc
// implementation and duplication functions, while MOZ_JEMALLOC_API is
// dedicated to jemalloc specific functions.
//
//
// All these functions are meant to be called with no prefix from Gecko code.
// In most cases, this is because that's how they are available at runtime.
// However, on Android, this relies on faulty.lib (the custom dynamic linker)
// resolving mozglue symbols before libc symbols, which is guaranteed by the
// way faulty.lib works (it respects the DT_NEEDED order, and libc always
// appears after mozglue ; which we double check when building anyways)
//
//
// Within libmozglue (when MOZ_MEMORY_IMPL is defined), all the functions
// should be suffixed with "_impl" both for declarations and use.
// That is, the implementation declaration for e.g. strdup would look like:
//   char* strdup_impl(const char *)
// That implementation would call malloc by using "malloc_impl".

#if defined(MOZ_MEMORY_IMPL) && !defined(IMPL_MFBT)
#ifdef MFBT_API // mozilla/Types.h was already included
#    error mozmemory_wrap.h has to be included before mozilla/Types.h when MOZ_MEMORY_IMPL is set and IMPL_MFBT is not.
#endif
#define IMPL_MFBT
#endif

#include "mozilla/Types.h"

#ifndef MOZ_EXTERN_C
#ifdef __cplusplus
#define MOZ_EXTERN_C extern "C"
#else
#define MOZ_EXTERN_C
#endif
#endif

#ifdef MOZ_MEMORY_IMPL
#define MOZ_JEMALLOC_API MOZ_EXTERN_C MFBT_API
#if defined(XP_WIN)
#define mozmem_malloc_impl(a) je_##a
#else
#define MOZ_MEMORY_API MOZ_EXTERN_C MFBT_API
#if defined(MOZ_WIDGET_ANDROID)
#define MOZ_WRAP_NEW_DELETE
#endif
#endif
#endif
#ifdef XP_WIN
#define mozmem_dup_impl(a) wrap_##a
#endif

#if !defined(MOZ_MEMORY_IMPL)
#define MOZ_MEMORY_API MOZ_EXTERN_C MFBT_API
#define MOZ_JEMALLOC_API MOZ_EXTERN_C MFBT_API
#endif

#ifndef MOZ_MEMORY_API
#define MOZ_MEMORY_API MOZ_EXTERN_C
#endif
#ifndef MOZ_JEMALLOC_API
#define MOZ_JEMALLOC_API MOZ_EXTERN_C
#endif

#ifndef mozmem_malloc_impl
#define mozmem_malloc_impl(a) a
#endif
#ifndef mozmem_dup_impl
#define mozmem_dup_impl(a) a
#endif

// Malloc implementation functions
#define malloc_impl mozmem_malloc_impl(malloc)
#define posix_memalign_impl mozmem_malloc_impl(posix_memalign)
#define aligned_alloc_impl mozmem_malloc_impl(aligned_alloc)
#define calloc_impl mozmem_malloc_impl(calloc)
#define realloc_impl mozmem_malloc_impl(realloc)
#define free_impl mozmem_malloc_impl(free)
#define memalign_impl mozmem_malloc_impl(memalign)
#define valloc_impl mozmem_malloc_impl(valloc)
#define malloc_usable_size_impl mozmem_malloc_impl(malloc_usable_size)
#define malloc_good_size_impl mozmem_malloc_impl(malloc_good_size)

// Duplication functions
#define strndup_impl mozmem_dup_impl(strndup)
#define strdup_impl mozmem_dup_impl(strdup)
#ifdef XP_WIN
#define wcsdup_impl mozmem_dup_impl(wcsdup)
#define _aligned_malloc_impl mozmem_dup_impl(_aligned_malloc)
#endif

// String functions
#ifdef ANDROID
// Bug 801571 and Bug 879668, libstagefright uses vasprintf, causing malloc()/
// free() to be mismatched between bionic and mozglue implementation.
#define vasprintf_impl mozmem_dup_impl(vasprintf)
#define asprintf_impl mozmem_dup_impl(asprintf)
#endif

#endif // mozmemory_wrap_h

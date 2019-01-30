/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Portions of this file were originally under the following license:
//
// Copyright (C) 2006-2008 Jason Evans <jasone@FreeBSD.org>.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice(s), this list of conditions and the following disclaimer as
//    the first lines of this file unmodified other than the possible
//    addition of one or more copyright notices.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice(s), this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the
//    distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
// OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef _JEMALLOC_TYPES_H_
#define _JEMALLOC_TYPES_H_

// grab size_t
#ifdef _MSC_VER
#include <crtdefs.h>
#else
#include <stddef.h>
#endif
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MALLOC_USABLE_SIZE_CONST_PTR
#define MALLOC_USABLE_SIZE_CONST_PTR const
#endif

typedef MALLOC_USABLE_SIZE_CONST_PTR void* usable_ptr_t;

typedef size_t arena_id_t;

typedef struct arena_params_s
{
  size_t mMaxDirty;

#ifdef __cplusplus
  arena_params_s()
    : mMaxDirty(0)
  {
  }
#endif
} arena_params_t;

// jemalloc_stats() is not a stable interface.  When using jemalloc_stats_t, be
// sure that the compiled results of jemalloc.c are in sync with this header
// file.
typedef struct
{
  // Run-time configuration settings.
  bool opt_junk;    // Fill allocated memory with kAllocJunk?
  bool opt_zero;    // Fill allocated memory with 0x0?
  size_t narenas;   // Number of arenas.
  size_t quantum;   // Allocation quantum.
  size_t small_max; // Max quantum-spaced allocation size.
  size_t large_max; // Max sub-chunksize allocation size.
  size_t chunksize; // Size of each virtual memory mapping.
  size_t page_size; // Size of pages.
  size_t dirty_max; // Max dirty pages per arena.

  // Current memory usage statistics.
  size_t mapped;      // Bytes mapped (not necessarily committed).
  size_t allocated;   // Bytes allocated (committed, in use by application).
  size_t waste;       // Bytes committed, not in use by the
                      // application, and not intentionally left
                      // unused (i.e., not dirty).
  size_t page_cache;  // Committed, unused pages kept around as a
                      // cache.  (jemalloc calls these "dirty".)
  size_t bookkeeping; // Committed bytes used internally by the
                      // allocator.
  size_t bin_unused;  // Bytes committed to a bin but currently unused.
} jemalloc_stats_t;

enum PtrInfoTag
{
  // The pointer is not currently known to the allocator.
  // 'addr' and 'size' are always 0.
  TagUnknown,

  // The pointer is within a live allocation.
  // 'addr' and 'size' describe the allocation.
  TagLiveSmall,
  TagLiveLarge,
  TagLiveHuge,

  // The pointer is within a small freed allocation.
  // 'addr' and 'size' describe the allocation.
  TagFreedSmall,

  // The pointer is within a freed page. Details about the original
  // allocation, including its size, are not available.
  // 'addr' and 'size' describe the page.
  TagFreedPageDirty,
  TagFreedPageDecommitted,
  TagFreedPageMadvised,
  TagFreedPageZeroed,
};

// The information in jemalloc_ptr_info_t could be represented in a variety of
// ways. The chosen representation has the following properties.
// - The number of fields is minimized.
// - The 'tag' field unambiguously defines the meaning of the subsequent fields.
// Helper functions are used to group together related categories of tags.
typedef struct
{
  enum PtrInfoTag tag;
  void* addr;  // meaning depends on tag; see above
  size_t size; // meaning depends on tag; see above
} jemalloc_ptr_info_t;

static inline bool
jemalloc_ptr_is_live(jemalloc_ptr_info_t* info)
{
  return info->tag == TagLiveSmall || info->tag == TagLiveLarge ||
         info->tag == TagLiveHuge;
}

static inline bool
jemalloc_ptr_is_freed(jemalloc_ptr_info_t* info)
{
  return info->tag == TagFreedSmall || info->tag == TagFreedPageDirty ||
         info->tag == TagFreedPageDecommitted ||
         info->tag == TagFreedPageMadvised || info->tag == TagFreedPageZeroed;
}

static inline bool
jemalloc_ptr_is_freed_page(jemalloc_ptr_info_t* info)
{
  return info->tag == TagFreedPageDirty ||
         info->tag == TagFreedPageDecommitted ||
         info->tag == TagFreedPageMadvised || info->tag == TagFreedPageZeroed;
}

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _JEMALLOC_TYPES_H_

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

#include <stdint.h>

// grab size_t
#ifdef _MSC_VER
#  include <crtdefs.h>
#else
#  include <stddef.h>
#endif
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MALLOC_USABLE_SIZE_CONST_PTR
#  define MALLOC_USABLE_SIZE_CONST_PTR const
#endif

typedef MALLOC_USABLE_SIZE_CONST_PTR void* usable_ptr_t;

typedef size_t arena_id_t;

#define ARENA_FLAG_RANDOMIZE_SMALL_MASK 0x3
#define ARENA_FLAG_RANDOMIZE_SMALL_DEFAULT 0
#define ARENA_FLAG_RANDOMIZE_SMALL_ENABLED 1
#define ARENA_FLAG_RANDOMIZE_SMALL_DISABLED 2

// Arenas are usually protected by a lock (ARENA_FLAG_THREAD_SAFE) however some
// arenas are accessed by only the main thread
// (ARENA_FLAG_THREAD_MAIN_THREAD_ONLY) and their locking can be skipped.
#define ARENA_FLAG_THREAD_MASK 0x4
#define ARENA_FLAG_THREAD_MAIN_THREAD_ONLY 0x4
#define ARENA_FLAG_THREAD_SAFE 0x0

typedef struct arena_params_s {
  size_t mMaxDirty;
  // Arena specific modifiers which override the value passed to
  // moz_set_max_dirty_page_modifier. If value > 0 is passed to that function,
  // and mMaxDirtyIncreaseOverride != 0, mMaxDirtyIncreaseOverride will be used
  // instead, and similarly if value < 0 is passed and mMaxDirtyDecreaseOverride
  // != 0, mMaxDirtyDecreaseOverride will be used as the modifier.
  int32_t mMaxDirtyIncreaseOverride;
  int32_t mMaxDirtyDecreaseOverride;

  uint32_t mFlags;

#ifdef __cplusplus
  arena_params_s()
      : mMaxDirty(0),
        mMaxDirtyIncreaseOverride(0),
        mMaxDirtyDecreaseOverride(0),
        mFlags(0) {}
#endif
} arena_params_t;

// jemalloc_stats() is not a stable interface.  When using jemalloc_stats_t, be
// sure that the compiled results of jemalloc.c are in sync with this header
// file.
typedef struct {
  // Run-time configuration settings.
  bool opt_junk;            // Fill allocated memory with kAllocJunk?
  bool opt_zero;            // Fill allocated memory with 0x0?
  size_t narenas;           // Number of arenas.
  size_t quantum;           // Allocation quantum.
  size_t quantum_max;       // Max quantum-spaced allocation size.
  size_t quantum_wide;      // Allocation quantum (QuantuWide).
  size_t quantum_wide_max;  // Max quantum-wide-spaced allocation size.
  size_t subpage_max;       // Max subpage allocation size.
  size_t large_max;         // Max sub-chunksize allocation size.
  size_t chunksize;         // Size of each virtual memory mapping.
  size_t page_size;         // Size of pages.
  size_t dirty_max;         // Max dirty pages per arena.

  // Current memory usage statistics.
  size_t mapped;          // Bytes mapped (not necessarily committed).
  size_t allocated;       // Bytes allocated (committed, in use by application).
  size_t waste;           // Bytes committed, not in use by the
                          // application, and not intentionally left
                          // unused (i.e., not dirty).
  size_t pages_dirty;     // Committed, unused pages kept around as a cache.
  size_t pages_fresh;     // Unused pages that have never been touched.
  size_t pages_madvised;  // Unsed pages we told the kernel we don't need.
  size_t bookkeeping;     // Committed bytes used internally by the
                          // allocator.
  size_t bin_unused;      // Bytes committed to a bin but currently unused.
} jemalloc_stats_t;

typedef struct {
  size_t size;               // The size of objects in this bin, zero if this
                             // bin stats array entry is unused (no more bins).
  size_t num_non_full_runs;  // The number of non-full runs
  size_t num_runs;           // The number of runs in this bin
  size_t bytes_unused;       // The unallocated bytes across all these bins
  size_t bytes_total;        // The total storage area for runs in this bin,
  size_t bytes_per_run;      // The number of bytes per run, including headers.
} jemalloc_bin_stats_t;

enum PtrInfoTag {
  // The pointer is not currently known to the allocator.
  // 'addr', 'size', and 'arenaId' are always 0.
  TagUnknown,

  // The pointer is within a live allocation.
  // 'addr', 'size', and 'arenaId' describe the allocation.
  TagLiveAlloc,

  // The pointer is within a small freed allocation.
  // 'addr', 'size', and 'arenaId' describe the allocation.
  TagFreedAlloc,

  // The pointer is within a freed page. Details about the original
  // allocation, including its size, are not available.
  // 'addr', 'size', and 'arenaId' describe the page.
  TagFreedPage,
};

// The information in jemalloc_ptr_info_t could be represented in a variety of
// ways. The chosen representation has the following properties.
// - The number of fields is minimized.
// - The 'tag' field unambiguously defines the meaning of the subsequent fields.
// Helper functions are used to group together related categories of tags.
typedef struct jemalloc_ptr_info_s {
  enum PtrInfoTag tag;
  void* addr;   // meaning depends on tag; see above
  size_t size;  // meaning depends on tag; see above

#ifdef MOZ_DEBUG
  arena_id_t arenaId;  // meaning depends on tag; see above
#endif

#ifdef __cplusplus
  jemalloc_ptr_info_s() = default;
  jemalloc_ptr_info_s(enum PtrInfoTag aTag, void* aAddr, size_t aSize,
                      arena_id_t aArenaId)
      : tag(aTag),
        addr(aAddr),
        size(aSize)
#  ifdef MOZ_DEBUG
        ,
        arenaId(aArenaId)
#  endif
  {
  }
#endif
} jemalloc_ptr_info_t;

static inline bool jemalloc_ptr_is_live(jemalloc_ptr_info_t* info) {
  return info->tag == TagLiveAlloc;
}

static inline bool jemalloc_ptr_is_freed(jemalloc_ptr_info_t* info) {
  return info->tag == TagFreedAlloc || info->tag == TagFreedPage;
}

static inline bool jemalloc_ptr_is_freed_page(jemalloc_ptr_info_t* info) {
  return info->tag == TagFreedPage;
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // _JEMALLOC_TYPES_H_

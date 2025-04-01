/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Linux kernels since 5.17 have a feature for assigning names to
// ranges of anonymous memory (i.e., memory that doesn't have a "name"
// in the form of an underlying mapped file).  These names are
// reported in /proc/<pid>/smaps alongside system-level memory usage
// information such as Proportional Set Size (memory usage adjusted
// for sharing between processes), which allows reporting this
// information at a finer granularity than would otherwise be possible
// (e.g., separating malloc() heap from JS heap).
//
// Existing memory can be tagged with MozTagAnonymousMemory(); it will
// tag the range of complete pages containing the given interval, so
// the results may be inexact if the range isn't page-aligned.
// MozTaggedAnonymousMmap() can be used like mmap() with an extra
// parameter, and will tag the returned memory if the mapping was
// successful (and if it was in fact anonymous).
//
// NOTE: The pointer given as the "tag" argument MUST remain valid as
// long as the mapping exists.  The referenced string is read when
// /proc/<pid>/smaps or /proc/<pid>/maps is read, not when the tag is
// established, so freeing it or changing its contents will have
// unexpected results.  Using a static string is probably best.
//
// Also note that this header can be used by both C and C++ code.

#ifndef mozilla_TaggedAnonymousMemory_h
#define mozilla_TaggedAnonymousMemory_h

#ifndef XP_WIN

#  ifdef __wasi__
#    include <stdlib.h>
#  else
#    include <sys/types.h>
#    include <sys/mman.h>
#  endif  // __wasi__

#  include "mozilla/Types.h"

#  ifdef XP_LINUX

#    ifdef __cplusplus
extern "C" {
#    endif

MFBT_API void MozTagAnonymousMemory(const void* aPtr, size_t aLength,
                                    const char* aTag);

MFBT_API void* MozTaggedAnonymousMmap(void* aAddr, size_t aLength, int aProt,
                                      int aFlags, int aFd, off_t aOffset,
                                      const char* aTag);

#    ifdef __cplusplus
}  // extern "C"
#    endif

#  else  // XP_LINUX

static inline void MozTagAnonymousMemory(const void* aPtr, size_t aLength,
                                         const char* aTag) {}

static inline void* MozTaggedAnonymousMmap(void* aAddr, size_t aLength,
                                           int aProt, int aFlags, int aFd,
                                           off_t aOffset, const char* aTag) {
#    ifdef __wasi__
  MOZ_CRASH("We don't use this memory for WASI right now.");
  return nullptr;
#    else
  return mmap(aAddr, aLength, aProt, aFlags, aFd, aOffset);
#    endif
}

#  endif  // XP_LINUX

#endif  // !XP_WIN

#endif  // mozilla_TaggedAnonymousMemory_h

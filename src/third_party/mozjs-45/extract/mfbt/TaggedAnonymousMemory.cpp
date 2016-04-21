/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef ANDROID

#include "mozilla/TaggedAnonymousMemory.h"

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "mozilla/Assertions.h"

// These constants are copied from <sys/prctl.h>, because the headers
// used for building may not have them even though the running kernel
// supports them.
#ifndef PR_SET_VMA
#define PR_SET_VMA		0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME		0
#endif

namespace mozilla {

// Returns 0 for success and -1 (with errno) for error.
static int
TagAnonymousMemoryAligned(const void* aPtr, size_t aLength, const char* aTag)
{
  return prctl(PR_SET_VMA,
               PR_SET_VMA_ANON_NAME,
               reinterpret_cast<unsigned long>(aPtr),
               aLength,
               reinterpret_cast<unsigned long>(aTag));
}

// On some architectures, it's possible for the page size to be larger
// than the PAGE_SIZE we were compiled with.  This computes the
// equivalent of PAGE_MASK.
static uintptr_t
GetPageMask()
{
  static uintptr_t mask = 0;

  if (mask == 0) {
    uintptr_t pageSize = sysconf(_SC_PAGESIZE);
    mask = ~(pageSize - 1);
    MOZ_ASSERT((pageSize & (pageSize - 1)) == 0,
               "Page size must be a power of 2!");
  }
  return mask;
}

} // namespace mozilla

int
MozTaggedMemoryIsSupported(void)
{
  static int supported = -1;

  if (supported == -1) {
    // Tagging an empty range always "succeeds" if the feature is supported,
    // regardless of the start pointer.
    supported = mozilla::TagAnonymousMemoryAligned(nullptr, 0, nullptr) == 0;
  }
  return supported;
}

void
MozTagAnonymousMemory(const void* aPtr, size_t aLength, const char* aTag)
{
  if (MozTaggedMemoryIsSupported()) {
    // The kernel will round up the end of the range to the next page
    // boundary if it's not aligned (comments indicate this behavior
    // is based on that of madvise), but it will reject the request if
    // the start is not aligned.  We therefore round down the start
    // address and adjust the length accordingly.
    uintptr_t addr = reinterpret_cast<uintptr_t>(aPtr);
    uintptr_t end = addr + aLength;
    uintptr_t addrRounded = addr & mozilla::GetPageMask();
    const void* ptrRounded = reinterpret_cast<const void*>(addrRounded);

    mozilla::TagAnonymousMemoryAligned(ptrRounded, end - addrRounded, aTag);
  }
}

void*
MozTaggedAnonymousMmap(void* aAddr, size_t aLength, int aProt, int aFlags,
                       int aFd, off_t aOffset, const char* aTag)
{
  void* mapped = mmap(aAddr, aLength, aProt, aFlags, aFd, aOffset);
  if (MozTaggedMemoryIsSupported() &&
      (aFlags & MAP_ANONYMOUS) == MAP_ANONYMOUS &&
      mapped != MAP_FAILED) {
    mozilla::TagAnonymousMemoryAligned(mapped, aLength, aTag);
  }
  return mapped;
}

#endif // ANDROID

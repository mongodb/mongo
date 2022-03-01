/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A poison value that can be used to fill a memory space with
 * an address that leads to a safe crash when dereferenced.
 */

#include "mozilla/Poison.h"

#include "mozilla/Assertions.h"
#ifdef _WIN32
#  include <windows.h>
#elif !defined(__OS2__)
#  include <unistd.h>
#  ifndef __wasi__
#    include <sys/mman.h>
#    ifndef MAP_ANON
#      ifdef MAP_ANONYMOUS
#        define MAP_ANON MAP_ANONYMOUS
#      else
#        error "Don't know how to get anonymous memory"
#      endif
#    endif
#  endif
#endif

// Freed memory is filled with a poison value, which we arrange to
// form a pointer either to an always-unmapped region of the address
// space, or to a page that has been reserved and rendered
// inaccessible via OS primitives.  See tests/TestPoisonArea.cpp for
// extensive discussion of the requirements for this page.  The code
// from here to 'class FreeList' needs to be kept in sync with that
// file.

#ifdef _WIN32
static void* ReserveRegion(uintptr_t aRegion, uintptr_t aSize) {
  return VirtualAlloc((void*)aRegion, aSize, MEM_RESERVE, PAGE_NOACCESS);
}

static void ReleaseRegion(void* aRegion, uintptr_t aSize) {
  VirtualFree(aRegion, aSize, MEM_RELEASE);
}

static bool ProbeRegion(uintptr_t aRegion, uintptr_t aSize) {
  SYSTEM_INFO sinfo;
  GetSystemInfo(&sinfo);
  if (aRegion >= (uintptr_t)sinfo.lpMaximumApplicationAddress &&
      aRegion + aSize >= (uintptr_t)sinfo.lpMaximumApplicationAddress) {
    return true;
  } else {
    return false;
  }
}

static uintptr_t GetDesiredRegionSize() {
  SYSTEM_INFO sinfo;
  GetSystemInfo(&sinfo);
  return sinfo.dwAllocationGranularity;
}

#  define RESERVE_FAILED 0

#elif defined(__OS2__)
static void* ReserveRegion(uintptr_t aRegion, uintptr_t aSize) {
  // OS/2 doesn't support allocation at an arbitrary address,
  // so return an address that is known to be invalid.
  return (void*)0xFFFD0000;
}

static void ReleaseRegion(void* aRegion, uintptr_t aSize) { return; }

static bool ProbeRegion(uintptr_t aRegion, uintptr_t aSize) {
  // There's no reliable way to probe an address in the system
  // arena other than by touching it and seeing if a trap occurs.
  return false;
}

static uintptr_t GetDesiredRegionSize() {
  // Page size is fixed at 4k.
  return 0x1000;
}

#  define RESERVE_FAILED 0

#elif defined(__wasi__)

#  define RESERVE_FAILED 0

static void* ReserveRegion(uintptr_t aRegion, uintptr_t aSize) {
  return RESERVE_FAILED;
}

static void ReleaseRegion(void* aRegion, uintptr_t aSize) { return; }

static bool ProbeRegion(uintptr_t aRegion, uintptr_t aSize) {
  const auto pageSize = 1 << 16;
  MOZ_ASSERT(pageSize == sysconf(_SC_PAGESIZE));
  auto heapSize = __builtin_wasm_memory_size(0) * pageSize;
  return aRegion + aSize < heapSize;
}

static uintptr_t GetDesiredRegionSize() { return 0; }

#else  // __wasi__

#  include "mozilla/TaggedAnonymousMemory.h"

static void* ReserveRegion(uintptr_t aRegion, uintptr_t aSize) {
  return MozTaggedAnonymousMmap(reinterpret_cast<void*>(aRegion), aSize,
                                PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0,
                                "poison");
}

static void ReleaseRegion(void* aRegion, uintptr_t aSize) {
  munmap(aRegion, aSize);
}

static bool ProbeRegion(uintptr_t aRegion, uintptr_t aSize) {
#  ifdef XP_SOLARIS
  if (posix_madvise(reinterpret_cast<void*>(aRegion), aSize,
                    POSIX_MADV_NORMAL)) {
#  else
  if (madvise(reinterpret_cast<void*>(aRegion), aSize, MADV_NORMAL)) {
#  endif
    return true;
  } else {
    return false;
  }
}

static uintptr_t GetDesiredRegionSize() { return sysconf(_SC_PAGESIZE); }

#  define RESERVE_FAILED MAP_FAILED

#endif  // system dependencies

static_assert((sizeof(uintptr_t) == 4 || sizeof(uintptr_t) == 8) &&
              (sizeof(uintptr_t) == sizeof(void*)));

static uintptr_t ReservePoisonArea(uintptr_t rgnsize) {
  if (sizeof(uintptr_t) == 8) {
    // Use the hardware-inaccessible region.
    // We have to avoid 64-bit constants and shifts by 32 bits, since this
    // code is compiled in 32-bit mode, although it is never executed there.
    return (((uintptr_t(0x7FFFFFFFu) << 31) << 1 | uintptr_t(0xF0DEAFFFu)) &
            ~(rgnsize - 1));
  }

  // First see if we can allocate the preferred poison address from the OS.
  uintptr_t candidate = (0xF0DEAFFF & ~(rgnsize - 1));
  void* result = ReserveRegion(candidate, rgnsize);
  if (result == (void*)candidate) {
    // success - inaccessible page allocated
    return candidate;
  }

  // That didn't work, so see if the preferred address is within a range
  // of permanently inacessible memory.
  if (ProbeRegion(candidate, rgnsize)) {
    // success - selected page cannot be usable memory
    if (result != RESERVE_FAILED) {
      ReleaseRegion(result, rgnsize);
    }
    return candidate;
  }

  // The preferred address is already in use.  Did the OS give us a
  // consolation prize?
  if (result != RESERVE_FAILED) {
    return uintptr_t(result);
  }

  // It didn't, so try to allocate again, without any constraint on
  // the address.
  result = ReserveRegion(0, rgnsize);
  if (result != RESERVE_FAILED) {
    return uintptr_t(result);
  }

  MOZ_CRASH("no usable poison region identified");
}

static uintptr_t GetPoisonValue(uintptr_t aBase, uintptr_t aSize) {
  if (aSize == 0) {  // can't happen
    return 0;
  }
  return aBase + aSize / 2 - 1;
}

// Poison is used so pervasively throughout the codebase that we decided it was
// best to actually use ordered dynamic initialization of globals (AKA static
// constructors) for this. This way everything will have properly initialized
// poison -- except other dynamic initialization code in libmozglue, which there
// shouldn't be much of. (libmozglue is one of the first things loaded, and
// specifically comes before libxul, so nearly all gecko code runs strictly
// after this.)
extern "C" {
uintptr_t gMozillaPoisonSize = GetDesiredRegionSize();
uintptr_t gMozillaPoisonBase = ReservePoisonArea(gMozillaPoisonSize);
uintptr_t gMozillaPoisonValue =
    GetPoisonValue(gMozillaPoisonBase, gMozillaPoisonSize);
}

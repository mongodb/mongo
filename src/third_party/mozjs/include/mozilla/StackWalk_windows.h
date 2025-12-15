/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StackWalk_windows_h
#define mozilla_StackWalk_windows_h

#include "mozilla/Array.h"
#include "mozilla/Types.h"

#if defined(_M_AMD64) || defined(_M_ARM64)
/**
 * This function enables strategy (1) for avoiding deadlocks between the stack
 * walking thread and the suspended thread. In aStackWalkLocks the caller must
 * provide pointers to the two ntdll-internal SRW locks acquired by
 * RtlLookupFunctionEntry. These locks are LdrpInvertedFunctionTableSRWLock and
 * RtlpDynamicFunctionTableLock -- we don't need to know which one is which.
 * Until InitializeStackWalkLocks function is called, strategy (2) is used.
 *
 * See comment in StackWalk.cpp
 */
MFBT_API
void InitializeStackWalkLocks(const mozilla::Array<void*, 2>& aStackWalkLocks);

/**
 * As part of strategy (2) for avoiding deadlocks between the stack walking
 * thread and the suspended thread, we mark stack walk suppression paths by
 * putting them under the scope of a AutoSuppressStackWalking object. Any code
 * path that may do an exclusive acquire of LdrpInvertedFunctionTableSRWLock or
 * RtlpDynamicFunctionTableLock should be marked this way, to ensure that
 * strategy (2) can properly mitigate all deadlock scenarios.
 *
 * See comment in StackWalk.cpp
 */
struct MOZ_RAII AutoSuppressStackWalking {
  MFBT_API AutoSuppressStackWalking();
  MFBT_API ~AutoSuppressStackWalking();
};

#  if defined(IMPL_MFBT)
void SuppressStackWalking();
void DesuppressStackWalking();
#  endif  // defined(IMPL_MFBT)

MFBT_API void RegisterJitCodeRegion(uint8_t* aStart, size_t size);

MFBT_API void UnregisterJitCodeRegion(uint8_t* aStart, size_t size);
#endif  // _M_AMD64 || _M_ARM64

#endif  // mozilla_StackWalk_windows_h

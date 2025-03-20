/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StackWalk_windows_h
#define mozilla_StackWalk_windows_h

#include "mozilla/Types.h"

#if defined(_M_AMD64) || defined(_M_ARM64)
/**
 * Allow stack walkers to work around the egregious win64 dynamic lookup table
 * list API by locking around SuspendThread to avoid deadlock.
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

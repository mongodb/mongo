/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/PlatformMutex.h"

#include <windows.h>

#include "MutexPlatformData_windows.h"

mozilla::detail::MutexImpl::MutexImpl()
{
  InitializeSRWLock(&platformData()->lock);
}

mozilla::detail::MutexImpl::~MutexImpl()
{
}

void
mozilla::detail::MutexImpl::lock()
{
  AcquireSRWLockExclusive(&platformData()->lock);
}

void
mozilla::detail::MutexImpl::unlock()
{
  ReleaseSRWLockExclusive(&platformData()->lock);
}

mozilla::detail::MutexImpl::PlatformData*
mozilla::detail::MutexImpl::platformData()
{
  static_assert(sizeof(platformData_) >= sizeof(PlatformData),
                "platformData_ is too small");
  return reinterpret_cast<PlatformData*>(platformData_);
}

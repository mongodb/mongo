/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"

#include <float.h>
#include <intrin.h>
#include <stdlib.h>
#include <windows.h>

#include "mozilla/PlatformConditionVariable.h"
#include "mozilla/PlatformMutex.h"
#include "MutexPlatformData_windows.h"

// Some versions of the Windows SDK have a bug where some interlocked functions
// are not redefined as compiler intrinsics. Fix that for the interlocked
// functions that are used in this file.
#if defined(_MSC_VER) && !defined(InterlockedExchangeAdd)
#define InterlockedExchangeAdd(addend, value)                                  \
  _InterlockedExchangeAdd((volatile long*)(addend), (long)(value))
#endif

#if defined(_MSC_VER) && !defined(InterlockedIncrement)
#define InterlockedIncrement(addend)                                           \
  _InterlockedIncrement((volatile long*)(addend))
#endif

// Wrapper for native condition variable APIs.
struct mozilla::detail::ConditionVariableImpl::PlatformData
{
  CONDITION_VARIABLE cv_;
};

mozilla::detail::ConditionVariableImpl::ConditionVariableImpl()
{
  InitializeConditionVariable(&platformData()->cv_);
}

void
mozilla::detail::ConditionVariableImpl::notify_one()
{
  WakeConditionVariable(&platformData()->cv_);
}

void
mozilla::detail::ConditionVariableImpl::notify_all()
{
  WakeAllConditionVariable(&platformData()->cv_);
}

void
mozilla::detail::ConditionVariableImpl::wait(MutexImpl& lock)
{
  SRWLOCK* srwlock = &lock.platformData()->lock;
  bool r = SleepConditionVariableSRW(&platformData()->cv_, srwlock, INFINITE, 0);
  MOZ_RELEASE_ASSERT(r);
}

mozilla::detail::CVStatus
mozilla::detail::ConditionVariableImpl::wait_for(MutexImpl& lock,
                                                 const mozilla::TimeDuration& rel_time)
{
  SRWLOCK* srwlock = &lock.platformData()->lock;

  // Note that DWORD is unsigned, so we have to be careful to clamp at 0.
  // If rel_time is Forever, then ToMilliseconds is +inf, which evaluates as
  // greater than UINT32_MAX, resulting in the correct INFINITE wait.
  double msecd = rel_time.ToMilliseconds();
  DWORD msec = msecd < 0.0
               ? 0
               : msecd > UINT32_MAX
                 ? INFINITE
                 : static_cast<DWORD>(msecd);

  BOOL r = SleepConditionVariableSRW(&platformData()->cv_, srwlock, msec, 0);
  if (r)
    return CVStatus::NoTimeout;
  MOZ_RELEASE_ASSERT(GetLastError() == ERROR_TIMEOUT);
  return CVStatus::Timeout;
}

mozilla::detail::ConditionVariableImpl::~ConditionVariableImpl()
{
  // Native condition variables don't require cleanup.
}

inline mozilla::detail::ConditionVariableImpl::PlatformData*
mozilla::detail::ConditionVariableImpl::platformData()
{
  static_assert(sizeof platformData_ >= sizeof(PlatformData),
                "platformData_ is too small");
  return reinterpret_cast<PlatformData*>(platformData_);
}

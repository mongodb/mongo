/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

#include "mozilla/PlatformMutex.h"
#include "MutexPlatformData_posix.h"

#define TRY_CALL_PTHREADS(call, msg)            \
  {                                             \
    int result = (call);                        \
    if (result != 0) {                          \
      errno = result;                           \
      perror(msg);                              \
      MOZ_CRASH(msg);                           \
    }                                           \
  }

mozilla::detail::MutexImpl::MutexImpl()
{
  pthread_mutexattr_t* attrp = nullptr;

  // Linux with glibc and FreeBSD support adaptive mutexes that spin
  // for a short number of tries before sleeping.  NSPR's locks did
  // this, too, and it seems like a reasonable thing to do.
#if (defined(__linux__) && defined(__GLIBC__)) || defined(__FreeBSD__)
#define ADAPTIVE_MUTEX_SUPPORTED
#endif

#if defined(DEBUG)
#define ATTR_REQUIRED
#define MUTEX_KIND PTHREAD_MUTEX_ERRORCHECK
#elif defined(ADAPTIVE_MUTEX_SUPPORTED)
#define ATTR_REQUIRED
#define MUTEX_KIND PTHREAD_MUTEX_ADAPTIVE_NP
#endif

#if defined(ATTR_REQUIRED)
  pthread_mutexattr_t attr;

  TRY_CALL_PTHREADS(pthread_mutexattr_init(&attr),
                    "mozilla::detail::MutexImpl::MutexImpl: pthread_mutexattr_init failed");

  TRY_CALL_PTHREADS(pthread_mutexattr_settype(&attr, MUTEX_KIND),
                    "mozilla::detail::MutexImpl::MutexImpl: pthread_mutexattr_settype failed");
  attrp = &attr;
#endif

  TRY_CALL_PTHREADS(pthread_mutex_init(&platformData()->ptMutex, attrp),
                    "mozilla::detail::MutexImpl::MutexImpl: pthread_mutex_init failed");

#if defined(ATTR_REQUIRED)
  TRY_CALL_PTHREADS(pthread_mutexattr_destroy(&attr),
                    "mozilla::detail::MutexImpl::MutexImpl: pthread_mutexattr_destroy failed");
#endif
}

mozilla::detail::MutexImpl::~MutexImpl()
{
  TRY_CALL_PTHREADS(pthread_mutex_destroy(&platformData()->ptMutex),
                    "mozilla::detail::MutexImpl::~MutexImpl: pthread_mutex_destroy failed");
}

void
mozilla::detail::MutexImpl::lock()
{
  TRY_CALL_PTHREADS(pthread_mutex_lock(&platformData()->ptMutex),
                    "mozilla::detail::MutexImpl::lock: pthread_mutex_lock failed");
}

void
mozilla::detail::MutexImpl::unlock()
{
  TRY_CALL_PTHREADS(pthread_mutex_unlock(&platformData()->ptMutex),
                    "mozilla::detail::MutexImpl::unlock: pthread_mutex_unlock failed");
}

#undef TRY_CALL_PTHREADS

mozilla::detail::MutexImpl::PlatformData*
mozilla::detail::MutexImpl::platformData()
{
  static_assert(sizeof(platformData_) >= sizeof(PlatformData),
                "platformData_ is too small");
  return reinterpret_cast<PlatformData*>(platformData_);
}

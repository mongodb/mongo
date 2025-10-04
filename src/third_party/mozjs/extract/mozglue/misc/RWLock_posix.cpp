/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef XP_WIN
#  error This file should only be compiled on non-Windows platforms.
#endif

#include "mozilla/PlatformRWLock.h"

#include "mozilla/Assertions.h"

#include <errno.h>

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
mozilla::detail::RWLockImpl::RWLockImpl() {
  MOZ_RELEASE_ASSERT(pthread_rwlock_init(&mRWLock, nullptr) == 0,
                     "pthread_rwlock_init failed");
}

mozilla::detail::RWLockImpl::~RWLockImpl() {
  MOZ_RELEASE_ASSERT(pthread_rwlock_destroy(&mRWLock) == 0,
                     "pthread_rwlock_destroy failed");
}

bool mozilla::detail::RWLockImpl::tryReadLock() {
  int rv = pthread_rwlock_tryrdlock(&mRWLock);
  // We allow EDEADLK here because it has been observed returned on macos when
  // the write lock is held by the current thread.
  MOZ_RELEASE_ASSERT(rv == 0 || rv == EBUSY || rv == EDEADLK,
                     "pthread_rwlock_tryrdlock failed");
  return rv == 0;
}

void mozilla::detail::RWLockImpl::readLock() {
  MOZ_RELEASE_ASSERT(pthread_rwlock_rdlock(&mRWLock) == 0,
                     "pthread_rwlock_rdlock failed");
}

void mozilla::detail::RWLockImpl::readUnlock() {
  MOZ_RELEASE_ASSERT(pthread_rwlock_unlock(&mRWLock) == 0,
                     "pthread_rwlock_unlock failed");
}

bool mozilla::detail::RWLockImpl::tryWriteLock() {
  int rv = pthread_rwlock_trywrlock(&mRWLock);
  // We allow EDEADLK here because it has been observed returned on macos when
  // the write lock is held by the current thread.
  MOZ_RELEASE_ASSERT(rv == 0 || rv == EBUSY || rv == EDEADLK,
                     "pthread_rwlock_trywrlock failed");
  return rv == 0;
}

void mozilla::detail::RWLockImpl::writeLock() {
  MOZ_RELEASE_ASSERT(pthread_rwlock_wrlock(&mRWLock) == 0,
                     "pthread_rwlock_wrlock failed");
}

void mozilla::detail::RWLockImpl::writeUnlock() {
  MOZ_RELEASE_ASSERT(pthread_rwlock_unlock(&mRWLock) == 0,
                     "pthread_rwlock_unlock failed");
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_glue_MozglueUtils_h
#define mozilla_glue_MozglueUtils_h

#include <windows.h>

#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"

namespace mozilla {
namespace glue {

#ifdef DEBUG

class MOZ_STATIC_CLASS Win32SRWLock final {
 public:
  // Microsoft guarantees that '0' is never a valid thread id
  // https://docs.microsoft.com/en-ca/windows/desktop/ProcThread/thread-handles-and-identifiers
  static const DWORD kInvalidThreadId = 0;

  constexpr Win32SRWLock()
      : mExclusiveThreadId(kInvalidThreadId), mLock(SRWLOCK_INIT) {}

  ~Win32SRWLock() { MOZ_ASSERT(mExclusiveThreadId == kInvalidThreadId); }

  void LockShared() {
    MOZ_ASSERT(
        mExclusiveThreadId != GetCurrentThreadId(),
        "Deadlock detected - A thread attempted to acquire a shared lock on "
        "a SRWLOCK when it already owns the exclusive lock on it.");

    ::AcquireSRWLockShared(&mLock);
  }

  void UnlockShared() { ::ReleaseSRWLockShared(&mLock); }

  void LockExclusive() {
    MOZ_ASSERT(
        mExclusiveThreadId != GetCurrentThreadId(),
        "Deadlock detected - A thread attempted to acquire an exclusive lock "
        "on a SRWLOCK when it already owns the exclusive lock on it.");

    ::AcquireSRWLockExclusive(&mLock);
    mExclusiveThreadId = GetCurrentThreadId();
  }

  void UnlockExclusive() {
    MOZ_ASSERT(mExclusiveThreadId == GetCurrentThreadId());

    mExclusiveThreadId = kInvalidThreadId;
    ::ReleaseSRWLockExclusive(&mLock);
  }

  Win32SRWLock(const Win32SRWLock&) = delete;
  Win32SRWLock(Win32SRWLock&&) = delete;
  Win32SRWLock& operator=(const Win32SRWLock&) = delete;
  Win32SRWLock& operator=(Win32SRWLock&&) = delete;

 private:
  // "Relaxed" memory ordering is fine. Threads will see other thread IDs
  // appear here in some non-deterministic ordering (or not at all) and simply
  // ignore them.
  //
  // But a thread will only read its own ID if it previously wrote it, and a
  // single thread doesn't need a memory barrier to read its own write.

  Atomic<DWORD, Relaxed> mExclusiveThreadId;
  SRWLOCK mLock;
};

#else  // DEBUG

class MOZ_STATIC_CLASS Win32SRWLock final {
 public:
  constexpr Win32SRWLock() : mLock(SRWLOCK_INIT) {}

  void LockShared() { ::AcquireSRWLockShared(&mLock); }

  void UnlockShared() { ::ReleaseSRWLockShared(&mLock); }

  void LockExclusive() { ::AcquireSRWLockExclusive(&mLock); }

  void UnlockExclusive() { ::ReleaseSRWLockExclusive(&mLock); }

  ~Win32SRWLock() = default;

  Win32SRWLock(const Win32SRWLock&) = delete;
  Win32SRWLock(Win32SRWLock&&) = delete;
  Win32SRWLock& operator=(const Win32SRWLock&) = delete;
  Win32SRWLock& operator=(Win32SRWLock&&) = delete;

 private:
  SRWLOCK mLock;
};

#endif

class MOZ_RAII AutoSharedLock final {
 public:
  explicit AutoSharedLock(Win32SRWLock& aLock) : mLock(aLock) {
    mLock.LockShared();
  }

  ~AutoSharedLock() { mLock.UnlockShared(); }

  AutoSharedLock(const AutoSharedLock&) = delete;
  AutoSharedLock(AutoSharedLock&&) = delete;
  AutoSharedLock& operator=(const AutoSharedLock&) = delete;
  AutoSharedLock& operator=(AutoSharedLock&&) = delete;

 private:
  Win32SRWLock& mLock;
};

class MOZ_RAII AutoExclusiveLock final {
 public:
  explicit AutoExclusiveLock(Win32SRWLock& aLock) : mLock(aLock) {
    mLock.LockExclusive();
  }

  ~AutoExclusiveLock() { mLock.UnlockExclusive(); }

  AutoExclusiveLock(const AutoExclusiveLock&) = delete;
  AutoExclusiveLock(AutoExclusiveLock&&) = delete;
  AutoExclusiveLock& operator=(const AutoExclusiveLock&) = delete;
  AutoExclusiveLock& operator=(AutoExclusiveLock&&) = delete;

 private:
  Win32SRWLock& mLock;
};

}  // namespace glue
}  // namespace mozilla

#endif  //  mozilla_glue_MozglueUtils_h

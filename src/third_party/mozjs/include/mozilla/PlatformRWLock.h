/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_PlatformRWLock_h
#define mozilla_PlatformRWLock_h

#include "mozilla/Types.h"

#ifndef XP_WIN
#  include <pthread.h>
#endif

namespace mozilla::detail {

class RWLockImpl {
 public:
  explicit MFBT_API RWLockImpl();
  MFBT_API ~RWLockImpl();

 protected:
  [[nodiscard]] MFBT_API bool tryReadLock();
  MFBT_API void readLock();
  MFBT_API void readUnlock();

  [[nodiscard]] MFBT_API bool tryWriteLock();
  MFBT_API void writeLock();
  MFBT_API void writeUnlock();

 private:
  RWLockImpl(const RWLockImpl&) = delete;
  void operator=(const RWLockImpl&) = delete;
  RWLockImpl(RWLockImpl&&) = delete;
  void operator=(RWLockImpl&&) = delete;
  bool operator==(const RWLockImpl& rhs) = delete;

#ifndef XP_WIN
  pthread_rwlock_t mRWLock;
#else
  // SRWLock is pointer-sized. We declare it in such a fashion here to avoid
  // pulling in windows.h wherever this header is used.
  void* mRWLock;
#endif
};

}  // namespace mozilla::detail

#endif  // mozilla_PlatformRWLock_h

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef XP_WIN
#  error This file should only be compiled on Windows.
#endif

#include "mozilla/PlatformRWLock.h"

#include <windows.h>

#define NativeHandle(m) (reinterpret_cast<SRWLOCK*>(&m))

mozilla::detail::RWLockImpl::RWLockImpl() {
  static_assert(sizeof(SRWLOCK) <= sizeof(mRWLock), "SRWLOCK is too big!");
  InitializeSRWLock(NativeHandle(mRWLock));
}

mozilla::detail::RWLockImpl::~RWLockImpl() {}

bool mozilla::detail::RWLockImpl::tryReadLock() {
  return TryAcquireSRWLockShared(NativeHandle(mRWLock));
}

void mozilla::detail::RWLockImpl::readLock() {
  AcquireSRWLockShared(NativeHandle(mRWLock));
}

void mozilla::detail::RWLockImpl::readUnlock() {
  ReleaseSRWLockShared(NativeHandle(mRWLock));
}

bool mozilla::detail::RWLockImpl::tryWriteLock() {
  return TryAcquireSRWLockExclusive(NativeHandle(mRWLock));
}

void mozilla::detail::RWLockImpl::writeLock() {
  AcquireSRWLockExclusive(NativeHandle(mRWLock));
}

void mozilla::detail::RWLockImpl::writeUnlock() {
  ReleaseSRWLockExclusive(NativeHandle(mRWLock));
}

#undef NativeHandle

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"

#include <errno.h>
#include <stdio.h>

#include "mozilla/PlatformMutex.h"
#include "MutexPlatformData_noop.h"

mozilla::detail::MutexImpl::MutexImpl() {}

mozilla::detail::MutexImpl::~MutexImpl() {}

inline void mozilla::detail::MutexImpl::mutexLock() {}

bool mozilla::detail::MutexImpl::tryLock() { return mutexTryLock(); }

bool mozilla::detail::MutexImpl::mutexTryLock() { return true; }

void mozilla::detail::MutexImpl::lock() { mutexLock(); }

void mozilla::detail::MutexImpl::unlock() {}

mozilla::detail::MutexImpl::PlatformData*
mozilla::detail::MutexImpl::platformData() {
  static_assert(sizeof(platformData_) >= sizeof(PlatformData),
                "platformData_ is too small");
  return reinterpret_cast<PlatformData*>(platformData_);
}

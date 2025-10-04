/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"

#include "mozilla/PlatformConditionVariable.h"
#include "mozilla/PlatformMutex.h"
#include "MutexPlatformData_noop.h"

using mozilla::TimeDuration;

struct mozilla::detail::ConditionVariableImpl::PlatformData {};

mozilla::detail::ConditionVariableImpl::ConditionVariableImpl() {}

mozilla::detail::ConditionVariableImpl::~ConditionVariableImpl() {}

void mozilla::detail::ConditionVariableImpl::notify_one() {}

void mozilla::detail::ConditionVariableImpl::notify_all() {}

void mozilla::detail::ConditionVariableImpl::wait(MutexImpl&) {
  // On WASI, there are no threads, so we never wait (either the condvar must
  // be ready or there is a deadlock).
}

mozilla::CVStatus mozilla::detail::ConditionVariableImpl::wait_for(
    MutexImpl&, const TimeDuration&) {
  return CVStatus::NoTimeout;
}

mozilla::detail::ConditionVariableImpl::PlatformData*
mozilla::detail::ConditionVariableImpl::platformData() {
  static_assert(sizeof platformData_ >= sizeof(PlatformData),
                "platformData_ is too small");
  return reinterpret_cast<PlatformData*>(platformData_);
}

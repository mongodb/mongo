/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "threading/Thread.h"
#include "mozilla/Assertions.h"

namespace js {

Thread::~Thread() { MOZ_RELEASE_ASSERT(!joinable()); }

Thread::Thread(Thread&& aOther) {
  id_ = aOther.id_;
  aOther.id_ = ThreadId();
  options_ = aOther.options_;
}

Thread& Thread::operator=(Thread&& aOther) {
  MOZ_RELEASE_ASSERT(!joinable());
  id_ = aOther.id_;
  aOther.id_ = ThreadId();
  options_ = aOther.options_;
  return *this;
}

ThreadId Thread::get_id() { return id_; }

bool Thread::joinable() { return id_ != ThreadId(); }

}  // namespace js

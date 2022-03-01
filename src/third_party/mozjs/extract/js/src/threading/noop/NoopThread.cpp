/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"

#include "threading/noop/ThreadPlatformData.h"

namespace js {

inline ThreadId::PlatformData* ThreadId::platformData() {
  return reinterpret_cast<PlatformData*>(platformData_);
}

inline const ThreadId::PlatformData* ThreadId::platformData() const {
  return reinterpret_cast<const PlatformData*>(platformData_);
}

ThreadId::ThreadId() {}
ThreadId::operator bool() const { return false; }
bool ThreadId::operator==(const ThreadId& aOther) const { return true; }
bool Thread::create(void* (*aMain)(void*), void* aArg) { return false; }
void Thread::join() {}
void Thread::detach() {}
ThreadId ThreadId::ThisThreadId() { return ThreadId(); }
void ThisThread::SetName(const char*) {}
void ThisThread::GetName(char*, size_t) {}
void ThisThread::SleepMilliseconds(size_t) {
  MOZ_CRASH("There is no any implementation for sleep.");
}

}  // namespace js

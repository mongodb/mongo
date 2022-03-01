/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"

#include <chrono>
#include <thread>

#include "js/Utility.h"
#include "threading/posix/ThreadPlatformData.h"
#include "threading/Thread.h"

namespace js {

inline ThreadId::PlatformData* ThreadId::platformData() {
  static_assert(sizeof platformData_ >= sizeof(PlatformData),
                "platformData_ is too small");
  return reinterpret_cast<PlatformData*>(platformData_);
}

inline const ThreadId::PlatformData* ThreadId::platformData() const {
  static_assert(sizeof platformData_ >= sizeof(PlatformData),
                "platformData_ is too small");
  return reinterpret_cast<const PlatformData*>(platformData_);
}

ThreadId::ThreadId() { platformData()->hasThread = false; }

ThreadId::operator bool() const { return platformData()->hasThread; }

bool ThreadId::operator==(const ThreadId& aOther) const {
  const PlatformData& self = *platformData();
  const PlatformData& other = *aOther.platformData();
  return (!self.hasThread && !other.hasThread) ||
         (self.hasThread == other.hasThread &&
          pthread_equal(self.ptThread, other.ptThread));
}

bool Thread::create(void* (*aMain)(void*), void* aArg) {
  MOZ_RELEASE_ASSERT(!joinable());

  if (oom::ShouldFailWithOOM()) {
    return false;
  }

  pthread_attr_t attrs;
  int r = pthread_attr_init(&attrs);
  MOZ_RELEASE_ASSERT(!r);
  if (options_.stackSize()) {
    r = pthread_attr_setstacksize(&attrs, options_.stackSize());
    MOZ_RELEASE_ASSERT(!r);
  }

  r = pthread_create(&id_.platformData()->ptThread, &attrs, aMain, aArg);
  if (r) {
    // On either Windows or POSIX we can't be sure if id_ was initialised. So
    // reset it manually.
    id_ = ThreadId();
    return false;
  }
  id_.platformData()->hasThread = true;
  return true;
}

void Thread::join() {
  MOZ_RELEASE_ASSERT(joinable());
  int r = pthread_join(id_.platformData()->ptThread, nullptr);
  MOZ_RELEASE_ASSERT(!r);
  id_ = ThreadId();
}

void Thread::detach() {
  MOZ_RELEASE_ASSERT(joinable());
  int r = pthread_detach(id_.platformData()->ptThread);
  MOZ_RELEASE_ASSERT(!r);
  id_ = ThreadId();
}

ThreadId ThreadId::ThisThreadId() {
  ThreadId id;
  id.platformData()->ptThread = pthread_self();
  id.platformData()->hasThread = true;
  MOZ_RELEASE_ASSERT(id != ThreadId());
  return id;
}

void ThisThread::SetName(const char* name) {
  MOZ_RELEASE_ASSERT(name);

#if (defined(__APPLE__) && defined(__MACH__)) || defined(__linux__)
#  if defined(XP_DARWIN)
  // Mac OS X has a length limit of 63 characters, but there is no API
  // exposing it.
#    define SETNAME_LENGTH_CONSTRAINT 63
#  else
  // On linux the name may not be longer than 16 bytes, including
  // the null terminator. Truncate the name to 15 characters.
#    define SETNAME_LENGTH_CONSTRAINT 15
#  endif
  char nameBuf[SETNAME_LENGTH_CONSTRAINT + 1];

  strncpy(nameBuf, name, sizeof nameBuf - 1);
  nameBuf[sizeof nameBuf - 1] = '\0';
  name = nameBuf;
#endif

  int rv;
#ifdef XP_DARWIN
  rv = pthread_setname_np(name);
#elif defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  pthread_set_name_np(pthread_self(), name);
  rv = 0;
#elif defined(__NetBSD__)
  rv = pthread_setname_np(pthread_self(), "%s", (void*)name);
#else
  rv = pthread_setname_np(pthread_self(), name);
#endif
  MOZ_RELEASE_ASSERT(!rv);
}

void ThisThread::GetName(char* nameBuffer, size_t len) {
  MOZ_RELEASE_ASSERT(len >= 16);

  int rv = -1;
#ifdef HAVE_PTHREAD_GETNAME_NP
  rv = pthread_getname_np(pthread_self(), nameBuffer, len);
#elif defined(HAVE_PTHREAD_GET_NAME_NP)
  pthread_get_name_np(pthread_self(), nameBuffer, len);
  rv = 0;
#elif defined(__linux__)
  rv = prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(nameBuffer));
#endif

  if (rv) {
    nameBuffer[0] = '\0';
  }
}

void ThisThread::SleepMilliseconds(size_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace js

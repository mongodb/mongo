/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_posix_PlatformData_h
#define threading_posix_PlatformData_h

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) && defined(__MACH__)
#  include <dlfcn.h>
#endif

#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#  include <pthread_np.h>
#endif

#if defined(__linux__)
#  include <sys/prctl.h>
#endif

#include "threading/Thread.h"

namespace js {

class ThreadId::PlatformData {
  friend class Thread;
  friend class ThreadId;
  pthread_t ptThread;

  // pthread_t does not have a default initializer, so we have to carry a bool
  // to tell whether it is safe to compare or not.
  bool hasThread;
};

}  // namespace js

#endif  // threading_posix_PlatformData_h

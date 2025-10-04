/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MutexPlatformData_posix_h
#define MutexPlatformData_posix_h

#include <pthread.h>

#include "mozilla/PlatformMutex.h"

struct mozilla::detail::MutexImpl::PlatformData {
  pthread_mutex_t ptMutex;
};

#endif  // MutexPlatformData_posix_h

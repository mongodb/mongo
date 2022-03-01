/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_noop_ThreadPlatformData_h
#define threading_noop_ThreadPlatformData_h

#include <stdlib.h>
#include <string.h>

#include "threading/Thread.h"

namespace js {

class ThreadId::PlatformData {
  friend class Thread;
  friend class ThreadId;
};

}  // namespace js

#endif  // threading_noop_ThreadPlatformData_h

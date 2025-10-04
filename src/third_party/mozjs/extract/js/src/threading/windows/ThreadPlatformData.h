/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_windows_PlatformData_h
#define threading_windows_PlatformData_h

#include <assert.h>
#include <new.h>
#include <process.h>

#include "threading/Thread.h"

#include "util/WindowsWrapper.h"

namespace js {

class ThreadId::PlatformData {
  friend class Thread;
  friend class ThreadId;

  HANDLE handle;
  unsigned id;
};

}  // namespace js

#endif  // threading_windows_PlatformData_h

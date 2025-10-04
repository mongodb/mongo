/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "UniquePtrExtensions.h"

#include "mozilla/Assertions.h"
#include "mozilla/DebugOnly.h"

#ifdef XP_WIN
#  include <windows.h>
#else
#  include <errno.h>
#  include <unistd.h>
#endif

namespace mozilla {
namespace detail {

void FileHandleDeleter::operator()(FileHandleHelper aHelper) {
  if (aHelper != nullptr) {
    DebugOnly<bool> ok;
#ifdef XP_WIN
    ok = CloseHandle(aHelper);
#else
    ok = close(aHelper) == 0 || errno == EINTR;
#endif
    MOZ_ASSERT(ok, "failed to close file handle");
  }
}

}  // namespace detail
}  // namespace mozilla

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RuntimeExceptionModule.h"

#include <cstdint>

#include "mozilla/ProcessType.h"

#if defined(XP_WIN)
#  include <windows.h>
#  if defined(__MINGW32__) || defined(__MINGW64__)
// Add missing constants and types for mingw builds
#    define HREPORT HANDLE
#    define PWER_SUBMIT_RESULT WER_SUBMIT_RESULT*
#    define WER_MAX_PREFERRED_MODULES_BUFFER (256)
#  endif               // defined(__MINGW32__) || defined(__MINGW64__)
#  include <werapi.h>  // For WerRegisterRuntimeExceptionModule()
#  include <stdlib.h>

#  include "mozilla/mozalloc_oom.h"
#  include "mozilla/Unused.h"

using mozilla::Unused;
#endif

namespace CrashReporter {

#ifdef XP_WIN

struct InProcessWindowsErrorReportingData {
  uint32_t mProcessType;
  size_t* mOOMAllocationSizePtr;
};

static InProcessWindowsErrorReportingData gInProcessWerData;
const static size_t kModulePathLength = MAX_PATH + 1;
static wchar_t sModulePath[kModulePathLength];

bool GetRuntimeExceptionModulePath(wchar_t* aPath, const size_t aLength) {
  const wchar_t* kModuleName = L"mozwer.dll";
  DWORD res = ::GetModuleFileNameW(nullptr, aPath, aLength);
  if ((res > 0) && (res != aLength)) {
    wchar_t* last_backslash = wcsrchr(aPath, L'\\');
    if (last_backslash) {
      *(last_backslash + 1) = L'\0';
      if (wcscat_s(aPath, aLength, kModuleName) == 0) {
        return true;
      }
    }
  }

  return false;
}

#endif  // XP_WIN

void RegisterRuntimeExceptionModule() {
#ifdef XP_WIN
#  if defined(DEBUG)
  // In debug builds, disable the crash reporter by default, and allow to
  // enable it with the MOZ_CRASHREPORTER environment variable.
  const char* envvar = getenv("MOZ_CRASHREPORTER");
  if (!envvar || !*envvar) {
    return;
  }
#  else
  // In other builds, enable the crash reporter by default, and allow
  // disabling it with the MOZ_CRASHREPORTER_DISABLE environment variable.
  const char* envvar = getenv("MOZ_CRASHREPORTER_DISABLE");
  if (envvar && *envvar) {
    return;
  }
#  endif

  // If sModulePath is set we have already registerd the module.
  if (*sModulePath) {
    return;
  }

  // If we fail to get the path just return.
  if (!GetRuntimeExceptionModulePath(sModulePath, kModulePathLength)) {
    return;
  }

  gInProcessWerData.mProcessType = mozilla::GetGeckoProcessType();
  gInProcessWerData.mOOMAllocationSizePtr = &gOOMAllocationSize;
  if (FAILED(::WerRegisterRuntimeExceptionModule(sModulePath,
                                                 &gInProcessWerData))) {
    // The registration failed null out sModulePath to record this.
    *sModulePath = L'\0';
    return;
  }
#endif  // XP_WIN
}

void UnregisterRuntimeExceptionModule() {
#ifdef XP_WIN
  // If sModulePath is set then we have registered the module.
  if (*sModulePath) {
    Unused << ::WerUnregisterRuntimeExceptionModule(sModulePath,
                                                    &gInProcessWerData);
    *sModulePath = L'\0';
  }
#endif  // XP_WIN
}

}  // namespace CrashReporter

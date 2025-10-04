/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_WindowsEnumProcessModules_h
#define mozilla_WindowsEnumProcessModules_h

#include <windows.h>
#include <psapi.h>

#include "mozilla/FunctionRef.h"
#include "mozilla/NativeNt.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/WinHeaderOnlyUtils.h"

namespace mozilla {

// Why don't we use CreateToolhelp32Snapshot instead of EnumProcessModules?
// CreateToolhelp32Snapshot gets the ANSI versions of module path strings
// via ntdll!RtlQueryProcessDebugInformation and stores them into a snapshot.
// Module32FirstW/Module32NextW re-converts ANSI into Unicode, but it cannot
// restore lost information.   This means we still need GetModuleFileNameEx
// even when we use CreateToolhelp32Snapshot, but EnumProcessModules is faster.
inline bool EnumerateProcessModules(
    const FunctionRef<void(const wchar_t*, HMODULE)>& aCallback) {
  DWORD modulesSize;
  if (!::EnumProcessModules(nt::kCurrentProcess, nullptr, 0, &modulesSize)) {
    return false;
  }

  DWORD modulesNum = modulesSize / sizeof(HMODULE);
  UniquePtr<HMODULE[]> modules = MakeUnique<HMODULE[]>(modulesNum);
  if (!::EnumProcessModules(nt::kCurrentProcess, modules.get(),
                            modulesNum * sizeof(HMODULE), &modulesSize)) {
    return false;
  }

  // The list may have shrunk between calls
  if (modulesSize / sizeof(HMODULE) < modulesNum) {
    modulesNum = modulesSize / sizeof(HMODULE);
  }

  for (DWORD i = 0; i < modulesNum; ++i) {
    UniquePtr<wchar_t[]> modulePath = GetFullModulePath(modules[i]);
    if (!modulePath) {
      continue;
    }

    // Please note that modules[i] could be invalid if the module
    // was unloaded after GetFullModulePath succeeded.
    aCallback(modulePath.get(), modules[i]);
  }

  return true;
}

}  // namespace mozilla

#endif  // mozilla_WindowsEnumProcessModules_h

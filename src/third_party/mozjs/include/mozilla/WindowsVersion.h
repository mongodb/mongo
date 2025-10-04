/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_WindowsVersion_h
#define mozilla_WindowsVersion_h

#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include <stdint.h>
#include <windows.h>

namespace mozilla {

inline bool IsWindows10BuildOrLater(uint32_t aBuild) {
  static Atomic<uint32_t> minBuild(0);
  static Atomic<uint32_t> maxBuild(UINT32_MAX);

  if (minBuild >= aBuild) {
    return true;
  }

  if (aBuild >= maxBuild) {
    return false;
  }

  OSVERSIONINFOEXW info;
  ZeroMemory(&info, sizeof(OSVERSIONINFOEXW));
  info.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);
  info.dwMajorVersion = 10;
  info.dwBuildNumber = aBuild;

  DWORDLONG conditionMask = 0;
  VER_SET_CONDITION(conditionMask, VER_MAJORVERSION, VER_GREATER_EQUAL);
  VER_SET_CONDITION(conditionMask, VER_MINORVERSION, VER_GREATER_EQUAL);
  VER_SET_CONDITION(conditionMask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
  VER_SET_CONDITION(conditionMask, VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
  VER_SET_CONDITION(conditionMask, VER_SERVICEPACKMINOR, VER_GREATER_EQUAL);

  if (VerifyVersionInfoW(&info,
                         VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER |
                             VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR,
                         conditionMask)) {
    minBuild = aBuild;
    return true;
  }

  maxBuild = aBuild;
  return false;
}

MOZ_ALWAYS_INLINE bool IsWin10AnniversaryUpdateOrLater() {
  return IsWindows10BuildOrLater(14393);
}

MOZ_ALWAYS_INLINE bool IsWin10CreatorsUpdateOrLater() {
  return IsWindows10BuildOrLater(15063);
}

MOZ_ALWAYS_INLINE bool IsWin10FallCreatorsUpdateOrLater() {
  return IsWindows10BuildOrLater(16299);
}

MOZ_ALWAYS_INLINE bool IsWin10Sep2018UpdateOrLater() {
  return IsWindows10BuildOrLater(17763);
}

MOZ_ALWAYS_INLINE bool IsWin11OrLater() {
  return IsWindows10BuildOrLater(22000);
}

MOZ_ALWAYS_INLINE bool IsWin1122H2OrLater() {
  return IsWindows10BuildOrLater(22621);
}

}  // namespace mozilla

#endif /* mozilla_WindowsVersion_h */

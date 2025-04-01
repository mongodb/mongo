/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef PreXULSkeletonUI_h_
#define PreXULSkeletonUI_h_

#include <windows.h>
#include "mozilla/EnumSet.h"
#include "mozilla/Maybe.h"
#include "mozilla/Result.h"
#include "mozilla/Types.h"
#include "mozilla/Vector.h"

namespace mozilla {

// These unfortunately need to be kept in sync with the window style and
// extended window style computations in nsWindow. Luckily those styles seem
// to not vary based off of any user settings for the initial toplevel window,
// so we're safe here for now.
static const DWORD kPreXULSkeletonUIWindowStyle =
    WS_CLIPCHILDREN | WS_DLGFRAME | WS_BORDER | WS_MAXIMIZEBOX |
    WS_MINIMIZEBOX | WS_SIZEBOX | WS_SYSMENU;
static const DWORD kPreXULSkeletonUIWindowStyleEx = WS_EX_WINDOWEDGE;

struct CSSPixelSpan {
  double start;
  double end;
};

struct DevPixelSpan {
  int start;
  int end;
};

enum class SkeletonUIDensity { Default, Touch, Compact };

struct SkeletonUISettings {
  uint32_t screenX;
  uint32_t screenY;
  uint32_t width;
  uint32_t height;
  CSSPixelSpan urlbarSpan;
  CSSPixelSpan searchbarSpan;
  double cssToDevPixelScaling;
  Vector<CSSPixelSpan> springs;
  bool maximized;
  bool menubarShown;
  bool bookmarksToolbarShown;
  bool rtlEnabled;
  SkeletonUIDensity uiDensity;
};

enum class ThemeMode : uint32_t { Invalid, Default, Dark, Light };

enum class SkeletonUIFlag : uint8_t {
  MenubarShown,
  BookmarksToolbarShown,
  RtlEnabled,
  TouchDensity,
  CompactDensity,
};

struct ThemeColors {
  uint32_t backgroundColor;
  uint32_t toolbarForegroundColor;
  uint32_t tabBarColor;
  uint32_t tabColor;
  uint32_t tabOutlineColor;
  uint32_t chromeContentDividerColor;
  uint32_t urlbarColor;
  uint32_t urlbarBorderColor;
  uint32_t animationColor;
};

enum class PreXULSkeletonUIError : uint32_t {
  None,
  Disabled,
  EnabledKeyDoesNotExist,
  OOM,
  Cmdline,
  EnvVars,
  FailedToOpenRegistryKey,
  RegistryError,
  FailedLoadingDynamicProcs,
  FailedGettingLock,
  FilesystemFailure,
  NoStartWithLastProfile,
  FailedRegisteringWindowClass,
  CorruptData,
  BadWindowDimensions,
  FailedGettingMonitorInfo,
  CreateWindowFailed,
  FailedGettingDC,
  FailedBlitting,
  FailedFillingBottomRect,
  CrashedOnce,
  BadUIDensity,
  Unknown,
};

inline const wchar_t* GetPreXULSkeletonUIErrorString(
    PreXULSkeletonUIError error) {
  switch (error) {
    case PreXULSkeletonUIError::None:
      return L"None";
    case PreXULSkeletonUIError::Disabled:
      return L"Disabled";
    case PreXULSkeletonUIError::OOM:
      return L"OOM";
    case PreXULSkeletonUIError::Cmdline:
      return L"Cmdline";
    case PreXULSkeletonUIError::EnvVars:
      return L"EnvVars";
    case PreXULSkeletonUIError::FailedToOpenRegistryKey:
      return L"FailedToOpenRegistryKey";
    case PreXULSkeletonUIError::RegistryError:
      return L"RegistryError";
    case PreXULSkeletonUIError::FailedLoadingDynamicProcs:
      return L"FailedLoadingDynamicProcs";
    case PreXULSkeletonUIError::FailedGettingLock:
      return L"FailedGettingLock";
    case PreXULSkeletonUIError::FilesystemFailure:
      return L"FilesystemFailure";
    case PreXULSkeletonUIError::NoStartWithLastProfile:
      return L"NoStartWithLastProfile";
    case PreXULSkeletonUIError::FailedRegisteringWindowClass:
      return L"FailedRegisteringWindowClass";
    case PreXULSkeletonUIError::CorruptData:
      return L"CorruptData";
    case PreXULSkeletonUIError::BadWindowDimensions:
      return L"BadWindowDimensions";
    case PreXULSkeletonUIError::FailedGettingMonitorInfo:
      return L"FailedGettingMonitorInfo";
    case PreXULSkeletonUIError::EnabledKeyDoesNotExist:
      return L"EnabledKeyDoesNotExist";
    case PreXULSkeletonUIError::CreateWindowFailed:
      return L"CreateWindowFailed";
    case PreXULSkeletonUIError::FailedGettingDC:
      return L"FailedGettingDC";
    case PreXULSkeletonUIError::FailedBlitting:
      return L"FailedBlitting";
    case PreXULSkeletonUIError::FailedFillingBottomRect:
      return L"FailedFillingBottomRect";
    case PreXULSkeletonUIError::CrashedOnce:
      return L"CrashedOnce";
    case PreXULSkeletonUIError::BadUIDensity:
      return L"BadUIDensity";
    case PreXULSkeletonUIError::Unknown:
      return L"Unknown";
  }

  MOZ_ASSERT_UNREACHABLE();
  return L"Unknown";
}

enum class PreXULSkeletonUIProgress : uint32_t {
  Started,
  Completed,
};

MFBT_API void CreateAndStorePreXULSkeletonUI(HINSTANCE hInstance, int argc,
                                             char** argv);
MFBT_API void CleanupProcessRuntime();
MFBT_API bool GetPreXULSkeletonUIWasShown();
MFBT_API HWND ConsumePreXULSkeletonUIHandle();
MFBT_API Maybe<PreXULSkeletonUIError> GetPreXULSkeletonUIErrorReason();
MFBT_API bool WasPreXULSkeletonUIMaximized();
MFBT_API Result<Ok, PreXULSkeletonUIError> PersistPreXULSkeletonUIValues(
    const SkeletonUISettings& settings);
MFBT_API bool GetPreXULSkeletonUIEnabled();
MFBT_API Result<Ok, PreXULSkeletonUIError> SetPreXULSkeletonUIEnabledIfAllowed(
    bool value);
MFBT_API void PollPreXULSkeletonUIEvents();
MFBT_API Result<Ok, PreXULSkeletonUIError> SetPreXULSkeletonUIThemeId(
    ThemeMode theme);
MFBT_API Result<Ok, PreXULSkeletonUIError> NotePreXULSkeletonUIRestarting();

}  // namespace mozilla

#endif

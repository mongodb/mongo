/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_MOZGLUE_MISC_WINDOWSDPIINITIALIZATION_H_
#define MOZILLA_MOZGLUE_MISC_WINDOWSDPIINITIALIZATION_H_
#include "mozilla/Types.h"

namespace mozilla {

// The result codes that may be returned from WindowsDpiInitialization()
enum class WindowsDpiInitializationResult : uint32_t {
  Success,
  FindSetProcessDpiAwarenessContextFailed,
  SetProcessDpiAwarenessContextFailed,
  FindSetProcessDpiAwarenessFailed,
  SetProcessDpiAwarenessFailed,
  SetProcessDPIAwareFailed,
};

// Get a string representation of any WindowsDpiInitializationResult value
inline const char* WindowsDpiInitializationResultString(
    WindowsDpiInitializationResult result) {
  switch (result) {
    case WindowsDpiInitializationResult::Success:
      return "Success";
    case WindowsDpiInitializationResult::
        FindSetProcessDpiAwarenessContextFailed:
      return "Failed to find SetProcessDpiAwarenessContext";
    case WindowsDpiInitializationResult::SetProcessDpiAwarenessContextFailed:
      return "SetProcessDpiAwarenessContext failed";
    case WindowsDpiInitializationResult::FindSetProcessDpiAwarenessFailed:
      return "Failed to find SetProcessDpiAwareness";
    case WindowsDpiInitializationResult::SetProcessDpiAwarenessFailed:
      return "SetProcessDpiAwareness failed";
    case WindowsDpiInitializationResult::SetProcessDPIAwareFailed:
      return "SetProcessDPIAware failed";
    default:
      return "Unknown result";
  }
}

// Initialize DPI awareness to the best available for the current OS
// According to MSDN, this will be:
// Per-Monitor V2 for Windows 10 Creators Update (1703) and later
// Per-Monitor V1 for Windows 8.1 and later
// System DPI for Vista and later (we don't support anything older)
// https://docs.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows
MFBT_API WindowsDpiInitializationResult WindowsDpiInitialization();

}  // namespace mozilla

#endif  // MOZILLA_MOZGLUE_MISC_WINDOWSDPIINITIALIZATION_H_

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/WindowsDpiInitialization.h"

#include "mozilla/DynamicallyLinkedFunctionPtr.h"
#include "mozilla/WindowsProcessMitigations.h"
#include "mozilla/WindowsVersion.h"

#include <shellscalingapi.h>
#include <windows.h>

namespace mozilla {

typedef HRESULT(WINAPI* SetProcessDpiAwarenessType)(PROCESS_DPI_AWARENESS);
typedef BOOL(WINAPI* SetProcessDpiAwarenessContextType)(DPI_AWARENESS_CONTEXT);

WindowsDpiInitializationResult WindowsDpiInitialization() {
  // DPI Awareness can't be used in a Win32k Lockdown process, so there's
  // nothing to do
  if (IsWin32kLockedDown()) {
    return WindowsDpiInitializationResult::Success;
  }

  // From MSDN:
  //  SetProcessDpiAwarenessContext() was added in the Win10 Anniversary Update
  //  SetProcessDpiAwareness() was added in Windows 8.1
  //  SetProcessDpiAware() was added in Windows Vista
  //
  //  DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 wasn't added later until
  //  the Creators Update, so if it fails we just fall back to
  //  DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
  if (IsWin10AnniversaryUpdateOrLater()) {
    DynamicallyLinkedFunctionPtr<SetProcessDpiAwarenessContextType>
        setProcessDpiAwarenessContext(L"user32.dll",
                                      "SetProcessDpiAwarenessContext");
    if (!setProcessDpiAwarenessContext) {
      return WindowsDpiInitializationResult::
          FindSetProcessDpiAwarenessContextFailed;
    }

    if (!setProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !setProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
      return WindowsDpiInitializationResult::
          SetProcessDpiAwarenessContextFailed;
    }

    return WindowsDpiInitializationResult::Success;
  } else if (IsWin8Point1OrLater()) {
    DynamicallyLinkedFunctionPtr<SetProcessDpiAwarenessType>
        setProcessDpiAwareness(L"Shcore.dll", "SetProcessDpiAwareness");
    if (!setProcessDpiAwareness) {
      return WindowsDpiInitializationResult::FindSetProcessDpiAwarenessFailed;
    }

    if (FAILED(setProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
      return WindowsDpiInitializationResult::SetProcessDpiAwarenessFailed;
    }

    return WindowsDpiInitializationResult::Success;
  } else {
    if (!SetProcessDPIAware()) {
      return WindowsDpiInitializationResult::SetProcessDPIAwareFailed;
    }

    return WindowsDpiInitializationResult::Success;
  }
}

}  // namespace mozilla

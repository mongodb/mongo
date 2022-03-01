/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_GetKnownFolderPath_h
#define mozilla_GetKnownFolderPath_h

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include "mozilla/glue/Debug.h"
#include "mozilla/UniquePtr.h"

namespace mozilla {

struct LoadedCoTaskMemFreeDeleter {
  void operator()(void* ptr) {
    static decltype(CoTaskMemFree)* coTaskMemFree = nullptr;
    if (!coTaskMemFree) {
      // Just let this get cleaned up when the process is terminated, because
      // we're going to load it anyway elsewhere.
      HMODULE ole32Dll = ::LoadLibraryW(L"ole32");
      if (!ole32Dll) {
        printf_stderr(
            "Could not load ole32 - will not free with CoTaskMemFree");
        return;
      }
      coTaskMemFree = reinterpret_cast<decltype(coTaskMemFree)>(
          ::GetProcAddress(ole32Dll, "CoTaskMemFree"));
      if (!coTaskMemFree) {
        printf_stderr("Could not find CoTaskMemFree");
        return;
      }
    }
    coTaskMemFree(ptr);
  }
};

UniquePtr<wchar_t, LoadedCoTaskMemFreeDeleter> GetKnownFolderPath(
    REFKNOWNFOLDERID folderId);

}  // namespace mozilla

#endif

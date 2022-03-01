/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GetKnownFolderPath.h"

namespace mozilla {

UniquePtr<wchar_t, LoadedCoTaskMemFreeDeleter> GetKnownFolderPath(
    REFKNOWNFOLDERID folderId) {
  static decltype(SHGetKnownFolderPath)* shGetKnownFolderPath = nullptr;
  if (!shGetKnownFolderPath) {
    // We could go out of our way to `FreeLibrary` on this, decrementing its
    // ref count and potentially unloading it. However doing so would be either
    // effectively a no-op, or counterproductive. Just let it get cleaned up
    // when the process is terminated, because we're going to load it anyway
    // elsewhere.
    HMODULE shell32Dll = ::LoadLibraryW(L"shell32");
    if (!shell32Dll) {
      return nullptr;
    }
    shGetKnownFolderPath = reinterpret_cast<decltype(shGetKnownFolderPath)>(
        ::GetProcAddress(shell32Dll, "SHGetKnownFolderPath"));
    if (!shGetKnownFolderPath) {
      return nullptr;
    }
  }
  PWSTR path = nullptr;
  shGetKnownFolderPath(folderId, 0, nullptr, &path);
  return UniquePtr<wchar_t, LoadedCoTaskMemFreeDeleter>(path);
}

}  // namespace mozilla

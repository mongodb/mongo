/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <libloaderapi.h>

BOOL WINAPI DllMain(HINSTANCE aInstDll, DWORD aReason, LPVOID) {
  if (aReason == DLL_PROCESS_ATTACH) {
    ::DisableThreadLibraryCalls(aInstDll);

    // mozglue.dll imports RtlGenRandom from advapi32.dll as SystemFunction036,
    // but the actual function is implemented in cryptbase.dll.  To avoid
    // loading a fake cryptbase.dll from the installation directory, we preload
    // cryptbase.dll from the system directory.
    ::LoadLibraryExW(L"cryptbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  }
  return TRUE;
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_WindowsMapRemoteView_h
#define mozilla_WindowsMapRemoteView_h

#include "mozilla/Types.h"

#include <windows.h>

namespace mozilla {

MFBT_API PVOID MapRemoteViewOfFile(HANDLE aFileMapping, HANDLE aProcess,
                                   ULONG64 aOffset, PVOID aBaseAddress,
                                   SIZE_T aViewSize, ULONG aAllocationType,
                                   ULONG aProtectionFlags);

MFBT_API bool UnmapRemoteViewOfFile(HANDLE aProcess, PVOID aBaseAddress);

}  // namespace mozilla

#endif  // mozilla_WindowsMapRemoteView_h

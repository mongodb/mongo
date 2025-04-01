/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WindowsDpiAwareness_h_
#define WindowsDpiAwareness_h_

#include <windows.h>

#if !defined(DPI_AWARENESS_CONTEXT_DECLARED) && \
    !defined(DPI_AWARENESS_CONTEXT_UNAWARE)

DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);

typedef enum DPI_AWARENESS {
  DPI_AWARENESS_INVALID = -1,
  DPI_AWARENESS_UNAWARE = 0,
  DPI_AWARENESS_SYSTEM_AWARE = 1,
  DPI_AWARENESS_PER_MONITOR_AWARE = 2
} DPI_AWARENESS;

#  define DPI_AWARENESS_CONTEXT_UNAWARE ((DPI_AWARENESS_CONTEXT)-1)
#  define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE ((DPI_AWARENESS_CONTEXT)-2)
#  define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((DPI_AWARENESS_CONTEXT)-3)

#  define DPI_AWARENESS_CONTEXT_DECLARED
#endif  // (DPI_AWARENESS_CONTEXT_DECLARED)

typedef DPI_AWARENESS_CONTEXT(WINAPI* SetThreadDpiAwarenessContextProc)(
    DPI_AWARENESS_CONTEXT);
typedef BOOL(WINAPI* EnableNonClientDpiScalingProc)(HWND);
typedef int(WINAPI* GetSystemMetricsForDpiProc)(int, UINT);

#endif

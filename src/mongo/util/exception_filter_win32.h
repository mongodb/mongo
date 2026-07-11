// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

namespace mongo {

void setWindowsUnhandledExceptionFilter();

/**
 * Controls whether Windows minidump generation is enabled. Called by the
 * win32MinidumpEnabled server parameter's on_update callback to decouple
 * the exception filter (in //src/mongo:base) from the IDL server parameter
 * infrastructure (in //src/mongo/db:windows_options).
 */
void setWin32MinidumpEnabled(bool enabled);

/**
 * on_update callback for the win32MinidumpEnabled server parameter.
 */
Status onUpdateWin32MinidumpEnabled(bool newValue);

#ifdef _WIN32

/**
 * Windows unhandled exception filter
 */
LONG WINAPI exceptionFilter(struct _EXCEPTION_POINTERS* excPointers);

#endif

}  // namespace mongo

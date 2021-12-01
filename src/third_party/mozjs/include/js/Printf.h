/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Printf_h
#define js_Printf_h

#include "mozilla/Printf.h"

#include <stdarg.h>

#include "jstypes.h"
#include "js/Utility.h"

/* Wrappers for mozilla::Smprintf and friends that are used throughout
   JS.  */

extern JS_PUBLIC_API(JS::UniqueChars) JS_smprintf(const char* fmt, ...)
    MOZ_FORMAT_PRINTF(1, 2);

extern JS_PUBLIC_API(JS::UniqueChars) JS_sprintf_append(JS::UniqueChars&& last,
                                                        const char* fmt, ...)
     MOZ_FORMAT_PRINTF(2, 3);

extern JS_PUBLIC_API(JS::UniqueChars) JS_vsmprintf(const char* fmt, va_list ap)
    MOZ_FORMAT_PRINTF(1, 0);
extern JS_PUBLIC_API(JS::UniqueChars) JS_vsprintf_append(JS::UniqueChars&& last,
                                                         const char* fmt, va_list ap)
    MOZ_FORMAT_PRINTF(2, 0);

#endif /* js_Printf_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Portable safe sprintf code.
 *
 * Author: Kipp E.B. Hickman
 */

#include "js/Printf.h"

#include "mozilla/Printf.h"

#include "js/AllocPolicy.h"

using namespace js;

typedef mozilla::SmprintfPolicyPointer<js::SystemAllocPolicy> JSSmprintfPointer;

JS_PUBLIC_API(JS::UniqueChars) JS_smprintf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    JSSmprintfPointer result = mozilla::Vsmprintf<js::SystemAllocPolicy>(fmt, ap);
    va_end(ap);
    return JS::UniqueChars(result.release());
}

JS_PUBLIC_API(JS::UniqueChars) JS_sprintf_append(JS::UniqueChars&& last, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    JSSmprintfPointer lastPtr(last.release());
    JSSmprintfPointer result =
        mozilla::VsmprintfAppend<js::SystemAllocPolicy>(Move(lastPtr), fmt, ap);
    va_end(ap);
    return JS::UniqueChars(result.release());
}

JS_PUBLIC_API(JS::UniqueChars) JS_vsmprintf(const char* fmt, va_list ap)
{
    return JS::UniqueChars(mozilla::Vsmprintf<js::SystemAllocPolicy>(fmt, ap).release());
}

JS_PUBLIC_API(JS::UniqueChars) JS_vsprintf_append(JS::UniqueChars&& last,
                                                  const char* fmt, va_list ap)
{
    JSSmprintfPointer lastPtr(last.release());
    return JS::UniqueChars(mozilla::VsmprintfAppend<js::SystemAllocPolicy>(Move(lastPtr),
                                                                           fmt, ap).release());
}

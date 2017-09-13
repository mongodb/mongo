/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86-shared/AssemblerBuffer-x86-shared.h"

#include "jsopcode.h"

void js::jit::GenericAssembler::spew(const char* fmt, va_list va)
{
    // Buffer to hold the formatted string. Note that this may contain
    // '%' characters, so do not pass it directly to printf functions.
    char buf[200];

    int i = vsnprintf(buf, sizeof(buf), fmt, va);

    if (i > -1) {
        if (printer)
            printer->printf("%s\n", buf);
        js::jit::JitSpew(js::jit::JitSpew_Codegen, "%s", buf);
    }
}

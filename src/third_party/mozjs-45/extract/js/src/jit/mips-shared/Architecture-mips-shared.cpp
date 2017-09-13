/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips-shared/Architecture-mips-shared.h"

#include <fcntl.h>
#include <unistd.h>

#include "jit/RegisterSets.h"

#define HWCAP_MIPS (1 << 31)
#define HWCAP_FPU (1 << 0)

namespace js {
namespace jit {

uint32_t GetMIPSFlags()
{
    static bool isSet = false;
    static uint32_t flags = 0;
    if (isSet)
        return flags;
#if defined(JS_SIMULATOR_MIPS32) || defined(JS_SIMULATOR_MIPS64)
    isSet = true;
    flags |= HWCAP_FPU;
    return flags;
#else

#ifdef __linux__
    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (!fp)
        return false;

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    fread(buf, sizeof(char), sizeof(buf) - 1, fp);
    fclose(fp);
    if (strstr(buf, "FPU"))
        flags |= HWCAP_FPU;

    isSet = true;
    return flags;
#endif

    return flags;
#endif // JS_SIMULATOR_MIPS32 || JS_SIMULATOR_MIPS64
}

bool hasFPU()
{
    return js::jit::GetMIPSFlags() & HWCAP_FPU;
}

Registers::Code
Registers::FromName(const char* name)
{
    for (size_t i = 0; i < Total; i++) {
        if (strcmp(GetName(i), name) == 0)
            return Code(i);
    }

    return Invalid;
}

} // namespace ion
} // namespace js


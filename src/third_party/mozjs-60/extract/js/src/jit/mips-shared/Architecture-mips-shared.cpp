/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips-shared/Architecture-mips-shared.h"

#include <fcntl.h>
#include <unistd.h>

#include "jit/RegisterSets.h"

#define HWCAP_MIPS (1 << 28)
#define HWCAP_LOONGSON (1 << 27)
#define HWCAP_R2 (1 << 26)
#define HWCAP_FPU (1 << 0)

namespace js {
namespace jit {

static uint32_t
get_mips_flags()
{
    uint32_t flags = HWCAP_MIPS;

#if defined(JS_SIMULATOR_MIPS32) || defined(JS_SIMULATOR_MIPS64)
    flags |= HWCAP_FPU;
    flags |= HWCAP_R2;
#else
# ifdef __linux__
    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (!fp)
        return flags;

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    (void)fread(buf, sizeof(char), sizeof(buf) - 1, fp);
    fclose(fp);
    if (strstr(buf, "FPU"))
        flags |= HWCAP_FPU;
    if (strstr(buf, "Loongson"))
        flags |= HWCAP_LOONGSON;
    if (strstr(buf, "mips32r2") || strstr(buf, "mips64r2"))
        flags |= HWCAP_R2;
# endif
#endif // JS_SIMULATOR_MIPS32 || JS_SIMULATOR_MIPS64
    return flags;
}

static bool check_fpu()
{
    return mips_private::Flags & HWCAP_FPU;
}

static bool check_loongson()
{
    return mips_private::Flags & HWCAP_LOONGSON;
}

static bool check_r2()
{
    return mips_private::Flags & HWCAP_R2;
}

namespace mips_private {
    // Cache a local copy so we only have to read /proc/cpuinfo once.
    uint32_t Flags = get_mips_flags();
    bool hasFPU = check_fpu();;
    bool isLoongson = check_loongson();
    bool hasR2 = check_r2();
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


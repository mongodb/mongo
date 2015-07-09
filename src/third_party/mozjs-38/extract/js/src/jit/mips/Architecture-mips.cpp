/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips/Architecture-mips.h"

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
#ifdef JS_MIPS_SIMULATOR
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

    return false;
#endif // JS_MIPS_SIMULATOR
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

FloatRegisters::Code
FloatRegisters::FromName(const char* name)
{
    for (size_t i = 0; i < Total; i++) {
        if (strcmp(GetName(i), name) == 0)
            return Code(i);
    }

    return Invalid;
}

FloatRegister
FloatRegister::doubleOverlay(unsigned int which) const
{
    MOZ_ASSERT(!isInvalid());
    if (kind_ != Double)
        return FloatRegister(code_ & ~1, Double);
    return *this;
}

FloatRegister
FloatRegister::singleOverlay(unsigned int which) const
{
    MOZ_ASSERT(!isInvalid());
    if (kind_ == Double) {
        // Only even registers are double
        MOZ_ASSERT(code_ % 2 == 0);
        MOZ_ASSERT(which < 2);
        return FloatRegister(code_ + which, Single);
    }
    MOZ_ASSERT(which == 0);
    return FloatRegister(code_, Single);
}

FloatRegisterSet
FloatRegister::ReduceSetForPush(const FloatRegisterSet& s)
{
    FloatRegisterSet mod;
    for (TypedRegisterIterator<FloatRegister> iter(s); iter.more(); iter++) {
        if ((*iter).isSingle()) {
            // Even for single size registers save complete double register.
            mod.addUnchecked((*iter).doubleOverlay());
        } else {
            mod.addUnchecked(*iter);
        }
    }
    return mod;
}

uint32_t
FloatRegister::GetSizeInBytes(const FloatRegisterSet& s)
{
    uint64_t bits = s.bits();
    uint32_t ret = mozilla::CountPopulation32(bits & 0xffffffff) * sizeof(float);
    ret +=  mozilla::CountPopulation32(bits >> 32) * sizeof(double);
    return ret;
}
uint32_t
FloatRegister::GetPushSizeInBytes(const FloatRegisterSet& s)
{
    FloatRegisterSet ss = s.reduceSetForPush();
    uint64_t bits = ss.bits();
    // We are only pushing double registers.
    MOZ_ASSERT((bits & 0xffffffff) == 0);
    uint32_t ret =  mozilla::CountPopulation32(bits >> 32) * sizeof(double);
    return ret;
}
uint32_t
FloatRegister::getRegisterDumpOffsetInBytes()
{
    if (isSingle())
        return id() * sizeof(float);
    if (isDouble())
        return id() * sizeof(double);
    MOZ_CRASH();
}

} // namespace ion
} // namespace js


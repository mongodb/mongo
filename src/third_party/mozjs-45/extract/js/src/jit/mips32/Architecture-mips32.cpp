/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips32/Architecture-mips32.h"

#include "jit/RegisterSets.h"

namespace js {
namespace jit {

const char * const Registers::RegNames[] = { "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
                                             "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                             "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                                             "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra" };

const uint32_t Allocatable = 14;

const Registers::SetType Registers::ArgRegMask = Registers::SharedArgRegMask;

const Registers::SetType Registers::JSCallMask =
    (1 << Registers::a2) |
    (1 << Registers::a3);

const Registers::SetType Registers::CallMask =
    (1 << Registers::v0) |
    (1 << Registers::v1);  // used for double-size returns

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
    LiveFloatRegisterSet mod;
    for (FloatRegisterIterator iter(s); iter.more(); iter++) {
        if ((*iter).isSingle()) {
            // Even for single size registers save complete double register.
            mod.addUnchecked((*iter).doubleOverlay());
        } else {
            mod.addUnchecked(*iter);
        }
    }
    return mod.set();
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


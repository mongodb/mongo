/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86-shared/Architecture-x86-shared.h"
#if !defined(JS_CODEGEN_X86) && !defined(JS_CODEGEN_X64)
# error "Wrong architecture. Only x86 and x64 should build this file!"
#endif

#include "jit/RegisterSets.h"

const char*
js::jit::FloatRegister::name() const {
    static const char* const names[] = {

#ifdef JS_CODEGEN_X64
#define FLOAT_REGS_(TYPE) \
        "%xmm0" TYPE, "%xmm1" TYPE, "%xmm2" TYPE, "%xmm3" TYPE, \
        "%xmm4" TYPE, "%xmm5" TYPE, "%xmm6" TYPE, "%xmm7" TYPE, \
        "%xmm8" TYPE, "%xmm9" TYPE, "%xmm10" TYPE, "%xmm11" TYPE, \
        "%xmm12" TYPE, "%xmm13" TYPE, "%xmm14" TYPE, "%xmm15" TYPE
#else
#define FLOAT_REGS_(TYPE) \
        "%xmm0" TYPE, "%xmm1" TYPE, "%xmm2" TYPE, "%xmm3" TYPE, \
        "%xmm4" TYPE, "%xmm5" TYPE, "%xmm6" TYPE, "%xmm7" TYPE
#endif

        // These should be enumerated in the same order as in
        // FloatRegisters::ContentType.
        FLOAT_REGS_(".s"),
        FLOAT_REGS_(".d"),
        FLOAT_REGS_(".i4"),
        FLOAT_REGS_(".s4")
#undef FLOAT_REGS_

    };
    MOZ_ASSERT(size_t(code()) < mozilla::ArrayLength(names));
    return names[size_t(code())];
}

js::jit::FloatRegisterSet
js::jit::FloatRegister::ReduceSetForPush(const FloatRegisterSet& s)
{
    SetType bits = s.bits();

    // Ignore all SIMD register, if not supported.
    if (!JitSupportsSimd())
        bits &= Codes::AllPhysMask * Codes::SpreadScalar;

    // Exclude registers which are already pushed with a larger type. High bits
    // are associated with larger register types. Thus we keep the set of
    // registers which are not included in larger type.
    bits &= ~(bits >> (1 * Codes::TotalPhys));
    bits &= ~(bits >> (2 * Codes::TotalPhys));
    bits &= ~(bits >> (3 * Codes::TotalPhys));

    return FloatRegisterSet(bits);
}

uint32_t
js::jit::FloatRegister::GetPushSizeInBytes(const FloatRegisterSet& s)
{
    SetType all = s.bits();
    SetType set128b =
        (all >> (uint32_t(Codes::Simd128) * Codes::TotalPhys)) & Codes::AllPhysMask;
    SetType doubleSet =
        (all >> (uint32_t(Codes::Double) * Codes::TotalPhys)) & Codes::AllPhysMask;
    SetType singleSet =
        (all >> (uint32_t(Codes::Single) * Codes::TotalPhys)) & Codes::AllPhysMask;

    // PushRegsInMask pushes the largest register first, and thus avoids pushing
    // aliased registers. So we have to filter out the physical registers which
    // are already pushed as part of larger registers.
    SetType set64b = doubleSet & ~set128b;
    SetType set32b = singleSet & ~set64b  & ~set128b;

    static_assert(Codes::AllPhysMask <= 0xffff, "We can safely use CountPopulation32");
    uint32_t count32b = mozilla::CountPopulation32(set32b);

#if defined(JS_CODEGEN_X64)
    // If we have an odd number of 32 bits values, then we increase the size to
    // keep the stack aligned on 8 bytes. Note: Keep in sync with
    // PushRegsInMask, and PopRegsInMaskIgnore.
    count32b += count32b & 1;
#endif

    return mozilla::CountPopulation32(set128b) * (4 * sizeof(int32_t))
        + mozilla::CountPopulation32(set64b) * sizeof(double)
        + count32b * sizeof(float);
}
uint32_t
js::jit::FloatRegister::getRegisterDumpOffsetInBytes()
{
    return uint32_t(encoding()) * sizeof(FloatRegisters::RegisterContent);
}

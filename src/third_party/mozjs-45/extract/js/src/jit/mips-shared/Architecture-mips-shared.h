/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_Architecture_mips_shared_h
#define jit_mips_shared_Architecture_mips_shared_h

#include "mozilla/MathAlgorithms.h"

#include <limits.h>
#include <stdint.h>

#include "js/Utility.h"

// gcc appears to use _mips_hard_float to denote
// that the target is a hard-float target.
#ifdef _mips_hard_float
#define JS_CODEGEN_MIPS_HARDFP
#endif

#if (defined(_MIPS_SIM) && (_MIPS_SIM == _ABIO32)) || defined(JS_SIMULATOR_MIPS32)
#define USES_O32_ABI
#elif (defined(_MIPS_SIM) && (_MIPS_SIM == _ABI64)) || defined(JS_SIMULATOR_MIPS64)
#define USES_N64_ABI
#else
#error "Unsupported ABI"
#endif

namespace js {
namespace jit {

class Registers
{
  public:
    enum RegisterID {
        r0 = 0,
        r1,
        r2,
        r3,
        r4,
        r5,
        r6,
        r7,
        r8,
        r9,
        r10,
        r11,
        r12,
        r13,
        r14,
        r15,
        r16,
        r17,
        r18,
        r19,
        r20,
        r21,
        r22,
        r23,
        r24,
        r25,
        r26,
        r27,
        r28,
        r29,
        r30,
        r31,
        zero = r0,
        at = r1,
        v0 = r2,
        v1 = r3,
        a0 = r4,
        a1 = r5,
        a2 = r6,
        a3 = r7,
#if defined(USES_O32_ABI)
        t0 = r8,
        t1 = r9,
        t2 = r10,
        t3 = r11,
        t4 = r12,
        t5 = r13,
        t6 = r14,
        t7 = r15,
        ta0 = t4,
        ta1 = t5,
        ta2 = t6,
        ta3 = t7,
#elif defined(USES_N64_ABI)
        a4 = r8,
        a5 = r9,
        a6 = r10,
        a7 = r11,
        t0 = r12,
        t1 = r13,
        t2 = r14,
        t3 = r15,
        ta0 = a4,
        ta1 = a5,
        ta2 = a6,
        ta3 = a7,
#endif
        s0 = r16,
        s1 = r17,
        s2 = r18,
        s3 = r19,
        s4 = r20,
        s5 = r21,
        s6 = r22,
        s7 = r23,
        t8 = r24,
        t9 = r25,
        k0 = r26,
        k1 = r27,
        gp = r28,
        sp = r29,
        fp = r30,
        ra = r31,
        invalid_reg
    };
    typedef uint8_t Code;
    typedef RegisterID Encoding;

    // Content spilled during bailouts.
    union RegisterContent {
        uintptr_t r;
    };

    static const char * const RegNames[];
    static const char* GetName(Code code) {
        MOZ_ASSERT(code < Total);
        return RegNames[code];
    }
    static const char* GetName(Encoding i) {
        return GetName(Code(i));
    }

    static Code FromName(const char* name);

    static const Encoding StackPointer = sp;
    static const Encoding Invalid = invalid_reg;

    static const uint32_t Total = 32;
    static const uint32_t Allocatable;

    typedef uint32_t SetType;
    static const SetType AllMask = 0xffffffff;
    static const SetType SharedArgRegMask = (1 << a0) | (1 << a1) | (1 << a2) | (1 << a3);
    static const SetType ArgRegMask;

    static const SetType VolatileMask =
        (1 << Registers::v0) |
        (1 << Registers::v1) |
        (1 << Registers::a0) |
        (1 << Registers::a1) |
        (1 << Registers::a2) |
        (1 << Registers::a3) |
        (1 << Registers::t0) |
        (1 << Registers::t1) |
        (1 << Registers::t2) |
        (1 << Registers::t3) |
        (1 << Registers::ta0) |
        (1 << Registers::ta1) |
        (1 << Registers::ta2) |
        (1 << Registers::ta3);

    // We use this constant to save registers when entering functions. This
    // is why $ra is added here even though it is not "Non Volatile".
    static const SetType NonVolatileMask =
        (1 << Registers::s0) |
        (1 << Registers::s1) |
        (1 << Registers::s2) |
        (1 << Registers::s3) |
        (1 << Registers::s4) |
        (1 << Registers::s5) |
        (1 << Registers::s6) |
        (1 << Registers::s7) |
        (1 << Registers::ra);

    static const SetType WrapperMask =
        VolatileMask |         // = arguments
        (1 << Registers::t0) | // = outReg
        (1 << Registers::t1);  // = argBase

    static const SetType NonAllocatableMask =
        (1 << Registers::zero) |
        (1 << Registers::at) | // at = scratch
        (1 << Registers::t8) | // t8 = scratch
        (1 << Registers::t9) | // t9 = scratch
        (1 << Registers::k0) |
        (1 << Registers::k1) |
        (1 << Registers::gp) |
        (1 << Registers::sp) |
        (1 << Registers::fp) |
        (1 << Registers::ra);

    // Registers that can be allocated without being saved, generally.
    static const SetType TempMask = VolatileMask & ~NonAllocatableMask;

    // Registers returned from a JS -> JS call.
    static const SetType JSCallMask;

    // Registers returned from a JS -> C call.
    static const SetType SharedCallMask = (1 << Registers::v0);
    static const SetType CallMask;

    static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;

    static uint32_t SetSize(SetType x) {
        static_assert(sizeof(SetType) == 4, "SetType must be 32 bits");
        return mozilla::CountPopulation32(x);
    }
    static uint32_t FirstBit(SetType x) {
        return mozilla::CountTrailingZeroes32(x);
    }
    static uint32_t LastBit(SetType x) {
        return 31 - mozilla::CountLeadingZeroes32(x);
    }
};

// Smallest integer type that can hold a register bitmask.
typedef uint32_t PackedRegisterMask;

class FloatRegistersMIPSShared
{
  public:
    enum FPRegisterID {
        f0 = 0,
        f1,
        f2,
        f3,
        f4,
        f5,
        f6,
        f7,
        f8,
        f9,
        f10,
        f11,
        f12,
        f13,
        f14,
        f15,
        f16,
        f17,
        f18,
        f19,
        f20,
        f21,
        f22,
        f23,
        f24,
        f25,
        f26,
        f27,
        f28,
        f29,
        f30,
        f31,
        invalid_freg
    };
    typedef FPRegisterID Code;
    typedef FPRegisterID Encoding;

    // Content spilled during bailouts.
    union RegisterContent {
        double d;
    };

    static const char* GetName(Code code) {
        static const char * const Names[] = { "f0", "f1", "f2", "f3",  "f4", "f5",  "f6", "f7",
                                              "f8", "f9",  "f10", "f11", "f12", "f13",
                                              "f14", "f15", "f16", "f17", "f18", "f19",
                                              "f20", "f21", "f22", "f23", "f24", "f25",
                                              "f26", "f27", "f28", "f29", "f30", "f31"};
        return Names[code];
    }

    static const Code Invalid = invalid_freg;

    typedef uint64_t SetType;
};

template <typename T>
class TypedRegisterSet;

class FloatRegisterMIPSShared
{
  public:
    bool isSimd128() const { return false; }

    typedef FloatRegistersMIPSShared::SetType SetType;

    static uint32_t SetSize(SetType x) {
        static_assert(sizeof(SetType) == 8, "SetType must be 64 bits");
        return mozilla::CountPopulation32(x);
    }
    static uint32_t FirstBit(SetType x) {
        return mozilla::CountTrailingZeroes64(x);
    }
    static uint32_t LastBit(SetType x) {
        return 63 - mozilla::CountLeadingZeroes64(x);
    }
};

uint32_t GetMIPSFlags();
bool hasFPU();

// MIPS doesn't have double registers that can NOT be treated as float32.
inline bool
hasUnaliasedDouble() {
    return false;
}

// On MIPS, fn-double aliases both fn-float32 and fn+1-float32, so if you need
// to convert a float32 to a double as a temporary, you need a temporary
// double register.
inline bool
hasMultiAlias() {
    return true;
}

// See the comments above AsmJSMappedSize in AsmJSValidate.h for more info.
// TODO: Implement this for MIPS. Note that it requires Codegen to respect the
// offset field of AsmJSHeapAccess.
static const size_t AsmJSCheckedImmediateRange = 0;
static const size_t AsmJSImmediateRange = 0;

} // namespace jit
} // namespace js

#endif /* jit_mips_shared_Architecture_mips_shared_h */

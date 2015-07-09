/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_Architecture_x64_h
#define jit_x64_Architecture_x64_h

#include "jit/shared/Constants-x86-shared.h"

namespace js {
namespace jit {

// In bytes: slots needed for potential memory->memory move spills.
//   +8 for cycles
//   +8 for gpr spills
//   +8 for double spills
static const uint32_t ION_FRAME_SLACK_SIZE     = 24;

#ifdef _WIN64
static const uint32_t ShadowStackSpace = 32;
#else
static const uint32_t ShadowStackSpace = 0;
#endif

class Registers {
  public:
    typedef X86Encoding::RegisterID Code;
    typedef uint32_t SetType;
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
    static const char* GetName(Code code) {
        static const char * const Names[] = { "rax", "rcx", "rdx", "rbx",
                                              "rsp", "rbp", "rsi", "rdi",
                                              "r8",  "r9",  "r10", "r11",
                                              "r12", "r13", "r14", "r15" };
        return Names[code];
    }

    static Code FromName(const char* name) {
        for (size_t i = 0; i < Total; i++) {
            if (strcmp(GetName(Code(i)), name) == 0)
                return Code(i);
        }
        return Invalid;
    }

    static const Code StackPointer = X86Encoding::rsp;
    static const Code Invalid = X86Encoding::invalid_reg;

    static const uint32_t Total = 16;
    static const uint32_t TotalPhys = 16;
    static const uint32_t Allocatable = 14;

    static const uint32_t AllMask = (1 << Total) - 1;

    static const uint32_t ArgRegMask =
# if !defined(_WIN64)
        (1 << X86Encoding::rdi) |
        (1 << X86Encoding::rsi) |
# endif
        (1 << X86Encoding::rdx) |
        (1 << X86Encoding::rcx) |
        (1 << X86Encoding::r8) |
        (1 << X86Encoding::r9);

    static const uint32_t VolatileMask =
        (1 << X86Encoding::rax) |
        (1 << X86Encoding::rcx) |
        (1 << X86Encoding::rdx) |
# if !defined(_WIN64)
        (1 << X86Encoding::rsi) |
        (1 << X86Encoding::rdi) |
# endif
        (1 << X86Encoding::r8) |
        (1 << X86Encoding::r9) |
        (1 << X86Encoding::r10) |
        (1 << X86Encoding::r11);

    static const uint32_t NonVolatileMask =
        (1 << X86Encoding::rbx) |
#if defined(_WIN64)
        (1 << X86Encoding::rsi) |
        (1 << X86Encoding::rdi) |
#endif
        (1 << X86Encoding::rbp) |
        (1 << X86Encoding::r12) |
        (1 << X86Encoding::r13) |
        (1 << X86Encoding::r14) |
        (1 << X86Encoding::r15);

    static const uint32_t WrapperMask = VolatileMask;

    static const uint32_t SingleByteRegs = VolatileMask | NonVolatileMask;

    static const uint32_t NonAllocatableMask =
        (1 << X86Encoding::rsp) |
        (1 << X86Encoding::r11);      // This is ScratchReg.

    static const uint32_t AllocatableMask = AllMask & ~NonAllocatableMask;

    // Registers that can be allocated without being saved, generally.
    static const uint32_t TempMask = VolatileMask & ~NonAllocatableMask;

    // Registers returned from a JS -> JS call.
    static const uint32_t JSCallMask =
        (1 << X86Encoding::rcx);

    // Registers returned from a JS -> C call.
    static const uint32_t CallMask =
        (1 << X86Encoding::rax);
};

// Smallest integer type that can hold a register bitmask.
typedef uint16_t PackedRegisterMask;

class FloatRegisters {
  public:
    typedef X86Encoding::XMMRegisterID Code;
    typedef uint32_t SetType;
    static const char* GetName(Code code) {
        return X86Encoding::XMMRegName(code);
    }

    static Code FromName(const char* name) {
        for (size_t i = 0; i < Total; i++) {
            if (strcmp(GetName(Code(i)), name) == 0)
                return Code(i);
        }
        return Invalid;
    }

    static const Code Invalid = X86Encoding::invalid_xmm;

    static const uint32_t Total = 16;
    static const uint32_t TotalPhys = 16;

    static const uint32_t Allocatable = 15;

    static const uint32_t AllMask = (1 << Total) - 1;
    static const uint32_t AllDoubleMask = AllMask;
    static const uint32_t VolatileMask =
#if defined(_WIN64)
        (1 << X86Encoding::xmm0) |
        (1 << X86Encoding::xmm1) |
        (1 << X86Encoding::xmm2) |
        (1 << X86Encoding::xmm3) |
        (1 << X86Encoding::xmm4) |
        (1 << X86Encoding::xmm5);
#else
        AllMask;
#endif

    static const uint32_t NonVolatileMask = AllMask & ~VolatileMask;

    static const uint32_t WrapperMask = VolatileMask;

    static const uint32_t NonAllocatableMask =
        (1 << X86Encoding::xmm15);    // This is ScratchDoubleReg.

    static const uint32_t AllocatableMask = AllMask & ~NonAllocatableMask;
};

template <typename T>
class TypedRegisterSet;

struct FloatRegister {
    typedef FloatRegisters Codes;
    typedef Codes::Code Code;
    typedef Codes::SetType SetType;
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
    Code code_;

    static FloatRegister FromCode(uint32_t i) {
        MOZ_ASSERT(i < FloatRegisters::Total);
        FloatRegister r = { (FloatRegisters::Code)i };
        return r;
    }
    Code code() const {
        MOZ_ASSERT((uint32_t)code_ < FloatRegisters::Total);
        return code_;
    }
    const char* name() const {
        return FloatRegisters::GetName(code());
    }
    bool volatile_() const {
        return !!((1 << code()) & FloatRegisters::VolatileMask);
    }
    bool operator !=(FloatRegister other) const {
        return other.code_ != code_;
    }
    bool operator ==(FloatRegister other) const {
        return other.code_ == code_;
    }
    bool aliases(FloatRegister other) const {
        return other.code_ == code_;
    }
    uint32_t numAliased() const {
        return 1;
    }
    // N.B. FloatRegister is an explicit outparam here because msvc-2010
    // miscompiled it on win64 when the value was simply returned
    void aliased(uint32_t aliasIdx, FloatRegister* ret) {
        MOZ_ASSERT(aliasIdx == 0);
        *ret = *this;
    }
    // This function mostly exists for the ARM backend.  It is to ensure that two
    // floating point registers' types are equivalent.  e.g. S0 is not equivalent
    // to D16, since S0 holds a float32, and D16 holds a Double.
    // Since all floating point registers on x86 and x64 are equivalent, it is
    // reasonable for this function to do the same.
    bool equiv(FloatRegister other) const {
        return true;
    }
    uint32_t size() const {
        return sizeof(double);
    }
    uint32_t numAlignedAliased() const {
        return 1;
    }
    void alignedAliased(uint32_t aliasIdx, FloatRegister* ret) {
        MOZ_ASSERT(aliasIdx == 0);
        *ret = *this;
    }
    static TypedRegisterSet<FloatRegister> ReduceSetForPush(const TypedRegisterSet<FloatRegister>& s);
    static uint32_t GetSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
    static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
    uint32_t getRegisterDumpOffsetInBytes();
};

// Arm/D32 has double registers that can NOT be treated as float32
// and this requires some dances in lowering.
inline bool
hasUnaliasedDouble()
{
    return false;
}

// On ARM, Dn aliases both S2n and S2n+1, so if you need to convert a float32
// to a double as a temporary, you need a temporary double register.
inline bool
hasMultiAlias()
{
    return false;
}

} // namespace jit
} // namespace js

#endif /* jit_x64_Architecture_x64_h */

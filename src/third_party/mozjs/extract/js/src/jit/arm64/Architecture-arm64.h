/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_Architecture_arm64_h
#define jit_arm64_Architecture_arm64_h

#include "mozilla/Assertions.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/arm64/vixl/Instructions-vixl.h"
#include "jit/shared/Architecture-shared.h"

#include "js/Utility.h"

#define JS_HAS_HIDDEN_SP
static const uint32_t HiddenSPEncoding = vixl::kSPRegInternalCode;

namespace js {
namespace jit {

// AArch64 has 32 64-bit integer registers, x0 though x31.
//  x31 is special and functions as both the stack pointer and a zero register.
//  The bottom 32 bits of each of the X registers is accessible as w0 through w31.
//  The program counter is no longer accessible as a register.
// SIMD and scalar floating-point registers share a register bank.
//  32 bit float registers are s0 through s31.
//  64 bit double registers are d0 through d31.
//  128 bit SIMD registers are v0 through v31.
// e.g., s0 is the bottom 32 bits of d0, which is the bottom 64 bits of v0.

// AArch64 Calling Convention:
//  x0 - x7: arguments and return value
//  x8: indirect result (struct) location
//  x9 - x15: temporary registers
//  x16 - x17: intra-call-use registers (PLT, linker)
//  x18: platform specific use (TLS)
//  x19 - x28: callee-saved registers
//  x29: frame pointer
//  x30: link register

// AArch64 Calling Convention for Floats:
//  d0 - d7: arguments and return value
//  d8 - d15: callee-saved registers
//   Bits 64:128 are not saved for v8-v15.
//  d16 - d31: temporary registers

// AArch64 does not have soft float.

class Registers {
  public:
    enum RegisterID {
        w0  =  0, x0  =  0,
        w1  =  1, x1  =  1,
        w2  =  2, x2  =  2,
        w3  =  3, x3  =  3,
        w4  =  4, x4  =  4,
        w5  =  5, x5  =  5,
        w6  =  6, x6  =  6,
        w7  =  7, x7  =  7,
        w8  =  8, x8  =  8,
        w9  =  9, x9  =  9,
        w10 = 10, x10 = 10,
        w11 = 11, x11 = 11,
        w12 = 12, x12 = 12,
        w13 = 13, x13 = 13,
        w14 = 14, x14 = 14,
        w15 = 15, x15 = 15,
        w16 = 16, x16 = 16, ip0 = 16, // MacroAssembler scratch register 1.
        w17 = 17, x17 = 17, ip1 = 17, // MacroAssembler scratch register 2.
        w18 = 18, x18 = 18, tls = 18, // Platform-specific use (TLS).
        w19 = 19, x19 = 19,
        w20 = 20, x20 = 20,
        w21 = 21, x21 = 21,
        w22 = 22, x22 = 22,
        w23 = 23, x23 = 23,
        w24 = 24, x24 = 24,
        w25 = 25, x25 = 25,
        w26 = 26, x26 = 26,
        w27 = 27, x27 = 27,
        w28 = 28, x28 = 28,
        w29 = 29, x29 = 29, fp = 29,
        w30 = 30, x30 = 30, lr = 30,
        w31 = 31, x31 = 31, wzr = 31, xzr = 31, sp = 31, // Special: both stack pointer and a zero register.
        invalid_reg
    };
    typedef uint8_t Code;
    typedef uint32_t Encoding;
    typedef uint32_t SetType;

    union RegisterContent {
        uintptr_t r;
    };

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
        static const char* const Names[] =
            { "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",
              "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19",
              "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29",
              "lr", "sp", "invalid" };
        return Names[code];
    }
    static const char* GetName(uint32_t i) {
        MOZ_ASSERT(i < Total);
        return GetName(Code(i));
    }

    static Code FromName(const char* name);

    // If SP is used as the base register for a memory load or store, then the value
    // of the stack pointer prior to adding any offset must be quadword (16 byte) aligned,
    // or else a stack aligment exception will be generated.
    static const Code StackPointer = sp;

    static const Code Invalid = invalid_reg;

    static const uint32_t Total = 32;
    static const uint32_t TotalPhys = 32;
    static const uint32_t Allocatable = 27; // No named special-function registers.

    static const SetType AllMask = 0xFFFFFFFF;

    static const SetType ArgRegMask =
        (1 << Registers::x0) | (1 << Registers::x1) |
        (1 << Registers::x2) | (1 << Registers::x3) |
        (1 << Registers::x4) | (1 << Registers::x5) |
        (1 << Registers::x6) | (1 << Registers::x7) |
        (1 << Registers::x8);

    static const SetType VolatileMask =
        (1 << Registers::x0) | (1 << Registers::x1) |
        (1 << Registers::x2) | (1 << Registers::x3) |
        (1 << Registers::x4) | (1 << Registers::x5) |
        (1 << Registers::x6) | (1 << Registers::x7) |
        (1 << Registers::x8) | (1 << Registers::x9) |
        (1 << Registers::x10) | (1 << Registers::x11) |
        (1 << Registers::x11) | (1 << Registers::x12) |
        (1 << Registers::x13) | (1 << Registers::x14) |
        (1 << Registers::x14) | (1 << Registers::x15) |
        (1 << Registers::x16) | (1 << Registers::x17) |
        (1 << Registers::x18);

    static const SetType NonVolatileMask =
        (1 << Registers::x19) | (1 << Registers::x20) |
        (1 << Registers::x21) | (1 << Registers::x22) |
        (1 << Registers::x23) | (1 << Registers::x24) |
        (1 << Registers::x25) | (1 << Registers::x26) |
        (1 << Registers::x27) | (1 << Registers::x28) |
        (1 << Registers::x29) | (1 << Registers::x30);

    static const SetType SingleByteRegs = VolatileMask | NonVolatileMask;

    static const SetType NonAllocatableMask =
        (1 << Registers::x28) | // PseudoStackPointer.
        (1 << Registers::ip0) | // First scratch register.
        (1 << Registers::ip1) | // Second scratch register.
        (1 << Registers::tls) |
        (1 << Registers::lr) |
        (1 << Registers::sp);

    static const SetType WrapperMask = VolatileMask;

    // Registers returned from a JS -> JS call.
    static const SetType JSCallMask = (1 << Registers::x2);

    // Registers returned from a JS -> C call.
    static const SetType CallMask = (1 << Registers::x0);

    static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

// Smallest integer type that can hold a register bitmask.
typedef uint32_t PackedRegisterMask;

template <typename T>
class TypedRegisterSet;

class FloatRegisters
{
  public:
    enum FPRegisterID {
        s0  =  0, d0  =  0, v0  =  0,
        s1  =  1, d1  =  1, v1  =  1,
        s2  =  2, d2  =  2, v2  =  2,
        s3  =  3, d3  =  3, v3  =  3,
        s4  =  4, d4  =  4, v4  =  4,
        s5  =  5, d5  =  5, v5  =  5,
        s6  =  6, d6  =  6, v6  =  6,
        s7  =  7, d7  =  7, v7  =  7,
        s8  =  8, d8  =  8, v8  =  8,
        s9  =  9, d9  =  9, v9  =  9,
        s10 = 10, d10 = 10, v10 = 10,
        s11 = 11, d11 = 11, v11 = 11,
        s12 = 12, d12 = 12, v12 = 12,
        s13 = 13, d13 = 13, v13 = 13,
        s14 = 14, d14 = 14, v14 = 14,
        s15 = 15, d15 = 15, v15 = 15,
        s16 = 16, d16 = 16, v16 = 16,
        s17 = 17, d17 = 17, v17 = 17,
        s18 = 18, d18 = 18, v18 = 18,
        s19 = 19, d19 = 19, v19 = 19,
        s20 = 20, d20 = 20, v20 = 20,
        s21 = 21, d21 = 21, v21 = 21,
        s22 = 22, d22 = 22, v22 = 22,
        s23 = 23, d23 = 23, v23 = 23,
        s24 = 24, d24 = 24, v24 = 24,
        s25 = 25, d25 = 25, v25 = 25,
        s26 = 26, d26 = 26, v26 = 26,
        s27 = 27, d27 = 27, v27 = 27,
        s28 = 28, d28 = 28, v28 = 28,
        s29 = 29, d29 = 29, v29 = 29,
        s30 = 30, d30 = 30, v30 = 30,
        s31 = 31, d31 = 31, v31 = 31, // Scratch register.
        invalid_fpreg
    };
    typedef uint8_t Code;
    typedef FPRegisterID Encoding;
    typedef uint64_t SetType;

    static const char* GetName(Code code) {
        static const char* const Names[] =
            { "d0",  "d1",  "d2",  "d3",  "d4",  "d5",  "d6",  "d7",  "d8",  "d9",
              "d10", "d11", "d12", "d13", "d14", "d15", "d16", "d17", "d18", "d19",
              "d20", "d21", "d22", "d23", "d24", "d25", "d26", "d27", "d28", "d29",
              "d30", "d31", "invalid" };
        return Names[code];
    }

    static const char* GetName(uint32_t i) {
        MOZ_ASSERT(i < TotalPhys);
        return GetName(Code(i));
    }

    static Code FromName(const char* name);

    static const Code Invalid = invalid_fpreg;

    static const uint32_t Total = 64;
    static const uint32_t TotalPhys = 32;
    static const SetType AllMask = 0xFFFFFFFFFFFFFFFFULL;
    static const SetType AllPhysMask = 0xFFFFFFFFULL;
    static const SetType SpreadCoefficient = 0x100000001ULL;

    static const uint32_t Allocatable = 31; // Without d31, the scratch register.

    // d31 is the ScratchFloatReg.
    static const SetType NonVolatileMask =
        SetType((1 << FloatRegisters::d8) | (1 << FloatRegisters::d9) |
                (1 << FloatRegisters::d10) | (1 << FloatRegisters::d11) |
                (1 << FloatRegisters::d12) | (1 << FloatRegisters::d13) |
                (1 << FloatRegisters::d14) | (1 << FloatRegisters::d15) |
                (1 << FloatRegisters::d16) | (1 << FloatRegisters::d17) |
                (1 << FloatRegisters::d18) | (1 << FloatRegisters::d19) |
                (1 << FloatRegisters::d20) | (1 << FloatRegisters::d21) |
                (1 << FloatRegisters::d22) | (1 << FloatRegisters::d23) |
                (1 << FloatRegisters::d24) | (1 << FloatRegisters::d25) |
                (1 << FloatRegisters::d26) | (1 << FloatRegisters::d27) |
                (1 << FloatRegisters::d28) | (1 << FloatRegisters::d29) |
                (1 << FloatRegisters::d30)) * SpreadCoefficient;

    static const SetType VolatileMask = AllMask & ~NonVolatileMask;
    static const SetType AllDoubleMask = AllPhysMask << TotalPhys;
    static const SetType AllSingleMask = AllPhysMask;

    static const SetType WrapperMask = VolatileMask;

    // d31 is the ScratchFloatReg.
    static const SetType NonAllocatableMask = (SetType(1) << FloatRegisters::d31) * SpreadCoefficient;

    static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
    union RegisterContent {
        float s;
        double d;
    };
    enum Kind {
        Double,
        Single
    };
};

// In bytes: slots needed for potential memory->memory move spills.
//   +8 for cycles
//   +8 for gpr spills
//   +8 for double spills
static const uint32_t ION_FRAME_SLACK_SIZE = 24;

static const uint32_t ShadowStackSpace = 0;

// TODO:
// This constant needs to be updated to account for whatever near/far branching
// strategy is used by ARM64.
static const uint32_t JumpImmediateRange = UINT32_MAX;

static const uint32_t ABIStackAlignment = 16;
static const uint32_t CodeAlignment = 16;
static const bool StackKeptAligned = false;

// Although sp is only usable if 16-byte alignment is kept,
// the Pseudo-StackPointer enables use of 8-byte alignment.
static const uint32_t StackAlignment = 8;
static const uint32_t NativeFrameSize = 8;

struct FloatRegister
{
    typedef FloatRegisters Codes;
    typedef Codes::Code Code;
    typedef Codes::Encoding Encoding;
    typedef Codes::SetType SetType;

    union RegisterContent {
        float s;
        double d;
    };

    constexpr FloatRegister(uint32_t code, FloatRegisters::Kind k)
      : code_(FloatRegisters::Code(code & 31)),
        k_(k)
    { }

    explicit constexpr FloatRegister(uint32_t code)
      : code_(FloatRegisters::Code(code & 31)),
        k_(FloatRegisters::Kind(code >> 5))
    { }

    constexpr FloatRegister()
      : code_(FloatRegisters::Code(-1)),
        k_(FloatRegisters::Double)
    { }

    static uint32_t SetSize(SetType x) {
        static_assert(sizeof(SetType) == 8, "SetType must be 64 bits");
        x |= x >> FloatRegisters::TotalPhys;
        x &= FloatRegisters::AllPhysMask;
        return mozilla::CountPopulation32(x);
    }

    static FloatRegister FromCode(uint32_t i) {
        MOZ_ASSERT(i < FloatRegisters::Total);
        FloatRegister r(i);
        return r;
    }
    Code code() const {
        MOZ_ASSERT((uint32_t)code_ < FloatRegisters::Total);
        return Code(code_ | (k_ << 5));
    }
    Encoding encoding() const {
        return Encoding(code_);
    }

    const char* name() const {
        return FloatRegisters::GetName(code());
    }
    bool volatile_() const {
        return !!((SetType(1) << code()) & FloatRegisters::VolatileMask);
    }
    bool operator!=(FloatRegister other) const {
        return other.code_ != code_ || other.k_ != k_;
    }
    bool operator==(FloatRegister other) const {
        return other.code_ == code_ && other.k_ == k_;
    }
    bool aliases(FloatRegister other) const {
        return other.code_ == code_;
    }
    uint32_t numAliased() const {
        return 2;
    }
    static FloatRegisters::Kind otherkind(FloatRegisters::Kind k) {
        if (k == FloatRegisters::Double)
            return FloatRegisters::Single;
        return FloatRegisters::Double;
    }
    void aliased(uint32_t aliasIdx, FloatRegister* ret) {
        if (aliasIdx == 0)
            *ret = *this;
        else
            *ret = FloatRegister(code_, otherkind(k_));
    }
    // This function mostly exists for the ARM backend.  It is to ensure that two
    // floating point registers' types are equivalent.  e.g. S0 is not equivalent
    // to D16, since S0 holds a float32, and D16 holds a Double.
    // Since all floating point registers on x86 and x64 are equivalent, it is
    // reasonable for this function to do the same.
    bool equiv(FloatRegister other) const {
        return k_ == other.k_;
    }
    constexpr uint32_t size() const {
        return k_ == FloatRegisters::Double ? sizeof(double) : sizeof(float);
    }
    uint32_t numAlignedAliased() {
        return numAliased();
    }
    void alignedAliased(uint32_t aliasIdx, FloatRegister* ret) {
        MOZ_ASSERT(aliasIdx < numAliased());
        aliased(aliasIdx, ret);
    }
    SetType alignedOrDominatedAliasedSet() const {
        return Codes::SpreadCoefficient << code_;
    }

    bool isSingle() const {
        return k_ == FloatRegisters::Single;
    }
    bool isDouble() const {
        return k_ == FloatRegisters::Double;
    }
    bool isSimd128() const {
        return false;
    }

    FloatRegister asSingle() const { return FloatRegister(code_, FloatRegisters::Single); }
    FloatRegister asDouble() const { return FloatRegister(code_, FloatRegisters::Double); }
    FloatRegister asSimd128() const { MOZ_CRASH(); }

    static uint32_t FirstBit(SetType x) {
        JS_STATIC_ASSERT(sizeof(SetType) == 8);
        return mozilla::CountTrailingZeroes64(x);
    }
    static uint32_t LastBit(SetType x) {
        JS_STATIC_ASSERT(sizeof(SetType) == 8);
        return 63 - mozilla::CountLeadingZeroes64(x);
    }

    static constexpr RegTypeName DefaultType = RegTypeName::Float64;

    template <RegTypeName Name = DefaultType>
    static SetType LiveAsIndexableSet(SetType s) {
        return SetType(0);
    }

    template <RegTypeName Name = DefaultType>
    static SetType AllocatableAsIndexableSet(SetType s) {
        static_assert(Name != RegTypeName::Any, "Allocatable set are not iterable");
        return LiveAsIndexableSet<Name>(s);
    }

    static TypedRegisterSet<FloatRegister> ReduceSetForPush(const TypedRegisterSet<FloatRegister>& s);
    static uint32_t GetSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
    static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
    uint32_t getRegisterDumpOffsetInBytes();

  public:
    Code code_ : 8;
    FloatRegisters::Kind k_ : 1;
};

template <> inline FloatRegister::SetType
FloatRegister::LiveAsIndexableSet<RegTypeName::Float32>(SetType set)
{
    return set & FloatRegisters::AllSingleMask;
}

template <> inline FloatRegister::SetType
FloatRegister::LiveAsIndexableSet<RegTypeName::Float64>(SetType set)
{
    return set & FloatRegisters::AllDoubleMask;
}

template <> inline FloatRegister::SetType
FloatRegister::LiveAsIndexableSet<RegTypeName::Any>(SetType set)
{
    return set;
}

// ARM/D32 has double registers that cannot be treated as float32.
// Luckily, ARMv8 doesn't have the same misfortune.
inline bool
hasUnaliasedDouble()
{
    return false;
}

// ARM prior to ARMv8 also has doubles that alias multiple floats.
// Again, ARMv8 is in the clear.
inline bool
hasMultiAlias()
{
    return false;
}

uint32_t GetARM64Flags();

} // namespace jit
} // namespace js

#endif // jit_arm64_Architecture_arm64_h

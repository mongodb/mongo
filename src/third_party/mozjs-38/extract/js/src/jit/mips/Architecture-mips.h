/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_Architecture_mips_h
#define jit_mips_Architecture_mips_h

#include "mozilla/MathAlgorithms.h"

#include <limits.h>
#include <stdint.h>

#include "js/Utility.h"

// gcc appears to use _mips_hard_float to denote
// that the target is a hard-float target.
#ifdef _mips_hard_float
#define JS_CODEGEN_MIPS_HARDFP
#endif

#if _MIPS_SIM == _ABIO32
#define USES_O32_ABI
#else
#error "Unsupported ABI"
#endif

namespace js {
namespace jit {

// Shadow stack space is not required on MIPS.
static const uint32_t ShadowStackSpace = 4 * sizeof(uintptr_t);

// These offsets are specific to nunboxing, and capture offsets into the
// components of a js::Value.
// Size of MIPS32 general purpose registers is 32 bits.
static const int32_t NUNBOX32_TYPE_OFFSET = 4;
static const int32_t NUNBOX32_PAYLOAD_OFFSET = 0;

// Size of each bailout table entry.
// For MIPS this is 2 instructions relative call.
static const uint32_t BAILOUT_TABLE_ENTRY_SIZE = 2 * sizeof(void*);

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
        t0 = r8,
        t1 = r9,
        t2 = r10,
        t3 = r11,
        t4 = r12,
        t5 = r13,
        t6 = r14,
        t7 = r15,
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
    typedef RegisterID Code;

    static const char* GetName(Code code) {
        static const char * const Names[] = { "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
                                              "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                              "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                                              "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
        return Names[code];
    }
    static const char* GetName(uint32_t i) {
        MOZ_ASSERT(i < Total);
        return GetName(Code(i));
    }

    static Code FromName(const char* name);

    static const Code StackPointer = sp;
    static const Code Invalid = invalid_reg;

    static const uint32_t Total = 32;
    static const uint32_t Allocatable = 14;

    static const uint32_t AllMask = 0xffffffff;
    static const uint32_t ArgRegMask = (1 << a0) | (1 << a1) | (1 << a2) | (1 << a3);

    static const uint32_t VolatileMask =
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
        (1 << Registers::t4) |
        (1 << Registers::t5) |
        (1 << Registers::t6) |
        (1 << Registers::t7);

    // We use this constant to save registers when entering functions. This
    // is why $ra is added here even though it is not "Non Volatile".
    static const uint32_t NonVolatileMask =
        (1 << Registers::s0) |
        (1 << Registers::s1) |
        (1 << Registers::s2) |
        (1 << Registers::s3) |
        (1 << Registers::s4) |
        (1 << Registers::s5) |
        (1 << Registers::s6) |
        (1 << Registers::s7) |
        (1 << Registers::ra);

    static const uint32_t WrapperMask =
        VolatileMask |         // = arguments
        (1 << Registers::t0) | // = outReg
        (1 << Registers::t1);  // = argBase

    static const uint32_t NonAllocatableMask =
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
    static const uint32_t TempMask = VolatileMask & ~NonAllocatableMask;

    // Registers returned from a JS -> JS call.
    static const uint32_t JSCallMask =
        (1 << Registers::a2) |
        (1 << Registers::a3);

    // Registers returned from a JS -> C call.
    static const uint32_t CallMask =
        (1 << Registers::v0) |
        (1 << Registers::v1);  // used for double-size returns

    static const uint32_t AllocatableMask = AllMask & ~NonAllocatableMask;

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
};

// Smallest integer type that can hold a register bitmask.
typedef uint32_t PackedRegisterMask;


// MIPS32 can have two types of floating-point coprocessors:
// - 32 bit floating-point coprocessor - In this case, there are 32 single
// precision registers and pairs of even and odd float registers are used as
// double precision registers. Example: f0 (double) is composed of
// f0 and f1 (single).
// - 64 bit floating-point coprocessor - In this case, there are 32 double
// precision register which can also be used as single precision registers.

// When using O32 ABI, floating-point coprocessor is 32 bit.
// When using N32 ABI, floating-point coprocessor is 64 bit.
class FloatRegisters
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

    static const char* GetName(Code code) {
        static const char * const Names[] = { "f0", "f1", "f2", "f3",  "f4", "f5",  "f6", "f7",
                                              "f8", "f9",  "f10", "f11", "f12", "f13",
                                              "f14", "f15", "f16", "f17", "f18", "f19",
                                              "f20", "f21", "f22", "f23", "f24", "f25",
                                              "f26", "f27", "f28", "f29", "f30", "f31"};
        return Names[code];
    }
    static const char* GetName(uint32_t i) {
        MOZ_ASSERT(i < Total);
        return GetName(Code(i % 32));
    }

    static Code FromName(const char* name);

    static const Code Invalid = invalid_freg;

    static const uint32_t Total = 64;
    static const uint32_t TotalDouble = 16;
    static const uint32_t TotalSingle = 32;
    static const uint32_t Allocatable = 42;
    // When saving all registers we only need to do is save double registers.
    static const uint32_t TotalPhys = 16;
    static const uint64_t AllDoubleMask = 0x55555555ULL << 32;
    static const uint64_t AllMask = AllDoubleMask | ((1ULL << 32) - 1);

    static const uint64_t NonVolatileDoubleMask =
        ((1ULL << FloatRegisters::f20) |
         (1ULL << FloatRegisters::f22) |
         (1ULL << FloatRegisters::f24) |
         (1ULL << FloatRegisters::f26) |
         (1ULL << FloatRegisters::f28) |
         (1ULL << FloatRegisters::f30)) << 32;

    // f20-single and f21-single alias f20-double ...
    static const uint64_t NonVolatileMask =
        NonVolatileDoubleMask |
        (1ULL << FloatRegisters::f20) |
        (1ULL << FloatRegisters::f21) |
        (1ULL << FloatRegisters::f22) |
        (1ULL << FloatRegisters::f23) |
        (1ULL << FloatRegisters::f24) |
        (1ULL << FloatRegisters::f25) |
        (1ULL << FloatRegisters::f26) |
        (1ULL << FloatRegisters::f27) |
        (1ULL << FloatRegisters::f28) |
        (1ULL << FloatRegisters::f29) |
        (1ULL << FloatRegisters::f30) |
        (1ULL << FloatRegisters::f31);

    static const uint64_t VolatileMask = AllMask & ~NonVolatileMask;
    static const uint64_t VolatileDoubleMask = AllDoubleMask & ~NonVolatileDoubleMask;

    static const uint64_t WrapperMask = VolatileMask;

    static const uint64_t NonAllocatableDoubleMask =
        ((1ULL << FloatRegisters::f16) |
         (1ULL << FloatRegisters::f18)) << 32;
    // f16-single and f17-single alias f16-double ...
    static const uint64_t NonAllocatableMask =
        NonAllocatableDoubleMask |
        (1ULL << FloatRegisters::f16) |
        (1ULL << FloatRegisters::f17) |
        (1ULL << FloatRegisters::f18) |
        (1ULL << FloatRegisters::f19);

    // Registers that can be allocated without being saved, generally.
    static const uint64_t TempMask = VolatileMask & ~NonAllocatableMask;

    static const uint64_t AllocatableMask = AllMask & ~NonAllocatableMask;

    typedef uint64_t SetType;
};

template <typename T>
class TypedRegisterSet;

class FloatRegister
{
  public:
    enum RegType {
        Single = 0x0,
        Double = 0x1,
    };

    typedef FloatRegisters Codes;
    typedef Codes::Code Code;

    uint32_t code_ : 6;
  protected:
    RegType kind_ : 1;

  public:
    MOZ_CONSTEXPR FloatRegister(uint32_t code, RegType kind = Double)
      : code_ (Code(code)), kind_(kind)
    { }
    MOZ_CONSTEXPR FloatRegister()
      : code_(Code(FloatRegisters::invalid_freg)), kind_(Double)
    { }

    bool operator==(const FloatRegister& other) const {
        MOZ_ASSERT(!isInvalid());
        MOZ_ASSERT(!other.isInvalid());
        return kind_ == other.kind_ && code_ == other.code_;
    }
    bool isDouble() const { return kind_ == Double; }
    bool isSingle() const { return kind_ == Single; }
    bool equiv(const FloatRegister& other) const { return other.kind_ == kind_; }
    size_t size() const { return (kind_ == Double) ? 8 : 4; }
    bool isInvalid() const {
        return code_ == FloatRegisters::invalid_freg;
    }

    FloatRegister doubleOverlay(unsigned int which = 0) const;
    FloatRegister singleOverlay(unsigned int which = 0) const;
    FloatRegister sintOverlay(unsigned int which = 0) const;
    FloatRegister uintOverlay(unsigned int which = 0) const;

    Code code() const {
        MOZ_ASSERT(!isInvalid());
        return Code(code_  | (kind_ << 5));
    }
    uint32_t id() const {
        return code_;
    }
    static FloatRegister FromCode(uint32_t i) {
        uint32_t code = i & 31;
        uint32_t kind = i >> 5;
        return FloatRegister(code, RegType(kind));
    }
    // This is similar to FromCode except for double registers on O32.
    static FloatRegister FromIndex(uint32_t index, RegType kind) {
#if defined(USES_O32_ABI)
        if (kind == Double)
            return FloatRegister(index * 2, RegType(kind));
#endif
        return FloatRegister(index, RegType(kind));
    }

    bool volatile_() const {
        if (isDouble())
            return !!((1ULL << code_) & FloatRegisters::VolatileMask);
        return !!((1ULL << (code_ & ~1)) & FloatRegisters::VolatileMask);
    }
    const char* name() const {
        return FloatRegisters::GetName(code_);
    }
    bool operator != (const FloatRegister& other) const {
        return other.kind_ != kind_ || code_ != other.code_;
    }
    bool aliases(const FloatRegister& other) {
        if (kind_ == other.kind_)
            return code_ == other.code_;
        return doubleOverlay() == other.doubleOverlay();
    }
    uint32_t numAliased() const {
        if (isDouble()) {
            MOZ_ASSERT((code_ & 1) == 0);
            return 3;
        }
        return 2;
    }
    void aliased(uint32_t aliasIdx, FloatRegister* ret) {
        if (aliasIdx == 0) {
            *ret = *this;
            return;
        }
        if (isDouble()) {
            MOZ_ASSERT((code_ & 1) == 0);
            MOZ_ASSERT(aliasIdx <= 2);
            *ret = singleOverlay(aliasIdx - 1);
            return;
        }
        MOZ_ASSERT(aliasIdx == 1);
        *ret = doubleOverlay(aliasIdx - 1);
    }
    uint32_t numAlignedAliased() const {
        if (isDouble()) {
            MOZ_ASSERT((code_ & 1) == 0);
            return 2;
        }
        // f1-float32 has 0 other aligned aliases, 1 total.
        // f0-float32 has 1 other aligned alias, 2 total.
        return 2 - (code_ & 1);
    }
    // |        f0-double        |
    // | f0-float32 | f1-float32 |
    // We only push double registers on MIPS. So, if we've stored f0-double
    // we also want to f0-float32 is stored there.
    void alignedAliased(uint32_t aliasIdx, FloatRegister* ret) {
        MOZ_ASSERT(isDouble());
        MOZ_ASSERT((code_ & 1) == 0);
        if (aliasIdx == 0) {
            *ret = *this;
            return;
        }
        MOZ_ASSERT(aliasIdx == 1);
        *ret = singleOverlay(aliasIdx - 1);
    }
    typedef FloatRegisters::SetType SetType;
    static uint32_t SetSize(SetType x) {
        static_assert(sizeof(SetType) == 8, "SetType must be 64 bits");
        return mozilla::CountPopulation32(x);
    }
    static Code FromName(const char* name) {
        return FloatRegisters::FromName(name);
    }
    static TypedRegisterSet<FloatRegister> ReduceSetForPush(const TypedRegisterSet<FloatRegister>& s);
    static uint32_t GetSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
    static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
    uint32_t getRegisterDumpOffsetInBytes();
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

} // namespace jit
} // namespace js

#endif /* jit_mips_Architecture_mips_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_Architecture_mips32_h
#define jit_mips32_Architecture_mips32_h

#include "mozilla/MathAlgorithms.h"

#include <limits.h>
#include <stdint.h>

#include "jit/mips-shared/Architecture-mips-shared.h"

#include "js/Utility.h"

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

// MIPS32 can have two types of floating-point coprocessors:
// - 32 bit floating-point coprocessor - In this case, there are 32 single
// precision registers and pairs of even and odd float registers are used as
// double precision registers. Example: f0 (double) is composed of
// f0 and f1 (single).
// - 64 bit floating-point coprocessor - In this case, there are 32 double
// precision register which can also be used as single precision registers.

// When using O32 ABI, floating-point coprocessor is 32 bit.
// When using N32 ABI, floating-point coprocessor is 64 bit.
class FloatRegisters : public FloatRegistersMIPSShared
{
  public:
    static const char* GetName(uint32_t i) {
        MOZ_ASSERT(i < Total);
        return FloatRegistersMIPSShared::GetName(Code(i % 32));
    }

    static Code FromName(const char* name);

    static const uint32_t Total = 64;
    static const uint32_t TotalDouble = 16;
    static const uint32_t RegisterIdLimit = 32;
    // Workarounds: On Loongson CPU-s the odd FP registers behave differently
    // in fp-32 mode than standard MIPS.
#if defined(_MIPS_ARCH_LOONGSON3A)
    static const uint32_t TotalSingle = 16;
    static const uint32_t Allocatable = 28;
    static const SetType AllSingleMask = 0x55555555ULL;
#else
    static const uint32_t TotalSingle = 32;
    static const uint32_t Allocatable = 42;
    static const SetType AllSingleMask = (1ULL << 32) - 1;
#endif
    // When saving all registers we only need to do is save double registers.
    static const uint32_t TotalPhys = 16;

    static_assert(sizeof(SetType) * 8 >= Total,
                  "SetType should be large enough to enumerate all registers.");

    static const SetType AllDoubleMask = 0x55555555ULL << 32;
    static const SetType AllMask = AllDoubleMask | AllSingleMask;

    static const SetType NonVolatileDoubleMask =
        ((1ULL << FloatRegisters::f20) |
         (1ULL << FloatRegisters::f22) |
         (1ULL << FloatRegisters::f24) |
         (1ULL << FloatRegisters::f26) |
         (1ULL << FloatRegisters::f28) |
         (1ULL << FloatRegisters::f30)) << 32;

    // f20-single and f21-single alias f20-double ...
    static const SetType NonVolatileMask =
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

    static const SetType VolatileMask = AllMask & ~NonVolatileMask;
    static const SetType VolatileDoubleMask = AllDoubleMask & ~NonVolatileDoubleMask;

    static const SetType WrapperMask = VolatileMask;

    static const SetType NonAllocatableDoubleMask =
        ((1ULL << FloatRegisters::f16) |
         (1ULL << FloatRegisters::f18)) << 32;
    // f16-single and f17-single alias f16-double ...
    static const SetType NonAllocatableMask =
        NonAllocatableDoubleMask |
        (1ULL << FloatRegisters::f16) |
        (1ULL << FloatRegisters::f17) |
        (1ULL << FloatRegisters::f18) |
        (1ULL << FloatRegisters::f19);

    // Registers that can be allocated without being saved, generally.
    static const SetType TempMask = VolatileMask & ~NonAllocatableMask;

    static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

class FloatRegister : public FloatRegisterMIPSShared
{
  public:
    enum RegType {
        Single = 0x0,
        Double = 0x1,
    };

    typedef FloatRegisters Codes;
    typedef Codes::Code Code;
    typedef Codes::Encoding Encoding;

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
    bool equiv(const FloatRegister& other) const { return other.kind_ == kind_; }
    size_t size() const { return (kind_ == Double) ? 8 : 4; }
    bool isInvalid() const {
        return code_ == FloatRegisters::invalid_freg;
    }

    bool isSingle() const { return kind_ == Single; }
    bool isDouble() const { return kind_ == Double; }

    FloatRegister doubleOverlay(unsigned int which = 0) const;
    FloatRegister singleOverlay(unsigned int which = 0) const;
    FloatRegister sintOverlay(unsigned int which = 0) const;
    FloatRegister uintOverlay(unsigned int which = 0) const;

    FloatRegister asSingle() const { return singleOverlay(); }
    FloatRegister asDouble() const { return doubleOverlay(); }
    FloatRegister asSimd128() const { MOZ_CRASH("NYI"); }

    Code code() const {
        MOZ_ASSERT(!isInvalid());
        return Code(code_  | (kind_ << 5));
    }
    Encoding encoding() const {
        MOZ_ASSERT(!isInvalid());
        return Encoding(code_);
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
        // Only even FP registers are avaiable for Loongson on O32.
# if defined(_MIPS_ARCH_LOONGSON3A)
        return FloatRegister(index * 2, kind);
# else
        if (kind == Double)
            return FloatRegister(index * 2, kind);
# endif
#endif
        return FloatRegister(index, kind);
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

    SetType alignedOrDominatedAliasedSet() const {
        if (isSingle())
            return SetType(1) << code_;

        MOZ_ASSERT(isDouble());
        return SetType(0b11) << code_;
    }

    static Code FromName(const char* name) {
        return FloatRegisters::FromName(name);
    }
    static TypedRegisterSet<FloatRegister> ReduceSetForPush(const TypedRegisterSet<FloatRegister>& s);
    static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
    uint32_t getRegisterDumpOffsetInBytes();
};

// In order to handle functions such as int(*)(int, double) where the first
// argument is a general purpose register, and the second argument is a floating
// point register, we have to store the double content into 2 general purpose
// registers, namely a2 and a3.
#define JS_CODEGEN_REGISTER_PAIR 1

} // namespace jit
} // namespace js

#endif /* jit_mips32_Architecture_mips32_h */

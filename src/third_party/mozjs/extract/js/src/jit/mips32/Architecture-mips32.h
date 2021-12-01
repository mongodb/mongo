/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_Architecture_mips32_h
#define jit_mips32_Architecture_mips32_h

#include "mozilla/EndianUtils.h"
#include "mozilla/MathAlgorithms.h"

#include <limits.h>
#include <stdint.h>

#include "jit/mips-shared/Architecture-mips-shared.h"

#include "js/Utility.h"

namespace js {
namespace jit {

static const uint32_t ShadowStackSpace = 4 * sizeof(uintptr_t);

// These offsets are specific to nunboxing, and capture offsets into the
// components of a js::Value.
// Size of MIPS32 general purpose registers is 32 bits.
#if MOZ_LITTLE_ENDIAN
static const int32_t NUNBOX32_TYPE_OFFSET = 4;
static const int32_t NUNBOX32_PAYLOAD_OFFSET = 0;
#else
static const int32_t NUNBOX32_TYPE_OFFSET = 0;
static const int32_t NUNBOX32_PAYLOAD_OFFSET = 4;
#endif

// Size of each bailout table entry.
// For MIPS this is 2 instructions relative call.
static const uint32_t BAILOUT_TABLE_ENTRY_SIZE = 2 * sizeof(void*);

// MIPS32 can have two types of floating-point coprocessors modes:
// - FR=0 mode/ 32-bit FPRs - Historical default, there are 32 single
// precision registers and pairs of even and odd float registers are used as
// double precision registers. Example: f0 (double) is composed of
// f0 and f1 (single). Loongson3A FPU running in this mode doesn't allow
// use of odd registers for single precision arithmetic.
// - FR=1 mode/ 64-bit FPRs - In this case, there are 32 double precision register
// which can also be used as single precision registers.
// More info https://dmz-portal.imgtec.com/wiki/MIPS_O32_ABI_-_FR0_and_FR1_Interlinking

// Currently we enable 16 even single precision registers which can be also can be used
// as double precision registers. It enables jit code to run even on Loongson3A.
// It does not support FR=1 mode because MacroAssembler threats odd single precision
// registers as high parts of even double precision registers.
#ifdef __mips_fpr
static_assert(__mips_fpr == 32, "MIPS32 jit only supports FR=0 fpu mode.");
#endif

class FloatRegisters : public FloatRegistersMIPSShared
{
  public:
    static const char* GetName(uint32_t i) {
        MOZ_ASSERT(i < RegisterIdLimit);
        return FloatRegistersMIPSShared::GetName(Encoding(i % 32));
    }

    static Encoding FromName(const char* name);

    static const uint32_t Total = 32;
    static const uint32_t TotalDouble = 16;
    static const uint32_t TotalSingle = 16;

    static const uint32_t Allocatable = 30;
    static const SetType AllSingleMask = (1ULL << TotalSingle) - 1;

    static const SetType AllDoubleMask = ((1ULL << TotalDouble) - 1) << TotalSingle;
    static const SetType AllMask = AllDoubleMask | AllSingleMask;

    // When saving all registers we only need to do is save double registers.
    static const uint32_t TotalPhys = 16;
    static const uint32_t RegisterIdLimit = 32;

    static_assert(sizeof(SetType) * 8 >= Total,
                  "SetType should be large enough to enumerate all registers.");

    static const SetType NonVolatileMask =
        ((SetType(1) << (FloatRegisters::f20 >> 1)) |
         (SetType(1) << (FloatRegisters::f22 >> 1)) |
         (SetType(1) << (FloatRegisters::f24 >> 1)) |
         (SetType(1) << (FloatRegisters::f26 >> 1)) |
         (SetType(1) << (FloatRegisters::f28 >> 1)) |
         (SetType(1) << (FloatRegisters::f30 >> 1))) * ((1 << TotalSingle) + 1);

    static const SetType VolatileMask = AllMask & ~NonVolatileMask;

    static const SetType WrapperMask = VolatileMask;

    static const SetType NonAllocatableMask =
        (SetType(1) << (FloatRegisters::f18 >> 1)) * ((1 << TotalSingle) + 1);

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

    Encoding code_ : 6;
  protected:
    RegType kind_ : 1;

  public:
    constexpr FloatRegister(uint32_t code, RegType kind = Double)
      : code_ (Encoding(code)), kind_(kind)
    { }
    constexpr FloatRegister()
      : code_(FloatRegisters::invalid_freg), kind_(Double)
    { }

    bool operator==(const FloatRegister& other) const {
        MOZ_ASSERT(!isInvalid());
        MOZ_ASSERT(!other.isInvalid());
        return kind_ == other.kind_ && code_ == other.code_;
    }
    bool equiv(const FloatRegister& other) const { return other.kind_ == kind_; }
    size_t size() const { return (kind_ == Double) ? 8 : 4; }
    size_t pushSize() const { return size(); }
    bool isInvalid() const {
        return code_ == FloatRegisters::invalid_freg;
    }

    bool isNotOdd() const { return !isInvalid() && ((code_ & 1) == 0); }

    bool isSingle() const { return kind_ == Single; }
    bool isDouble() const { return kind_ == Double; }

    FloatRegister doubleOverlay() const;
    FloatRegister singleOverlay() const;

    FloatRegister asSingle() const { return singleOverlay(); }
    FloatRegister asDouble() const { return doubleOverlay(); }
    FloatRegister asSimd128() const { MOZ_CRASH("NYI"); }

    Code code() const {
        MOZ_ASSERT(isNotOdd());
        return Code((code_ >> 1)  | (kind_ << 4));
    }
    Encoding encoding() const {
        MOZ_ASSERT(!isInvalid());
        return code_;
    }
    uint32_t id() const {
        MOZ_ASSERT(!isInvalid());
        return code_;
    }
    static FloatRegister FromCode(uint32_t i) {
        uint32_t code = i & 15;
        uint32_t kind = i >> 4;
        return FloatRegister(Encoding(code << 1), RegType(kind));
    }

    static FloatRegister FromIndex(uint32_t index, RegType kind) {
        MOZ_ASSERT(index < 16);
        return FloatRegister(Encoding(index << 1), kind);
    }

    bool volatile_() const {
        return !!((SetType(1) << code()) & FloatRegisters::VolatileMask);
    }
    const char* name() const {
        return FloatRegisters::GetName(code_);
    }
    bool operator != (const FloatRegister& other) const {
        return other.kind_ != kind_ || code_ != other.code_;
    }
    bool aliases(const FloatRegister& other) {
        MOZ_ASSERT(isNotOdd());
        return code_ == other.code_;
    }
    uint32_t numAliased() const {
        MOZ_ASSERT(isNotOdd());
        return 2;
    }
    void aliased(uint32_t aliasIdx, FloatRegister* ret) {
        MOZ_ASSERT(isNotOdd());

        if (aliasIdx == 0) {
            *ret = *this;
            return;
        }
        MOZ_ASSERT(aliasIdx == 1);
        if (isDouble()) {
            *ret = singleOverlay();
        } else {
            *ret = doubleOverlay();
        }
    }
    uint32_t numAlignedAliased() const {
        MOZ_ASSERT(isNotOdd());
        return 2;
    }
    void alignedAliased(uint32_t aliasIdx, FloatRegister* ret) {
        MOZ_ASSERT(isNotOdd());

        if (aliasIdx == 0) {
            *ret = *this;
            return;
        }
        MOZ_ASSERT(aliasIdx == 1);
        if (isDouble()) {
            *ret = singleOverlay();
        } else {
            *ret = doubleOverlay();
        }
    }

    SetType alignedOrDominatedAliasedSet() const {
        MOZ_ASSERT(isNotOdd());
        return (SetType(1) << (code_ >> 1)) * ((1 << FloatRegisters::TotalSingle) + 1);
    }

    static constexpr RegTypeName DefaultType = RegTypeName::Float64;

    template <RegTypeName = DefaultType>
    static SetType LiveAsIndexableSet(SetType s) {
        return SetType(0);
    }

    template <RegTypeName Name = DefaultType>
    static SetType AllocatableAsIndexableSet(SetType s) {
        static_assert(Name != RegTypeName::Any, "Allocatable set are not iterable");
        return LiveAsIndexableSet<Name>(s);
    }

    static Code FromName(const char* name) {
        return FloatRegisters::FromName(name);
    }
    static TypedRegisterSet<FloatRegister> ReduceSetForPush(const TypedRegisterSet<FloatRegister>& s);
    static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
    uint32_t getRegisterDumpOffsetInBytes();
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

template <> inline FloatRegister::SetType
FloatRegister::AllocatableAsIndexableSet<RegTypeName::Float32>(SetType set)
{
    // Single registers are not dominating any smaller registers, thus masking
    // is enough to convert an allocatable set into a set of register list all
    // single register available.
    return set & FloatRegisters::AllSingleMask;
}

template <> inline FloatRegister::SetType
FloatRegister::AllocatableAsIndexableSet<RegTypeName::Float64>(SetType set)
{
    return set & FloatRegisters::AllDoubleMask;
}

// In order to handle functions such as int(*)(int, double) where the first
// argument is a general purpose register, and the second argument is a floating
// point register, we have to store the double content into 2 general purpose
// registers, namely a2 and a3.
#define JS_CODEGEN_REGISTER_PAIR 1

} // namespace jit
} // namespace js

#endif /* jit_mips32_Architecture_mips32_h */

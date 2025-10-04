/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_Architecture_x86_h
#define jit_x86_shared_Architecture_x86_h

#if !defined(JS_CODEGEN_X86) && !defined(JS_CODEGEN_X64)
#  error "Unsupported architecture!"
#endif

#include "mozilla/MathAlgorithms.h"

#include <algorithm>
#include <string.h>

#include "jit/shared/Architecture-shared.h"

#include "jit/x86-shared/Constants-x86-shared.h"

namespace js {
namespace jit {

#if defined(JS_CODEGEN_X86)
// These offsets are specific to nunboxing, and capture offsets into the
// components of a js::Value.
static const int32_t NUNBOX32_TYPE_OFFSET = 4;
static const int32_t NUNBOX32_PAYLOAD_OFFSET = 0;
#endif

#if defined(JS_CODEGEN_X64) && defined(_WIN64)
static constexpr uint32_t ShadowStackSpace = 32;
#else
static constexpr uint32_t ShadowStackSpace = 0;
#endif

static const uint32_t JumpImmediateRange = INT32_MAX;

class Registers {
 public:
  using Code = uint8_t;
  using Encoding = X86Encoding::RegisterID;

  // Content spilled during bailouts.
  union RegisterContent {
    uintptr_t r;
  };

#if defined(JS_CODEGEN_X86)
  using SetType = uint8_t;

  static const char* GetName(Code code) {
    return X86Encoding::GPRegName(Encoding(code));
  }

  static const uint32_t Total = 8;
  static const uint32_t TotalPhys = 8;
  static const uint32_t Allocatable = 7;

#elif defined(JS_CODEGEN_X64)
  using SetType = uint16_t;

  static const char* GetName(Code code) {
    static const char* const Names[] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    return Names[code];
  }

  static const uint32_t Total = 16;
  static const uint32_t TotalPhys = 16;
  static const uint32_t Allocatable = 14;
#endif

  static uint32_t SetSize(SetType x) {
    static_assert(sizeof(SetType) <= 4, "SetType must be, at most, 32 bits");
    return mozilla::CountPopulation32(x);
  }
  static uint32_t FirstBit(SetType x) {
    return mozilla::CountTrailingZeroes32(x);
  }
  static uint32_t LastBit(SetType x) {
    return 31 - mozilla::CountLeadingZeroes32(x);
  }

  static Code FromName(const char* name) {
    for (size_t i = 0; i < Total; i++) {
      if (strcmp(GetName(Code(i)), name) == 0) {
        return Code(i);
      }
    }
    return Invalid;
  }

  static const Encoding StackPointer = X86Encoding::rsp;
  static const Encoding Invalid = X86Encoding::invalid_reg;

  static const SetType AllMask = (1 << Total) - 1;

#if defined(JS_CODEGEN_X86)
  static const SetType ArgRegMask = 0;

  static const SetType VolatileMask = (1 << X86Encoding::rax) |
                                      (1 << X86Encoding::rcx) |
                                      (1 << X86Encoding::rdx);

  static const SetType WrapperMask = VolatileMask | (1 << X86Encoding::rbx);

  static const SetType SingleByteRegs =
      (1 << X86Encoding::rax) | (1 << X86Encoding::rcx) |
      (1 << X86Encoding::rdx) | (1 << X86Encoding::rbx);

  static const SetType NonAllocatableMask =
      (1 << X86Encoding::rsp) | (1 << X86Encoding::rbp);

  // Registers returned from a JS -> JS call.
  static const SetType JSCallMask =
      (1 << X86Encoding::rcx) | (1 << X86Encoding::rdx);

  // Registers returned from a JS -> C call.
  static const SetType CallMask = (1 << X86Encoding::rax);

#elif defined(JS_CODEGEN_X64)
  static const SetType ArgRegMask =
#  if !defined(_WIN64)
      (1 << X86Encoding::rdi) | (1 << X86Encoding::rsi) |
#  endif
      (1 << X86Encoding::rdx) | (1 << X86Encoding::rcx) |
      (1 << X86Encoding::r8) | (1 << X86Encoding::r9);

  static const SetType VolatileMask = (1 << X86Encoding::rax) | ArgRegMask |
                                      (1 << X86Encoding::r10) |
                                      (1 << X86Encoding::r11);

  static const SetType WrapperMask = VolatileMask;

  static const SetType SingleByteRegs = AllMask & ~(1 << X86Encoding::rsp);

  static const SetType NonAllocatableMask =
      (1 << X86Encoding::rsp) | (1 << X86Encoding::rbp) |
      (1 << X86Encoding::r11);  // This is ScratchReg.

  // Registers returned from a JS -> JS call.
  static const SetType JSCallMask = (1 << X86Encoding::rcx);

  // Registers returned from a JS -> C call.
  static const SetType CallMask = (1 << X86Encoding::rax);

#endif

  static const SetType NonVolatileMask =
      AllMask & ~VolatileMask & ~(1 << X86Encoding::rsp);

  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

using PackedRegisterMask = Registers::SetType;

class FloatRegisters {
 public:
  using Encoding = X86Encoding::XMMRegisterID;

  // Observe that there is a Simd128 type on both x86 and x64 whether SIMD is
  // implemented/enabled or not, and that the RegisterContent union is large
  // enough for a V128 datum always.  Producers and consumers of a register dump
  // must be aware of this even if they don't need to save/restore values in the
  // high lanes of the SIMD registers.  See the DumpAllRegs() implementations,
  // for example.

  enum ContentType {
    Single,   // 32-bit float.
    Double,   // 64-bit double.
    Simd128,  // 128-bit Wasm SIMD type.
    NumTypes
  };

  // Content spilled during bailouts.
  union RegisterContent {
    float s;
    double d;
    uint8_t v128[16];
  };

  static const char* GetName(Encoding code) {
    return X86Encoding::XMMRegName(code);
  }

  static Encoding FromName(const char* name) {
    for (size_t i = 0; i < Total; i++) {
      if (strcmp(GetName(Encoding(i)), name) == 0) {
        return Encoding(i);
      }
    }
    return Invalid;
  }

  static const Encoding Invalid = X86Encoding::invalid_xmm;

#if defined(JS_CODEGEN_X86)
  static const uint32_t Total = 8 * NumTypes;
  static const uint32_t TotalPhys = 8;
  static const uint32_t Allocatable = 7;
  using SetType = uint32_t;
#elif defined(JS_CODEGEN_X64)
  static const uint32_t Total = 16 * NumTypes;
  static const uint32_t TotalPhys = 16;
  static const uint32_t Allocatable = 15;
  using SetType = uint64_t;
#endif

  static_assert(sizeof(SetType) * 8 >= Total,
                "SetType should be large enough to enumerate all registers.");

  // Magic values which are used to duplicate a mask of physical register for
  // a specific type of register. A multiplication is used to copy and shift
  // the bits of the physical register mask.
  static const SetType SpreadSingle = SetType(1)
                                      << (uint32_t(Single) * TotalPhys);
  static const SetType SpreadDouble = SetType(1)
                                      << (uint32_t(Double) * TotalPhys);
  static const SetType SpreadSimd128 = SetType(1)
                                       << (uint32_t(Simd128) * TotalPhys);
  static const SetType SpreadScalar = SpreadSingle | SpreadDouble;
  static const SetType SpreadVector = SpreadSimd128;
  static const SetType Spread = SpreadScalar | SpreadVector;

  static const SetType AllPhysMask = ((1 << TotalPhys) - 1);
  static const SetType AllMask = AllPhysMask * Spread;
  static const SetType AllDoubleMask = AllPhysMask * SpreadDouble;
  static const SetType AllSingleMask = AllPhysMask * SpreadSingle;
  static const SetType AllVector128Mask = AllPhysMask * SpreadSimd128;

#if defined(JS_CODEGEN_X86)
  static const SetType NonAllocatableMask =
      Spread * (1 << X86Encoding::xmm7);  // This is ScratchDoubleReg.

#elif defined(JS_CODEGEN_X64)
  static const SetType NonAllocatableMask =
      Spread * (1 << X86Encoding::xmm15);  // This is ScratchDoubleReg.
#endif

#if defined(JS_CODEGEN_X64) && defined(_WIN64)
  static const SetType VolatileMask =
      ((1 << X86Encoding::xmm0) | (1 << X86Encoding::xmm1) |
       (1 << X86Encoding::xmm2) | (1 << X86Encoding::xmm3) |
       (1 << X86Encoding::xmm4) | (1 << X86Encoding::xmm5)) *
      Spread;
#else
  static const SetType VolatileMask = AllMask;
#endif

  static const SetType NonVolatileMask = AllMask & ~VolatileMask;
  static const SetType WrapperMask = VolatileMask;
  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

static const uint32_t SpillSlotSize =
    std::max(sizeof(Registers::RegisterContent),
             sizeof(FloatRegisters::RegisterContent));

template <typename T>
class TypedRegisterSet;

struct FloatRegister {
  using Codes = FloatRegisters;
  using Code = size_t;
  using Encoding = Codes::Encoding;
  using SetType = Codes::SetType;
  static uint32_t SetSize(SetType x) {
    // Count the number of non-aliased registers, for the moment.
    //
    // Copy the set bits of each typed register to the low part of the of
    // the Set, and count the number of registers. This is made to avoid
    // registers which are allocated twice with different types (such as in
    // AllMask).
    x |= x >> (2 * Codes::TotalPhys);
    x |= x >> Codes::TotalPhys;
    x &= Codes::AllPhysMask;
    static_assert(Codes::AllPhysMask <= 0xffff,
                  "We can safely use CountPopulation32");
    return mozilla::CountPopulation32(x);
  }

#if defined(JS_CODEGEN_X86)
  static uint32_t FirstBit(SetType x) {
    static_assert(sizeof(SetType) == 4, "SetType must be 32 bits");
    return mozilla::CountTrailingZeroes32(x);
  }
  static uint32_t LastBit(SetType x) {
    return 31 - mozilla::CountLeadingZeroes32(x);
  }

#elif defined(JS_CODEGEN_X64)
  static uint32_t FirstBit(SetType x) {
    static_assert(sizeof(SetType) == 8, "SetType must be 64 bits");
    return mozilla::CountTrailingZeroes64(x);
  }
  static uint32_t LastBit(SetType x) {
    return 63 - mozilla::CountLeadingZeroes64(x);
  }
#endif

 private:
  // Note: These fields are using one extra bit to make the invalid enumerated
  // values fit, and thus prevent a warning.
  Codes::Encoding reg_ : 5;
  Codes::ContentType type_ : 3;
  bool isInvalid_ : 1;

  // Constants used for exporting/importing the float register code.
#if defined(JS_CODEGEN_X86)
  static const size_t RegSize = 3;
#elif defined(JS_CODEGEN_X64)
  static const size_t RegSize = 4;
#endif
  static const size_t RegMask = (1 << RegSize) - 1;

 public:
  constexpr FloatRegister()
      : reg_(Codes::Encoding(0)), type_(Codes::Single), isInvalid_(true) {}
  constexpr FloatRegister(uint32_t r, Codes::ContentType k)
      : reg_(Codes::Encoding(r)), type_(k), isInvalid_(false) {}
  constexpr FloatRegister(Codes::Encoding r, Codes::ContentType k)
      : reg_(r), type_(k), isInvalid_(false) {}

  static FloatRegister FromCode(uint32_t i) {
    MOZ_ASSERT(i < Codes::Total);
    return FloatRegister(i & RegMask, Codes::ContentType(i >> RegSize));
  }

  bool isSingle() const {
    MOZ_ASSERT(!isInvalid());
    return type_ == Codes::Single;
  }
  bool isDouble() const {
    MOZ_ASSERT(!isInvalid());
    return type_ == Codes::Double;
  }
  bool isSimd128() const {
    MOZ_ASSERT(!isInvalid());
    return type_ == Codes::Simd128;
  }
  bool isInvalid() const { return isInvalid_; }

  FloatRegister asSingle() const {
    MOZ_ASSERT(!isInvalid());
    return FloatRegister(reg_, Codes::Single);
  }
  FloatRegister asDouble() const {
    MOZ_ASSERT(!isInvalid());
    return FloatRegister(reg_, Codes::Double);
  }
  FloatRegister asSimd128() const {
    MOZ_ASSERT(!isInvalid());
    return FloatRegister(reg_, Codes::Simd128);
  }

  uint32_t size() const {
    MOZ_ASSERT(!isInvalid());
    if (isSingle()) {
      return sizeof(float);
    }
    if (isDouble()) {
      return sizeof(double);
    }
    MOZ_ASSERT(isSimd128());
    return 4 * sizeof(int32_t);
  }

  Code code() const {
    MOZ_ASSERT(!isInvalid());
    MOZ_ASSERT(uint32_t(reg_) < Codes::TotalPhys);
    // :TODO: ARM is doing the same thing, but we should avoid this, except
    // that the RegisterSets depends on this.
    return Code(reg_ | (type_ << RegSize));
  }
  Encoding encoding() const {
    MOZ_ASSERT(!isInvalid());
    MOZ_ASSERT(uint32_t(reg_) < Codes::TotalPhys);
    return reg_;
  }
  // defined in Assembler-x86-shared.cpp
  const char* name() const;
  bool volatile_() const {
    return !!((SetType(1) << code()) & FloatRegisters::VolatileMask);
  }
  bool operator!=(FloatRegister other) const {
    return other.reg_ != reg_ || other.type_ != type_;
  }
  bool operator==(FloatRegister other) const {
    return other.reg_ == reg_ && other.type_ == type_;
  }
  bool aliases(FloatRegister other) const { return other.reg_ == reg_; }
  // Check if two floating point registers have the same type.
  bool equiv(FloatRegister other) const { return other.type_ == type_; }

  uint32_t numAliased() const { return Codes::NumTypes; }
  uint32_t numAlignedAliased() const { return numAliased(); }

  FloatRegister aliased(uint32_t aliasIdx) const {
    MOZ_ASSERT(aliasIdx < Codes::NumTypes);
    return FloatRegister(
        reg_, Codes::ContentType((aliasIdx + type_) % Codes::NumTypes));
  }
  FloatRegister alignedAliased(uint32_t aliasIdx) const {
    return aliased(aliasIdx);
  }

  SetType alignedOrDominatedAliasedSet() const { return Codes::Spread << reg_; }

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

  static TypedRegisterSet<FloatRegister> ReduceSetForPush(
      const TypedRegisterSet<FloatRegister>& s);
  static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
  uint32_t getRegisterDumpOffsetInBytes();
};

template <>
inline FloatRegister::SetType
FloatRegister::LiveAsIndexableSet<RegTypeName::Float32>(SetType set) {
  return set & FloatRegisters::AllSingleMask;
}

template <>
inline FloatRegister::SetType
FloatRegister::LiveAsIndexableSet<RegTypeName::Float64>(SetType set) {
  return set & FloatRegisters::AllDoubleMask;
}

template <>
inline FloatRegister::SetType
FloatRegister::LiveAsIndexableSet<RegTypeName::Vector128>(SetType set) {
  return set & FloatRegisters::AllVector128Mask;
}

template <>
inline FloatRegister::SetType
FloatRegister::LiveAsIndexableSet<RegTypeName::Any>(SetType set) {
  return set;
}

// Arm/D32 has double registers that can NOT be treated as float32
// and this requires some dances in lowering.
inline bool hasUnaliasedDouble() { return false; }

// On ARM, Dn aliases both S2n and S2n+1, so if you need to convert a float32
// to a double as a temporary, you need a temporary double register.
inline bool hasMultiAlias() { return false; }

}  // namespace jit
}  // namespace js

#endif /* jit_x86_shared_Architecture_x86_h */

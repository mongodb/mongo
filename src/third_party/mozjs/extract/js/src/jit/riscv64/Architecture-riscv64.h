/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_riscv64_Architecture_riscv64_h
#define jit_riscv64_Architecture_riscv64_h

// JitSpewer.h is included through MacroAssembler implementations for other
// platforms, so include it here to avoid inadvertent build bustage.
#include "mozilla/MathAlgorithms.h"

#include <algorithm>
#include <iterator>

#include "jit/JitSpewer.h"
#include "jit/shared/Architecture-shared.h"
#include "js/Utility.h"

namespace js {
namespace jit {

static const uint32_t SimdMemoryAlignment =
    16;  // Make it 4 to avoid a bunch of div-by-zero warnings

// RISCV64 has 32 64-bit integer registers, x0 though x31.
//  The program counter is not accessible as a register.

// RISCV INT Register Convention:
// Name          Alias          Usage
// x0            zero           hardwired to 0, ignores writes
// x1            ra             return address for calls
// x2            sp             stack pointer
// x3            gp             global pointer
// x4            tp             thread pointer
// x5-x7         t0-t2          temporary register 0
// x8            fp/s0          Callee-saved register 0 or frame pointer
// x9            s1             Callee-saved register 1
// x10-x11       a0-a1          return value or function argument
// x12-x17       a2-a7          function argument 2
// x18-x27       s2-s11         Callee-saved register
// x28-x31       t3-t6          temporary register 3

// RISCV-64 FP Register Convention:
//  Name         Alias           Usage
//  $f0-$f7      $ft0-$ft7       Temporary registers
//  $f8-$f9      $fs0-$fs1       Callee-saved registers
//  $f10-$f11    $fa0-$fa1       Return values
//  $f12-$f17    $fa2-$fa7       Args values
//  $f18-$f27    $fs2-$fs11      Callee-saved registers
//  $f28-$f31    $ft8-$ft11      Temporary registers
class Registers {
 public:
  enum RegisterID {
    x0 = 0,
    x1,
    x2,
    x3,
    x4,
    x5,
    x6,
    x7,
    x8,
    x9,
    x10,
    x11,
    x12,
    x13,
    x14,
    x15,
    x16,
    x17,
    x18,
    x19,
    x20,
    x21,
    x22,
    x23,
    x24,
    x25,
    x26,
    x27,
    x28,
    x29,
    x30,
    x31,
    zero = x0,
    ra = x1,
    sp = x2,
    gp = x3,
    tp = x4,
    t0 = x5,
    t1 = x6,
    t2 = x7,
    fp = x8,
    s1 = x9,
    a0 = x10,
    a1 = x11,
    a2 = x12,
    a3 = x13,
    a4 = x14,
    a5 = x15,
    a6 = x16,
    a7 = x17,
    s2 = x18,
    s3 = x19,
    s4 = x20,
    s5 = x21,
    s6 = x22,
    s7 = x23,
    s8 = x24,
    s9 = x25,
    s10 = x26,
    s11 = x27,
    t3 = x28,
    t4 = x29,
    t5 = x30,
    t6 = x31,
    invalid_reg,
  };
  typedef uint8_t Code;
  typedef RegisterID Encoding;
  union RegisterContent {
    uintptr_t r;
  };

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
  static const char* GetName(uint32_t code) {
    static const char* const Names[] = {
        "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "fp", "s1", "a0",
        "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
        "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
    static_assert(Total == std::size(Names), "Table is the correct size");
    if (code >= Total) {
      return "invalid";
    }
    return Names[code];
  }

  static Code FromName(const char*);

  static const Encoding StackPointer = sp;
  static const Encoding Invalid = invalid_reg;
  static const uint32_t Total = 32;
  static const uint32_t TotalPhys = 32;
  static const uint32_t Allocatable = 24;
  static const SetType NoneMask = 0x0;
  static const SetType AllMask = 0xFFFFFFFF;
  static const SetType ArgRegMask =
      (1 << Registers::a0) | (1 << Registers::a1) | (1 << Registers::a2) |
      (1 << Registers::a3) | (1 << Registers::a4) | (1 << Registers::a5) |
      (1 << Registers::a6) | (1 << Registers::a7);

  static const SetType VolatileMask =
      ArgRegMask | (1 << Registers::t0) | (1 << Registers::t1) |
      (1 << Registers::t2) | (1 << Registers::t3) | (1 << Registers::t4) |
      (1 << Registers::t5) | (1 << Registers::t6);

  // We use this constant to save registers when entering functions. This
  // is why $ra is added here even though it is not "Non Volatile".
  static const SetType NonVolatileMask =
      (1 << Registers::ra) | (1 << Registers::fp) | (1 << Registers::s1) |
      (1 << Registers::s2) | (1 << Registers::s3) | (1 << Registers::s4) |
      (1 << Registers::s5) | (1 << Registers::s6) | (1 << Registers::s7) |
      (1 << Registers::s8) | (1 << Registers::s9) | (1 << Registers::s10) |
      (1 << Registers::s11);

  static const SetType NonAllocatableMask =
      (1 << Registers::zero) |  // Always be zero.
      (1 << Registers::t4) |    // Scratch reg
      (1 << Registers::t5) |    // Scratch reg
      (1 << Registers::t6) |    // Scratch reg or call reg
      (1 << Registers::s11) |   // Scratch reg
      (1 << Registers::ra) | (1 << Registers::tp) | (1 << Registers::sp) |
      (1 << Registers::fp) | (1 << Registers::gp);

  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;

  // Registers returned from a JS -> JS call.
  static const SetType JSCallMask = (1 << Registers::a2);

  // Registers returned from a JS -> C call.
  static const SetType CallMask = (1 << Registers::a0);

  static const SetType WrapperMask = VolatileMask;
};

// Smallest integer type that can hold a register bitmask.
typedef uint32_t PackedRegisterMask;

class FloatRegisters {
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
    invalid_reg,
    ft0 = f0,
    ft1 = f1,
    ft2 = f2,
    ft3 = f3,
    ft4 = f4,
    ft5 = f5,
    ft6 = f6,
    ft7 = f7,
    fs0 = f8,
    fs1 = f9,
    fa0 = f10,
    fa1 = f11,
    fa2 = f12,
    fa3 = f13,
    fa4 = f14,
    fa5 = f15,
    fa6 = f16,
    fa7 = f17,
    fs2 = f18,
    fs3 = f19,
    fs4 = f20,
    fs5 = f21,
    fs6 = f22,
    fs7 = f23,
    fs8 = f24,
    fs9 = f25,
    fs10 = f26,
    fs11 = f27,  // Scratch register
    ft8 = f28,
    ft9 = f29,
    ft10 = f30,  // Scratch register
    ft11 = f31
  };

  enum Kind : uint8_t { Double, NumTypes, Single };

  typedef FPRegisterID Code;
  typedef FPRegisterID Encoding;
  union RegisterContent {
    float s;
    double d;
  };

  static const char* GetName(uint32_t code) {
    static const char* const Names[] = {
        "ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6",  "ft7",
        "fs0", "fs2", "fa0",  "fa1",  "fa2", "fa3", "fa4",  "fa5",
        "fa6", "fa7", "fs2",  "fs3",  "fs4", "fs5", "fs6",  "fs7",
        "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"};
    static_assert(TotalPhys == std::size(Names), "Table is the correct size");
    if (code >= Total) {
      return "invalid";
    }
    return Names[code];
  }

  static Code FromName(const char* name);

  typedef uint32_t SetType;

  static const Code Invalid = invalid_reg;
  static const uint32_t Total = 32;
  static const uint32_t TotalPhys = 32;
  static const uint32_t Allocatable = 23;
  static const SetType AllPhysMask = 0xFFFFFFFF;
  static const SetType AllMask = 0xFFFFFFFF;
  static const SetType AllDoubleMask = AllMask;
  // Single values are stored as 64 bits values (NaN-boxed) when pushing them to
  // the stack, we do not require making distinctions between the 2 types, and
  // therefore the masks are overlapping.See The RISC-V Instruction Set Manual
  // for 14.2 NaN Boxing of Narrower Values.
  static const SetType AllSingleMask = AllMask;
  static const SetType NonVolatileMask =
      SetType((1 << FloatRegisters::fs0) | (1 << FloatRegisters::fs1) |
              (1 << FloatRegisters::fs2) | (1 << FloatRegisters::fs3) |
              (1 << FloatRegisters::fs4) | (1 << FloatRegisters::fs5) |
              (1 << FloatRegisters::fs6) | (1 << FloatRegisters::fs7) |
              (1 << FloatRegisters::fs8) | (1 << FloatRegisters::fs9) |
              (1 << FloatRegisters::fs10) | (1 << FloatRegisters::fs11));
  static const SetType VolatileMask = AllMask & ~NonVolatileMask;

  // fs11/ft10 is the scratch register.
  static const SetType NonAllocatableMask =
      SetType((1 << FloatRegisters::fs11) | (1 << FloatRegisters::ft10));

  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

template <typename T>
class TypedRegisterSet;

struct FloatRegister {
 public:
  typedef FloatRegisters Codes;
  typedef Codes::Code Code;
  typedef Codes::Encoding Encoding;
  typedef Codes::SetType SetType;

  static uint32_t SetSize(SetType x) {
    static_assert(sizeof(SetType) == 4, "SetType must be 32 bits");
    x &= FloatRegisters::AllPhysMask;
    return mozilla::CountPopulation32(x);
  }

  static uint32_t FirstBit(SetType x) {
    static_assert(sizeof(SetType) == 4, "SetType");
    return mozilla::CountTrailingZeroes64(x);
  }
  static uint32_t LastBit(SetType x) {
    static_assert(sizeof(SetType) == 4, "SetType");
    return 31 - mozilla::CountLeadingZeroes64(x);
  }

  static FloatRegister FromCode(uint32_t i) {
    uint32_t code = i & 0x1f;
    return FloatRegister(Code(code));
  }
  bool isSimd128() const { return false; }
  bool isInvalid() const { return invalid_; }
  FloatRegister asSingle() const {
    MOZ_ASSERT(!invalid_);
    return FloatRegister(Encoding(encoding_), FloatRegisters::Single);
  }
  FloatRegister asDouble() const {
    MOZ_ASSERT(!invalid_);
    return FloatRegister(Encoding(encoding_), FloatRegisters::Double);
  }
  FloatRegister asSimd128() const { MOZ_CRASH(); }
  constexpr Code code() const {
    MOZ_ASSERT(!invalid_);
    return encoding_;
  }
  Encoding encoding() const { return encoding_; }
  const char* name() const { return FloatRegisters::GetName(code()); }
  bool volatile_() const {
    MOZ_ASSERT(!invalid_);
    return !!((SetType(1) << code()) & FloatRegisters::VolatileMask);
  }
  bool operator!=(FloatRegister other) const { return code() != other.code(); }
  bool operator==(FloatRegister other) const { return code() == other.code(); }
  bool aliases(FloatRegister other) const {
    return other.encoding_ == encoding_;
  }
  uint32_t numAliased() const { return 1; }
  FloatRegister aliased(uint32_t aliasIdx) const {
    MOZ_ASSERT(aliasIdx == 0);
    return *this;
  }
  // Ensure that two floating point registers' types are equivalent.
  bool equiv(FloatRegister other) const {
    MOZ_ASSERT(!invalid_);
    return kind_ == other.kind_;
  }
  constexpr uint32_t size() const {
    MOZ_ASSERT(!invalid_);
    if (kind_ == FloatRegisters::Double) {
      return sizeof(double);
    }
    MOZ_ASSERT(kind_ == FloatRegisters::Single);
    return sizeof(float);
  }
  uint32_t numAlignedAliased() { return numAliased(); }
  FloatRegister alignedAliased(uint32_t aliasIdx) {
    MOZ_ASSERT(aliasIdx < numAliased());
    return aliased(aliasIdx);
  }
  SetType alignedOrDominatedAliasedSet() const { return SetType(1) << code(); }
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

  FloatRegister singleOverlay() const;
  FloatRegister doubleOverlay() const;

  static TypedRegisterSet<FloatRegister> ReduceSetForPush(
      const TypedRegisterSet<FloatRegister>& s);

  uint32_t getRegisterDumpOffsetInBytes() {
#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

    return code() * sizeof(double);
  }
  static Code FromName(const char* name);

  // This is used in static initializers, so produce a bogus value instead of
  // crashing.
  static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>& s);

 private:
  typedef Codes::Kind Kind;
  // These fields only hold valid values: an invalid register is always
  // represented as a valid encoding and kind with the invalid_ bit set.
  Encoding encoding_;  // 32 encodings
  Kind kind_;          // Double, Single; more later
  bool invalid_;

 public:
  constexpr FloatRegister(Encoding encoding, Kind kind)
      : encoding_(encoding), kind_(kind), invalid_(false) {
    MOZ_ASSERT(uint32_t(encoding) < Codes::Total);
  }

  constexpr FloatRegister(Encoding encoding)
      : encoding_(encoding), kind_(FloatRegisters::Double), invalid_(false) {
    MOZ_ASSERT(uint32_t(encoding) < Codes::Total);
  }

  constexpr FloatRegister()
      : encoding_(FloatRegisters::invalid_reg),
        kind_(FloatRegisters::Double),
        invalid_(true) {}

  bool isSingle() const {
    MOZ_ASSERT(!invalid_);
    // On riscv64 arch, float register and double register using the same
    // register file.
    return kind_ == FloatRegisters::Single || kind_ == FloatRegisters::Double;
  }
  bool isDouble() const {
    MOZ_ASSERT(!invalid_);
    return kind_ == FloatRegisters::Double;
  }

  Encoding code() { return encoding_; }
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
FloatRegister::LiveAsIndexableSet<RegTypeName::Any>(SetType set) {
  return set;
}

inline bool hasUnaliasedDouble() { return false; }
inline bool hasMultiAlias() { return false; }

static constexpr uint32_t ShadowStackSpace = 0;
static const uint32_t JumpImmediateRange = INT32_MAX;

#ifdef JS_NUNBOX32
static const int32_t NUNBOX32_TYPE_OFFSET = 4;
static const int32_t NUNBOX32_PAYLOAD_OFFSET = 0;
#endif

static const uint32_t SpillSlotSize =
    std::max(sizeof(Registers::RegisterContent),
             sizeof(FloatRegisters::RegisterContent));

inline uint32_t GetRISCV64Flags() { return 0; }

}  // namespace jit
}  // namespace js

#endif /* jit_riscv64_Architecture_riscv64_h */

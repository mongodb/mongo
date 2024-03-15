/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_loong64_Architecture_loong64_h
#define jit_loong64_Architecture_loong64_h

#include "mozilla/MathAlgorithms.h"

#include <algorithm>
#include <iterator>

#include "jit/shared/Architecture-shared.h"

#include "js/Utility.h"

namespace js {
namespace jit {

// LoongArch64 has 32 64-bit integer registers, r0 though r31.
//  The program counter is not accessible as a register.
//
// SIMD and scalar floating-point registers share a register bank.
//  Floating-point registers are f0 through f31.
//  128 bit SIMD registers are vr0 through vr31.
//  e.g., f0 is the bottom 64 bits of vr0.

// LoongArch64 INT Register Convention:
//  Name         Alias           Usage
//  $r0          $zero           Constant zero
//  $r1          $ra             Return address
//  $r2          $tp             TLS
//  $r3          $sp             Stack pointer
//  $r4-$r11     $a0-$a7         Argument registers
//  $r4-$r5      $v0-$v1         Return values
//  $r12-$r20    $t0-$t8         Temporary registers
//  $r21         $x              Reserved
//  $r22         $fp             Frame pointer
//  $r23-$r31    $s0-$s8         Callee-saved registers

// LoongArch64 FP Register Convention:
//  Name         Alias           Usage
//  $f0-$f7      $fa0-$fa7       Argument registers
//  $f0-$f1      $fv0-$fv1       Return values
//  $f8-f23      $ft0-$ft15      Temporary registers
//  $f24-$f31    $fs0-$fs7       Callee-saved registers

class Registers {
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
    ra = r1,
    tp = r2,
    sp = r3,
    a0 = r4,
    a1 = r5,
    a2 = r6,
    a3 = r7,
    a4 = r8,
    a5 = r9,
    a6 = r10,
    a7 = r11,
    t0 = r12,
    t1 = r13,
    t2 = r14,
    t3 = r15,
    t4 = r16,
    t5 = r17,
    t6 = r18,
    t7 = r19,
    t8 = r20,
    rx = r21,
    fp = r22,
    s0 = r23,
    s1 = r24,
    s2 = r25,
    s3 = r26,
    s4 = r27,
    s5 = r28,
    s6 = r29,
    s7 = r30,
    s8 = r31,
    invalid_reg,
  };
  typedef uint8_t Code;
  typedef RegisterID Encoding;
  typedef uint32_t SetType;

  static const Encoding StackPointer = sp;
  static const Encoding Invalid = invalid_reg;

  // Content spilled during bailouts.
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

  static const char* GetName(uint32_t code) {
    static const char* const Names[] = {
        "zero", "ra", "tp", "sp", "a0", "a1", "a2", "a3", "a4", "a5", "a6",
        "a7",   "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "rx",
        "fp",   "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8"};
    static_assert(Total == std::size(Names), "Table is the correct size");
    if (code >= Total) {
      return "invalid";
    }
    return Names[code];
  }

  static Code FromName(const char* name);

  static const uint32_t Total = 32;
  static const uint32_t TotalPhys = 32;
  static const uint32_t Allocatable =
      23;  // No named special-function registers.

  static const SetType AllMask = 0xFFFFFFFF;
  static const SetType NoneMask = 0x0;

  static const SetType ArgRegMask =
      (1 << Registers::a0) | (1 << Registers::a1) | (1 << Registers::a2) |
      (1 << Registers::a3) | (1 << Registers::a4) | (1 << Registers::a5) |
      (1 << Registers::a6) | (1 << Registers::a7);

  static const SetType VolatileMask =
      (1 << Registers::a0) | (1 << Registers::a1) | (1 << Registers::a2) |
      (1 << Registers::a3) | (1 << Registers::a4) | (1 << Registers::a5) |
      (1 << Registers::a6) | (1 << Registers::a7) | (1 << Registers::t0) |
      (1 << Registers::t1) | (1 << Registers::t2) | (1 << Registers::t3) |
      (1 << Registers::t4) | (1 << Registers::t5) | (1 << Registers::t6);

  // We use this constant to save registers when entering functions. This
  // is why $ra is added here even though it is not "Non Volatile".
  static const SetType NonVolatileMask =
      (1 << Registers::ra) | (1 << Registers::fp) | (1 << Registers::s0) |
      (1 << Registers::s1) | (1 << Registers::s2) | (1 << Registers::s3) |
      (1 << Registers::s4) | (1 << Registers::s5) | (1 << Registers::s6) |
      (1 << Registers::s7) | (1 << Registers::s8);

  static const SetType NonAllocatableMask =
      (1 << Registers::zero) |  // Always be zero.
      (1 << Registers::t7) |    // First scratch register.
      (1 << Registers::t8) |    // Second scratch register.
      (1 << Registers::rx) |    // Reserved Register.
      (1 << Registers::ra) | (1 << Registers::tp) | (1 << Registers::sp) |
      (1 << Registers::fp);

  static const SetType WrapperMask = VolatileMask;

  // Registers returned from a JS -> JS call.
  static const SetType JSCallMask = (1 << Registers::a2);

  // Registers returned from a JS -> C call.
  static const SetType CallMask = (1 << Registers::a0);

  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

// Smallest integer type that can hold a register bitmask.
typedef uint32_t PackedRegisterMask;

template <typename T>
class TypedRegisterSet;

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
    f23,  // Scratch register.
    f24,
    f25,
    f26,
    f27,
    f28,
    f29,
    f30,
    f31,
  };

  // Eight bits: (invalid << 7) | (kind << 5) | encoding
  typedef uint8_t Code;
  typedef FPRegisterID Encoding;
  typedef uint64_t SetType;

  enum Kind : uint8_t { Double, Single, NumTypes };

  static constexpr Code Invalid = 0x80;

  static const char* GetName(uint32_t code) {
    static const char* const Names[] = {
        "f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
        "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15",
        "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",
        "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31"};
    static_assert(TotalPhys == std::size(Names), "Table is the correct size");
    if (code >= Total) {
      return "invalid";
    }
    return Names[code];
  }

  static Code FromName(const char* name);

  static const uint32_t TotalPhys = 32;
  static const uint32_t Total = TotalPhys * NumTypes;
  static const uint32_t Allocatable = 31;  // Without f23, the scratch register.

  static_assert(sizeof(SetType) * 8 >= Total,
                "SetType should be large enough to enumerate all registers.");

  // Magic values which are used to duplicate a mask of physical register for
  // a specific type of register. A multiplication is used to copy and shift
  // the bits of the physical register mask.
  static const SetType SpreadSingle = SetType(1)
                                      << (uint32_t(Single) * TotalPhys);
  static const SetType SpreadDouble = SetType(1)
                                      << (uint32_t(Double) * TotalPhys);
  static const SetType Spread = SpreadSingle | SpreadDouble;

  static const SetType AllPhysMask = ((SetType(1) << TotalPhys) - 1);
  static const SetType AllMask = AllPhysMask * Spread;
  static const SetType AllSingleMask = AllPhysMask * SpreadSingle;
  static const SetType AllDoubleMask = AllPhysMask * SpreadDouble;
  static const SetType NoneMask = SetType(0);

  // TODO(loong64): Much less than ARM64 here.
  static const SetType NonVolatileMask =
      SetType((1 << FloatRegisters::f24) | (1 << FloatRegisters::f25) |
              (1 << FloatRegisters::f26) | (1 << FloatRegisters::f27) |
              (1 << FloatRegisters::f28) | (1 << FloatRegisters::f29) |
              (1 << FloatRegisters::f30) | (1 << FloatRegisters::f31)) *
      Spread;

  static const SetType VolatileMask = AllMask & ~NonVolatileMask;

  static const SetType WrapperMask = VolatileMask;

  // f23 is the scratch register.
  static const SetType NonAllocatableMask =
      (SetType(1) << FloatRegisters::f23) * Spread;

  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;

  // Content spilled during bailouts.
  union RegisterContent {
    float s;
    double d;
  };

  static constexpr Encoding encoding(Code c) {
    // assert() not available in constexpr function.
    // assert(c < Total);
    return Encoding(c & 31);
  }

  static constexpr Kind kind(Code c) {
    // assert() not available in constexpr function.
    // assert(c < Total && ((c >> 5) & 3) < NumTypes);
    return Kind((c >> 5) & 3);
  }

  static constexpr Code fromParts(uint32_t encoding, uint32_t kind,
                                  uint32_t invalid) {
    return Code((invalid << 7) | (kind << 5) | encoding);
  }
};

static const uint32_t SpillSlotSize =
    std::max(sizeof(Registers::RegisterContent),
             sizeof(FloatRegisters::RegisterContent));

static const uint32_t ShadowStackSpace = 0;
static const uint32_t SizeOfReturnAddressAfterCall = 0;

// When our only strategy for far jumps is to encode the offset directly, and
// not insert any jump islands during assembly for even further jumps, then the
// architecture restricts us to -2^27 .. 2^27-4, to fit into a signed 28-bit
// value.  We further reduce this range to allow the far-jump inserting code to
// have some breathing room.
static const uint32_t JumpImmediateRange = ((1 << 27) - (20 * 1024 * 1024));

struct FloatRegister {
  typedef FloatRegisters Codes;
  typedef size_t Code;
  typedef Codes::Encoding Encoding;
  typedef Codes::SetType SetType;

  static uint32_t SetSize(SetType x) {
    static_assert(sizeof(SetType) == 8, "SetType must be 64 bits");
    x |= x >> FloatRegisters::TotalPhys;
    x &= FloatRegisters::AllPhysMask;
    return mozilla::CountPopulation32(x);
  }

  static uint32_t FirstBit(SetType x) {
    static_assert(sizeof(SetType) == 8, "SetType");
    return mozilla::CountTrailingZeroes64(x);
  }
  static uint32_t LastBit(SetType x) {
    static_assert(sizeof(SetType) == 8, "SetType");
    return 63 - mozilla::CountLeadingZeroes64(x);
  }

 private:
  // These fields only hold valid values: an invalid register is always
  // represented as a valid encoding and kind with the invalid_ bit set.
  uint8_t encoding_;  // 32 encodings
  uint8_t kind_;      // Double, Single; more later
  bool invalid_;

  typedef Codes::Kind Kind;

 public:
  constexpr FloatRegister(Encoding encoding, Kind kind)
      : encoding_(encoding), kind_(kind), invalid_(false) {
    // assert(uint32_t(encoding) < Codes::TotalPhys);
  }

  constexpr FloatRegister()
      : encoding_(0), kind_(FloatRegisters::Double), invalid_(true) {}

  static FloatRegister FromCode(uint32_t i) {
    MOZ_ASSERT(i < Codes::Total);
    return FloatRegister(FloatRegisters::encoding(i), FloatRegisters::kind(i));
  }

  bool isSingle() const {
    MOZ_ASSERT(!invalid_);
    return kind_ == FloatRegisters::Single;
  }
  bool isDouble() const {
    MOZ_ASSERT(!invalid_);
    return kind_ == FloatRegisters::Double;
  }
  bool isSimd128() const {
    MOZ_ASSERT(!invalid_);
    return false;
  }
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

  constexpr uint32_t size() const {
    MOZ_ASSERT(!invalid_);
    if (kind_ == FloatRegisters::Double) {
      return sizeof(double);
    }
    MOZ_ASSERT(kind_ == FloatRegisters::Single);
    return sizeof(float);
  }

  constexpr Code code() const {
    // assert(!invalid_);
    return Codes::fromParts(encoding_, kind_, invalid_);
  }

  constexpr Encoding encoding() const {
    MOZ_ASSERT(!invalid_);
    return Encoding(encoding_);
  }

  const char* name() const { return FloatRegisters::GetName(code()); }
  bool volatile_() const {
    MOZ_ASSERT(!invalid_);
    return !!((SetType(1) << code()) & FloatRegisters::VolatileMask);
  }
  constexpr bool operator!=(FloatRegister other) const {
    return code() != other.code();
  }
  constexpr bool operator==(FloatRegister other) const {
    return code() == other.code();
  }

  bool aliases(FloatRegister other) const {
    return other.encoding_ == encoding_;
  }
  // Ensure that two floating point registers' types are equivalent.
  bool equiv(FloatRegister other) const {
    MOZ_ASSERT(!invalid_);
    return kind_ == other.kind_;
  }

  uint32_t numAliased() const { return Codes::NumTypes; }
  uint32_t numAlignedAliased() { return numAliased(); }

  FloatRegister aliased(uint32_t aliasIdx) {
    MOZ_ASSERT(!invalid_);
    MOZ_ASSERT(aliasIdx < numAliased());
    return FloatRegister(Encoding(encoding_),
                         Kind((aliasIdx + kind_) % numAliased()));
  }
  FloatRegister alignedAliased(uint32_t aliasIdx) {
    MOZ_ASSERT(aliasIdx < numAliased());
    return aliased(aliasIdx);
  }
  SetType alignedOrDominatedAliasedSet() const {
    return Codes::Spread << encoding_;
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
FloatRegister::LiveAsIndexableSet<RegTypeName::Any>(SetType set) {
  return set;
}

// LoongArch doesn't have double registers that cannot be treated as float32.
inline bool hasUnaliasedDouble() { return false; }

// LoongArch doesn't have double registers that alias multiple floats.
inline bool hasMultiAlias() { return false; }

uint32_t GetLOONG64Flags();

}  // namespace jit
}  // namespace js

#endif /* jit_loong64_Architecture_loong64_h */

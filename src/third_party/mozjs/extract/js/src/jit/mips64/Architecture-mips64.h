/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_Architecture_mips64_h
#define jit_mips64_Architecture_mips64_h

#include "mozilla/MathAlgorithms.h"

#include <limits.h>
#include <stdint.h>

#include "jit/mips-shared/Architecture-mips-shared.h"

#include "js/Utility.h"

namespace js {
namespace jit {

// Shadow stack space is not required on MIPS64.
static const uint32_t ShadowStackSpace = 0;

// MIPS64 have 64 bit floating-point coprocessor. There are 32 double
// precision register which can also be used as single precision registers.
class FloatRegisters : public FloatRegistersMIPSShared {
 public:
  enum ContentType { Single, Double, NumTypes };

  static const char* GetName(uint32_t i) {
    MOZ_ASSERT(i < TotalPhys);
    return FloatRegistersMIPSShared::GetName(Encoding(i));
  }

  static Encoding FromName(const char* name);

  static const uint32_t Total = 32 * NumTypes;
#ifdef MIPSR6
  static const uint32_t Allocatable = 60;
#else
  static const uint32_t Allocatable = 62;
#endif
  // When saving all registers we only need to do is save double registers.
  static const uint32_t TotalPhys = 32;

  static_assert(sizeof(SetType) * 8 >= Total,
                "SetType should be large enough to enumerate all registers.");

  // Magic values which are used to duplicate a mask of physical register for
  // a specific type of register. A multiplication is used to copy and shift
  // the bits of the physical register mask.
  static const SetType SpreadSingle = SetType(1)
                                      << (uint32_t(Single) * TotalPhys);
  static const SetType SpreadDouble = SetType(1)
                                      << (uint32_t(Double) * TotalPhys);
  static const SetType SpreadScalar = SpreadSingle | SpreadDouble;
  static const SetType SpreadVector = 0;
  static const SetType Spread = SpreadScalar | SpreadVector;

  static const SetType AllPhysMask = ((SetType(1) << TotalPhys) - 1);
  static const SetType AllMask = AllPhysMask * Spread;
  static const SetType AllSingleMask = AllPhysMask * SpreadSingle;
  static const SetType AllDoubleMask = AllPhysMask * SpreadDouble;

  static const SetType NonVolatileMask =
      ((1U << FloatRegisters::f24) | (1U << FloatRegisters::f25) |
       (1U << FloatRegisters::f26) | (1U << FloatRegisters::f27) |
       (1U << FloatRegisters::f28) | (1U << FloatRegisters::f29) |
       (1U << FloatRegisters::f30) | (1U << FloatRegisters::f31)) *
          SpreadScalar |
      AllPhysMask * SpreadVector;

  static const SetType VolatileMask = AllMask & ~NonVolatileMask;

  static const SetType WrapperMask = VolatileMask;

#ifdef MIPSR6
  static const SetType NonAllocatableMask =
      ((1U << FloatRegisters::f23) | (1U << FloatRegisters::f24)) * Spread;
#else
  static const SetType NonAllocatableMask =
      (1U << FloatRegisters::f23) * Spread;
#endif

  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

template <typename T>
class TypedRegisterSet;

class FloatRegister : public FloatRegisterMIPSShared {
 public:
  typedef FloatRegisters Codes;
  typedef size_t Code;
  typedef Codes::Encoding Encoding;
  typedef Codes::ContentType ContentType;

  Encoding reg_ : 6;

 private:
  ContentType kind_ : 3;

 public:
  constexpr FloatRegister(uint32_t r, ContentType kind = Codes::Double)
      : reg_(Encoding(r)), kind_(kind) {}
  constexpr FloatRegister()
      : reg_(Encoding(FloatRegisters::invalid_freg)), kind_(Codes::Double) {}

  static uint32_t SetSize(SetType x) {
    // Count the number of non-aliased registers.
    x |= x >> Codes::TotalPhys;
    x &= Codes::AllPhysMask;
    static_assert(Codes::AllPhysMask <= 0xffffffff,
                  "We can safely use CountPopulation32");
    return mozilla::CountPopulation32(x);
  }

  bool operator==(const FloatRegister& other) const {
    MOZ_ASSERT(!isInvalid());
    MOZ_ASSERT(!other.isInvalid());
    return kind_ == other.kind_ && reg_ == other.reg_;
  }
  bool equiv(const FloatRegister& other) const { return other.kind_ == kind_; }
  size_t size() const {
    return (kind_ == Codes::Double) ? sizeof(double) : sizeof(float);
  }
  // Always push doubles to maintain 8-byte stack alignment.
  size_t pushSize() const { return sizeof(double); }
  bool isInvalid() const { return reg_ == FloatRegisters::invalid_freg; }

  bool isSingle() const { return kind_ == Codes::Single; }
  bool isDouble() const { return kind_ == Codes::Double; }
  bool isSimd128() const { return false; }

  FloatRegister singleOverlay() const;
  FloatRegister doubleOverlay() const;

  FloatRegister asSingle() const { return singleOverlay(); }
  FloatRegister asDouble() const { return doubleOverlay(); }
  FloatRegister asSimd128() const { MOZ_CRASH("NYI"); }

  Code code() const {
    MOZ_ASSERT(!isInvalid());
    return Code(reg_ | (kind_ << 5));
  }
  Encoding encoding() const {
    MOZ_ASSERT(!isInvalid());
    MOZ_ASSERT(uint32_t(reg_) < Codes::TotalPhys);
    return reg_;
  }
  uint32_t id() const { return reg_; }
  static FloatRegister FromCode(uint32_t i) {
    uint32_t code = i & 0x1f;
    uint32_t kind = i >> 5;
    return FloatRegister(Code(code), ContentType(kind));
  }

  bool volatile_() const {
    return !!((1 << reg_) & FloatRegisters::VolatileMask);
  }
  const char* name() const { return FloatRegisters::GetName(reg_); }
  bool operator!=(const FloatRegister& other) const {
    return kind_ != other.kind_ || reg_ != other.reg_;
  }
  bool aliases(const FloatRegister& other) { return reg_ == other.reg_; }
  uint32_t numAliased() const { return 2; }
  FloatRegister aliased(uint32_t aliasIdx) {
    if (aliasIdx == 0) {
      return *this;
    }
    MOZ_ASSERT(aliasIdx == 1);
    if (isDouble()) {
      return singleOverlay();
    }
    return doubleOverlay();
  }
  uint32_t numAlignedAliased() const { return 2; }
  FloatRegister alignedAliased(uint32_t aliasIdx) {
    MOZ_ASSERT(isDouble());
    if (aliasIdx == 0) {
      return *this;
    }
    MOZ_ASSERT(aliasIdx == 1);
    return singleOverlay();
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

  static Code FromName(const char* name) {
    return FloatRegisters::FromName(name);
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

}  // namespace jit
}  // namespace js

#endif /* jit_mips64_Architecture_mips64_h */

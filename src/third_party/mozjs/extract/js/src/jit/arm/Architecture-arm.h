/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_Architecture_arm_h
#define jit_arm_Architecture_arm_h

#include "mozilla/MathAlgorithms.h"

#include <algorithm>
#include <limits.h>
#include <stdint.h>

#include "jit/shared/Architecture-shared.h"

#include "js/Utility.h"

// GCC versions 4.6 and above define __ARM_PCS_VFP to denote a hard-float
// ABI target. The iOS toolchain doesn't define anything specific here,
// but iOS always supports VFP.
#if defined(__ARM_PCS_VFP) || defined(XP_IOS)
#  define JS_CODEGEN_ARM_HARDFP
#endif

namespace js {
namespace jit {

// These offsets are specific to nunboxing, and capture offsets into the
// components of a js::Value.
static const int32_t NUNBOX32_TYPE_OFFSET = 4;
static const int32_t NUNBOX32_PAYLOAD_OFFSET = 0;

static constexpr uint32_t ShadowStackSpace = 0;

// How far forward/back can a jump go? Provide a generous buffer for thunks.
static const uint32_t JumpImmediateRange = 20 * 1024 * 1024;

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
    fp = r11,
    r12,
    ip = r12,
    r13,
    sp = r13,
    r14,
    lr = r14,
    r15,
    pc = r15,
    invalid_reg
  };
  typedef uint8_t Code;
  typedef RegisterID Encoding;

  // Content spilled during bailouts.
  union RegisterContent {
    uintptr_t r;
  };

  static const char* GetName(Code code) {
    MOZ_ASSERT(code < Total);
    static const char* const Names[] = {"r0",  "r1", "r2",  "r3", "r4",  "r5",
                                        "r6",  "r7", "r8",  "r9", "r10", "r11",
                                        "r12", "sp", "r14", "pc"};
    return Names[code];
  }
  static const char* GetName(Encoding i) { return GetName(Code(i)); }

  static Code FromName(const char* name);

  static const Encoding StackPointer = sp;
  static const Encoding Invalid = invalid_reg;

  static const uint32_t Total = 16;
  static const uint32_t Allocatable = 13;

  typedef uint32_t SetType;

  static const SetType AllMask = (1 << Total) - 1;
  static const SetType ArgRegMask =
      (1 << r0) | (1 << r1) | (1 << r2) | (1 << r3);

  static const SetType VolatileMask =
      (1 << r0) | (1 << r1) | (1 << Registers::r2) |
      (1 << Registers::r3)
#if defined(XP_IOS)
      // per
      // https://developer.apple.com/library/ios/documentation/Xcode/Conceptual/iPhoneOSABIReference/Articles/ARMv6FunctionCallingConventions.html#//apple_ref/doc/uid/TP40009021-SW4
      | (1 << Registers::r9)
#endif
      ;

  static const SetType NonVolatileMask =
      (1 << Registers::r4) | (1 << Registers::r5) | (1 << Registers::r6) |
      (1 << Registers::r7) | (1 << Registers::r8) |
#if !defined(XP_IOS)
      (1 << Registers::r9) |
#endif
      (1 << Registers::r10) | (1 << Registers::r11) | (1 << Registers::r12) |
      (1 << Registers::r14);

  static const SetType WrapperMask = VolatileMask |          // = arguments
                                     (1 << Registers::r4) |  // = outReg
                                     (1 << Registers::r5);   // = argBase

  static const SetType NonAllocatableMask =
      (1 << Registers::sp) | (1 << Registers::r12) |  // r12 = ip = scratch
      (1 << Registers::lr) | (1 << Registers::pc) | (1 << Registers::fp);

  // Registers returned from a JS -> JS call.
  static const SetType JSCallMask = (1 << Registers::r2) | (1 << Registers::r3);

  // Registers returned from a JS -> C call.
  static const SetType CallMask =
      (1 << Registers::r0) |
      (1 << Registers::r1);  // Used for double-size returns.

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
typedef uint16_t PackedRegisterMask;

class FloatRegisters {
 public:
  enum FPRegisterID {
    s0,
    s1,
    s2,
    s3,
    s4,
    s5,
    s6,
    s7,
    s8,
    s9,
    s10,
    s11,
    s12,
    s13,
    s14,
    s15,
    s16,
    s17,
    s18,
    s19,
    s20,
    s21,
    s22,
    s23,
    s24,
    s25,
    s26,
    s27,
    s28,
    s29,
    s30,
    s31,
    d0,
    d1,
    d2,
    d3,
    d4,
    d5,
    d6,
    d7,
    d8,
    d9,
    d10,
    d11,
    d12,
    d13,
    d14,
    d15,
    d16,
    d17,
    d18,
    d19,
    d20,
    d21,
    d22,
    d23,
    d24,
    d25,
    d26,
    d27,
    d28,
    d29,
    d30,
    d31,
    invalid_freg
  };

  typedef uint32_t Code;
  typedef FPRegisterID Encoding;

  // Content spilled during bailouts.
  union RegisterContent {
    double d;
  };

  static const char* GetDoubleName(Encoding code) {
    static const char* const Names[] = {
        "d0",  "d1",  "d2",  "d3",  "d4",  "d5",  "d6",  "d7",
        "d8",  "d9",  "d10", "d11", "d12", "d13", "d14", "d15",
        "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
        "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31"};
    return Names[code];
  }
  static const char* GetSingleName(Encoding code) {
    static const char* const Names[] = {
        "s0",  "s1",  "s2",  "s3",  "s4",  "s5",  "s6",  "s7",
        "s8",  "s9",  "s10", "s11", "s12", "s13", "s14", "s15",
        "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
        "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31"};
    return Names[code];
  }

  static Code FromName(const char* name);

  static const Encoding Invalid = invalid_freg;
  static const uint32_t Total = 48;
  static const uint32_t TotalDouble = 16;
  static const uint32_t TotalSingle = 32;
  static const uint32_t Allocatable = 45;
  // There are only 32 places that we can put values.
  static const uint32_t TotalPhys = 32;
  static uint32_t ActualTotalPhys();

  /* clang-format off */
    // ARM float registers overlap in a way that for 1 double registers, in the
    // range d0-d15, we have 2 singles register in the range s0-s31. d16-d31
    // have no single register aliases.  The aliasing rule state that d{n}
    // aliases s{2n} and s{2n+1}, for n in [0 .. 15].
    //
    // The register set is used to represent either allocatable register or live
    // registers. The register maps d0-d15 and s0-s31 to a single bit each. The
    // registers d16-d31 are not used at the moment.
    //
    // uuuu uuuu uuuu uuuu dddd dddd dddd dddd ssss ssss ssss ssss ssss ssss ssss ssss
    //                     ^                 ^ ^                                     ^
    //                     '-- d15      d0 --' '-- s31                          s0 --'
    //
    // LiveSet are handled by adding the bit of each register without
    // considering the aliases.
    //
    // AllocatableSet are handled by adding and removing the bit of each
    // aligned-or-dominated-aliased registers.
    //
    //     ...0...00... : s{2n}, s{2n+1} and d{n} are not available
    //     ...1...01... : s{2n} is available (*)
    //     ...0...10... : s{2n+1} is available
    //     ...1...11... : s{2n}, s{2n+1} and d{n} are available
    //
    // (*) Note that d{n} bit is set, but is not available because s{2n+1} bit
    // is not set, which is required as d{n} dominates s{2n+1}. The d{n} bit is
    // set, because s{2n} is aligned.
    //
    //        |        d{n}       |
    //        | s{2n+1} |  s{2n}  |
    //
  /* clang-format on */
  typedef uint64_t SetType;
  static const SetType AllSingleMask = (1ull << TotalSingle) - 1;
  static const SetType AllDoubleMask = ((1ull << TotalDouble) - 1)
                                       << TotalSingle;
  static const SetType AllMask = AllDoubleMask | AllSingleMask;

  // d15 is the ScratchFloatReg.
  static const SetType NonVolatileDoubleMask =
      ((1ULL << d8) | (1ULL << d9) | (1ULL << d10) | (1ULL << d11) |
       (1ULL << d12) | (1ULL << d13) | (1ULL << d14));
  // s30 and s31 alias d15.
  static const SetType NonVolatileMask =
      (NonVolatileDoubleMask |
       ((1 << s16) | (1 << s17) | (1 << s18) | (1 << s19) | (1 << s20) |
        (1 << s21) | (1 << s22) | (1 << s23) | (1 << s24) | (1 << s25) |
        (1 << s26) | (1 << s27) | (1 << s28) | (1 << s29) | (1 << s30)));

  static const SetType VolatileMask = AllMask & ~NonVolatileMask;
  static const SetType VolatileDoubleMask =
      AllDoubleMask & ~NonVolatileDoubleMask;

  static const SetType WrapperMask = VolatileMask;

  // d15 is the ARM scratch float register.
  // s30 and s31 alias d15.
  static const SetType NonAllocatableMask =
      ((1ULL << d15)) | (1ULL << s30) | (1ULL << s31);

  static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

static const uint32_t SpillSlotSize =
    std::max(sizeof(Registers::RegisterContent),
             sizeof(FloatRegisters::RegisterContent));

template <typename T>
class TypedRegisterSet;

class VFPRegister {
 public:
  // What type of data is being stored in this register? UInt / Int are
  // specifically for vcvt, where we need to know how the data is supposed to
  // be converted.
  enum RegType : uint8_t { Single = 0x0, Double = 0x1, UInt = 0x2, Int = 0x3 };

  typedef FloatRegisters Codes;
  typedef Codes::Code Code;
  typedef Codes::Encoding Encoding;

  // Bitfields below are all uint32_t to make sure MSVC packs them correctly.
 public:
  // ARM doesn't have more than 32 registers of each type, so 5 bits should
  // suffice.
  uint32_t code_ : 5;

 protected:
  uint32_t kind : 2;
  uint32_t _isInvalid : 1;
  uint32_t _isMissing : 1;

 public:
  constexpr VFPRegister(uint32_t r, RegType k)
      : code_(Code(r)), kind(k), _isInvalid(false), _isMissing(false) {}
  constexpr VFPRegister()
      : code_(Code(0)), kind(Double), _isInvalid(true), _isMissing(false) {}

  constexpr VFPRegister(RegType k, uint32_t id, bool invalid, bool missing)
      : code_(Code(id)), kind(k), _isInvalid(invalid), _isMissing(missing) {}

  explicit constexpr VFPRegister(Code id)
      : code_(id), kind(Double), _isInvalid(false), _isMissing(false) {}
  bool operator==(const VFPRegister& other) const {
    return kind == other.kind && code_ == other.code_ &&
           isInvalid() == other.isInvalid();
  }
  bool operator!=(const VFPRegister& other) const { return !operator==(other); }

  bool isSingle() const { return kind == Single; }
  bool isDouble() const { return kind == Double; }
  bool isSimd128() const { return false; }
  bool isFloat() const { return (kind == Double) || (kind == Single); }
  bool isInt() const { return (kind == UInt) || (kind == Int); }
  bool isSInt() const { return kind == Int; }
  bool isUInt() const { return kind == UInt; }
  bool equiv(const VFPRegister& other) const { return other.kind == kind; }
  size_t size() const { return (kind == Double) ? 8 : 4; }
  bool isInvalid() const { return _isInvalid; }
  bool isMissing() const {
    MOZ_ASSERT(!_isInvalid);
    return _isMissing;
  }

  VFPRegister doubleOverlay(unsigned int which = 0) const;
  VFPRegister singleOverlay(unsigned int which = 0) const;
  VFPRegister sintOverlay(unsigned int which = 0) const;
  VFPRegister uintOverlay(unsigned int which = 0) const;

  VFPRegister asSingle() const { return singleOverlay(); }
  VFPRegister asDouble() const { return doubleOverlay(); }
  VFPRegister asSimd128() const { MOZ_CRASH("NYI"); }

  struct VFPRegIndexSplit;
  VFPRegIndexSplit encode();

  // For serializing values.
  struct VFPRegIndexSplit {
    const uint32_t block : 4;
    const uint32_t bit : 1;

   private:
    friend VFPRegIndexSplit js::jit::VFPRegister::encode();

    VFPRegIndexSplit(uint32_t block_, uint32_t bit_)
        : block(block_), bit(bit_) {
      MOZ_ASSERT(block == block_);
      MOZ_ASSERT(bit == bit_);
    }
  };

  Code code() const {
    MOZ_ASSERT(!_isInvalid && !_isMissing);
    // This should only be used in areas where we only have doubles and
    // singles.
    MOZ_ASSERT(isFloat());
    return Code(code_ | (kind << 5));
  }
  Encoding encoding() const {
    MOZ_ASSERT(!_isInvalid && !_isMissing);
    return Encoding(code_);
  }
  uint32_t id() const { return code_; }
  static VFPRegister FromCode(uint32_t i) {
    uint32_t code = i & 31;
    uint32_t kind = i >> 5;
    return VFPRegister(code, RegType(kind));
  }
  bool volatile_() const {
    if (isDouble()) {
      return !!((1ULL << (code_ >> 1)) & FloatRegisters::VolatileMask);
    }
    return !!((1ULL << code_) & FloatRegisters::VolatileMask);
  }
  const char* name() const {
    if (isDouble()) {
      return FloatRegisters::GetDoubleName(Encoding(code_));
    }
    return FloatRegisters::GetSingleName(Encoding(code_));
  }
  bool aliases(const VFPRegister& other) {
    if (kind == other.kind) {
      return code_ == other.code_;
    }
    return doubleOverlay() == other.doubleOverlay();
  }
  static const int NumAliasedDoubles = 16;
  uint32_t numAliased() const {
    if (isDouble()) {
      if (code_ < NumAliasedDoubles) {
        return 3;
      }
      return 1;
    }
    return 2;
  }

  VFPRegister aliased(uint32_t aliasIdx) {
    if (aliasIdx == 0) {
      return *this;
    }
    if (isDouble()) {
      MOZ_ASSERT(code_ < NumAliasedDoubles);
      MOZ_ASSERT(aliasIdx <= 2);
      return singleOverlay(aliasIdx - 1);
    }
    MOZ_ASSERT(aliasIdx == 1);
    return doubleOverlay(aliasIdx - 1);
  }
  uint32_t numAlignedAliased() const {
    if (isDouble()) {
      if (code_ < NumAliasedDoubles) {
        return 2;
      }
      return 1;
    }
    // s1 has 0 other aligned aliases, 1 total.
    // s0 has 1 other aligned aliase, 2 total.
    return 2 - (code_ & 1);
  }
  // |   d0    |
  // | s0 | s1 |
  // If we've stored s0 and s1 in memory, we also want to say that d0 is
  // stored there, but it is only stored at the location where it is aligned
  // e.g. at s0, not s1.
  VFPRegister alignedAliased(uint32_t aliasIdx) {
    if (aliasIdx == 0) {
      return *this;
    }
    MOZ_ASSERT(aliasIdx == 1);
    if (isDouble()) {
      MOZ_ASSERT(code_ < NumAliasedDoubles);
      return singleOverlay(aliasIdx - 1);
    }
    MOZ_ASSERT((code_ & 1) == 0);
    return doubleOverlay(aliasIdx - 1);
  }

  typedef FloatRegisters::SetType SetType;

  // This function is used to ensure that Register set can take all Single
  // registers, even if we are taking a mix of either double or single
  // registers.
  //
  //   s0.alignedOrDominatedAliasedSet() == s0 | d0.
  //   s1.alignedOrDominatedAliasedSet() == s1.
  //   d0.alignedOrDominatedAliasedSet() == s0 | s1 | d0.
  //
  // This way the Allocatable register set does not have to do any arithmetics
  // to know if a register is available or not, as we have the following
  // relations:
  //
  //  d0.alignedOrDominatedAliasedSet() ==
  //      s0.alignedOrDominatedAliasedSet() | s1.alignedOrDominatedAliasedSet()
  //
  //  s0.alignedOrDominatedAliasedSet() & s1.alignedOrDominatedAliasedSet() == 0
  //
  SetType alignedOrDominatedAliasedSet() const {
    if (isSingle()) {
      if (code_ % 2 != 0) {
        return SetType(1) << code_;
      }
      return (SetType(1) << code_) | (SetType(1) << (32 + code_ / 2));
    }

    MOZ_ASSERT(isDouble());
    return (SetType(0b11) << (code_ * 2)) | (SetType(1) << (32 + code_));
  }

  static constexpr RegTypeName DefaultType = RegTypeName::Float64;

  template <RegTypeName = DefaultType>
  static SetType LiveAsIndexableSet(SetType s) {
    return SetType(0);
  }

  template <RegTypeName Name = DefaultType>
  static SetType AllocatableAsIndexableSet(SetType s) {
    static_assert(Name != RegTypeName::Any, "Allocatable set are not iterable");
    return SetType(0);
  }

  static uint32_t SetSize(SetType x) {
    static_assert(sizeof(SetType) == 8, "SetType must be 64 bits");
    return mozilla::CountPopulation32(x);
  }
  static Code FromName(const char* name) {
    return FloatRegisters::FromName(name);
  }
  static TypedRegisterSet<VFPRegister> ReduceSetForPush(
      const TypedRegisterSet<VFPRegister>& s);
  static uint32_t GetPushSizeInBytes(const TypedRegisterSet<VFPRegister>& s);
  uint32_t getRegisterDumpOffsetInBytes();
  static uint32_t FirstBit(SetType x) {
    return mozilla::CountTrailingZeroes64(x);
  }
  static uint32_t LastBit(SetType x) {
    return 63 - mozilla::CountLeadingZeroes64(x);
  }
};

template <>
inline VFPRegister::SetType
VFPRegister::LiveAsIndexableSet<RegTypeName::Float32>(SetType set) {
  return set & FloatRegisters::AllSingleMask;
}

template <>
inline VFPRegister::SetType
VFPRegister::LiveAsIndexableSet<RegTypeName::Float64>(SetType set) {
  return set & FloatRegisters::AllDoubleMask;
}

template <>
inline VFPRegister::SetType VFPRegister::LiveAsIndexableSet<RegTypeName::Any>(
    SetType set) {
  return set;
}

template <>
inline VFPRegister::SetType
VFPRegister::AllocatableAsIndexableSet<RegTypeName::Float32>(SetType set) {
  // Single registers are not dominating any smaller registers, thus masking
  // is enough to convert an allocatable set into a set of register list all
  // single register available.
  return set & FloatRegisters::AllSingleMask;
}

template <>
inline VFPRegister::SetType
VFPRegister::AllocatableAsIndexableSet<RegTypeName::Float64>(SetType set) {
  /* clang-format off */
    // An allocatable float register set is represented as follow:
    //
    // uuuu uuuu uuuu uuuu dddd dddd dddd dddd ssss ssss ssss ssss ssss ssss ssss ssss
    //                     ^                 ^ ^                                     ^
    //                     '-- d15      d0 --' '-- s31                          s0 --'
    //
    //     ...0...00... : s{2n}, s{2n+1} and d{n} are not available
    //     ...1...01... : s{2n} is available
    //     ...0...10... : s{2n+1} is available
    //     ...1...11... : s{2n}, s{2n+1} and d{n} are available
    //
    // The goal of this function is to return the set of double registers which
    // are available as an indexable bit set. This implies that iff a double bit
    // is set in the returned set, then the register is available.
    //
    // To do so, this functions converts the 32 bits set of single registers
    // into a 16 bits set of equivalent double registers. Then, we mask out
    // double registers which do not have all the single register that compose
    // them. As d{n} bit is set when s{2n} is available, we only need to take
    // s{2n+1} into account.
  /* clang-format on */

  // Convert  s7s6s5s4 s3s2s1s0  into  s7s5s3s1, for all s0-s31.
  SetType s2d = AllocatableAsIndexableSet<RegTypeName::Float32>(set);
  static_assert(FloatRegisters::TotalSingle == 32, "Wrong mask");
  s2d = (0xaaaaaaaa & s2d) >> 1;  // Filter s{2n+1} registers.
  // Group adjacent bits as follow:
  //     0.0.s3.s1 == ((0.s3.0.s1) >> 1 | (0.s3.0.s1)) & 0b0011;
  s2d = ((s2d >> 1) | s2d) & 0x33333333;  // 0a0b --> 00ab
  s2d = ((s2d >> 2) | s2d) & 0x0f0f0f0f;  // 00ab00cd --> 0000abcd
  s2d = ((s2d >> 4) | s2d) & 0x00ff00ff;
  s2d = ((s2d >> 8) | s2d) & 0x0000ffff;
  // Move the s7s5s3s1 to the aliased double positions.
  s2d = s2d << FloatRegisters::TotalSingle;

  // Note: We currently do not use any representation for d16-d31.
  static_assert(FloatRegisters::TotalDouble == 16,
                "d16-d31 do not have a single register mapping");

  // Filter out any double register which are not allocatable due to
  // non-aligned dominated single registers.
  return set & s2d;
}

// The only floating point register set that we work with are the VFP Registers.
typedef VFPRegister FloatRegister;

uint32_t GetARMFlags();
bool HasARMv7();
bool HasMOVWT();
bool HasLDSTREXBHD();  // {LD,ST}REX{B,H,D}
bool HasDMBDSBISB();   // DMB, DSB, and ISB
bool HasVFPv3();
bool HasVFP();
bool Has32DP();
bool HasIDIV();
bool HasNEON();

extern volatile uint32_t armHwCapFlags;

// Not part of the HWCAP flag, but we need to know these and these bits are not
// used. Define these here so that their use can be inlined by the simulator.

// A bit to flag when signaled alignment faults are to be fixed up.
#define HWCAP_FIXUP_FAULT (1 << 24)

// A bit to flag when the flags are uninitialized, so they can be atomically
// set.
#define HWCAP_UNINITIALIZED (1 << 25)

// A bit to flag when alignment faults are enabled and signal.
#define HWCAP_ALIGNMENT_FAULT (1 << 26)

// A bit to flag the use of the hardfp ABI.
#define HWCAP_USE_HARDFP_ABI (1 << 27)

// A bit to flag the use of the ARMv7 arch, otherwise ARMv6.
#define HWCAP_ARMv7 (1 << 28)

// Top three bits are reserved, do not use them.

// Returns true when cpu alignment faults are enabled and signaled, and thus we
// should ensure loads and stores are aligned.
inline bool HasAlignmentFault() {
  MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
  return armHwCapFlags & HWCAP_ALIGNMENT_FAULT;
}

#ifdef JS_SIMULATOR_ARM
// Returns true when cpu alignment faults will be fixed up by the
// "operating system", which functionality we will emulate.
inline bool FixupFault() {
  MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
  return armHwCapFlags & HWCAP_FIXUP_FAULT;
}
#endif

// Arm/D32 has double registers that can NOT be treated as float32 and this
// requires some dances in lowering.
inline bool hasUnaliasedDouble() { return Has32DP(); }

// On ARM, Dn aliases both S2n and S2n+1, so if you need to convert a float32 to
// a double as a temporary, you need a temporary double register.
inline bool hasMultiAlias() { return true; }

// InitARMFlags is called from the JitContext constructor to read the hardware
// flags.  The call is a no-op after the first call, or if the JS shell has
// already set the flags (it has a command line switch for this, see
// ParseARMHwCapFlags).
//
// If the environment variable ARMHWCAP is set then the flags are read from it
// instead; see ParseARMHwCapFlags.
void InitARMFlags();

// Register a string denoting ARM hardware flags. During engine initialization,
// these flags will then be used instead of the actual hardware capabilities.
// This must be called before JS_Init and the passed string's buffer must
// outlive the JS_Init call.
void SetARMHwCapFlagsString(const char* armHwCap);

// Retrive the ARM hardware flags at a bitmask.  They must have been set.
uint32_t GetARMFlags();

// If the simulator is used then the ABI choice is dynamic. Otherwise the ABI is
// static and useHardFpABI is inlined so that unused branches can be optimized
// away.
#ifdef JS_SIMULATOR_ARM
bool UseHardFpABI();
#else
static inline bool UseHardFpABI() {
#  if defined(JS_CODEGEN_ARM_HARDFP)
  return true;
#  else
  return false;
#  endif
}
#endif

// In order to handle SoftFp ABI calls, we need to be able to express that we
// have ABIArg which are represented by pair of general purpose registers.
#define JS_CODEGEN_REGISTER_PAIR 1

}  // namespace jit
}  // namespace js

#endif /* jit_arm_Architecture_arm_h */

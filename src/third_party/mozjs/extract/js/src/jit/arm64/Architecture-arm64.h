/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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
//
//  x31 (or, more accurately, the integer register with encoding 31, since
//  there is no x31 per se) is special and functions as both the stack pointer
//  and a zero register.
//
//  The bottom 32 bits of each of the X registers is accessible as w0 through
//  w31. The program counter is not accessible as a register.
//
// SIMD and scalar floating-point registers share a register bank.
//  32 bit float registers are s0 through s31.
//  64 bit double registers are d0 through d31.
//  128 bit SIMD registers are v0 through v31.
//  e.g., s0 is the bottom 32 bits of d0, which is the bottom 64 bits of v0.

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
    w0 = 0,
    x0 = 0,
    w1 = 1,
    x1 = 1,
    w2 = 2,
    x2 = 2,
    w3 = 3,
    x3 = 3,
    w4 = 4,
    x4 = 4,
    w5 = 5,
    x5 = 5,
    w6 = 6,
    x6 = 6,
    w7 = 7,
    x7 = 7,
    w8 = 8,
    x8 = 8,
    w9 = 9,
    x9 = 9,
    w10 = 10,
    x10 = 10,
    w11 = 11,
    x11 = 11,
    w12 = 12,
    x12 = 12,
    w13 = 13,
    x13 = 13,
    w14 = 14,
    x14 = 14,
    w15 = 15,
    x15 = 15,
    w16 = 16,
    x16 = 16,
    ip0 = 16,  // MacroAssembler scratch register 1.
    w17 = 17,
    x17 = 17,
    ip1 = 17,  // MacroAssembler scratch register 2.
    w18 = 18,
    x18 = 18,
    tls = 18,  // Platform-specific use (TLS).
    w19 = 19,
    x19 = 19,
    w20 = 20,
    x20 = 20,
    w21 = 21,
    x21 = 21,
    w22 = 22,
    x22 = 22,
    w23 = 23,
    x23 = 23,
    w24 = 24,
    x24 = 24,
    w25 = 25,
    x25 = 25,
    w26 = 26,
    x26 = 26,
    w27 = 27,
    x27 = 27,
    w28 = 28,
    x28 = 28,
    w29 = 29,
    x29 = 29,
    fp = 29,
    w30 = 30,
    x30 = 30,
    lr = 30,
    w31 = 31,
    x31 = 31,
    wzr = 31,
    xzr = 31,
    sp = 31,  // Special: both stack pointer and a zero register.
  };
  typedef uint8_t Code;
  typedef uint32_t Encoding;
  typedef uint32_t SetType;

  static const Code Invalid = 0xFF;

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
        "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",
        "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",
        "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
        "x24", "x25", "x26", "x27", "x28", "x29", "lr",  "sp"};
    static_assert(Total == sizeof(Names) / sizeof(Names[0]),
                  "Table is the correct size");
    if (code >= Total) {
      return "invalid";
    }
    return Names[code];
  }

  static Code FromName(const char* name);

  static const uint32_t Total = 32;
  static const uint32_t TotalPhys = 32;
  static const uint32_t Allocatable =
      27;  // No named special-function registers.

  static const SetType AllMask = 0xFFFFFFFF;
  static const SetType NoneMask = 0x0;

  static const SetType ArgRegMask =
      (1 << Registers::x0) | (1 << Registers::x1) | (1 << Registers::x2) |
      (1 << Registers::x3) | (1 << Registers::x4) | (1 << Registers::x5) |
      (1 << Registers::x6) | (1 << Registers::x7) | (1 << Registers::x8);

  static const SetType VolatileMask =
      (1 << Registers::x0) | (1 << Registers::x1) | (1 << Registers::x2) |
      (1 << Registers::x3) | (1 << Registers::x4) | (1 << Registers::x5) |
      (1 << Registers::x6) | (1 << Registers::x7) | (1 << Registers::x8) |
      (1 << Registers::x9) | (1 << Registers::x10) | (1 << Registers::x11) |
      (1 << Registers::x12) | (1 << Registers::x13) | (1 << Registers::x14) |
      (1 << Registers::x15) | (1 << Registers::x16) | (1 << Registers::x17) |
      (1 << Registers::x18);

  static const SetType NonVolatileMask =
      (1 << Registers::x19) | (1 << Registers::x20) | (1 << Registers::x21) |
      (1 << Registers::x22) | (1 << Registers::x23) | (1 << Registers::x24) |
      (1 << Registers::x25) | (1 << Registers::x26) | (1 << Registers::x27) |
      (1 << Registers::x28) | (1 << Registers::x29) | (1 << Registers::x30);

  static const SetType SingleByteRegs = VolatileMask | NonVolatileMask;

  static const SetType NonAllocatableMask =
      (1 << Registers::x28) |  // PseudoStackPointer.
      (1 << Registers::ip0) |  // First scratch register.
      (1 << Registers::ip1) |  // Second scratch register.
      (1 << Registers::tls) | (1 << Registers::lr) | (1 << Registers::sp);

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

// 128-bit bitset for FloatRegisters::SetType.

class Bitset128 {
  // The order (hi, lo) looks best in the debugger.
  uint64_t hi, lo;

 public:
  MOZ_IMPLICIT constexpr Bitset128(uint64_t initial) : hi(0), lo(initial) {}
  MOZ_IMPLICIT constexpr Bitset128(const Bitset128& that)
      : hi(that.hi), lo(that.lo) {}

  constexpr Bitset128(uint64_t hi, uint64_t lo) : hi(hi), lo(lo) {}

  constexpr uint64_t high() const { return hi; }

  constexpr uint64_t low() const { return lo; }

  constexpr Bitset128 operator|(Bitset128 that) const {
    return Bitset128(hi | that.hi, lo | that.lo);
  }

  constexpr Bitset128 operator&(Bitset128 that) const {
    return Bitset128(hi & that.hi, lo & that.lo);
  }

  constexpr Bitset128 operator^(Bitset128 that) const {
    return Bitset128(hi ^ that.hi, lo ^ that.lo);
  }

  constexpr Bitset128 operator~() const { return Bitset128(~hi, ~lo); }

  // We must avoid shifting by the word width, which is complex.  Inlining plus
  // shift-by-constant will remove a lot of code in the normal case.

  constexpr Bitset128 operator<<(size_t shift) const {
    if (shift == 0) {
      return *this;
    }
    if (shift < 64) {
      return Bitset128((hi << shift) | (lo >> (64 - shift)), lo << shift);
    }
    if (shift == 64) {
      return Bitset128(lo, 0);
    }
    return Bitset128(lo << (shift - 64), 0);
  }

  constexpr Bitset128 operator>>(size_t shift) const {
    if (shift == 0) {
      return *this;
    }
    if (shift < 64) {
      return Bitset128(hi >> shift, (lo >> shift) | (hi << (64 - shift)));
    }
    if (shift == 64) {
      return Bitset128(0, hi);
    }
    return Bitset128(0, hi >> (shift - 64));
  }

  constexpr bool operator==(Bitset128 that) const {
    return lo == that.lo && hi == that.hi;
  }

  constexpr bool operator!=(Bitset128 that) const {
    return lo != that.lo || hi != that.hi;
  }

  constexpr bool operator!() const { return (hi | lo) == 0; }

  Bitset128& operator|=(const Bitset128& that) {
    hi |= that.hi;
    lo |= that.lo;
    return *this;
  }

  Bitset128& operator&=(const Bitset128& that) {
    hi &= that.hi;
    lo &= that.lo;
    return *this;
  }

  uint32_t size() const {
    return mozilla::CountPopulation64(hi) + mozilla::CountPopulation64(lo);
  }

  uint32_t countTrailingZeroes() const {
    if (lo) {
      return mozilla::CountTrailingZeroes64(lo);
    }
    return mozilla::CountTrailingZeroes64(hi) + 64;
  }

  uint32_t countLeadingZeroes() const {
    if (hi) {
      return mozilla::CountLeadingZeroes64(hi);
    }
    return mozilla::CountLeadingZeroes64(lo) + 64;
  }
};

class FloatRegisters {
 public:
  enum FPRegisterID {
    s0 = 0,
    d0 = 0,
    v0 = 0,
    s1 = 1,
    d1 = 1,
    v1 = 1,
    s2 = 2,
    d2 = 2,
    v2 = 2,
    s3 = 3,
    d3 = 3,
    v3 = 3,
    s4 = 4,
    d4 = 4,
    v4 = 4,
    s5 = 5,
    d5 = 5,
    v5 = 5,
    s6 = 6,
    d6 = 6,
    v6 = 6,
    s7 = 7,
    d7 = 7,
    v7 = 7,
    s8 = 8,
    d8 = 8,
    v8 = 8,
    s9 = 9,
    d9 = 9,
    v9 = 9,
    s10 = 10,
    d10 = 10,
    v10 = 10,
    s11 = 11,
    d11 = 11,
    v11 = 11,
    s12 = 12,
    d12 = 12,
    v12 = 12,
    s13 = 13,
    d13 = 13,
    v13 = 13,
    s14 = 14,
    d14 = 14,
    v14 = 14,
    s15 = 15,
    d15 = 15,
    v15 = 15,
    s16 = 16,
    d16 = 16,
    v16 = 16,
    s17 = 17,
    d17 = 17,
    v17 = 17,
    s18 = 18,
    d18 = 18,
    v18 = 18,
    s19 = 19,
    d19 = 19,
    v19 = 19,
    s20 = 20,
    d20 = 20,
    v20 = 20,
    s21 = 21,
    d21 = 21,
    v21 = 21,
    s22 = 22,
    d22 = 22,
    v22 = 22,
    s23 = 23,
    d23 = 23,
    v23 = 23,
    s24 = 24,
    d24 = 24,
    v24 = 24,
    s25 = 25,
    d25 = 25,
    v25 = 25,
    s26 = 26,
    d26 = 26,
    v26 = 26,
    s27 = 27,
    d27 = 27,
    v27 = 27,
    s28 = 28,
    d28 = 28,
    v28 = 28,
    s29 = 29,
    d29 = 29,
    v29 = 29,
    s30 = 30,
    d30 = 30,
    v30 = 30,
    s31 = 31,
    d31 = 31,
    v31 = 31,  // Scratch register.
  };

  // Eight bits: (invalid << 7) | (kind << 5) | encoding
  typedef uint8_t Code;
  typedef FPRegisterID Encoding;
  typedef Bitset128 SetType;

  enum Kind : uint8_t { Single, Double, Simd128, NumTypes };

  static constexpr Code Invalid = 0x80;

  static const char* GetName(uint32_t code) {
    // clang-format off
    static const char* const Names[] = {
        "s0",  "s1",  "s2",  "s3",  "s4",  "s5",  "s6",  "s7",  "s8",  "s9",
        "s10", "s11", "s12", "s13", "s14", "s15", "s16", "s17", "s18", "s19",
        "s20", "s21", "s22", "s23", "s24", "s25", "s26", "s27", "s28", "s29",
        "s30", "s31",

        "d0",  "d1",  "d2",  "d3",  "d4",  "d5",  "d6",  "d7",  "d8",  "d9",
        "d10", "d11", "d12", "d13", "d14", "d15", "d16", "d17", "d18", "d19",
        "d20", "d21", "d22", "d23", "d24", "d25", "d26", "d27", "d28", "d29",
        "d30", "d31",

        "v0",  "v1",  "v2",  "v3",  "v4",  "v5",  "v6",  "v7",  "v8",  "v9",
        "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19",
        "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29",
        "v30", "v31",
    };
    // clang-format on
    static_assert(Total == sizeof(Names) / sizeof(Names[0]),
                  "Table is the correct size");
    if (code >= Total) {
      return "invalid";
    }
    return Names[code];
  }

  static Code FromName(const char* name);

  static const uint32_t TotalPhys = 32;
  static const uint32_t Total = TotalPhys * NumTypes;
  static const uint32_t Allocatable = 31;  // Without d31, the scratch register.

  static_assert(sizeof(SetType) * 8 >= Total,
                "SetType should be large enough to enumerate all registers.");

  static constexpr unsigned ShiftSingle = uint32_t(Single) * TotalPhys;
  static constexpr unsigned ShiftDouble = uint32_t(Double) * TotalPhys;
  static constexpr unsigned ShiftSimd128 = uint32_t(Simd128) * TotalPhys;

  static constexpr SetType NoneMask = SetType(0);
  static constexpr SetType AllPhysMask = ~(~SetType(0) << TotalPhys);
  static constexpr SetType AllSingleMask = AllPhysMask << ShiftSingle;
  static constexpr SetType AllDoubleMask = AllPhysMask << ShiftDouble;
  static constexpr SetType AllSimd128Mask = AllPhysMask << ShiftSimd128;
  static constexpr SetType AllMask =
      AllDoubleMask | AllSingleMask | AllSimd128Mask;
  static constexpr SetType AliasMask = (SetType(1) << ShiftSingle) |
                                       (SetType(1) << ShiftDouble) |
                                       (SetType(1) << ShiftSimd128);

  static_assert(ShiftSingle == 0,
                "Or the NonVolatileMask must be computed differently");

  // s31 is the ScratchFloatReg.
  static constexpr SetType NonVolatileSingleMask =
      SetType((1 << FloatRegisters::s8) | (1 << FloatRegisters::s9) |
              (1 << FloatRegisters::s10) | (1 << FloatRegisters::s11) |
              (1 << FloatRegisters::s12) | (1 << FloatRegisters::s13) |
              (1 << FloatRegisters::s14) | (1 << FloatRegisters::s15) |
              (1 << FloatRegisters::s16) | (1 << FloatRegisters::s17) |
              (1 << FloatRegisters::s18) | (1 << FloatRegisters::s19) |
              (1 << FloatRegisters::s20) | (1 << FloatRegisters::s21) |
              (1 << FloatRegisters::s22) | (1 << FloatRegisters::s23) |
              (1 << FloatRegisters::s24) | (1 << FloatRegisters::s25) |
              (1 << FloatRegisters::s26) | (1 << FloatRegisters::s27) |
              (1 << FloatRegisters::s28) | (1 << FloatRegisters::s29) |
              (1 << FloatRegisters::s30));

  static constexpr SetType NonVolatileMask =
      (NonVolatileSingleMask << ShiftSingle) |
      (NonVolatileSingleMask << ShiftDouble) |
      (NonVolatileSingleMask << ShiftSimd128);

  static constexpr SetType VolatileMask = AllMask & ~NonVolatileMask;

  static constexpr SetType WrapperMask = VolatileMask;

  static_assert(ShiftSingle == 0,
                "Or the NonAllocatableMask must be computed differently");

  // d31 is the ScratchFloatReg.
  static constexpr SetType NonAllocatableSingleMask =
      (SetType(1) << FloatRegisters::s31);

  static constexpr SetType NonAllocatableMask =
      NonAllocatableSingleMask | (NonAllocatableSingleMask << ShiftDouble) |
      (NonAllocatableSingleMask << ShiftSimd128);

  static constexpr SetType AllocatableMask = AllMask & ~NonAllocatableMask;

  // Content spilled during bailouts.
  union RegisterContent {
    float s;
    double d;
    uint8_t v128[16];
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

static const uint32_t ShadowStackSpace = 0;

// When our only strategy for far jumps is to encode the offset directly, and
// not insert any jump islands during assembly for even further jumps, then the
// architecture restricts us to -2^27 .. 2^27-4, to fit into a signed 28-bit
// value.  We further reduce this range to allow the far-jump inserting code to
// have some breathing room.
static const uint32_t JumpImmediateRange = ((1 << 27) - (20 * 1024 * 1024));

static const uint32_t ABIStackAlignment = 16;
static const uint32_t CodeAlignment = 16;
static const bool StackKeptAligned = false;

// Although sp is only usable if 16-byte alignment is kept,
// the Pseudo-StackPointer enables use of 8-byte alignment.
static const uint32_t StackAlignment = 8;
static const uint32_t NativeFrameSize = 8;

struct FloatRegister {
  typedef FloatRegisters Codes;
  typedef Codes::Code Code;
  typedef Codes::Encoding Encoding;
  typedef Codes::SetType SetType;

  static uint32_t SetSize(SetType x) {
    static_assert(sizeof(SetType) == 16, "SetType must be 128 bits");
    x |= x >> FloatRegisters::TotalPhys;
    x |= x >> FloatRegisters::TotalPhys;
    x &= FloatRegisters::AllPhysMask;
    MOZ_ASSERT(x.high() == 0);
    MOZ_ASSERT((x.low() >> 32) == 0);
    return mozilla::CountPopulation32(x.low());
  }

  static uint32_t FirstBit(SetType x) {
    static_assert(sizeof(SetType) == 16, "SetType");
    return x.countTrailingZeroes();
  }
  static uint32_t LastBit(SetType x) {
    static_assert(sizeof(SetType) == 16, "SetType");
    return 127 - x.countLeadingZeroes();
  }

  static constexpr size_t SizeOfSimd128 = 16;

 private:
  // These fields only hold valid values: an invalid register is always
  // represented as a valid encoding and kind with the invalid_ bit set.
  uint8_t encoding_;  // 32 encodings
  uint8_t kind_;      // Double, Single, Simd128
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
    return kind_ == FloatRegisters::Simd128;
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
  FloatRegister asSimd128() const {
    MOZ_ASSERT(!invalid_);
    return FloatRegister(Encoding(encoding_), FloatRegisters::Simd128);
  }

  constexpr uint32_t size() const {
    MOZ_ASSERT(!invalid_);
    if (kind_ == FloatRegisters::Double) {
      return sizeof(double);
    }
    if (kind_ == FloatRegisters::Single) {
      return sizeof(float);
    }
    MOZ_ASSERT(kind_ == FloatRegisters::Simd128);
    return SizeOfSimd128;
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
  // This function mostly exists for the ARM backend.  It is to ensure that two
  // floating point registers' types are equivalent.  e.g. S0 is not equivalent
  // to D16, since S0 holds a float32, and D16 holds a Double.
  // Since all floating point registers on x86 and x64 are equivalent, it is
  // reasonable for this function to do the same.
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
                         Kind((aliasIdx + kind_) % Codes::NumTypes));
  }
  FloatRegister alignedAliased(uint32_t aliasIdx) { return aliased(aliasIdx); }
  SetType alignedOrDominatedAliasedSet() const {
    return Codes::AliasMask << encoding_;
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

  // For N in 0..31, if any of sN, dN or qN is a member of `s`, the
  // returned set will contain all of sN, dN and qN.
  static TypedRegisterSet<FloatRegister> BroadcastToAllSizes(
      const TypedRegisterSet<FloatRegister>& s);
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
  return set & FloatRegisters::AllSimd128Mask;
}

template <>
inline FloatRegister::SetType
FloatRegister::LiveAsIndexableSet<RegTypeName::Any>(SetType set) {
  return set;
}

// ARM/D32 has double registers that cannot be treated as float32.
// Luckily, ARMv8 doesn't have the same misfortune.
inline bool hasUnaliasedDouble() { return false; }

// ARM prior to ARMv8 also has doubles that alias multiple floats.
// Again, ARMv8 is in the clear.
inline bool hasMultiAlias() { return false; }

uint32_t GetARM64Flags();

bool CanFlushICacheFromBackgroundThreads();

}  // namespace jit
}  // namespace js

#endif  // jit_arm64_Architecture_arm64_h

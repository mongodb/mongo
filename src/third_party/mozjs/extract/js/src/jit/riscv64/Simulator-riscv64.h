/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80: */
// Copyright 2021 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef jit_riscv64_Simulator_riscv64_h
#define jit_riscv64_Simulator_riscv64_h

#ifdef JS_SIMULATOR_RISCV64
#  include "mozilla/Atomics.h"

#  include <vector>

#  include "jit/IonTypes.h"
#  include "jit/riscv64/constant/Constant-riscv64.h"
#  include "jit/riscv64/constant/util-riscv64.h"
#  include "jit/riscv64/disasm/Disasm-riscv64.h"
#  include "js/ProfilingFrameIterator.h"
#  include "threading/Thread.h"
#  include "vm/MutexIDs.h"
#  include "wasm/WasmSignalHandlers.h"

namespace js {

namespace jit {

template <class Dest, class Source>
inline Dest bit_cast(const Source& source) {
  static_assert(sizeof(Dest) == sizeof(Source),
                "bit_cast requires source and destination to be the same size");
  static_assert(std::is_trivially_copyable<Dest>::value,
                "bit_cast requires the destination type to be copyable");
  static_assert(std::is_trivially_copyable<Source>::value,
                "bit_cast requires the source type to be copyable");

  Dest dest;
  memcpy(&dest, &source, sizeof(dest));
  return dest;
}

#  define ASSERT_TRIVIALLY_COPYABLE(T)                  \
    static_assert(std::is_trivially_copyable<T>::value, \
                  #T " should be trivially copyable")
#  define ASSERT_NOT_TRIVIALLY_COPYABLE(T)               \
    static_assert(!std::is_trivially_copyable<T>::value, \
                  #T " should not be trivially copyable")

constexpr uint32_t kHoleNanUpper32 = 0xFFF7FFFF;
constexpr uint32_t kHoleNanLower32 = 0xFFF7FFFF;

constexpr uint64_t kHoleNanInt64 =
    (static_cast<uint64_t>(kHoleNanUpper32) << 32) | kHoleNanLower32;
// Safety wrapper for a 32-bit floating-point value to make sure we don't lose
// the exact bit pattern during deoptimization when passing this value.
class Float32 {
 public:
  Float32() = default;

  // This constructor does not guarantee that bit pattern of the input value
  // is preserved if the input is a NaN.
  explicit Float32(float value) : bit_pattern_(bit_cast<uint32_t>(value)) {
    // Check that the provided value is not a NaN, because the bit pattern of a
    // NaN may be changed by a bit_cast, e.g. for signalling NaNs on
    // ia32.
    MOZ_ASSERT(!std::isnan(value));
  }

  uint32_t get_bits() const { return bit_pattern_; }

  float get_scalar() const { return bit_cast<float>(bit_pattern_); }

  bool is_nan() const {
    // Even though {get_scalar()} might flip the quiet NaN bit, it's ok here,
    // because this does not change the is_nan property.
    return std::isnan(get_scalar());
  }

  // Return a pointer to the field storing the bit pattern. Used in code
  // generation tests to store generated values there directly.
  uint32_t* get_bits_address() { return &bit_pattern_; }

  static constexpr Float32 FromBits(uint32_t bits) { return Float32(bits); }

 private:
  uint32_t bit_pattern_ = 0;

  explicit constexpr Float32(uint32_t bit_pattern)
      : bit_pattern_(bit_pattern) {}
};

ASSERT_TRIVIALLY_COPYABLE(Float32);

// Safety wrapper for a 64-bit floating-point value to make sure we don't lose
// the exact bit pattern during deoptimization when passing this value.
// TODO(ahaas): Unify this class with Double in double.h
class Float64 {
 public:
  Float64() = default;

  // This constructor does not guarantee that bit pattern of the input value
  // is preserved if the input is a NaN.
  explicit Float64(double value) : bit_pattern_(bit_cast<uint64_t>(value)) {
    // Check that the provided value is not a NaN, because the bit pattern of a
    // NaN may be changed by a bit_cast, e.g. for signalling NaNs on
    // ia32.
    MOZ_ASSERT(!std::isnan(value));
  }

  uint64_t get_bits() const { return bit_pattern_; }
  double get_scalar() const { return bit_cast<double>(bit_pattern_); }
  bool is_hole_nan() const { return bit_pattern_ == kHoleNanInt64; }
  bool is_nan() const {
    // Even though {get_scalar()} might flip the quiet NaN bit, it's ok here,
    // because this does not change the is_nan property.
    return std::isnan(get_scalar());
  }

  // Return a pointer to the field storing the bit pattern. Used in code
  // generation tests to store generated values there directly.
  uint64_t* get_bits_address() { return &bit_pattern_; }

  static constexpr Float64 FromBits(uint64_t bits) { return Float64(bits); }

 private:
  uint64_t bit_pattern_ = 0;

  explicit constexpr Float64(uint64_t bit_pattern)
      : bit_pattern_(bit_pattern) {}
};

ASSERT_TRIVIALLY_COPYABLE(Float64);

class JitActivation;

class Simulator;
class Redirection;
class CachePage;
class AutoLockSimulator;

// When the SingleStepCallback is called, the simulator is about to execute
// sim->get_pc() and the current machine state represents the completed
// execution of the previous pc.
typedef void (*SingleStepCallback)(void* arg, Simulator* sim, void* pc);

const intptr_t kPointerAlignment = 8;
const intptr_t kPointerAlignmentMask = kPointerAlignment - 1;

const intptr_t kDoubleAlignment = 8;
const intptr_t kDoubleAlignmentMask = kDoubleAlignment - 1;

// Number of general purpose registers.
const int kNumRegisters = 32;

// In the simulator, the PC register is simulated as the 34th register.
const int kPCRegister = 32;

// Number coprocessor registers.
const int kNumFPURegisters = 32;

// FPU (coprocessor 1) control registers. Currently only FCSR is implemented.
const int kFCSRRegister = 31;
const int kInvalidFPUControlRegister = -1;
const uint32_t kFPUInvalidResult = static_cast<uint32_t>(1 << 31) - 1;
const uint64_t kFPUInvalidResult64 = static_cast<uint64_t>(1ULL << 63) - 1;

// FCSR constants.
const uint32_t kFCSRInexactFlagBit = 2;
const uint32_t kFCSRUnderflowFlagBit = 3;
const uint32_t kFCSROverflowFlagBit = 4;
const uint32_t kFCSRDivideByZeroFlagBit = 5;
const uint32_t kFCSRInvalidOpFlagBit = 6;

const uint32_t kFCSRInexactCauseBit = 12;
const uint32_t kFCSRUnderflowCauseBit = 13;
const uint32_t kFCSROverflowCauseBit = 14;
const uint32_t kFCSRDivideByZeroCauseBit = 15;
const uint32_t kFCSRInvalidOpCauseBit = 16;

const uint32_t kFCSRInexactFlagMask = 1 << kFCSRInexactFlagBit;
const uint32_t kFCSRUnderflowFlagMask = 1 << kFCSRUnderflowFlagBit;
const uint32_t kFCSROverflowFlagMask = 1 << kFCSROverflowFlagBit;
const uint32_t kFCSRDivideByZeroFlagMask = 1 << kFCSRDivideByZeroFlagBit;
const uint32_t kFCSRInvalidOpFlagMask = 1 << kFCSRInvalidOpFlagBit;

const uint32_t kFCSRFlagMask =
    kFCSRInexactFlagMask | kFCSRUnderflowFlagMask | kFCSROverflowFlagMask |
    kFCSRDivideByZeroFlagMask | kFCSRInvalidOpFlagMask;

const uint32_t kFCSRExceptionFlagMask = kFCSRFlagMask ^ kFCSRInexactFlagMask;

// -----------------------------------------------------------------------------
// Utility types and functions for RISCV
#  ifdef JS_CODEGEN_RISCV32
using sreg_t = int32_t;
using reg_t = uint32_t;
using freg_t = uint64_t;
using sfreg_t = int64_t;
#  elif JS_CODEGEN_RISCV64
using sreg_t = int64_t;
using reg_t = uint64_t;
using freg_t = uint64_t;
using sfreg_t = int64_t;
#  else
#    error "Cannot detect Riscv's bitwidth"
#  endif

#  define sext32(x) ((sreg_t)(int32_t)(x))
#  define zext32(x) ((reg_t)(uint32_t)(x))

#  ifdef JS_CODEGEN_RISCV64
#    define sext_xlen(x) (((sreg_t)(x) << (64 - xlen)) >> (64 - xlen))
#    define zext_xlen(x) (((reg_t)(x) << (64 - xlen)) >> (64 - xlen))
#  elif JS_CODEGEN_RISCV32
#    define sext_xlen(x) (((sreg_t)(x) << (32 - xlen)) >> (32 - xlen))
#    define zext_xlen(x) (((reg_t)(x) << (32 - xlen)) >> (32 - xlen))
#  endif

#  define BIT(n) (0x1LL << n)
#  define QUIET_BIT_S(nan) (bit_cast<int32_t>(nan) & BIT(22))
#  define QUIET_BIT_D(nan) (bit_cast<int64_t>(nan) & BIT(51))
static inline bool isSnan(float fp) { return !QUIET_BIT_S(fp); }
static inline bool isSnan(double fp) { return !QUIET_BIT_D(fp); }
#  undef QUIET_BIT_S
#  undef QUIET_BIT_D

#  ifdef JS_CODEGEN_RISCV64
inline uint64_t mulhu(uint64_t a, uint64_t b) {
  __uint128_t full_result = ((__uint128_t)a) * ((__uint128_t)b);
  return full_result >> 64;
}

inline int64_t mulh(int64_t a, int64_t b) {
  __int128_t full_result = ((__int128_t)a) * ((__int128_t)b);
  return full_result >> 64;
}

inline int64_t mulhsu(int64_t a, uint64_t b) {
  __int128_t full_result = ((__int128_t)a) * ((__uint128_t)b);
  return full_result >> 64;
}
#  elif JS_CODEGEN_RISCV32
inline uint32_t mulhu(uint32_t a, uint32_t b) {
  uint64_t full_result = ((uint64_t)a) * ((uint64_t)b);
  uint64_t upper_part = full_result >> 32;
  return (uint32_t)upper_part;
}

inline int32_t mulh(int32_t a, int32_t b) {
  int64_t full_result = ((int64_t)a) * ((int64_t)b);
  int64_t upper_part = full_result >> 32;
  return (int32_t)upper_part;
}

inline int32_t mulhsu(int32_t a, uint32_t b) {
  int64_t full_result = ((int64_t)a) * ((uint64_t)b);
  int64_t upper_part = full_result >> 32;
  return (int32_t)upper_part;
}
#  endif

// Floating point helpers
#  define F32_SIGN ((uint32_t)1 << 31)
union u32_f32 {
  uint32_t u;
  float f;
};
inline float fsgnj32(float rs1, float rs2, bool n, bool x) {
  u32_f32 a = {.f = rs1}, b = {.f = rs2};
  u32_f32 res;
  res.u = (a.u & ~F32_SIGN) | ((((x)   ? a.u
                                 : (n) ? F32_SIGN
                                       : 0) ^
                                b.u) &
                               F32_SIGN);
  return res.f;
}

inline Float32 fsgnj32(Float32 rs1, Float32 rs2, bool n, bool x) {
  u32_f32 a = {.u = rs1.get_bits()}, b = {.u = rs2.get_bits()};
  u32_f32 res;
  if (x) {  // RO_FSQNJX_S
    res.u = (a.u & ~F32_SIGN) | ((a.u ^ b.u) & F32_SIGN);
  } else {
    if (n) {  // RO_FSGNJN_S
      res.u = (a.u & ~F32_SIGN) | ((F32_SIGN ^ b.u) & F32_SIGN);
    } else {  // RO_FSGNJ_S
      res.u = (a.u & ~F32_SIGN) | ((0 ^ b.u) & F32_SIGN);
    }
  }
  return Float32::FromBits(res.u);
}
#  define F64_SIGN ((uint64_t)1 << 63)
union u64_f64 {
  uint64_t u;
  double d;
};
inline double fsgnj64(double rs1, double rs2, bool n, bool x) {
  u64_f64 a = {.d = rs1}, b = {.d = rs2};
  u64_f64 res;
  res.u = (a.u & ~F64_SIGN) | ((((x)   ? a.u
                                 : (n) ? F64_SIGN
                                       : 0) ^
                                b.u) &
                               F64_SIGN);
  return res.d;
}

inline Float64 fsgnj64(Float64 rs1, Float64 rs2, bool n, bool x) {
  u64_f64 a = {.d = rs1.get_scalar()}, b = {.d = rs2.get_scalar()};
  u64_f64 res;
  if (x) {  // RO_FSQNJX_D
    res.u = (a.u & ~F64_SIGN) | ((a.u ^ b.u) & F64_SIGN);
  } else {
    if (n) {  // RO_FSGNJN_D
      res.u = (a.u & ~F64_SIGN) | ((F64_SIGN ^ b.u) & F64_SIGN);
    } else {  // RO_FSGNJ_D
      res.u = (a.u & ~F64_SIGN) | ((0 ^ b.u) & F64_SIGN);
    }
  }
  return Float64::FromBits(res.u);
}
inline bool is_boxed_float(int64_t v) { return (uint32_t)((v >> 32) + 1) == 0; }
inline int64_t box_float(float v) {
  return (0xFFFFFFFF00000000 | bit_cast<int32_t>(v));
}

inline uint64_t box_float(uint32_t v) { return (0xFFFFFFFF00000000 | v); }

// -----------------------------------------------------------------------------
// Utility functions

class SimInstructionBase : public InstructionBase {
 public:
  Type InstructionType() const { return type_; }
  inline Instruction* instr() const { return instr_; }
  inline int32_t operand() const { return operand_; }

 protected:
  SimInstructionBase() : operand_(-1), instr_(nullptr), type_(kUnsupported) {}
  explicit SimInstructionBase(Instruction* instr) {}

  int32_t operand_;
  Instruction* instr_;
  Type type_;

 private:
  SimInstructionBase& operator=(const SimInstructionBase&) = delete;
};

class SimInstruction : public InstructionGetters<SimInstructionBase> {
 public:
  SimInstruction() {}

  explicit SimInstruction(Instruction* instr) { *this = instr; }

  SimInstruction& operator=(Instruction* instr) {
    operand_ = *reinterpret_cast<const int32_t*>(instr);
    instr_ = instr;
    type_ = InstructionBase::InstructionType();
    MOZ_ASSERT(reinterpret_cast<void*>(&operand_) == this);
    return *this;
  }
};

// Per thread simulator state.
class Simulator {
  friend class RiscvDebugger;

 public:
  static bool FLAG_riscv_trap_to_simulator_debugger;
  static bool FLAG_trace_sim;
  static bool FLAG_debug_sim;
  static bool FLAG_riscv_print_watchpoint;
  // Registers are declared in order.
  enum Register {
    no_reg = -1,
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
    pc,
    kNumSimuRegisters,
    // alias
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
  };

  // Coprocessor registers.
  enum FPURegister {
    f0,
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
    kNumFPURegisters,
    // alias
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
    fs11 = f27,
    ft8 = f28,
    ft9 = f29,
    ft10 = f30,
    ft11 = f31
  };

  // Returns nullptr on OOM.
  static Simulator* Create();

  static void Destroy(Simulator* simulator);

  // Constructor/destructor are for internal use only; use the static methods
  // above.
  Simulator();
  ~Simulator();

  // RISCV decoding routine
  void DecodeRVRType();
  void DecodeRVR4Type();
  void DecodeRVRFPType();  // Special routine for R/OP_FP type
  void DecodeRVRAType();   // Special routine for R/AMO type
  void DecodeRVIType();
  void DecodeRVSType();
  void DecodeRVBType();
  void DecodeRVUType();
  void DecodeRVJType();
  void DecodeCRType();
  void DecodeCAType();
  void DecodeCIType();
  void DecodeCIWType();
  void DecodeCSSType();
  void DecodeCLType();
  void DecodeCSType();
  void DecodeCJType();
  void DecodeCBType();
#  ifdef CAN_USE_RVV_INSTRUCTIONS
  void DecodeVType();
  void DecodeRvvIVV();
  void DecodeRvvIVI();
  void DecodeRvvIVX();
  void DecodeRvvMVV();
  void DecodeRvvMVX();
  void DecodeRvvFVV();
  void DecodeRvvFVF();
  bool DecodeRvvVL();
  bool DecodeRvvVS();
#  endif
  // The currently executing Simulator instance. Potentially there can be one
  // for each native thread.
  static Simulator* Current();

  static inline uintptr_t StackLimit() {
    return Simulator::Current()->stackLimit();
  }

  uintptr_t* addressOfStackLimit();

  // Accessors for register state. Reading the pc value adheres to the MIPS
  // architecture specification and is off by a 8 from the currently executing
  // instruction.
  void setRegister(int reg, int64_t value);
  int64_t getRegister(int reg) const;
  // Same for FPURegisters.
  void setFpuRegister(int fpureg, int64_t value);
  void setFpuRegisterLo(int fpureg, int32_t value);
  void setFpuRegisterHi(int fpureg, int32_t value);
  void setFpuRegisterFloat(int fpureg, float value);
  void setFpuRegisterDouble(int fpureg, double value);
  void setFpuRegisterFloat(int fpureg, Float32 value);
  void setFpuRegisterDouble(int fpureg, Float64 value);

  int64_t getFpuRegister(int fpureg) const;
  int32_t getFpuRegisterLo(int fpureg) const;
  int32_t getFpuRegisterHi(int fpureg) const;
  float getFpuRegisterFloat(int fpureg) const;
  double getFpuRegisterDouble(int fpureg) const;
  Float32 getFpuRegisterFloat32(int fpureg) const;
  Float64 getFpuRegisterFloat64(int fpureg) const;

  inline int16_t shamt6() const { return (imm12() & 0x3F); }
  inline int16_t shamt5() const { return (imm12() & 0x1F); }
  inline int16_t rvc_shamt6() const { return instr_.RvcShamt6(); }
  inline int32_t s_imm12() const { return instr_.StoreOffset(); }
  inline int32_t u_imm20() const { return instr_.Imm20UValue() << 12; }
  inline int32_t rvc_u_imm6() const { return instr_.RvcImm6Value() << 12; }
  inline void require(bool check) {
    if (!check) {
      SignalException(kIllegalInstruction);
    }
  }

  // Special case of setRegister and getRegister to access the raw PC value.
  void set_pc(int64_t value);
  int64_t get_pc() const;

  SimInstruction instr_;
  // RISCV utlity API to access register value
  // Helpers for data value tracing.
  enum TraceType {
    BYTE,
    HALF,
    WORD,
#  if JS_CODEGEN_RISCV64
    DWORD,
#  endif
    FLOAT,
    DOUBLE,
    // FLOAT_DOUBLE,
    // WORD_DWORD
  };
  inline int32_t rs1_reg() const { return instr_.Rs1Value(); }
  inline sreg_t rs1() const { return getRegister(rs1_reg()); }
  inline float frs1() const { return getFpuRegisterFloat(rs1_reg()); }
  inline double drs1() const { return getFpuRegisterDouble(rs1_reg()); }
  inline Float32 frs1_boxed() const { return getFpuRegisterFloat32(rs1_reg()); }
  inline Float64 drs1_boxed() const { return getFpuRegisterFloat64(rs1_reg()); }
  inline int32_t rs2_reg() const { return instr_.Rs2Value(); }
  inline sreg_t rs2() const { return getRegister(rs2_reg()); }
  inline float frs2() const { return getFpuRegisterFloat(rs2_reg()); }
  inline double drs2() const { return getFpuRegisterDouble(rs2_reg()); }
  inline Float32 frs2_boxed() const { return getFpuRegisterFloat32(rs2_reg()); }
  inline Float64 drs2_boxed() const { return getFpuRegisterFloat64(rs2_reg()); }
  inline int32_t rs3_reg() const { return instr_.Rs3Value(); }
  inline sreg_t rs3() const { return getRegister(rs3_reg()); }
  inline float frs3() const { return getFpuRegisterFloat(rs3_reg()); }
  inline double drs3() const { return getFpuRegisterDouble(rs3_reg()); }
  inline Float32 frs3_boxed() const { return getFpuRegisterFloat32(rs3_reg()); }
  inline Float64 drs3_boxed() const { return getFpuRegisterFloat64(rs3_reg()); }
  inline int32_t rd_reg() const { return instr_.RdValue(); }
  inline int32_t frd_reg() const { return instr_.RdValue(); }
  inline int32_t rvc_rs1_reg() const { return instr_.RvcRs1Value(); }
  inline sreg_t rvc_rs1() const { return getRegister(rvc_rs1_reg()); }
  inline int32_t rvc_rs2_reg() const { return instr_.RvcRs2Value(); }
  inline sreg_t rvc_rs2() const { return getRegister(rvc_rs2_reg()); }
  inline double rvc_drs2() const { return getFpuRegisterDouble(rvc_rs2_reg()); }
  inline int32_t rvc_rs1s_reg() const { return instr_.RvcRs1sValue(); }
  inline sreg_t rvc_rs1s() const { return getRegister(rvc_rs1s_reg()); }
  inline int32_t rvc_rs2s_reg() const { return instr_.RvcRs2sValue(); }
  inline sreg_t rvc_rs2s() const { return getRegister(rvc_rs2s_reg()); }
  inline double rvc_drs2s() const {
    return getFpuRegisterDouble(rvc_rs2s_reg());
  }
  inline int32_t rvc_rd_reg() const { return instr_.RvcRdValue(); }
  inline int32_t rvc_frd_reg() const { return instr_.RvcRdValue(); }
  inline int16_t boffset() const { return instr_.BranchOffset(); }
  inline int16_t imm12() const { return instr_.Imm12Value(); }
  inline int32_t imm20J() const { return instr_.Imm20JValue(); }
  inline int32_t imm5CSR() const { return instr_.Rs1Value(); }
  inline int16_t csr_reg() const { return instr_.CsrValue(); }
  inline int16_t rvc_imm6() const { return instr_.RvcImm6Value(); }
  inline int16_t rvc_imm6_addi16sp() const {
    return instr_.RvcImm6Addi16spValue();
  }
  inline int16_t rvc_imm8_addi4spn() const {
    return instr_.RvcImm8Addi4spnValue();
  }
  inline int16_t rvc_imm6_lwsp() const { return instr_.RvcImm6LwspValue(); }
  inline int16_t rvc_imm6_ldsp() const { return instr_.RvcImm6LdspValue(); }
  inline int16_t rvc_imm6_swsp() const { return instr_.RvcImm6SwspValue(); }
  inline int16_t rvc_imm6_sdsp() const { return instr_.RvcImm6SdspValue(); }
  inline int16_t rvc_imm5_w() const { return instr_.RvcImm5WValue(); }
  inline int16_t rvc_imm5_d() const { return instr_.RvcImm5DValue(); }
  inline int16_t rvc_imm8_b() const { return instr_.RvcImm8BValue(); }

  // Helper for debugging memory access.
  inline void DieOrDebug();

#  if JS_CODEGEN_RISCV32
  template <typename T>
  void TraceRegWr(T value, TraceType t = WORD);
#  elif JS_CODEGEN_RISCV64
  void TraceRegWr(sreg_t value, TraceType t = DWORD);
#  endif
  void TraceMemWr(sreg_t addr, sreg_t value, TraceType t);
  template <typename T>
  void TraceMemRd(sreg_t addr, T value, sreg_t reg_value);
  void TraceMemRdDouble(sreg_t addr, double value, int64_t reg_value);
  void TraceMemRdDouble(sreg_t addr, Float64 value, int64_t reg_value);
  void TraceMemRdFloat(sreg_t addr, Float32 value, int64_t reg_value);

  template <typename T>
  void TraceLr(sreg_t addr, T value, sreg_t reg_value);

  template <typename T>
  void TraceSc(sreg_t addr, T value);

  template <typename T>
  void TraceMemWr(sreg_t addr, T value);
  void TraceMemWrDouble(sreg_t addr, double value);

  inline void set_rd(sreg_t value, bool trace = true) {
    setRegister(rd_reg(), value);
#  if JS_CODEGEN_RISCV64
    if (trace) TraceRegWr(getRegister(rd_reg()), DWORD);
#  elif JS_CODEGEN_RISCV32
    if (trace) TraceRegWr(getRegister(rd_reg()), WORD);
#  endif
  }
  inline void set_frd(float value, bool trace = true) {
    setFpuRegisterFloat(rd_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rd_reg()), FLOAT);
  }
  inline void set_frd(Float32 value, bool trace = true) {
    setFpuRegisterFloat(rd_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rd_reg()), FLOAT);
  }
  inline void set_drd(double value, bool trace = true) {
    setFpuRegisterDouble(rd_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rd_reg()), DOUBLE);
  }
  inline void set_drd(Float64 value, bool trace = true) {
    setFpuRegisterDouble(rd_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rd_reg()), DOUBLE);
  }
  inline void set_rvc_rd(sreg_t value, bool trace = true) {
    setRegister(rvc_rd_reg(), value);
#  if JS_CODEGEN_RISCV64
    if (trace) TraceRegWr(getRegister(rvc_rd_reg()), DWORD);
#  elif JS_CODEGEN_RISCV32
    if (trace) TraceRegWr(getRegister(rvc_rd_reg()), WORD);
#  endif
  }
  inline void set_rvc_rs1s(sreg_t value, bool trace = true) {
    setRegister(rvc_rs1s_reg(), value);
#  if JS_CODEGEN_RISCV64
    if (trace) TraceRegWr(getRegister(rvc_rs1s_reg()), DWORD);
#  elif JS_CODEGEN_RISCV32
    if (trace) TraceRegWr(getRegister(rvc_rs1s_reg()), WORD);
#  endif
  }
  inline void set_rvc_rs2(sreg_t value, bool trace = true) {
    setRegister(rvc_rs2_reg(), value);
#  if JS_CODEGEN_RISCV64
    if (trace) TraceRegWr(getRegister(rvc_rs2_reg()), DWORD);
#  elif JS_CODEGEN_RISCV32
    if (trace) TraceRegWr(getRegister(rvc_rs2_reg()), WORD);
#  endif
  }
  inline void set_rvc_drd(double value, bool trace = true) {
    setFpuRegisterDouble(rvc_rd_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rvc_rd_reg()), DOUBLE);
  }
  inline void set_rvc_drd(Float64 value, bool trace = true) {
    setFpuRegisterDouble(rvc_rd_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rvc_rd_reg()), DOUBLE);
  }
  inline void set_rvc_frd(Float32 value, bool trace = true) {
    setFpuRegisterFloat(rvc_rd_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rvc_rd_reg()), DOUBLE);
  }
  inline void set_rvc_rs2s(sreg_t value, bool trace = true) {
    setRegister(rvc_rs2s_reg(), value);
#  if JS_CODEGEN_RISCV64
    if (trace) TraceRegWr(getRegister(rvc_rs2s_reg()), DWORD);
#  elif JS_CODEGEN_RISCV32
    if (trace) TraceRegWr(getRegister(rvc_rs2s_reg()), WORD);
#  endif
  }
  inline void set_rvc_drs2s(double value, bool trace = true) {
    setFpuRegisterDouble(rvc_rs2s_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rvc_rs2s_reg()), DOUBLE);
  }
  inline void set_rvc_drs2s(Float64 value, bool trace = true) {
    setFpuRegisterDouble(rvc_rs2s_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rvc_rs2s_reg()), DOUBLE);
  }

  inline void set_rvc_frs2s(Float32 value, bool trace = true) {
    setFpuRegisterFloat(rvc_rs2s_reg(), value);
    if (trace) TraceRegWr(getFpuRegister(rvc_rs2s_reg()), FLOAT);
  }

  uint32_t get_dynamic_rounding_mode() { return read_csr_value(csr_frm); }

  // helper functions to read/write/set/clear CRC values/bits
  uint32_t read_csr_value(uint32_t csr) {
    switch (csr) {
      case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
        return (FCSR_ & kFcsrFlagsMask);
      case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
        return (FCSR_ & kFcsrFrmMask) >> kFcsrFrmShift;
      case csr_fcsr:  // Floating-Point Control and Status Register (RW)
        return (FCSR_ & kFcsrMask);
      default:
        MOZ_CRASH("UNIMPLEMENTED");
    }
  }

  void write_csr_value(uint32_t csr, reg_t val) {
    uint32_t value = (uint32_t)val;
    switch (csr) {
      case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
        MOZ_ASSERT(value <= ((1 << kFcsrFlagsBits) - 1));
        FCSR_ = (FCSR_ & (~kFcsrFlagsMask)) | value;
        break;
      case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
        MOZ_ASSERT(value <= ((1 << kFcsrFrmBits) - 1));
        FCSR_ = (FCSR_ & (~kFcsrFrmMask)) | (value << kFcsrFrmShift);
        break;
      case csr_fcsr:  // Floating-Point Control and Status Register (RW)
        MOZ_ASSERT(value <= ((1 << kFcsrBits) - 1));
        FCSR_ = (FCSR_ & (~kFcsrMask)) | value;
        break;
      default:
        MOZ_CRASH("UNIMPLEMENTED");
    }
  }

  void set_csr_bits(uint32_t csr, reg_t val) {
    uint32_t value = (uint32_t)val;
    switch (csr) {
      case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
        MOZ_ASSERT(value <= ((1 << kFcsrFlagsBits) - 1));
        FCSR_ = FCSR_ | value;
        break;
      case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
        MOZ_ASSERT(value <= ((1 << kFcsrFrmBits) - 1));
        FCSR_ = FCSR_ | (value << kFcsrFrmShift);
        break;
      case csr_fcsr:  // Floating-Point Control and Status Register (RW)
        MOZ_ASSERT(value <= ((1 << kFcsrBits) - 1));
        FCSR_ = FCSR_ | value;
        break;
      default:
        MOZ_CRASH("UNIMPLEMENTED");
    }
  }

  void clear_csr_bits(uint32_t csr, reg_t val) {
    uint32_t value = (uint32_t)val;
    switch (csr) {
      case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
        MOZ_ASSERT(value <= ((1 << kFcsrFlagsBits) - 1));
        FCSR_ = FCSR_ & (~value);
        break;
      case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
        MOZ_ASSERT(value <= ((1 << kFcsrFrmBits) - 1));
        FCSR_ = FCSR_ & (~(value << kFcsrFrmShift));
        break;
      case csr_fcsr:  // Floating-Point Control and Status Register (RW)
        MOZ_ASSERT(value <= ((1 << kFcsrBits) - 1));
        FCSR_ = FCSR_ & (~value);
        break;
      default:
        MOZ_CRASH("UNIMPLEMENTED");
    }
  }

  bool test_fflags_bits(uint32_t mask) {
    return (FCSR_ & kFcsrFlagsMask & mask) != 0;
  }

  void set_fflags(uint32_t flags) { set_csr_bits(csr_fflags, flags); }
  void clear_fflags(int32_t flags) { clear_csr_bits(csr_fflags, flags); }

  float RoundF2FHelper(float input_val, int rmode);
  double RoundF2FHelper(double input_val, int rmode);
  template <typename I_TYPE, typename F_TYPE>
  I_TYPE RoundF2IHelper(F_TYPE original, int rmode);

  template <typename T>
  T FMaxMinHelper(T a, T b, MaxMinKind kind);

  template <typename T>
  bool CompareFHelper(T input1, T input2, FPUCondition cc);

  template <typename T>
  T get_pc_as() const {
    return reinterpret_cast<T>(get_pc());
  }

  void enable_single_stepping(SingleStepCallback cb, void* arg);
  void disable_single_stepping();

  // Accessor to the internal simulator stack area.
  uintptr_t stackLimit() const;
  bool overRecursed(uintptr_t newsp = 0) const;
  bool overRecursedWithExtra(uint32_t extra) const;

  // Executes MIPS instructions until the PC reaches end_sim_pc.
  template <bool enableStopSimAt>
  void execute();

  // Sets up the simulator state and grabs the result on return.
  int64_t call(uint8_t* entry, int argument_count, ...);

  // Push an address onto the JS stack.
  uintptr_t pushAddress(uintptr_t address);

  // Pop an address from the JS stack.
  uintptr_t popAddress();

  // Debugger input.
  void setLastDebuggerInput(char* input);
  char* lastDebuggerInput() { return lastDebuggerInput_; }

  // Returns true if pc register contains one of the 'SpecialValues' defined
  // below (bad_ra, end_sim_pc).
  bool has_bad_pc() const;

 private:
  enum SpecialValues {
    // Known bad pc value to ensure that the simulator does not execute
    // without being properly setup.
    bad_ra = -1,
    // A pc value used to signal the simulator to stop execution.  Generally
    // the ra is set to this value on transition from native C code to
    // simulated execution, so that the simulator can "return" to the native
    // C code.
    end_sim_pc = -2,
    // Unpredictable value.
    Unpredictable = 0xbadbeaf
  };

  bool init();

  // Unsupported instructions use Format to print an error and stop execution.
  void format(SimInstruction* instr, const char* format);

  // Read and write memory.
  // RISCV Memory read/write methods
  template <typename T>
  T ReadMem(sreg_t addr, Instruction* instr);
  template <typename T>
  void WriteMem(sreg_t addr, T value, Instruction* instr);
  template <typename T, typename OP>
  T amo(sreg_t addr, OP f, Instruction* instr, TraceType t) {
    auto lhs = ReadMem<T>(addr, instr);
    // TODO(RISCV): trace memory read for AMO
    WriteMem<T>(addr, (T)f(lhs), instr);
    return lhs;
  }

  inline int32_t loadLinkedW(uint64_t addr, SimInstruction* instr);
  inline int storeConditionalW(uint64_t addr, int32_t value,
                               SimInstruction* instr);

  inline int64_t loadLinkedD(uint64_t addr, SimInstruction* instr);
  inline int storeConditionalD(uint64_t addr, int64_t value,
                               SimInstruction* instr);

  // Used for breakpoints and traps.
  void SoftwareInterrupt();

  // Stop helper functions.
  bool isWatchpoint(uint32_t code);
  bool IsTracepoint(uint32_t code);
  void printWatchpoint(uint32_t code);
  void handleStop(uint32_t code);
  bool isStopInstruction(SimInstruction* instr);
  bool isEnabledStop(uint32_t code);
  void enableStop(uint32_t code);
  void disableStop(uint32_t code);
  void increaseStopCounter(uint32_t code);
  void printStopInfo(uint32_t code);

  // Simulator breakpoints.
  struct Breakpoint {
    SimInstruction* location;
    bool enabled;
    bool is_tbreak;
  };
  std::vector<Breakpoint> breakpoints_;
  void SetBreakpoint(SimInstruction* breakpoint, bool is_tbreak);
  void ListBreakpoints();
  void CheckBreakpoints();

  JS::ProfilingFrameIterator::RegisterState registerState();
  void HandleWasmTrap();

  // Handle any wasm faults, returning true if the fault was handled.
  // This method is rather hot so inline the normal (no-wasm) case.
  bool MOZ_ALWAYS_INLINE handleWasmSegFault(uint64_t addr, unsigned numBytes) {
    if (MOZ_LIKELY(!js::wasm::CodeExists)) {
      return false;
    }

    uint8_t* newPC;
    if (!js::wasm::MemoryAccessTraps(registerState(), (uint8_t*)addr, numBytes,
                                     &newPC)) {
      return false;
    }

    LLBit_ = false;
    set_pc(int64_t(newPC));
    return true;
  }

  // Executes one instruction.
  void InstructionDecode(Instruction* instr);

  // ICache.
  // static void CheckICache(base::CustomMatcherHashMap* i_cache,
  //                         Instruction* instr);
  // static void FlushOnePage(base::CustomMatcherHashMap* i_cache, intptr_t
  // start,
  //                          size_t size);
  // static CachePage* GetCachePage(base::CustomMatcherHashMap* i_cache,
  //                                void* page);
  template <typename T, typename Func>
  inline T CanonicalizeFPUOpFMA(Func fn, T dst, T src1, T src2) {
    static_assert(std::is_floating_point<T>::value);
    auto alu_out = fn(dst, src1, src2);
    // if any input or result is NaN, the result is quiet_NaN
    if (std::isnan(alu_out) || std::isnan(src1) || std::isnan(src2) ||
        std::isnan(dst)) {
      // signaling_nan sets kInvalidOperation bit
      if (isSnan(alu_out) || isSnan(src1) || isSnan(src2) || isSnan(dst))
        set_fflags(kInvalidOperation);
      alu_out = std::numeric_limits<T>::quiet_NaN();
    }
    return alu_out;
  }

  template <typename T, typename Func>
  inline T CanonicalizeFPUOp3(Func fn) {
    static_assert(std::is_floating_point<T>::value);
    T src1 = std::is_same<float, T>::value ? frs1() : drs1();
    T src2 = std::is_same<float, T>::value ? frs2() : drs2();
    T src3 = std::is_same<float, T>::value ? frs3() : drs3();
    auto alu_out = fn(src1, src2, src3);
    // if any input or result is NaN, the result is quiet_NaN
    if (std::isnan(alu_out) || std::isnan(src1) || std::isnan(src2) ||
        std::isnan(src3)) {
      // signaling_nan sets kInvalidOperation bit
      if (isSnan(alu_out) || isSnan(src1) || isSnan(src2) || isSnan(src3))
        set_fflags(kInvalidOperation);
      alu_out = std::numeric_limits<T>::quiet_NaN();
    }
    return alu_out;
  }

  template <typename T, typename Func>
  inline T CanonicalizeFPUOp2(Func fn) {
    static_assert(std::is_floating_point<T>::value);
    T src1 = std::is_same<float, T>::value ? frs1() : drs1();
    T src2 = std::is_same<float, T>::value ? frs2() : drs2();
    auto alu_out = fn(src1, src2);
    // if any input or result is NaN, the result is quiet_NaN
    if (std::isnan(alu_out) || std::isnan(src1) || std::isnan(src2)) {
      // signaling_nan sets kInvalidOperation bit
      if (isSnan(alu_out) || isSnan(src1) || isSnan(src2))
        set_fflags(kInvalidOperation);
      alu_out = std::numeric_limits<T>::quiet_NaN();
    }
    return alu_out;
  }

  template <typename T, typename Func>
  inline T CanonicalizeFPUOp1(Func fn) {
    static_assert(std::is_floating_point<T>::value);
    T src1 = std::is_same<float, T>::value ? frs1() : drs1();
    auto alu_out = fn(src1);
    // if any input or result is NaN, the result is quiet_NaN
    if (std::isnan(alu_out) || std::isnan(src1)) {
      // signaling_nan sets kInvalidOperation bit
      if (isSnan(alu_out) || isSnan(src1)) set_fflags(kInvalidOperation);
      alu_out = std::numeric_limits<T>::quiet_NaN();
    }
    return alu_out;
  }

  template <typename Func>
  inline float CanonicalizeDoubleToFloatOperation(Func fn) {
    float alu_out = fn(drs1());
    if (std::isnan(alu_out) || std::isnan(drs1()))
      alu_out = std::numeric_limits<float>::quiet_NaN();
    return alu_out;
  }

  template <typename Func>
  inline float CanonicalizeDoubleToFloatOperation(Func fn, double frs) {
    float alu_out = fn(frs);
    if (std::isnan(alu_out) || std::isnan(drs1()))
      alu_out = std::numeric_limits<float>::quiet_NaN();
    return alu_out;
  }

  template <typename Func>
  inline float CanonicalizeFloatToDoubleOperation(Func fn, float frs) {
    double alu_out = fn(frs);
    if (std::isnan(alu_out) || std::isnan(frs1()))
      alu_out = std::numeric_limits<double>::quiet_NaN();
    return alu_out;
  }

  template <typename Func>
  inline float CanonicalizeFloatToDoubleOperation(Func fn) {
    double alu_out = fn(frs1());
    if (std::isnan(alu_out) || std::isnan(frs1()))
      alu_out = std::numeric_limits<double>::quiet_NaN();
    return alu_out;
  }

 public:
  static int64_t StopSimAt;

  // Runtime call support.
  static void* RedirectNativeFunction(void* nativeFunction,
                                      ABIFunctionType type);

 private:
  enum Exception {
    none,
    kIntegerOverflow,
    kIntegerUnderflow,
    kDivideByZero,
    kNumExceptions,
    // RISCV illegual instruction exception
    kIllegalInstruction,
  };
  int16_t exceptions[kNumExceptions];

  // Exceptions.
  void SignalException(Exception e);

  // Handle return value for runtime FP functions.
  void setCallResultDouble(double result);
  void setCallResultFloat(float result);
  void setCallResult(int64_t res);
  void setCallResult(__int128 res);

  void callInternal(uint8_t* entry);

  // Architecture state.
  // Registers.
  int64_t registers_[kNumSimuRegisters];
  // Coprocessor Registers.
  int64_t FPUregisters_[kNumFPURegisters];
  // FPU control register.
  uint32_t FCSR_;

  bool LLBit_;
  uintptr_t LLAddr_;
  int64_t lastLLValue_;

  // Simulator support.
  char* stack_;
  uintptr_t stackLimit_;
  bool pc_modified_;
  int64_t icount_;
  int64_t break_count_;

  // Debugger input.
  char* lastDebuggerInput_;

  intptr_t* watch_address_ = nullptr;
  intptr_t watch_value_ = 0;

  // Registered breakpoints.
  SimInstruction* break_pc_;
  Instr break_instr_;
  EmbeddedVector<char, 256> trace_buf_;

  // Single-stepping support
  bool single_stepping_;
  SingleStepCallback single_step_callback_;
  void* single_step_callback_arg_;

  // A stop is watched if its code is less than kNumOfWatchedStops.
  // Only watched stops support enabling/disabling and the counter feature.
  static const uint32_t kNumOfWatchedStops = 256;

  // Stop is disabled if bit 31 is set.
  static const uint32_t kStopDisabledBit = 1U << 31;

  // A stop is enabled, meaning the simulator will stop when meeting the
  // instruction, if bit 31 of watchedStops_[code].count is unset.
  // The value watchedStops_[code].count & ~(1 << 31) indicates how many times
  // the breakpoint was hit or gone through.
  struct StopCountAndDesc {
    uint32_t count_;
    char* desc_;
  };
  StopCountAndDesc watchedStops_[kNumOfWatchedStops];
};

// Process wide simulator state.
class SimulatorProcess {
  friend class Redirection;
  friend class AutoLockSimulatorCache;

 private:
  // ICache checking.
  struct ICacheHasher {
    typedef void* Key;
    typedef void* Lookup;
    static HashNumber hash(const Lookup& l);
    static bool match(const Key& k, const Lookup& l);
  };

 public:
  typedef HashMap<void*, CachePage*, ICacheHasher, SystemAllocPolicy> ICacheMap;

  static mozilla::Atomic<size_t, mozilla::ReleaseAcquire>
      ICacheCheckingDisableCount;
  static void FlushICache(void* start, size_t size);

  static void checkICacheLocked(SimInstruction* instr);

  static bool initialize() {
    singleton_ = js_new<SimulatorProcess>();
    return singleton_;
  }
  static void destroy() {
    js_delete(singleton_);
    singleton_ = nullptr;
  }

  SimulatorProcess();
  ~SimulatorProcess();

 private:
  static SimulatorProcess* singleton_;

  // This lock creates a critical section around 'redirection_' and
  // 'icache_', which are referenced both by the execution engine
  // and by the off-thread compiler (see Redirection::Get in the cpp file).
  Mutex cacheLock_ MOZ_UNANNOTATED;

  Redirection* redirection_;
  ICacheMap icache_;

 public:
  static ICacheMap& icache() {
    // Technically we need the lock to access the innards of the
    // icache, not to take its address, but the latter condition
    // serves as a useful complement to the former.
    singleton_->cacheLock_.assertOwnedByCurrentThread();
    return singleton_->icache_;
  }

  static Redirection* redirection() {
    singleton_->cacheLock_.assertOwnedByCurrentThread();
    return singleton_->redirection_;
  }

  static void setRedirection(js::jit::Redirection* redirection) {
    singleton_->cacheLock_.assertOwnedByCurrentThread();
    singleton_->redirection_ = redirection;
  }
};

}  // namespace jit
}  // namespace js

#endif /* JS_SIMULATOR_MIPS64 */

#endif /* jit_riscv64_Simulator_riscv64_h */

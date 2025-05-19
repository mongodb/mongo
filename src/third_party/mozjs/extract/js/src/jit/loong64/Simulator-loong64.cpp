/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80: */
// Copyright 2020 the V8 project authors. All rights reserved.
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

#include "jit/loong64/Simulator-loong64.h"

#include <float.h>
#include <limits>

#include "jit/AtomicOperations.h"
#include "jit/loong64/Assembler-loong64.h"
#include "js/Conversions.h"
#include "threading/LockGuard.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmSignalHandlers.h"

#define I8(v) static_cast<int8_t>(v)
#define I16(v) static_cast<int16_t>(v)
#define U16(v) static_cast<uint16_t>(v)
#define I32(v) static_cast<int32_t>(v)
#define U32(v) static_cast<uint32_t>(v)
#define I64(v) static_cast<int64_t>(v)
#define U64(v) static_cast<uint64_t>(v)
#define I128(v) static_cast<__int128_t>(v)
#define U128(v) static_cast<__uint128_t>(v)

#define I32_CHECK(v)                   \
  ({                                   \
    MOZ_ASSERT(I64(I32(v)) == I64(v)); \
    I32((v));                          \
  })

namespace js {
namespace jit {

static int64_t MultiplyHighSigned(int64_t u, int64_t v) {
  uint64_t u0, v0, w0;
  int64_t u1, v1, w1, w2, t;

  u0 = u & 0xFFFFFFFFL;
  u1 = u >> 32;
  v0 = v & 0xFFFFFFFFL;
  v1 = v >> 32;

  w0 = u0 * v0;
  t = u1 * v0 + (w0 >> 32);
  w1 = t & 0xFFFFFFFFL;
  w2 = t >> 32;
  w1 = u0 * v1 + w1;

  return u1 * v1 + w2 + (w1 >> 32);
}

static uint64_t MultiplyHighUnsigned(uint64_t u, uint64_t v) {
  uint64_t u0, v0, w0;
  uint64_t u1, v1, w1, w2, t;

  u0 = u & 0xFFFFFFFFL;
  u1 = u >> 32;
  v0 = v & 0xFFFFFFFFL;
  v1 = v >> 32;

  w0 = u0 * v0;
  t = u1 * v0 + (w0 >> 32);
  w1 = t & 0xFFFFFFFFL;
  w2 = t >> 32;
  w1 = u0 * v1 + w1;

  return u1 * v1 + w2 + (w1 >> 32);
}

// Precondition: 0 <= shift < 32
inline constexpr uint32_t RotateRight32(uint32_t value, uint32_t shift) {
  return (value >> shift) | (value << ((32 - shift) & 31));
}

// Precondition: 0 <= shift < 32
inline constexpr uint32_t RotateLeft32(uint32_t value, uint32_t shift) {
  return (value << shift) | (value >> ((32 - shift) & 31));
}

// Precondition: 0 <= shift < 64
inline constexpr uint64_t RotateRight64(uint64_t value, uint64_t shift) {
  return (value >> shift) | (value << ((64 - shift) & 63));
}

// Precondition: 0 <= shift < 64
inline constexpr uint64_t RotateLeft64(uint64_t value, uint64_t shift) {
  return (value << shift) | (value >> ((64 - shift) & 63));
}

// break instr with MAX_BREAK_CODE.
static const Instr kCallRedirInstr = op_break | CODEMask;

// -----------------------------------------------------------------------------
// LoongArch64 assembly various constants.

class SimInstruction {
 public:
  enum {
    kInstrSize = 4,
    // On LoongArch, PC cannot actually be directly accessed. We behave as if PC
    // was always the value of the current instruction being executed.
    kPCReadOffset = 0
  };

  // Get the raw instruction bits.
  inline Instr instructionBits() const {
    return *reinterpret_cast<const Instr*>(this);
  }

  // Set the raw instruction bits to value.
  inline void setInstructionBits(Instr value) {
    *reinterpret_cast<Instr*>(this) = value;
  }

  // Read one particular bit out of the instruction bits.
  inline int bit(int nr) const { return (instructionBits() >> nr) & 1; }

  // Read a bit field out of the instruction bits.
  inline int bits(int hi, int lo) const {
    return (instructionBits() >> lo) & ((2 << (hi - lo)) - 1);
  }

  // Instruction type.
  enum Type {
    kUnsupported = -1,
    kOp6Type,
    kOp7Type,
    kOp8Type,
    kOp10Type,
    kOp11Type,
    kOp12Type,
    kOp14Type,
    kOp15Type,
    kOp16Type,
    kOp17Type,
    kOp22Type,
    kOp24Type
  };

  // Get the encoding type of the instruction.
  Type instructionType() const;

  inline int rjValue() const { return bits(RJShift + RJBits - 1, RJShift); }

  inline int rkValue() const { return bits(RKShift + RKBits - 1, RKShift); }

  inline int rdValue() const { return bits(RDShift + RDBits - 1, RDShift); }

  inline int sa2Value() const { return bits(SAShift + SA2Bits - 1, SAShift); }

  inline int sa3Value() const { return bits(SAShift + SA3Bits - 1, SAShift); }

  inline int lsbwValue() const {
    return bits(LSBWShift + LSBWBits - 1, LSBWShift);
  }

  inline int msbwValue() const {
    return bits(MSBWShift + MSBWBits - 1, MSBWShift);
  }

  inline int lsbdValue() const {
    return bits(LSBDShift + LSBDBits - 1, LSBDShift);
  }

  inline int msbdValue() const {
    return bits(MSBDShift + MSBDBits - 1, MSBDShift);
  }

  inline int fdValue() const { return bits(FDShift + FDBits - 1, FDShift); }

  inline int fjValue() const { return bits(FJShift + FJBits - 1, FJShift); }

  inline int fkValue() const { return bits(FKShift + FKBits - 1, FKShift); }

  inline int faValue() const { return bits(FAShift + FABits - 1, FAShift); }

  inline int cdValue() const { return bits(CDShift + CDBits - 1, CDShift); }

  inline int cjValue() const { return bits(CJShift + CJBits - 1, CJShift); }

  inline int caValue() const { return bits(CAShift + CABits - 1, CAShift); }

  inline int condValue() const {
    return bits(CONDShift + CONDBits - 1, CONDShift);
  }

  inline int imm5Value() const {
    return bits(Imm5Shift + Imm5Bits - 1, Imm5Shift);
  }

  inline int imm6Value() const {
    return bits(Imm6Shift + Imm6Bits - 1, Imm6Shift);
  }

  inline int imm12Value() const {
    return bits(Imm12Shift + Imm12Bits - 1, Imm12Shift);
  }

  inline int imm14Value() const {
    return bits(Imm14Shift + Imm14Bits - 1, Imm14Shift);
  }

  inline int imm16Value() const {
    return bits(Imm16Shift + Imm16Bits - 1, Imm16Shift);
  }

  inline int imm20Value() const {
    return bits(Imm20Shift + Imm20Bits - 1, Imm20Shift);
  }

  inline int32_t imm26Value() const {
    return bits(Imm26Shift + Imm26Bits - 1, Imm26Shift);
  }

  // Say if the instruction is a debugger break/trap.
  bool isTrap() const;

 private:
  SimInstruction() = delete;
  SimInstruction(const SimInstruction& other) = delete;
  void operator=(const SimInstruction& other) = delete;
};

bool SimInstruction::isTrap() const {
  // is break??
  switch (bits(31, 15) << 15) {
    case op_break:
      return (instructionBits() != kCallRedirInstr) && (bits(15, 0) != 6);
    default:
      return false;
  };
}

SimInstruction::Type SimInstruction::instructionType() const {
  SimInstruction::Type kType = kUnsupported;

  // Check for kOp6Type
  switch (bits(31, 26) << 26) {
    case op_beqz:
    case op_bnez:
    case op_bcz:
    case op_jirl:
    case op_b:
    case op_bl:
    case op_beq:
    case op_bne:
    case op_blt:
    case op_bge:
    case op_bltu:
    case op_bgeu:
    case op_addu16i_d:
      kType = kOp6Type;
      break;
    default:
      kType = kUnsupported;
  }

  if (kType == kUnsupported) {
    // Check for kOp7Type
    switch (bits(31, 25) << 25) {
      case op_lu12i_w:
      case op_lu32i_d:
      case op_pcaddi:
      case op_pcalau12i:
      case op_pcaddu12i:
      case op_pcaddu18i:
        kType = kOp7Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp8Type
    switch (bits(31, 24) << 24) {
      case op_ll_w:
      case op_sc_w:
      case op_ll_d:
      case op_sc_d:
      case op_ldptr_w:
      case op_stptr_w:
      case op_ldptr_d:
      case op_stptr_d:
        kType = kOp8Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp10Type
    switch (bits(31, 22) << 22) {
      case op_bstrins_d:
      case op_bstrpick_d:
      case op_slti:
      case op_sltui:
      case op_addi_w:
      case op_addi_d:
      case op_lu52i_d:
      case op_andi:
      case op_ori:
      case op_xori:
      case op_ld_b:
      case op_ld_h:
      case op_ld_w:
      case op_ld_d:
      case op_st_b:
      case op_st_h:
      case op_st_w:
      case op_st_d:
      case op_ld_bu:
      case op_ld_hu:
      case op_ld_wu:
      case op_preld:
      case op_fld_s:
      case op_fst_s:
      case op_fld_d:
      case op_fst_d:
      case op_bstr_w:  // BSTRINS_W & BSTRPICK_W
        kType = kOp10Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp11Type
    switch (bits(31, 21) << 21) {
      case op_bstr_w:
        kType = kOp11Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp12Type
    switch (bits(31, 20) << 20) {
      case op_fmadd_s:
      case op_fmadd_d:
      case op_fmsub_s:
      case op_fmsub_d:
      case op_fnmadd_s:
      case op_fnmadd_d:
      case op_fnmsub_s:
      case op_fnmsub_d:
      case op_fcmp_cond_s:
      case op_fcmp_cond_d:
        kType = kOp12Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp14Type
    switch (bits(31, 18) << 18) {
      case op_bytepick_d:
      case op_fsel:
        kType = kOp14Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp15Type
    switch (bits(31, 17) << 17) {
      case op_bytepick_w:
      case op_alsl_w:
      case op_alsl_wu:
      case op_alsl_d:
        kType = kOp15Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp16Type
    switch (bits(31, 16) << 16) {
      case op_slli_d:
      case op_srli_d:
      case op_srai_d:
      case op_rotri_d:
        kType = kOp16Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp17Type
    switch (bits(31, 15) << 15) {
      case op_slli_w:
      case op_srli_w:
      case op_srai_w:
      case op_rotri_w:
      case op_add_w:
      case op_add_d:
      case op_sub_w:
      case op_sub_d:
      case op_slt:
      case op_sltu:
      case op_maskeqz:
      case op_masknez:
      case op_nor:
      case op_and:
      case op_or:
      case op_xor:
      case op_orn:
      case op_andn:
      case op_sll_w:
      case op_srl_w:
      case op_sra_w:
      case op_sll_d:
      case op_srl_d:
      case op_sra_d:
      case op_rotr_w:
      case op_rotr_d:
      case op_mul_w:
      case op_mul_d:
      case op_mulh_d:
      case op_mulh_du:
      case op_mulh_w:
      case op_mulh_wu:
      case op_mulw_d_w:
      case op_mulw_d_wu:
      case op_div_w:
      case op_mod_w:
      case op_div_wu:
      case op_mod_wu:
      case op_div_d:
      case op_mod_d:
      case op_div_du:
      case op_mod_du:
      case op_break:
      case op_fadd_s:
      case op_fadd_d:
      case op_fsub_s:
      case op_fsub_d:
      case op_fmul_s:
      case op_fmul_d:
      case op_fdiv_s:
      case op_fdiv_d:
      case op_fmax_s:
      case op_fmax_d:
      case op_fmin_s:
      case op_fmin_d:
      case op_fmaxa_s:
      case op_fmaxa_d:
      case op_fmina_s:
      case op_fmina_d:
      case op_fcopysign_s:
      case op_fcopysign_d:
      case op_ldx_b:
      case op_ldx_h:
      case op_ldx_w:
      case op_ldx_d:
      case op_stx_b:
      case op_stx_h:
      case op_stx_w:
      case op_stx_d:
      case op_ldx_bu:
      case op_ldx_hu:
      case op_ldx_wu:
      case op_fldx_s:
      case op_fldx_d:
      case op_fstx_s:
      case op_fstx_d:
      case op_amswap_w:
      case op_amswap_d:
      case op_amadd_w:
      case op_amadd_d:
      case op_amand_w:
      case op_amand_d:
      case op_amor_w:
      case op_amor_d:
      case op_amxor_w:
      case op_amxor_d:
      case op_ammax_w:
      case op_ammax_d:
      case op_ammin_w:
      case op_ammin_d:
      case op_ammax_wu:
      case op_ammax_du:
      case op_ammin_wu:
      case op_ammin_du:
      case op_amswap_db_w:
      case op_amswap_db_d:
      case op_amadd_db_w:
      case op_amadd_db_d:
      case op_amand_db_w:
      case op_amand_db_d:
      case op_amor_db_w:
      case op_amor_db_d:
      case op_amxor_db_w:
      case op_amxor_db_d:
      case op_ammax_db_w:
      case op_ammax_db_d:
      case op_ammin_db_w:
      case op_ammin_db_d:
      case op_ammax_db_wu:
      case op_ammax_db_du:
      case op_ammin_db_wu:
      case op_ammin_db_du:
      case op_dbar:
      case op_ibar:
        kType = kOp17Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp22Type
    switch (bits(31, 10) << 10) {
      case op_clo_w:
      case op_clz_w:
      case op_cto_w:
      case op_ctz_w:
      case op_clo_d:
      case op_clz_d:
      case op_cto_d:
      case op_ctz_d:
      case op_revb_2h:
      case op_revb_4h:
      case op_revb_2w:
      case op_revb_d:
      case op_revh_2w:
      case op_revh_d:
      case op_bitrev_4b:
      case op_bitrev_8b:
      case op_bitrev_w:
      case op_bitrev_d:
      case op_ext_w_h:
      case op_ext_w_b:
      case op_fabs_s:
      case op_fabs_d:
      case op_fneg_s:
      case op_fneg_d:
      case op_fsqrt_s:
      case op_fsqrt_d:
      case op_fmov_s:
      case op_fmov_d:
      case op_movgr2fr_w:
      case op_movgr2fr_d:
      case op_movgr2frh_w:
      case op_movfr2gr_s:
      case op_movfr2gr_d:
      case op_movfrh2gr_s:
      case op_movfcsr2gr:
      case op_movfr2cf:
      case op_movgr2cf:
      case op_fcvt_s_d:
      case op_fcvt_d_s:
      case op_ftintrm_w_s:
      case op_ftintrm_w_d:
      case op_ftintrm_l_s:
      case op_ftintrm_l_d:
      case op_ftintrp_w_s:
      case op_ftintrp_w_d:
      case op_ftintrp_l_s:
      case op_ftintrp_l_d:
      case op_ftintrz_w_s:
      case op_ftintrz_w_d:
      case op_ftintrz_l_s:
      case op_ftintrz_l_d:
      case op_ftintrne_w_s:
      case op_ftintrne_w_d:
      case op_ftintrne_l_s:
      case op_ftintrne_l_d:
      case op_ftint_w_s:
      case op_ftint_w_d:
      case op_ftint_l_s:
      case op_ftint_l_d:
      case op_ffint_s_w:
      case op_ffint_s_l:
      case op_ffint_d_w:
      case op_ffint_d_l:
      case op_frint_s:
      case op_frint_d:
        kType = kOp22Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  if (kType == kUnsupported) {
    // Check for kOp24Type
    switch (bits(31, 8) << 8) {
      case op_movcf2fr:
      case op_movcf2gr:
        kType = kOp24Type;
        break;
      default:
        kType = kUnsupported;
    }
  }

  return kType;
}

// C/C++ argument slots size.
const int kCArgSlotCount = 0;
const int kCArgsSlotsSize = kCArgSlotCount * sizeof(uintptr_t);

class CachePage {
 public:
  static const int LINE_VALID = 0;
  static const int LINE_INVALID = 1;

  static const int kPageShift = 12;
  static const int kPageSize = 1 << kPageShift;
  static const int kPageMask = kPageSize - 1;
  static const int kLineShift = 2;  // The cache line is only 4 bytes right now.
  static const int kLineLength = 1 << kLineShift;
  static const int kLineMask = kLineLength - 1;

  CachePage() { memset(&validity_map_, LINE_INVALID, sizeof(validity_map_)); }

  char* validityByte(int offset) {
    return &validity_map_[offset >> kLineShift];
  }

  char* cachedData(int offset) { return &data_[offset]; }

 private:
  char data_[kPageSize];  // The cached data.
  static const int kValidityMapSize = kPageSize >> kLineShift;
  char validity_map_[kValidityMapSize];  // One byte per line.
};

// Protects the icache() and redirection() properties of the
// Simulator.
class AutoLockSimulatorCache : public LockGuard<Mutex> {
  using Base = LockGuard<Mutex>;

 public:
  explicit AutoLockSimulatorCache()
      : Base(SimulatorProcess::singleton_->cacheLock_) {}
};

mozilla::Atomic<size_t, mozilla::ReleaseAcquire>
    SimulatorProcess::ICacheCheckingDisableCount(
        1);  // Checking is disabled by default.
SimulatorProcess* SimulatorProcess::singleton_ = nullptr;

int64_t Simulator::StopSimAt = -1;

Simulator* Simulator::Create() {
  auto sim = MakeUnique<Simulator>();
  if (!sim) {
    return nullptr;
  }

  if (!sim->init()) {
    return nullptr;
  }

  int64_t stopAt;
  char* stopAtStr = getenv("LOONG64_SIM_STOP_AT");
  if (stopAtStr && sscanf(stopAtStr, "%" PRIi64, &stopAt) == 1) {
    fprintf(stderr, "\nStopping simulation at icount %" PRIi64 "\n", stopAt);
    Simulator::StopSimAt = stopAt;
  }

  return sim.release();
}

void Simulator::Destroy(Simulator* sim) { js_delete(sim); }

// The loong64Debugger class is used by the simulator while debugging simulated
// code.
class loong64Debugger {
 public:
  explicit loong64Debugger(Simulator* sim) : sim_(sim) {}

  void stop(SimInstruction* instr);
  void debug();
  // Print all registers with a nice formatting.
  void printAllRegs();
  void printAllRegsIncludingFPU();

 private:
  // We set the breakpoint code to 0x7fff to easily recognize it.
  static const Instr kBreakpointInstr = op_break | (0x7fff & CODEMask);
  static const Instr kNopInstr = 0x0;

  Simulator* sim_;

  int64_t getRegisterValue(int regnum);
  int64_t getFPURegisterValueLong(int regnum);
  float getFPURegisterValueFloat(int regnum);
  double getFPURegisterValueDouble(int regnum);
  bool getValue(const char* desc, int64_t* value);

  // Set or delete a breakpoint. Returns true if successful.
  bool setBreakpoint(SimInstruction* breakpc);
  bool deleteBreakpoint(SimInstruction* breakpc);

  // Undo and redo all breakpoints. This is needed to bracket disassembly and
  // execution to skip past breakpoints when run from the debugger.
  void undoBreakpoints();
  void redoBreakpoints();
};

static void UNIMPLEMENTED() {
  printf("UNIMPLEMENTED instruction.\n");
  MOZ_CRASH();
}
static void UNREACHABLE() {
  printf("UNREACHABLE instruction.\n");
  MOZ_CRASH();
}
static void UNSUPPORTED() {
  printf("Unsupported instruction.\n");
  MOZ_CRASH();
}

void loong64Debugger::stop(SimInstruction* instr) {
  // Get the stop code.
  uint32_t code = instr->bits(25, 6);
  // Retrieve the encoded address, which comes just after this stop.
  char* msg =
      *reinterpret_cast<char**>(sim_->get_pc() + SimInstruction::kInstrSize);
  // Update this stop description.
  if (!sim_->watchedStops_[code].desc_) {
    sim_->watchedStops_[code].desc_ = msg;
  }
  // Print the stop message and code if it is not the default code.
  if (code != kMaxStopCode) {
    printf("Simulator hit stop %u: %s\n", code, msg);
  } else {
    printf("Simulator hit %s\n", msg);
  }
  sim_->set_pc(sim_->get_pc() + 2 * SimInstruction::kInstrSize);
  debug();
}

int64_t loong64Debugger::getRegisterValue(int regnum) {
  if (regnum == kPCRegister) {
    return sim_->get_pc();
  }
  return sim_->getRegister(regnum);
}

int64_t loong64Debugger::getFPURegisterValueLong(int regnum) {
  return sim_->getFpuRegister(regnum);
}

float loong64Debugger::getFPURegisterValueFloat(int regnum) {
  return sim_->getFpuRegisterFloat(regnum);
}

double loong64Debugger::getFPURegisterValueDouble(int regnum) {
  return sim_->getFpuRegisterDouble(regnum);
}

bool loong64Debugger::getValue(const char* desc, int64_t* value) {
  Register reg = Register::FromName(desc);
  if (reg != InvalidReg) {
    *value = getRegisterValue(reg.code());
    return true;
  }

  if (strncmp(desc, "0x", 2) == 0) {
    return sscanf(desc + 2, "%lx", reinterpret_cast<uint64_t*>(value)) == 1;
  }
  return sscanf(desc, "%lu", reinterpret_cast<uint64_t*>(value)) == 1;
}

bool loong64Debugger::setBreakpoint(SimInstruction* breakpc) {
  // Check if a breakpoint can be set. If not return without any side-effects.
  if (sim_->break_pc_ != nullptr) {
    return false;
  }

  // Set the breakpoint.
  sim_->break_pc_ = breakpc;
  sim_->break_instr_ = breakpc->instructionBits();
  // Not setting the breakpoint instruction in the code itself. It will be set
  // when the debugger shell continues.
  return true;
}

bool loong64Debugger::deleteBreakpoint(SimInstruction* breakpc) {
  if (sim_->break_pc_ != nullptr) {
    sim_->break_pc_->setInstructionBits(sim_->break_instr_);
  }

  sim_->break_pc_ = nullptr;
  sim_->break_instr_ = 0;
  return true;
}

void loong64Debugger::undoBreakpoints() {
  if (sim_->break_pc_) {
    sim_->break_pc_->setInstructionBits(sim_->break_instr_);
  }
}

void loong64Debugger::redoBreakpoints() {
  if (sim_->break_pc_) {
    sim_->break_pc_->setInstructionBits(kBreakpointInstr);
  }
}

void loong64Debugger::printAllRegs() {
  int64_t value;
  for (uint32_t i = 0; i < Registers::Total; i++) {
    value = getRegisterValue(i);
    printf("%3s: 0x%016" PRIx64 " %20" PRIi64 "   ", Registers::GetName(i),
           value, value);

    if (i % 2) {
      printf("\n");
    }
  }
  printf("\n");

  value = getRegisterValue(Simulator::pc);
  printf(" pc: 0x%016" PRIx64 "\n", value);
}

void loong64Debugger::printAllRegsIncludingFPU() {
  printAllRegs();

  printf("\n\n");
  // f0, f1, f2, ... f31.
  for (uint32_t i = 0; i < FloatRegisters::TotalPhys; i++) {
    printf("%3s: 0x%016" PRIi64 "\tflt: %-8.4g\tdbl: %-16.4g\n",
           FloatRegisters::GetName(i), getFPURegisterValueLong(i),
           getFPURegisterValueFloat(i), getFPURegisterValueDouble(i));
  }
}

static char* ReadLine(const char* prompt) {
  UniqueChars result;
  char lineBuf[256];
  int offset = 0;
  bool keepGoing = true;
  fprintf(stdout, "%s", prompt);
  fflush(stdout);
  while (keepGoing) {
    if (fgets(lineBuf, sizeof(lineBuf), stdin) == nullptr) {
      // fgets got an error. Just give up.
      return nullptr;
    }
    int len = strlen(lineBuf);
    if (len > 0 && lineBuf[len - 1] == '\n') {
      // Since we read a new line we are done reading the line. This
      // will exit the loop after copying this buffer into the result.
      keepGoing = false;
    }
    if (!result) {
      // Allocate the initial result and make room for the terminating '\0'
      result.reset(js_pod_malloc<char>(len + 1));
      if (!result) {
        return nullptr;
      }
    } else {
      // Allocate a new result with enough room for the new addition.
      int new_len = offset + len + 1;
      char* new_result = js_pod_malloc<char>(new_len);
      if (!new_result) {
        return nullptr;
      }
      // Copy the existing input into the new array and set the new
      // array as the result.
      memcpy(new_result, result.get(), offset * sizeof(char));
      result.reset(new_result);
    }
    // Copy the newly read line into the result.
    memcpy(result.get() + offset, lineBuf, len * sizeof(char));
    offset += len;
  }

  MOZ_ASSERT(result);
  result[offset] = '\0';
  return result.release();
}

static void DisassembleInstruction(uint64_t pc) {
  printf("Not supported on loongarch64 yet\n");
}

void loong64Debugger::debug() {
  intptr_t lastPC = -1;
  bool done = false;

#define COMMAND_SIZE 63
#define ARG_SIZE 255

#define STR(a) #a
#define XSTR(a) STR(a)

  char cmd[COMMAND_SIZE + 1];
  char arg1[ARG_SIZE + 1];
  char arg2[ARG_SIZE + 1];
  char* argv[3] = {cmd, arg1, arg2};

  // Make sure to have a proper terminating character if reaching the limit.
  cmd[COMMAND_SIZE] = 0;
  arg1[ARG_SIZE] = 0;
  arg2[ARG_SIZE] = 0;

  // Undo all set breakpoints while running in the debugger shell. This will
  // make them invisible to all commands.
  undoBreakpoints();

  while (!done && (sim_->get_pc() != Simulator::end_sim_pc)) {
    if (lastPC != sim_->get_pc()) {
      DisassembleInstruction(sim_->get_pc());
      printf("  0x%016" PRIi64 "  \n", sim_->get_pc());
      lastPC = sim_->get_pc();
    }
    char* line = ReadLine("sim> ");
    if (line == nullptr) {
      break;
    } else {
      char* last_input = sim_->lastDebuggerInput();
      if (strcmp(line, "\n") == 0 && last_input != nullptr) {
        line = last_input;
      } else {
        // Ownership is transferred to sim_;
        sim_->setLastDebuggerInput(line);
      }
      // Use sscanf to parse the individual parts of the command line. At the
      // moment no command expects more than two parameters.
      int argc = sscanf(line,
                              "%" XSTR(COMMAND_SIZE) "s "
                              "%" XSTR(ARG_SIZE) "s "
                              "%" XSTR(ARG_SIZE) "s",
                              cmd, arg1, arg2);
      if ((strcmp(cmd, "si") == 0) || (strcmp(cmd, "stepi") == 0)) {
        SimInstruction* instr =
            reinterpret_cast<SimInstruction*>(sim_->get_pc());
        if (!instr->isTrap()) {
          sim_->instructionDecode(
              reinterpret_cast<SimInstruction*>(sim_->get_pc()));
        } else {
          // Allow si to jump over generated breakpoints.
          printf("/!\\ Jumping over generated breakpoint.\n");
          sim_->set_pc(sim_->get_pc() + SimInstruction::kInstrSize);
        }
        sim_->icount_++;
      } else if ((strcmp(cmd, "c") == 0) || (strcmp(cmd, "cont") == 0)) {
        // Execute the one instruction we broke at with breakpoints disabled.
        sim_->instructionDecode(
            reinterpret_cast<SimInstruction*>(sim_->get_pc()));
        sim_->icount_++;
        // Leave the debugger shell.
        done = true;
      } else if ((strcmp(cmd, "p") == 0) || (strcmp(cmd, "print") == 0)) {
        if (argc == 2) {
          int64_t value;
          if (strcmp(arg1, "all") == 0) {
            printAllRegs();
          } else if (strcmp(arg1, "allf") == 0) {
            printAllRegsIncludingFPU();
          } else {
            Register reg = Register::FromName(arg1);
            FloatRegisters::Code fReg = FloatRegisters::FromName(arg1);
            if (reg != InvalidReg) {
              value = getRegisterValue(reg.code());
              printf("%s: 0x%016" PRIi64 " %20" PRIi64 " \n", arg1, value,
                     value);
            } else if (fReg != FloatRegisters::Invalid) {
              printf("%3s: 0x%016" PRIi64 "\tflt: %-8.4g\tdbl: %-16.4g\n",
                     FloatRegisters::GetName(fReg),
                     getFPURegisterValueLong(fReg),
                     getFPURegisterValueFloat(fReg),
                     getFPURegisterValueDouble(fReg));
            } else {
              printf("%s unrecognized\n", arg1);
            }
          }
        } else {
          printf("print <register> or print <fpu register> single\n");
        }
      } else if (strcmp(cmd, "stack") == 0 || strcmp(cmd, "mem") == 0) {
        int64_t* cur = nullptr;
        int64_t* end = nullptr;
        int next_arg = 1;

        if (strcmp(cmd, "stack") == 0) {
          cur = reinterpret_cast<int64_t*>(sim_->getRegister(Simulator::sp));
        } else {  // Command "mem".
          int64_t value;
          if (!getValue(arg1, &value)) {
            printf("%s unrecognized\n", arg1);
            continue;
          }
          cur = reinterpret_cast<int64_t*>(value);
          next_arg++;
        }

        int64_t words;
        if (argc == next_arg) {
          words = 10;
        } else {
          if (!getValue(argv[next_arg], &words)) {
            words = 10;
          }
        }
        end = cur + words;

        while (cur < end) {
          printf("  %p:  0x%016" PRIx64 " %20" PRIi64, cur, *cur, *cur);
          printf("\n");
          cur++;
        }

      } else if ((strcmp(cmd, "disasm") == 0) || (strcmp(cmd, "dpc") == 0) ||
                 (strcmp(cmd, "di") == 0)) {
        uint8_t* cur = nullptr;
        uint8_t* end = nullptr;

        if (argc == 1) {
          cur = reinterpret_cast<uint8_t*>(sim_->get_pc());
          end = cur + (10 * SimInstruction::kInstrSize);
        } else if (argc == 2) {
          Register reg = Register::FromName(arg1);
          if (reg != InvalidReg || strncmp(arg1, "0x", 2) == 0) {
            // The argument is an address or a register name.
            int64_t value;
            if (getValue(arg1, &value)) {
              cur = reinterpret_cast<uint8_t*>(value);
              // Disassemble 10 instructions at <arg1>.
              end = cur + (10 * SimInstruction::kInstrSize);
            }
          } else {
            // The argument is the number of instructions.
            int64_t value;
            if (getValue(arg1, &value)) {
              cur = reinterpret_cast<uint8_t*>(sim_->get_pc());
              // Disassemble <arg1> instructions.
              end = cur + (value * SimInstruction::kInstrSize);
            }
          }
        } else {
          int64_t value1;
          int64_t value2;
          if (getValue(arg1, &value1) && getValue(arg2, &value2)) {
            cur = reinterpret_cast<uint8_t*>(value1);
            end = cur + (value2 * SimInstruction::kInstrSize);
          }
        }

        while (cur < end) {
          DisassembleInstruction(uint64_t(cur));
          cur += SimInstruction::kInstrSize;
        }
      } else if (strcmp(cmd, "gdb") == 0) {
        printf("relinquishing control to gdb\n");
        asm("int $3");
        printf("regaining control from gdb\n");
      } else if (strcmp(cmd, "break") == 0) {
        if (argc == 2) {
          int64_t value;
          if (getValue(arg1, &value)) {
            if (!setBreakpoint(reinterpret_cast<SimInstruction*>(value))) {
              printf("setting breakpoint failed\n");
            }
          } else {
            printf("%s unrecognized\n", arg1);
          }
        } else {
          printf("break <address>\n");
        }
      } else if (strcmp(cmd, "del") == 0) {
        if (!deleteBreakpoint(nullptr)) {
          printf("deleting breakpoint failed\n");
        }
      } else if (strcmp(cmd, "flags") == 0) {
        printf("No flags on LOONG64 !\n");
      } else if (strcmp(cmd, "stop") == 0) {
        int64_t value;
        intptr_t stop_pc = sim_->get_pc() - 2 * SimInstruction::kInstrSize;
        SimInstruction* stop_instr = reinterpret_cast<SimInstruction*>(stop_pc);
        SimInstruction* msg_address = reinterpret_cast<SimInstruction*>(
            stop_pc + SimInstruction::kInstrSize);
        if ((argc == 2) && (strcmp(arg1, "unstop") == 0)) {
          // Remove the current stop.
          if (sim_->isStopInstruction(stop_instr)) {
            stop_instr->setInstructionBits(kNopInstr);
            msg_address->setInstructionBits(kNopInstr);
          } else {
            printf("Not at debugger stop.\n");
          }
        } else if (argc == 3) {
          // Print information about all/the specified breakpoint(s).
          if (strcmp(arg1, "info") == 0) {
            if (strcmp(arg2, "all") == 0) {
              printf("Stop information:\n");
              for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                   i++) {
                sim_->printStopInfo(i);
              }
            } else if (getValue(arg2, &value)) {
              sim_->printStopInfo(value);
            } else {
              printf("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "enable") == 0) {
            // Enable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                   i++) {
                sim_->enableStop(i);
              }
            } else if (getValue(arg2, &value)) {
              sim_->enableStop(value);
            } else {
              printf("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "disable") == 0) {
            // Disable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                   i++) {
                sim_->disableStop(i);
              }
            } else if (getValue(arg2, &value)) {
              sim_->disableStop(value);
            } else {
              printf("Unrecognized argument.\n");
            }
          }
        } else {
          printf("Wrong usage. Use help command for more information.\n");
        }
      } else if ((strcmp(cmd, "h") == 0) || (strcmp(cmd, "help") == 0)) {
        printf("cont\n");
        printf("  continue execution (alias 'c')\n");
        printf("stepi\n");
        printf("  step one instruction (alias 'si')\n");
        printf("print <register>\n");
        printf("  print register content (alias 'p')\n");
        printf("  use register name 'all' to print all registers\n");
        printf("printobject <register>\n");
        printf("  print an object from a register (alias 'po')\n");
        printf("stack [<words>]\n");
        printf("  dump stack content, default dump 10 words)\n");
        printf("mem <address> [<words>]\n");
        printf("  dump memory content, default dump 10 words)\n");
        printf("flags\n");
        printf("  print flags\n");
        printf("disasm [<instructions>]\n");
        printf("disasm [<address/register>]\n");
        printf("disasm [[<address/register>] <instructions>]\n");
        printf("  disassemble code, default is 10 instructions\n");
        printf("  from pc (alias 'di')\n");
        printf("gdb\n");
        printf("  enter gdb\n");
        printf("break <address>\n");
        printf("  set a break point on the address\n");
        printf("del\n");
        printf("  delete the breakpoint\n");
        printf("stop feature:\n");
        printf("  Description:\n");
        printf("    Stops are debug instructions inserted by\n");
        printf("    the Assembler::stop() function.\n");
        printf("    When hitting a stop, the Simulator will\n");
        printf("    stop and and give control to the Debugger.\n");
        printf("    All stop codes are watched:\n");
        printf("    - They can be enabled / disabled: the Simulator\n");
        printf("       will / won't stop when hitting them.\n");
        printf("    - The Simulator keeps track of how many times they \n");
        printf("      are met. (See the info command.) Going over a\n");
        printf("      disabled stop still increases its counter. \n");
        printf("  Commands:\n");
        printf("    stop info all/<code> : print infos about number <code>\n");
        printf("      or all stop(s).\n");
        printf("    stop enable/disable all/<code> : enables / disables\n");
        printf("      all or number <code> stop(s)\n");
        printf("    stop unstop\n");
        printf("      ignore the stop instruction at the current location\n");
        printf("      from now on\n");
      } else {
        printf("Unknown command: %s\n", cmd);
      }
    }
  }

  // Add all the breakpoints back to stop execution and enter the debugger
  // shell when hit.
  redoBreakpoints();

#undef COMMAND_SIZE
#undef ARG_SIZE

#undef STR
#undef XSTR
}

static bool AllOnOnePage(uintptr_t start, int size) {
  intptr_t start_page = (start & ~CachePage::kPageMask);
  intptr_t end_page = ((start + size) & ~CachePage::kPageMask);
  return start_page == end_page;
}

void Simulator::setLastDebuggerInput(char* input) {
  js_free(lastDebuggerInput_);
  lastDebuggerInput_ = input;
}

static CachePage* GetCachePageLocked(SimulatorProcess::ICacheMap& i_cache,
                                     void* page) {
  SimulatorProcess::ICacheMap::AddPtr p = i_cache.lookupForAdd(page);
  if (p) {
    return p->value();
  }
  AutoEnterOOMUnsafeRegion oomUnsafe;
  CachePage* new_page = js_new<CachePage>();
  if (!new_page || !i_cache.add(p, page, new_page)) {
    oomUnsafe.crash("Simulator CachePage");
  }
  return new_page;
}

// Flush from start up to and not including start + size.
static void FlushOnePageLocked(SimulatorProcess::ICacheMap& i_cache,
                               intptr_t start, int size) {
  MOZ_ASSERT(size <= CachePage::kPageSize);
  MOZ_ASSERT(AllOnOnePage(start, size - 1));
  MOZ_ASSERT((start & CachePage::kLineMask) == 0);
  MOZ_ASSERT((size & CachePage::kLineMask) == 0);
  void* page = reinterpret_cast<void*>(start & (~CachePage::kPageMask));
  int offset = (start & CachePage::kPageMask);
  CachePage* cache_page = GetCachePageLocked(i_cache, page);
  char* valid_bytemap = cache_page->validityByte(offset);
  memset(valid_bytemap, CachePage::LINE_INVALID, size >> CachePage::kLineShift);
}

static void FlushICacheLocked(SimulatorProcess::ICacheMap& i_cache,
                              void* start_addr, size_t size) {
  intptr_t start = reinterpret_cast<intptr_t>(start_addr);
  int intra_line = (start & CachePage::kLineMask);
  start -= intra_line;
  size += intra_line;
  size = ((size - 1) | CachePage::kLineMask) + 1;
  int offset = (start & CachePage::kPageMask);
  while (!AllOnOnePage(start, size - 1)) {
    int bytes_to_flush = CachePage::kPageSize - offset;
    FlushOnePageLocked(i_cache, start, bytes_to_flush);
    start += bytes_to_flush;
    size -= bytes_to_flush;
    MOZ_ASSERT((start & CachePage::kPageMask) == 0);
    offset = 0;
  }
  if (size != 0) {
    FlushOnePageLocked(i_cache, start, size);
  }
}

/* static */
void SimulatorProcess::checkICacheLocked(SimInstruction* instr) {
  intptr_t address = reinterpret_cast<intptr_t>(instr);
  void* page = reinterpret_cast<void*>(address & (~CachePage::kPageMask));
  void* line = reinterpret_cast<void*>(address & (~CachePage::kLineMask));
  int offset = (address & CachePage::kPageMask);
  CachePage* cache_page = GetCachePageLocked(icache(), page);
  char* cache_valid_byte = cache_page->validityByte(offset);
  bool cache_hit = (*cache_valid_byte == CachePage::LINE_VALID);
  char* cached_line = cache_page->cachedData(offset & ~CachePage::kLineMask);

  if (cache_hit) {
    // Check that the data in memory matches the contents of the I-cache.
    mozilla::DebugOnly<int> cmpret =
        memcmp(reinterpret_cast<void*>(instr), cache_page->cachedData(offset),
               SimInstruction::kInstrSize);
    MOZ_ASSERT(cmpret == 0);
  } else {
    // Cache miss.  Load memory into the cache.
    memcpy(cached_line, line, CachePage::kLineLength);
    *cache_valid_byte = CachePage::LINE_VALID;
  }
}

HashNumber SimulatorProcess::ICacheHasher::hash(const Lookup& l) {
  return U32(reinterpret_cast<uintptr_t>(l)) >> 2;
}

bool SimulatorProcess::ICacheHasher::match(const Key& k, const Lookup& l) {
  MOZ_ASSERT((reinterpret_cast<intptr_t>(k) & CachePage::kPageMask) == 0);
  MOZ_ASSERT((reinterpret_cast<intptr_t>(l) & CachePage::kPageMask) == 0);
  return k == l;
}

/* static */
void SimulatorProcess::FlushICache(void* start_addr, size_t size) {
  if (!ICacheCheckingDisableCount) {
    AutoLockSimulatorCache als;
    js::jit::FlushICacheLocked(icache(), start_addr, size);
  }
}

Simulator::Simulator() {
  // Set up simulator support first. Some of this information is needed to
  // setup the architecture state.

  // Note, allocation and anything that depends on allocated memory is
  // deferred until init(), in order to handle OOM properly.

  stack_ = nullptr;
  stackLimit_ = 0;
  pc_modified_ = false;
  icount_ = 0;
  break_count_ = 0;
  break_pc_ = nullptr;
  break_instr_ = 0;
  single_stepping_ = false;
  single_step_callback_ = nullptr;
  single_step_callback_arg_ = nullptr;

  // Set up architecture state.
  // All registers are initialized to zero to start with.
  for (int i = 0; i < Register::kNumSimuRegisters; i++) {
    registers_[i] = 0;
  }
  for (int i = 0; i < Simulator::FPURegister::kNumFPURegisters; i++) {
    FPUregisters_[i] = 0;
  }

  for (int i = 0; i < kNumCFRegisters; i++) {
    CFregisters_[i] = 0;
  }

  FCSR_ = 0;
  LLBit_ = false;
  LLAddr_ = 0;
  lastLLValue_ = 0;

  // The ra and pc are initialized to a known bad value that will cause an
  // access violation if the simulator ever tries to execute it.
  registers_[pc] = bad_ra;
  registers_[ra] = bad_ra;

  for (int i = 0; i < kNumExceptions; i++) {
    exceptions[i] = 0;
  }

  lastDebuggerInput_ = nullptr;
}

bool Simulator::init() {
  // Allocate 2MB for the stack. Note that we will only use 1MB, see below.
  static const size_t stackSize = 2 * 1024 * 1024;
  stack_ = js_pod_malloc<char>(stackSize);
  if (!stack_) {
    return false;
  }

  // Leave a safety margin of 1MB to prevent overrunning the stack when
  // pushing values (total stack size is 2MB).
  stackLimit_ = reinterpret_cast<uintptr_t>(stack_) + 1024 * 1024;

  // The sp is initialized to point to the bottom (high address) of the
  // allocated stack area. To be safe in potential stack underflows we leave
  // some buffer below.
  registers_[sp] = reinterpret_cast<int64_t>(stack_) + stackSize - 64;

  return true;
}

// When the generated code calls an external reference we need to catch that in
// the simulator.  The external reference will be a function compiled for the
// host architecture.  We need to call that function instead of trying to
// execute it with the simulator.  We do that by redirecting the external
// reference to a swi (software-interrupt) instruction that is handled by
// the simulator.  We write the original destination of the jump just at a known
// offset from the swi instruction so the simulator knows what to call.
class Redirection {
  friend class SimulatorProcess;

  // sim's lock must already be held.
  Redirection(void* nativeFunction, ABIFunctionType type)
      : nativeFunction_(nativeFunction),
        swiInstruction_(kCallRedirInstr),
        type_(type),
        next_(nullptr) {
    next_ = SimulatorProcess::redirection();
    if (!SimulatorProcess::ICacheCheckingDisableCount) {
      FlushICacheLocked(SimulatorProcess::icache(), addressOfSwiInstruction(),
                        SimInstruction::kInstrSize);
    }
    SimulatorProcess::setRedirection(this);
  }

 public:
  void* addressOfSwiInstruction() { return &swiInstruction_; }
  void* nativeFunction() const { return nativeFunction_; }
  ABIFunctionType type() const { return type_; }

  static Redirection* Get(void* nativeFunction, ABIFunctionType type) {
    AutoLockSimulatorCache als;

    Redirection* current = SimulatorProcess::redirection();
    for (; current != nullptr; current = current->next_) {
      if (current->nativeFunction_ == nativeFunction) {
        MOZ_ASSERT(current->type() == type);
        return current;
      }
    }

    // Note: we can't use js_new here because the constructor is private.
    AutoEnterOOMUnsafeRegion oomUnsafe;
    Redirection* redir = js_pod_malloc<Redirection>(1);
    if (!redir) {
      oomUnsafe.crash("Simulator redirection");
    }
    new (redir) Redirection(nativeFunction, type);
    return redir;
  }

  static Redirection* FromSwiInstruction(SimInstruction* swiInstruction) {
    uint8_t* addrOfSwi = reinterpret_cast<uint8_t*>(swiInstruction);
    uint8_t* addrOfRedirection =
        addrOfSwi - offsetof(Redirection, swiInstruction_);
    return reinterpret_cast<Redirection*>(addrOfRedirection);
  }

 private:
  void* nativeFunction_;
  uint32_t swiInstruction_;
  ABIFunctionType type_;
  Redirection* next_;
};

Simulator::~Simulator() { js_free(stack_); }

SimulatorProcess::SimulatorProcess()
    : cacheLock_(mutexid::SimulatorCacheLock), redirection_(nullptr) {
  if (getenv("LOONG64_SIM_ICACHE_CHECKS")) {
    ICacheCheckingDisableCount = 0;
  }
}

SimulatorProcess::~SimulatorProcess() {
  Redirection* r = redirection_;
  while (r) {
    Redirection* next = r->next_;
    js_delete(r);
    r = next;
  }
}

/* static */
void* Simulator::RedirectNativeFunction(void* nativeFunction,
                                        ABIFunctionType type) {
  Redirection* redirection = Redirection::Get(nativeFunction, type);
  return redirection->addressOfSwiInstruction();
}

// Get the active Simulator for the current thread.
Simulator* Simulator::Current() {
  JSContext* cx = TlsContext.get();
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
  return cx->simulator();
}

// Sets the register in the architecture state. It will also deal with updating
// Simulator internal state for special registers such as PC.
void Simulator::setRegister(int reg, int64_t value) {
  MOZ_ASSERT((reg >= 0) && (reg < Register::kNumSimuRegisters));
  if (reg == pc) {
    pc_modified_ = true;
  }

  // Zero register always holds 0.
  registers_[reg] = (reg == 0) ? 0 : value;
}

void Simulator::setFpuRegister(int fpureg, int64_t value) {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  FPUregisters_[fpureg] = value;
}

void Simulator::setFpuRegisterHiWord(int fpureg, int32_t value) {
  // Set ONLY upper 32-bits, leaving lower bits untouched.
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  int32_t* phiword;
  phiword = (reinterpret_cast<int32_t*>(&FPUregisters_[fpureg])) + 1;

  *phiword = value;
}

void Simulator::setFpuRegisterWord(int fpureg, int32_t value) {
  // Set ONLY lower 32-bits, leaving upper bits untouched.
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  int32_t* pword;
  pword = reinterpret_cast<int32_t*>(&FPUregisters_[fpureg]);

  *pword = value;
}

void Simulator::setFpuRegisterWordInvalidResult(float original, float rounded,
                                                int fpureg) {
  double max_int32 = static_cast<double>(INT32_MAX);
  double min_int32 = static_cast<double>(INT32_MIN);

  if (std::isnan(original)) {
    setFpuRegisterWord(fpureg, 0);
  } else if (rounded > max_int32) {
    setFpuRegister(fpureg, kFPUInvalidResult);
  } else if (rounded < min_int32) {
    setFpuRegister(fpureg, kFPUInvalidResultNegative);
  } else {
    UNREACHABLE();
  }
}

void Simulator::setFpuRegisterWordInvalidResult(double original, double rounded,
                                                int fpureg) {
  double max_int32 = static_cast<double>(INT32_MAX);
  double min_int32 = static_cast<double>(INT32_MIN);

  if (std::isnan(original)) {
    setFpuRegisterWord(fpureg, 0);
  } else if (rounded > max_int32) {
    setFpuRegisterWord(fpureg, kFPUInvalidResult);
  } else if (rounded < min_int32) {
    setFpuRegisterWord(fpureg, kFPUInvalidResultNegative);
  } else {
    UNREACHABLE();
  }
}

void Simulator::setFpuRegisterInvalidResult(float original, float rounded,
                                            int fpureg) {
  double max_int32 = static_cast<double>(INT32_MAX);
  double min_int32 = static_cast<double>(INT32_MIN);

  if (std::isnan(original)) {
    setFpuRegister(fpureg, 0);
  } else if (rounded > max_int32) {
    setFpuRegister(fpureg, kFPUInvalidResult);
  } else if (rounded < min_int32) {
    setFpuRegister(fpureg, kFPUInvalidResultNegative);
  } else {
    UNREACHABLE();
  }
}

void Simulator::setFpuRegisterInvalidResult(double original, double rounded,
                                            int fpureg) {
  double max_int32 = static_cast<double>(INT32_MAX);
  double min_int32 = static_cast<double>(INT32_MIN);

  if (std::isnan(original)) {
    setFpuRegister(fpureg, 0);
  } else if (rounded > max_int32) {
    setFpuRegister(fpureg, kFPUInvalidResult);
  } else if (rounded < min_int32) {
    setFpuRegister(fpureg, kFPUInvalidResultNegative);
  } else {
    UNREACHABLE();
  }
}

void Simulator::setFpuRegisterInvalidResult64(float original, float rounded,
                                              int fpureg) {
  // The value of INT64_MAX (2^63-1) can't be represented as double exactly,
  // loading the most accurate representation into max_int64, which is 2^63.
  double max_int64 = static_cast<double>(INT64_MAX);
  double min_int64 = static_cast<double>(INT64_MIN);

  if (std::isnan(original)) {
    setFpuRegister(fpureg, 0);
  } else if (rounded >= max_int64) {
    setFpuRegister(fpureg, kFPU64InvalidResult);
  } else if (rounded < min_int64) {
    setFpuRegister(fpureg, kFPU64InvalidResultNegative);
  } else {
    UNREACHABLE();
  }
}

void Simulator::setFpuRegisterInvalidResult64(double original, double rounded,
                                              int fpureg) {
  // The value of INT64_MAX (2^63-1) can't be represented as double exactly,
  // loading the most accurate representation into max_int64, which is 2^63.
  double max_int64 = static_cast<double>(INT64_MAX);
  double min_int64 = static_cast<double>(INT64_MIN);

  if (std::isnan(original)) {
    setFpuRegister(fpureg, 0);
  } else if (rounded >= max_int64) {
    setFpuRegister(fpureg, kFPU64InvalidResult);
  } else if (rounded < min_int64) {
    setFpuRegister(fpureg, kFPU64InvalidResultNegative);
  } else {
    UNREACHABLE();
  }
}

void Simulator::setFpuRegisterFloat(int fpureg, float value) {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  *mozilla::BitwiseCast<float*>(&FPUregisters_[fpureg]) = value;
}

void Simulator::setFpuRegisterDouble(int fpureg, double value) {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  *mozilla::BitwiseCast<double*>(&FPUregisters_[fpureg]) = value;
}

void Simulator::setCFRegister(int cfreg, bool value) {
  MOZ_ASSERT((cfreg >= 0) && (cfreg < kNumCFRegisters));
  CFregisters_[cfreg] = value;
}

bool Simulator::getCFRegister(int cfreg) const {
  MOZ_ASSERT((cfreg >= 0) && (cfreg < kNumCFRegisters));
  return CFregisters_[cfreg];
}

// Get the register from the architecture state. This function does handle
// the special case of accessing the PC register.
int64_t Simulator::getRegister(int reg) const {
  MOZ_ASSERT((reg >= 0) && (reg < Register::kNumSimuRegisters));
  if (reg == 0) {
    return 0;
  }
  return registers_[reg] + ((reg == pc) ? SimInstruction::kPCReadOffset : 0);
}

int64_t Simulator::getFpuRegister(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return FPUregisters_[fpureg];
}

int32_t Simulator::getFpuRegisterWord(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return *mozilla::BitwiseCast<int32_t*>(&FPUregisters_[fpureg]);
}

int32_t Simulator::getFpuRegisterSignedWord(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return *mozilla::BitwiseCast<int32_t*>(&FPUregisters_[fpureg]);
}

int32_t Simulator::getFpuRegisterHiWord(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return *((mozilla::BitwiseCast<int32_t*>(&FPUregisters_[fpureg])) + 1);
}

float Simulator::getFpuRegisterFloat(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return *mozilla::BitwiseCast<float*>(&FPUregisters_[fpureg]);
}

double Simulator::getFpuRegisterDouble(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return *mozilla::BitwiseCast<double*>(&FPUregisters_[fpureg]);
}

void Simulator::setCallResultDouble(double result) {
  setFpuRegisterDouble(f0, result);
}

void Simulator::setCallResultFloat(float result) {
  setFpuRegisterFloat(f0, result);
}

void Simulator::setCallResult(int64_t res) { setRegister(a0, res); }

void Simulator::setCallResult(__int128_t res) {
  setRegister(a0, I64(res));
  setRegister(a1, I64(res >> 64));
}

// Helper functions for setting and testing the FCSR register's bits.
void Simulator::setFCSRBit(uint32_t cc, bool value) {
  if (value) {
    FCSR_ |= (1 << cc);
  } else {
    FCSR_ &= ~(1 << cc);
  }
}

bool Simulator::testFCSRBit(uint32_t cc) { return FCSR_ & (1 << cc); }

unsigned int Simulator::getFCSRRoundingMode() {
  return FCSR_ & kFPURoundingModeMask;
}

// Sets the rounding error codes in FCSR based on the result of the rounding.
// Returns true if the operation was invalid.
template <typename T>
bool Simulator::setFCSRRoundError(double original, double rounded) {
  bool ret = false;

  setFCSRBit(kFCSRInexactCauseBit, false);
  setFCSRBit(kFCSRUnderflowCauseBit, false);
  setFCSRBit(kFCSROverflowCauseBit, false);
  setFCSRBit(kFCSRInvalidOpCauseBit, false);

  if (!std::isfinite(original) || !std::isfinite(rounded)) {
    setFCSRBit(kFCSRInvalidOpFlagBit, true);
    setFCSRBit(kFCSRInvalidOpCauseBit, true);
    ret = true;
  }

  if (original != rounded) {
    setFCSRBit(kFCSRInexactFlagBit, true);
    setFCSRBit(kFCSRInexactCauseBit, true);
  }

  if (rounded < DBL_MIN && rounded > -DBL_MIN && rounded != 0) {
    setFCSRBit(kFCSRUnderflowFlagBit, true);
    setFCSRBit(kFCSRUnderflowCauseBit, true);
    ret = true;
  }

  if ((long double)rounded > (long double)std::numeric_limits<T>::max() ||
      (long double)rounded < (long double)std::numeric_limits<T>::min()) {
    setFCSRBit(kFCSROverflowFlagBit, true);
    setFCSRBit(kFCSROverflowCauseBit, true);
    // The reference is not really clear but it seems this is required:
    setFCSRBit(kFCSRInvalidOpFlagBit, true);
    setFCSRBit(kFCSRInvalidOpCauseBit, true);
    ret = true;
  }

  return ret;
}

// For cvt instructions only
template <typename T>
void Simulator::roundAccordingToFCSR(T toRound, T* rounded,
                                     int32_t* rounded_int) {
  switch ((FCSR_ >> 8) & 3) {
    case kRoundToNearest:
      *rounded = std::floor(toRound + 0.5);
      *rounded_int = static_cast<int32_t>(*rounded);
      if ((*rounded_int & 1) != 0 && *rounded_int - toRound == 0.5) {
        // If the number is halfway between two integers,
        // round to the even one.
        *rounded_int -= 1;
        *rounded -= 1.;
      }
      break;
    case kRoundToZero:
      *rounded = trunc(toRound);
      *rounded_int = static_cast<int32_t>(*rounded);
      break;
    case kRoundToPlusInf:
      *rounded = std::ceil(toRound);
      *rounded_int = static_cast<int32_t>(*rounded);
      break;
    case kRoundToMinusInf:
      *rounded = std::floor(toRound);
      *rounded_int = static_cast<int32_t>(*rounded);
      break;
  }
}

template <typename T>
void Simulator::round64AccordingToFCSR(T toRound, T* rounded,
                                       int64_t* rounded_int) {
  switch ((FCSR_ >> 8) & 3) {
    case kRoundToNearest:
      *rounded = std::floor(toRound + 0.5);
      *rounded_int = static_cast<int64_t>(*rounded);
      if ((*rounded_int & 1) != 0 && *rounded_int - toRound == 0.5) {
        // If the number is halfway between two integers,
        // round to the even one.
        *rounded_int -= 1;
        *rounded -= 1.;
      }
      break;
    case kRoundToZero:
      *rounded = trunc(toRound);
      *rounded_int = static_cast<int64_t>(*rounded);
      break;
    case kRoundToPlusInf:
      *rounded = std::ceil(toRound);
      *rounded_int = static_cast<int64_t>(*rounded);
      break;
    case kRoundToMinusInf:
      *rounded = std::floor(toRound);
      *rounded_int = static_cast<int64_t>(*rounded);
      break;
  }
}

// Raw access to the PC register.
void Simulator::set_pc(int64_t value) {
  pc_modified_ = true;
  registers_[pc] = value;
}

bool Simulator::has_bad_pc() const {
  return ((registers_[pc] == bad_ra) || (registers_[pc] == end_sim_pc));
}

// Raw access to the PC register without the special adjustment when reading.
int64_t Simulator::get_pc() const { return registers_[pc]; }

JS::ProfilingFrameIterator::RegisterState Simulator::registerState() {
  wasm::RegisterState state;
  state.pc = (void*)get_pc();
  state.fp = (void*)getRegister(fp);
  state.sp = (void*)getRegister(sp);
  state.lr = (void*)getRegister(ra);
  return state;
}

uint8_t Simulator::readBU(uint64_t addr) {
  if (handleWasmSegFault(addr, 1)) {
    return 0xff;
  }

  uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
  return *ptr;
}

int8_t Simulator::readB(uint64_t addr) {
  if (handleWasmSegFault(addr, 1)) {
    return -1;
  }

  int8_t* ptr = reinterpret_cast<int8_t*>(addr);
  return *ptr;
}

void Simulator::writeB(uint64_t addr, uint8_t value) {
  if (handleWasmSegFault(addr, 1)) {
    return;
  }

  uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
  *ptr = value;
}

void Simulator::writeB(uint64_t addr, int8_t value) {
  if (handleWasmSegFault(addr, 1)) {
    return;
  }

  int8_t* ptr = reinterpret_cast<int8_t*>(addr);
  *ptr = value;
}

uint16_t Simulator::readHU(uint64_t addr, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 2)) {
    return 0xffff;
  }

  uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
  return *ptr;
}

int16_t Simulator::readH(uint64_t addr, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 2)) {
    return -1;
  }

  int16_t* ptr = reinterpret_cast<int16_t*>(addr);
  return *ptr;
}

void Simulator::writeH(uint64_t addr, uint16_t value, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 2)) {
    return;
  }

  uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
  LLBit_ = false;
  *ptr = value;
  return;
}

void Simulator::writeH(uint64_t addr, int16_t value, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 2)) {
    return;
  }

  int16_t* ptr = reinterpret_cast<int16_t*>(addr);
  LLBit_ = false;
  *ptr = value;
  return;
}

uint32_t Simulator::readWU(uint64_t addr, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 4)) {
    return -1;
  }

  uint32_t* ptr = reinterpret_cast<uint32_t*>(addr);
  return *ptr;
}

int32_t Simulator::readW(uint64_t addr, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 4)) {
    return -1;
  }

  int32_t* ptr = reinterpret_cast<int32_t*>(addr);
  return *ptr;
}

void Simulator::writeW(uint64_t addr, uint32_t value, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 4)) {
    return;
  }

  uint32_t* ptr = reinterpret_cast<uint32_t*>(addr);
  LLBit_ = false;
  *ptr = value;
  return;
}

void Simulator::writeW(uint64_t addr, int32_t value, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 4)) {
    return;
  }

  int32_t* ptr = reinterpret_cast<int32_t*>(addr);
  LLBit_ = false;
  *ptr = value;
  return;
}

int64_t Simulator::readDW(uint64_t addr, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 8)) {
    return -1;
  }

  intptr_t* ptr = reinterpret_cast<intptr_t*>(addr);
  return *ptr;
}

void Simulator::writeDW(uint64_t addr, int64_t value, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 8)) {
    return;
  }

  int64_t* ptr = reinterpret_cast<int64_t*>(addr);
  LLBit_ = false;
  *ptr = value;
  return;
}

double Simulator::readD(uint64_t addr, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 8)) {
    return NAN;
  }

  double* ptr = reinterpret_cast<double*>(addr);
  return *ptr;
}

void Simulator::writeD(uint64_t addr, double value, SimInstruction* instr) {
  if (handleWasmSegFault(addr, 8)) {
    return;
  }

  double* ptr = reinterpret_cast<double*>(addr);
  LLBit_ = false;
  *ptr = value;
  return;
}

int Simulator::loadLinkedW(uint64_t addr, SimInstruction* instr) {
  if ((addr & 3) == 0) {
    if (handleWasmSegFault(addr, 4)) {
      return -1;
    }

    volatile int32_t* ptr = reinterpret_cast<volatile int32_t*>(addr);
    int32_t value = *ptr;
    lastLLValue_ = value;
    LLAddr_ = addr;
    // Note that any memory write or "external" interrupt should reset this
    // value to false.
    LLBit_ = true;
    return value;
  }
  printf("Unaligned write at 0x%016" PRIx64 ", pc=0x%016" PRIxPTR "\n", addr,
         reinterpret_cast<intptr_t>(instr));
  MOZ_CRASH();
  return 0;
}

int Simulator::storeConditionalW(uint64_t addr, int value,
                                 SimInstruction* instr) {
  // Correct behavior in this case, as defined by architecture, is to just
  // return 0, but there is no point at allowing that. It is certainly an
  // indicator of a bug.
  if (addr != LLAddr_) {
    printf("SC to bad address: 0x%016" PRIx64 ", pc=0x%016" PRIx64
           ", expected: 0x%016" PRIx64 "\n",
           addr, reinterpret_cast<intptr_t>(instr), LLAddr_);
    MOZ_CRASH();
  }

  if ((addr & 3) == 0) {
    SharedMem<int32_t*> ptr =
        SharedMem<int32_t*>::shared(reinterpret_cast<int32_t*>(addr));

    if (!LLBit_) {
      return 0;
    }

    LLBit_ = false;
    LLAddr_ = 0;
    int32_t expected = int32_t(lastLLValue_);
    int32_t old =
        AtomicOperations::compareExchangeSeqCst(ptr, expected, int32_t(value));
    return (old == expected) ? 1 : 0;
  }
  printf("Unaligned SC at 0x%016" PRIx64 ", pc=0x%016" PRIxPTR "\n", addr,
         reinterpret_cast<intptr_t>(instr));
  MOZ_CRASH();
  return 0;
}

int64_t Simulator::loadLinkedD(uint64_t addr, SimInstruction* instr) {
  if ((addr & kPointerAlignmentMask) == 0) {
    if (handleWasmSegFault(addr, 8)) {
      return -1;
    }

    volatile int64_t* ptr = reinterpret_cast<volatile int64_t*>(addr);
    int64_t value = *ptr;
    lastLLValue_ = value;
    LLAddr_ = addr;
    // Note that any memory write or "external" interrupt should reset this
    // value to false.
    LLBit_ = true;
    return value;
  }
  printf("Unaligned write at 0x%016" PRIx64 ", pc=0x%016" PRIxPTR "\n", addr,
         reinterpret_cast<intptr_t>(instr));
  MOZ_CRASH();
  return 0;
}

int Simulator::storeConditionalD(uint64_t addr, int64_t value,
                                 SimInstruction* instr) {
  // Correct behavior in this case, as defined by architecture, is to just
  // return 0, but there is no point at allowing that. It is certainly an
  // indicator of a bug.
  if (addr != LLAddr_) {
    printf("SC to bad address: 0x%016" PRIx64 ", pc=0x%016" PRIx64
           ", expected: 0x%016" PRIx64 "\n",
           addr, reinterpret_cast<intptr_t>(instr), LLAddr_);
    MOZ_CRASH();
  }

  if ((addr & kPointerAlignmentMask) == 0) {
    SharedMem<int64_t*> ptr =
        SharedMem<int64_t*>::shared(reinterpret_cast<int64_t*>(addr));

    if (!LLBit_) {
      return 0;
    }

    LLBit_ = false;
    LLAddr_ = 0;
    int64_t expected = lastLLValue_;
    int64_t old =
        AtomicOperations::compareExchangeSeqCst(ptr, expected, int64_t(value));
    return (old == expected) ? 1 : 0;
  }
  printf("Unaligned SC at 0x%016" PRIx64 ", pc=0x%016" PRIxPTR "\n", addr,
         reinterpret_cast<intptr_t>(instr));
  MOZ_CRASH();
  return 0;
}

uintptr_t Simulator::stackLimit() const { return stackLimit_; }

uintptr_t* Simulator::addressOfStackLimit() { return &stackLimit_; }

bool Simulator::overRecursed(uintptr_t newsp) const {
  if (newsp == 0) {
    newsp = getRegister(sp);
  }
  return newsp <= stackLimit();
}

bool Simulator::overRecursedWithExtra(uint32_t extra) const {
  uintptr_t newsp = getRegister(sp) - extra;
  return newsp <= stackLimit();
}

// Unsupported instructions use format to print an error and stop execution.
void Simulator::format(SimInstruction* instr, const char* format) {
  printf("Simulator found unsupported instruction:\n 0x%016lx: %s\n",
         reinterpret_cast<intptr_t>(instr), format);
  MOZ_CRASH();
}

inline int32_t Simulator::rj_reg(SimInstruction* instr) const {
  return instr->rjValue();
}

inline int64_t Simulator::rj(SimInstruction* instr) const {
  return getRegister(rj_reg(instr));
}

inline uint64_t Simulator::rj_u(SimInstruction* instr) const {
  return static_cast<uint64_t>(getRegister(rj_reg(instr)));
}

inline int32_t Simulator::rk_reg(SimInstruction* instr) const {
  return instr->rkValue();
}

inline int64_t Simulator::rk(SimInstruction* instr) const {
  return getRegister(rk_reg(instr));
}

inline uint64_t Simulator::rk_u(SimInstruction* instr) const {
  return static_cast<uint64_t>(getRegister(rk_reg(instr)));
}

inline int32_t Simulator::rd_reg(SimInstruction* instr) const {
  return instr->rdValue();
}

inline int64_t Simulator::rd(SimInstruction* instr) const {
  return getRegister(rd_reg(instr));
}

inline uint64_t Simulator::rd_u(SimInstruction* instr) const {
  return static_cast<uint64_t>(getRegister(rd_reg(instr)));
}

inline int32_t Simulator::fa_reg(SimInstruction* instr) const {
  return instr->faValue();
}

inline float Simulator::fa_float(SimInstruction* instr) const {
  return getFpuRegisterFloat(fa_reg(instr));
}

inline double Simulator::fa_double(SimInstruction* instr) const {
  return getFpuRegisterDouble(fa_reg(instr));
}

inline int32_t Simulator::fj_reg(SimInstruction* instr) const {
  return instr->fjValue();
}

inline float Simulator::fj_float(SimInstruction* instr) const {
  return getFpuRegisterFloat(fj_reg(instr));
}

inline double Simulator::fj_double(SimInstruction* instr) const {
  return getFpuRegisterDouble(fj_reg(instr));
}

inline int32_t Simulator::fk_reg(SimInstruction* instr) const {
  return instr->fkValue();
}

inline float Simulator::fk_float(SimInstruction* instr) const {
  return getFpuRegisterFloat(fk_reg(instr));
}

inline double Simulator::fk_double(SimInstruction* instr) const {
  return getFpuRegisterDouble(fk_reg(instr));
}

inline int32_t Simulator::fd_reg(SimInstruction* instr) const {
  return instr->fdValue();
}

inline float Simulator::fd_float(SimInstruction* instr) const {
  return getFpuRegisterFloat(fd_reg(instr));
}

inline double Simulator::fd_double(SimInstruction* instr) const {
  return getFpuRegisterDouble(fd_reg(instr));
}

inline int32_t Simulator::cj_reg(SimInstruction* instr) const {
  return instr->cjValue();
}

inline bool Simulator::cj(SimInstruction* instr) const {
  return getCFRegister(cj_reg(instr));
}

inline int32_t Simulator::cd_reg(SimInstruction* instr) const {
  return instr->cdValue();
}

inline bool Simulator::cd(SimInstruction* instr) const {
  return getCFRegister(cd_reg(instr));
}

inline int32_t Simulator::ca_reg(SimInstruction* instr) const {
  return instr->caValue();
}

inline bool Simulator::ca(SimInstruction* instr) const {
  return getCFRegister(ca_reg(instr));
}

inline uint32_t Simulator::sa2(SimInstruction* instr) const {
  return instr->sa2Value();
}

inline uint32_t Simulator::sa3(SimInstruction* instr) const {
  return instr->sa3Value();
}

inline uint32_t Simulator::ui5(SimInstruction* instr) const {
  return instr->imm5Value();
}

inline uint32_t Simulator::ui6(SimInstruction* instr) const {
  return instr->imm6Value();
}

inline uint32_t Simulator::lsbw(SimInstruction* instr) const {
  return instr->lsbwValue();
}

inline uint32_t Simulator::msbw(SimInstruction* instr) const {
  return instr->msbwValue();
}

inline uint32_t Simulator::lsbd(SimInstruction* instr) const {
  return instr->lsbdValue();
}

inline uint32_t Simulator::msbd(SimInstruction* instr) const {
  return instr->msbdValue();
}

inline uint32_t Simulator::cond(SimInstruction* instr) const {
  return instr->condValue();
}

inline int32_t Simulator::si12(SimInstruction* instr) const {
  return (instr->imm12Value() << 20) >> 20;
}

inline uint32_t Simulator::ui12(SimInstruction* instr) const {
  return instr->imm12Value();
}

inline int32_t Simulator::si14(SimInstruction* instr) const {
  return (instr->imm14Value() << 18) >> 18;
}

inline int32_t Simulator::si16(SimInstruction* instr) const {
  return (instr->imm16Value() << 16) >> 16;
}

inline int32_t Simulator::si20(SimInstruction* instr) const {
  return (instr->imm20Value() << 12) >> 12;
}

ABI_FUNCTION_TYPE_SIM_PROTOTYPES

// Software interrupt instructions are used by the simulator to call into C++.
void Simulator::softwareInterrupt(SimInstruction* instr) {
  // the break_ instruction could get us here.
  mozilla::DebugOnly<int32_t> opcode_hi15 = instr->bits(31, 17);
  MOZ_ASSERT(opcode_hi15 == 0x15);
  uint32_t code = instr->bits(14, 0);

  if (instr->instructionBits() == kCallRedirInstr) {
    Redirection* redirection = Redirection::FromSwiInstruction(instr);
    uintptr_t nativeFn =
        reinterpret_cast<uintptr_t>(redirection->nativeFunction());

    // Get the SP for reading stack arguments
    int64_t* sp_ = reinterpret_cast<int64_t*>(getRegister(sp));

    // Store argument register values in local variables for ease of use below.
    int64_t a0_ = getRegister(a0);
    int64_t a1_ = getRegister(a1);
    int64_t a2_ = getRegister(a2);
    int64_t a3_ = getRegister(a3);
    int64_t a4_ = getRegister(a4);
    int64_t a5_ = getRegister(a5);
    int64_t a6_ = getRegister(a6);
    int64_t a7_ = getRegister(a7);
    float f0_s = getFpuRegisterFloat(f0);
    float f1_s = getFpuRegisterFloat(f1);
    float f2_s = getFpuRegisterFloat(f2);
    float f3_s = getFpuRegisterFloat(f3);
    float f4_s = getFpuRegisterFloat(f4);
    double f0_d = getFpuRegisterDouble(f0);
    double f1_d = getFpuRegisterDouble(f1);
    double f2_d = getFpuRegisterDouble(f2);
    double f3_d = getFpuRegisterDouble(f3);

    // This is dodgy but it works because the C entry stubs are never moved.
    // See comment in codegen-arm.cc and bug 1242173.
    int64_t saved_ra = getRegister(ra);

    bool stack_aligned = (getRegister(sp) & (ABIStackAlignment - 1)) == 0;
    if (!stack_aligned) {
      fprintf(stderr, "Runtime call with unaligned stack!\n");
      MOZ_CRASH();
    }

    if (single_stepping_) {
      single_step_callback_(single_step_callback_arg_, this, nullptr);
    }

    switch (redirection->type()) {
      ABI_FUNCTION_TYPE_LOONGARCH64_SIM_DISPATCH

      default:
        MOZ_CRASH("Unknown function type.");
    }

    if (single_stepping_) {
      single_step_callback_(single_step_callback_arg_, this, nullptr);
    }

    setRegister(ra, saved_ra);
    set_pc(getRegister(ra));
  } else if ((instr->bits(31, 15) << 15 == op_break) && code == kWasmTrapCode) {
    uint8_t* newPC;
    if (wasm::HandleIllegalInstruction(registerState(), &newPC)) {
      set_pc(int64_t(newPC));
      return;
    }
  } else if ((instr->bits(31, 15) << 15 == op_break) && code <= kMaxStopCode &&
             code != 6) {
    if (isWatchpoint(code)) {
      // printWatchpoint(code);
    } else {
      increaseStopCounter(code);
      handleStop(code, instr);
    }
  } else {
    // All remaining break_ codes, and all traps are handled here.
    loong64Debugger dbg(this);
    dbg.debug();
  }
}

// Stop helper functions.
bool Simulator::isWatchpoint(uint32_t code) {
  return (code <= kMaxWatchpointCode);
}

void Simulator::printWatchpoint(uint32_t code) {
  loong64Debugger dbg(this);
  ++break_count_;
  printf("\n---- break %d marker: %20" PRIi64 "  (instr count: %20" PRIi64
         ") ----\n",
         code, break_count_, icount_);
  dbg.printAllRegs();  // Print registers and continue running.
}

void Simulator::handleStop(uint32_t code, SimInstruction* instr) {
  // Stop if it is enabled, otherwise go on jumping over the stop
  // and the message address.
  if (isEnabledStop(code)) {
    loong64Debugger dbg(this);
    dbg.stop(instr);
  } else {
    set_pc(get_pc() + 1 * SimInstruction::kInstrSize);
  }
}

bool Simulator::isStopInstruction(SimInstruction* instr) {
  int32_t opcode_hi15 = instr->bits(31, 17);
  uint32_t code = static_cast<uint32_t>(instr->bits(14, 0));
  return (opcode_hi15 == 0x15) && code > kMaxWatchpointCode &&
         code <= kMaxStopCode;
}

bool Simulator::isEnabledStop(uint32_t code) {
  MOZ_ASSERT(code <= kMaxStopCode);
  MOZ_ASSERT(code > kMaxWatchpointCode);
  return !(watchedStops_[code].count_ & kStopDisabledBit);
}

void Simulator::enableStop(uint32_t code) {
  if (!isEnabledStop(code)) {
    watchedStops_[code].count_ &= ~kStopDisabledBit;
  }
}

void Simulator::disableStop(uint32_t code) {
  if (isEnabledStop(code)) {
    watchedStops_[code].count_ |= kStopDisabledBit;
  }
}

void Simulator::increaseStopCounter(uint32_t code) {
  MOZ_ASSERT(code <= kMaxStopCode);
  if ((watchedStops_[code].count_ & ~(1 << 31)) == 0x7fffffff) {
    printf(
        "Stop counter for code %i has overflowed.\n"
        "Enabling this code and reseting the counter to 0.\n",
        code);
    watchedStops_[code].count_ = 0;
    enableStop(code);
  } else {
    watchedStops_[code].count_++;
  }
}

// Print a stop status.
void Simulator::printStopInfo(uint32_t code) {
  if (code <= kMaxWatchpointCode) {
    printf("That is a watchpoint, not a stop.\n");
    return;
  } else if (code > kMaxStopCode) {
    printf("Code too large, only %u stops can be used\n", kMaxStopCode + 1);
    return;
  }
  const char* state = isEnabledStop(code) ? "Enabled" : "Disabled";
  int32_t count = watchedStops_[code].count_ & ~kStopDisabledBit;
  // Don't print the state of unused breakpoints.
  if (count != 0) {
    if (watchedStops_[code].desc_) {
      printf("stop %i - 0x%x: \t%s, \tcounter = %i, \t%s\n", code, code, state,
             count, watchedStops_[code].desc_);
    } else {
      printf("stop %i - 0x%x: \t%s, \tcounter = %i\n", code, code, state,
             count);
    }
  }
}

void Simulator::signalExceptions() {
  for (int i = 1; i < kNumExceptions; i++) {
    if (exceptions[i] != 0) {
      MOZ_CRASH("Error: Exception raised.");
    }
  }
}

// ReverseBits(value) returns |value| in reverse bit order.
template <typename T>
T ReverseBits(T value) {
  MOZ_ASSERT((sizeof(value) == 1) || (sizeof(value) == 2) ||
             (sizeof(value) == 4) || (sizeof(value) == 8));
  T result = 0;
  for (unsigned i = 0; i < (sizeof(value) * 8); i++) {
    result = (result << 1) | (value & 1);
    value >>= 1;
  }
  return result;
}

// Min/Max template functions for Double and Single arguments.

template <typename T>
static T FPAbs(T a);

template <>
double FPAbs<double>(double a) {
  return fabs(a);
}

template <>
float FPAbs<float>(float a) {
  return fabsf(a);
}

enum class MaxMinKind : int { kMin = 0, kMax = 1 };

template <typename T>
static bool FPUProcessNaNsAndZeros(T a, T b, MaxMinKind kind, T* result) {
  if (std::isnan(a) && std::isnan(b)) {
    *result = a;
  } else if (std::isnan(a)) {
    *result = b;
  } else if (std::isnan(b)) {
    *result = a;
  } else if (b == a) {
    // Handle -0.0 == 0.0 case.
    // std::signbit() returns int 0 or 1 so subtracting MaxMinKind::kMax
    // negates the result.
    *result = std::signbit(b) - static_cast<int>(kind) ? b : a;
  } else {
    return false;
  }
  return true;
}

template <typename T>
static T FPUMin(T a, T b) {
  T result;
  if (FPUProcessNaNsAndZeros(a, b, MaxMinKind::kMin, &result)) {
    return result;
  } else {
    return b < a ? b : a;
  }
}

template <typename T>
static T FPUMax(T a, T b) {
  T result;
  if (FPUProcessNaNsAndZeros(a, b, MaxMinKind::kMax, &result)) {
    return result;
  } else {
    return b > a ? b : a;
  }
}

template <typename T>
static T FPUMinA(T a, T b) {
  T result;
  if (!FPUProcessNaNsAndZeros(a, b, MaxMinKind::kMin, &result)) {
    if (FPAbs(a) < FPAbs(b)) {
      result = a;
    } else if (FPAbs(b) < FPAbs(a)) {
      result = b;
    } else {
      result = a < b ? a : b;
    }
  }
  return result;
}

template <typename T>
static T FPUMaxA(T a, T b) {
  T result;
  if (!FPUProcessNaNsAndZeros(a, b, MaxMinKind::kMin, &result)) {
    if (FPAbs(a) > FPAbs(b)) {
      result = a;
    } else if (FPAbs(b) > FPAbs(a)) {
      result = b;
    } else {
      result = a > b ? a : b;
    }
  }
  return result;
}

enum class KeepSign : bool { no = false, yes };

// Handle execution based on instruction types.
// decodeTypeImmediate
void Simulator::decodeTypeOp6(SimInstruction* instr) {
  // Next pc.
  int64_t next_pc = bad_ra;

  // Used for memory instructions.
  int64_t alu_out = 0;

  // Branch instructions common part.
  auto BranchAndLinkHelper = [this, &next_pc](SimInstruction* instr) {
    int64_t current_pc = get_pc();
    setRegister(ra, current_pc + SimInstruction::kInstrSize);
    int32_t offs26_low16 =
        static_cast<uint32_t>(instr->bits(25, 10) << 16) >> 16;
    int32_t offs26_high10 = static_cast<int32_t>(instr->bits(9, 0) << 22) >> 6;
    int32_t offs26 = offs26_low16 | offs26_high10;
    next_pc = current_pc + (offs26 << 2);
    set_pc(next_pc);
  };

  auto BranchOff16Helper = [this, &next_pc](SimInstruction* instr,
                                            bool do_branch) {
    int64_t current_pc = get_pc();
    int32_t offs16 = static_cast<int32_t>(instr->bits(25, 10) << 16) >> 16;
    int32_t offs = do_branch ? (offs16 << 2) : SimInstruction::kInstrSize;
    next_pc = current_pc + offs;
    set_pc(next_pc);
  };

  auto BranchOff21Helper = [this, &next_pc](SimInstruction* instr,
                                            bool do_branch) {
    int64_t current_pc = get_pc();
    int32_t offs21_low16 =
        static_cast<uint32_t>(instr->bits(25, 10) << 16) >> 16;
    int32_t offs21_high5 = static_cast<int32_t>(instr->bits(4, 0) << 27) >> 11;
    int32_t offs = offs21_low16 | offs21_high5;
    offs = do_branch ? (offs << 2) : SimInstruction::kInstrSize;
    next_pc = current_pc + offs;
    set_pc(next_pc);
  };

  auto BranchOff26Helper = [this, &next_pc](SimInstruction* instr) {
    int64_t current_pc = get_pc();
    int32_t offs26_low16 =
        static_cast<uint32_t>(instr->bits(25, 10) << 16) >> 16;
    int32_t offs26_high10 = static_cast<int32_t>(instr->bits(9, 0) << 22) >> 6;
    int32_t offs26 = offs26_low16 | offs26_high10;
    next_pc = current_pc + (offs26 << 2);
    set_pc(next_pc);
  };

  auto JumpOff16Helper = [this, &next_pc](SimInstruction* instr) {
    int32_t offs16 = static_cast<int32_t>(instr->bits(25, 10) << 16) >> 16;
    setRegister(rd_reg(instr), get_pc() + SimInstruction::kInstrSize);
    next_pc = rj(instr) + (offs16 << 2);
    set_pc(next_pc);
  };

  switch (instr->bits(31, 26) << 26) {
    case op_addu16i_d: {
      int32_t si16_upper = static_cast<int32_t>(si16(instr)) << 16;
      alu_out = static_cast<int64_t>(si16_upper) + rj(instr);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_beqz: {
      BranchOff21Helper(instr, rj(instr) == 0);
      break;
    }
    case op_bnez: {
      BranchOff21Helper(instr, rj(instr) != 0);
      break;
    }
    case op_bcz: {
      if (instr->bits(9, 8) == 0b00) {
        // BCEQZ
        BranchOff21Helper(instr, cj(instr) == false);
      } else if (instr->bits(9, 8) == 0b01) {
        // BCNEZ
        BranchOff21Helper(instr, cj(instr) == true);
      } else {
        UNREACHABLE();
      }
      break;
    }
    case op_jirl: {
      JumpOff16Helper(instr);
      break;
    }
    case op_b: {
      BranchOff26Helper(instr);
      break;
    }
    case op_bl: {
      BranchAndLinkHelper(instr);
      break;
    }
    case op_beq: {
      BranchOff16Helper(instr, rj(instr) == rd(instr));
      break;
    }
    case op_bne: {
      BranchOff16Helper(instr, rj(instr) != rd(instr));
      break;
    }
    case op_blt: {
      BranchOff16Helper(instr, rj(instr) < rd(instr));
      break;
    }
    case op_bge: {
      BranchOff16Helper(instr, rj(instr) >= rd(instr));
      break;
    }
    case op_bltu: {
      BranchOff16Helper(instr, rj_u(instr) < rd_u(instr));
      break;
    }
    case op_bgeu: {
      BranchOff16Helper(instr, rj_u(instr) >= rd_u(instr));
      break;
    }
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp7(SimInstruction* instr) {
  int64_t alu_out;

  switch (instr->bits(31, 25) << 25) {
    case op_lu12i_w: {
      int32_t si20_upper = static_cast<int32_t>(si20(instr) << 12);
      setRegister(rd_reg(instr), static_cast<int64_t>(si20_upper));
      break;
    }
    case op_lu32i_d: {
      int32_t si20_signExtend = static_cast<int32_t>(si20(instr) << 12) >> 12;
      int64_t lower_32bit_mask = 0xFFFFFFFF;
      alu_out = (static_cast<int64_t>(si20_signExtend) << 32) |
                (rd(instr) & lower_32bit_mask);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_pcaddi: {
      int32_t si20_signExtend = static_cast<int32_t>(si20(instr) << 12) >> 10;
      int64_t current_pc = get_pc();
      alu_out = static_cast<int64_t>(si20_signExtend) + current_pc;
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_pcalau12i: {
      int32_t si20_signExtend = static_cast<int32_t>(si20(instr) << 12);
      int64_t current_pc = get_pc();
      int64_t clear_lower12bit_mask = 0xFFFFFFFFFFFFF000;
      alu_out = static_cast<int64_t>(si20_signExtend) + current_pc;
      setRegister(rd_reg(instr), alu_out & clear_lower12bit_mask);
      break;
    }
    case op_pcaddu12i: {
      int32_t si20_signExtend = static_cast<int32_t>(si20(instr) << 12);
      int64_t current_pc = get_pc();
      alu_out = static_cast<int64_t>(si20_signExtend) + current_pc;
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_pcaddu18i: {
      int64_t si20_signExtend = (static_cast<int64_t>(si20(instr)) << 44) >> 26;
      int64_t current_pc = get_pc();
      alu_out = si20_signExtend + current_pc;
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp8(SimInstruction* instr) {
  int64_t addr = 0x0;
  int64_t si14_se = (static_cast<int64_t>(si14(instr)) << 50) >> 48;

  switch (instr->bits(31, 24) << 24) {
    case op_ldptr_w: {
      setRegister(rd_reg(instr), readW(rj(instr) + si14_se, instr));
      break;
    }
    case op_stptr_w: {
      writeW(rj(instr) + si14_se, static_cast<int32_t>(rd(instr)), instr);
      break;
    }
    case op_ldptr_d: {
      setRegister(rd_reg(instr), readDW(rj(instr) + si14_se, instr));
      break;
    }
    case op_stptr_d: {
      writeDW(rj(instr) + si14_se, rd(instr), instr);
      break;
    }
    case op_ll_w: {
      addr = si14_se + rj(instr);
      setRegister(rd_reg(instr), loadLinkedW(addr, instr));
      break;
    }
    case op_sc_w: {
      addr = si14_se + rj(instr);
      setRegister(
          rd_reg(instr),
          storeConditionalW(addr, static_cast<int32_t>(rd(instr)), instr));
      break;
    }
    case op_ll_d: {
      addr = si14_se + rj(instr);
      setRegister(rd_reg(instr), loadLinkedD(addr, instr));
      break;
    }
    case op_sc_d: {
      addr = si14_se + rj(instr);
      setRegister(rd_reg(instr), storeConditionalD(addr, rd(instr), instr));
      break;
    }
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp10(SimInstruction* instr) {
  int64_t alu_out = 0x0;
  int64_t si12_se = (static_cast<int64_t>(si12(instr)) << 52) >> 52;
  uint64_t si12_ze = (static_cast<uint64_t>(ui12(instr)) << 52) >> 52;

  switch (instr->bits(31, 22) << 22) {
    case op_bstrins_d: {
      uint8_t lsbd_ = lsbd(instr);
      uint8_t msbd_ = msbd(instr);
      MOZ_ASSERT(lsbd_ <= msbd_);
      uint8_t size = msbd_ - lsbd_ + 1;
      if (size < 64) {
        uint64_t mask = (1ULL << size) - 1;
        alu_out =
            (rd_u(instr) & ~(mask << lsbd_)) | ((rj_u(instr) & mask) << lsbd_);
        setRegister(rd_reg(instr), alu_out);
      } else if (size == 64) {
        setRegister(rd_reg(instr), rj(instr));
      }
      break;
    }
    case op_bstrpick_d: {
      uint8_t lsbd_ = lsbd(instr);
      uint8_t msbd_ = msbd(instr);
      MOZ_ASSERT(lsbd_ <= msbd_);
      uint8_t size = msbd_ - lsbd_ + 1;
      if (size < 64) {
        uint64_t mask = (1ULL << size) - 1;
        alu_out = (rj_u(instr) & (mask << lsbd_)) >> lsbd_;
        setRegister(rd_reg(instr), alu_out);
      } else if (size == 64) {
        setRegister(rd_reg(instr), rj(instr));
      }
      break;
    }
    case op_slti: {
      setRegister(rd_reg(instr), rj(instr) < si12_se ? 1 : 0);
      break;
    }
    case op_sltui: {
      setRegister(rd_reg(instr),
                  rj_u(instr) < static_cast<uint64_t>(si12_se) ? 1 : 0);
      break;
    }
    case op_addi_w: {
      int32_t alu32_out =
          static_cast<int32_t>(rj(instr)) + static_cast<int32_t>(si12_se);
      setRegister(rd_reg(instr), alu32_out);
      break;
    }
    case op_addi_d: {
      setRegister(rd_reg(instr), rj(instr) + si12_se);
      break;
    }
    case op_lu52i_d: {
      int64_t si12_se = static_cast<int64_t>(si12(instr)) << 52;
      uint64_t mask = (1ULL << 52) - 1;
      alu_out = si12_se + (rj(instr) & mask);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_andi: {
      setRegister(rd_reg(instr), rj(instr) & si12_ze);
      break;
    }
    case op_ori: {
      setRegister(rd_reg(instr), rj_u(instr) | si12_ze);
      break;
    }
    case op_xori: {
      setRegister(rd_reg(instr), rj_u(instr) ^ si12_ze);
      break;
    }
    case op_ld_b: {
      setRegister(rd_reg(instr), readB(rj(instr) + si12_se));
      break;
    }
    case op_ld_h: {
      setRegister(rd_reg(instr), readH(rj(instr) + si12_se, instr));
      break;
    }
    case op_ld_w: {
      setRegister(rd_reg(instr), readW(rj(instr) + si12_se, instr));
      break;
    }
    case op_ld_d: {
      setRegister(rd_reg(instr), readDW(rj(instr) + si12_se, instr));
      break;
    }
    case op_st_b: {
      writeB(rj(instr) + si12_se, static_cast<int8_t>(rd(instr)));
      break;
    }
    case op_st_h: {
      writeH(rj(instr) + si12_se, static_cast<int16_t>(rd(instr)), instr);
      break;
    }
    case op_st_w: {
      writeW(rj(instr) + si12_se, static_cast<int32_t>(rd(instr)), instr);
      break;
    }
    case op_st_d: {
      writeDW(rj(instr) + si12_se, rd(instr), instr);
      break;
    }
    case op_ld_bu: {
      setRegister(rd_reg(instr), readBU(rj(instr) + si12_se));
      break;
    }
    case op_ld_hu: {
      setRegister(rd_reg(instr), readHU(rj(instr) + si12_se, instr));
      break;
    }
    case op_ld_wu: {
      setRegister(rd_reg(instr), readWU(rj(instr) + si12_se, instr));
      break;
    }
    case op_fld_s: {
      setFpuRegister(fd_reg(instr), kFPUInvalidResult);  // Trash upper 32 bits.
      setFpuRegisterWord(fd_reg(instr), readW(rj(instr) + si12_se, instr));
      break;
    }
    case op_fst_s: {
      int32_t alu_out_32 = static_cast<int32_t>(getFpuRegister(fd_reg(instr)));
      writeW(rj(instr) + si12_se, alu_out_32, instr);
      break;
    }
    case op_fld_d: {
      setFpuRegisterDouble(fd_reg(instr), readD(rj(instr) + si12_se, instr));
      break;
    }
    case op_fst_d: {
      writeD(rj(instr) + si12_se, getFpuRegisterDouble(fd_reg(instr)), instr);
      break;
    }
    case op_preld:
      UNIMPLEMENTED();
      break;
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp11(SimInstruction* instr) {
  int64_t alu_out = 0x0;

  switch (instr->bits(31, 21) << 21) {
    case op_bstr_w: {
      MOZ_ASSERT(instr->bit(21) == 1);
      uint8_t lsbw_ = lsbw(instr);
      uint8_t msbw_ = msbw(instr);
      MOZ_ASSERT(lsbw_ <= msbw_);
      uint8_t size = msbw_ - lsbw_ + 1;
      uint64_t mask = (1ULL << size) - 1;
      if (instr->bit(15) == 0) {
        // BSTRINS_W
        alu_out = static_cast<int32_t>((rd_u(instr) & ~(mask << lsbw_)) |
                                       ((rj_u(instr) & mask) << lsbw_));
      } else {
        // BSTRPICK_W
        alu_out =
            static_cast<int32_t>((rj_u(instr) & (mask << lsbw_)) >> lsbw_);
      }
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp12(SimInstruction* instr) {
  switch (instr->bits(31, 20) << 20) {
    case op_fmadd_s: {
      setFpuRegisterFloat(
          fd_reg(instr),
          std::fma(fj_float(instr), fk_float(instr), fa_float(instr)));
      break;
    }
    case op_fmadd_d: {
      setFpuRegisterDouble(
          fd_reg(instr),
          std::fma(fj_double(instr), fk_double(instr), fa_double(instr)));
      break;
    }
    case op_fmsub_s: {
      setFpuRegisterFloat(
          fd_reg(instr),
          std::fma(-fj_float(instr), fk_float(instr), fa_float(instr)));
      break;
    }
    case op_fmsub_d: {
      setFpuRegisterDouble(
          fd_reg(instr),
          std::fma(-fj_double(instr), fk_double(instr), fa_double(instr)));
      break;
    }
    case op_fnmadd_s: {
      setFpuRegisterFloat(
          fd_reg(instr),
          std::fma(-fj_float(instr), fk_float(instr), -fa_float(instr)));
      break;
    }
    case op_fnmadd_d: {
      setFpuRegisterDouble(
          fd_reg(instr),
          std::fma(-fj_double(instr), fk_double(instr), -fa_double(instr)));
      break;
    }
    case op_fnmsub_s: {
      setFpuRegisterFloat(
          fd_reg(instr),
          std::fma(fj_float(instr), fk_float(instr), -fa_float(instr)));
      break;
    }
    case op_fnmsub_d: {
      setFpuRegisterDouble(
          fd_reg(instr),
          std::fma(fj_double(instr), fk_double(instr), -fa_double(instr)));
      break;
    }
    case op_fcmp_cond_s: {
      MOZ_ASSERT(instr->bits(4, 3) == 0);
      float fj = fj_float(instr);
      float fk = fk_float(instr);
      switch (cond(instr)) {
        case AssemblerLOONG64::CAF: {
          setCFRegister(cd_reg(instr), false);
          break;
        }
        case AssemblerLOONG64::CUN: {
          setCFRegister(cd_reg(instr), std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CEQ: {
          setCFRegister(cd_reg(instr), fj == fk);
          break;
        }
        case AssemblerLOONG64::CUEQ: {
          setCFRegister(cd_reg(instr),
                        (fj == fk) || std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CLT: {
          setCFRegister(cd_reg(instr), fj < fk);
          break;
        }
        case AssemblerLOONG64::CULT: {
          setCFRegister(cd_reg(instr),
                        (fj < fk) || std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CLE: {
          setCFRegister(cd_reg(instr), fj <= fk);
          break;
        }
        case AssemblerLOONG64::CULE: {
          setCFRegister(cd_reg(instr),
                        (fj <= fk) || std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CNE: {
          setCFRegister(cd_reg(instr), (fj < fk) || (fj > fk));
          break;
        }
        case AssemblerLOONG64::COR: {
          setCFRegister(cd_reg(instr), !std::isnan(fj) && !std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CUNE: {
          setCFRegister(cd_reg(instr), (fj < fk) || (fj > fk) ||
                                           std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::SAF:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SUN:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SEQ:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SUEQ:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SLT:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SULT:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SLE:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SULE:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SNE:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SOR:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SUNE:
          UNIMPLEMENTED();
          break;
        default:
          UNREACHABLE();
      }
      break;
    }
    case op_fcmp_cond_d: {
      MOZ_ASSERT(instr->bits(4, 3) == 0);
      double fj = fj_double(instr);
      double fk = fk_double(instr);
      switch (cond(instr)) {
        case AssemblerLOONG64::CAF: {
          setCFRegister(cd_reg(instr), false);
          break;
        }
        case AssemblerLOONG64::CUN: {
          setCFRegister(cd_reg(instr), std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CEQ: {
          setCFRegister(cd_reg(instr), fj == fk);
          break;
        }
        case AssemblerLOONG64::CUEQ: {
          setCFRegister(cd_reg(instr),
                        (fj == fk) || std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CLT: {
          setCFRegister(cd_reg(instr), fj < fk);
          break;
        }
        case AssemblerLOONG64::CULT: {
          setCFRegister(cd_reg(instr),
                        (fj < fk) || std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CLE: {
          setCFRegister(cd_reg(instr), fj <= fk);
          break;
        }
        case AssemblerLOONG64::CULE: {
          setCFRegister(cd_reg(instr),
                        (fj <= fk) || std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CNE: {
          setCFRegister(cd_reg(instr), (fj < fk) || (fj > fk));
          break;
        }
        case AssemblerLOONG64::COR: {
          setCFRegister(cd_reg(instr), !std::isnan(fj) && !std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::CUNE: {
          setCFRegister(cd_reg(instr),
                        (fj != fk) || std::isnan(fj) || std::isnan(fk));
          break;
        }
        case AssemblerLOONG64::SAF:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SUN:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SEQ:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SUEQ:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SLT:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SULT:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SLE:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SULE:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SNE:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SOR:
          UNIMPLEMENTED();
          break;
        case AssemblerLOONG64::SUNE:
          UNIMPLEMENTED();
          break;
        default:
          UNREACHABLE();
      }
      break;
    }
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp14(SimInstruction* instr) {
  int64_t alu_out = 0x0;

  switch (instr->bits(31, 18) << 18) {
    case op_bytepick_d: {
      uint8_t sa = sa3(instr) * 8;
      if (sa == 0) {
        alu_out = rk(instr);
      } else {
        int64_t mask = (1ULL << 63) >> (sa - 1);
        int64_t rk_hi = (rk(instr) & (~mask)) << sa;
        int64_t rj_lo = (rj(instr) & mask) >> (64 - sa);
        alu_out = rk_hi | rj_lo;
      }
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_fsel: {
      MOZ_ASSERT(instr->bits(19, 18) == 0);
      if (ca(instr) == 0) {
        setFpuRegisterDouble(fd_reg(instr), fj_double(instr));
      } else {
        setFpuRegisterDouble(fd_reg(instr), fk_double(instr));
      }
      break;
    }
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp15(SimInstruction* instr) {
  int64_t alu_out = 0x0;
  int32_t alu32_out = 0x0;

  switch (instr->bits(31, 17) << 17) {
    case op_bytepick_w: {
      MOZ_ASSERT(instr->bit(17) == 0);
      uint8_t sa = sa2(instr) * 8;
      if (sa == 0) {
        alu32_out = static_cast<int32_t>(rk(instr));
      } else {
        int32_t mask = (1 << 31) >> (sa - 1);
        int32_t rk_hi = (static_cast<int32_t>(rk(instr)) & (~mask)) << sa;
        int32_t rj_lo = (static_cast<uint32_t>(rj(instr)) & mask) >> (32 - sa);
        alu32_out = rk_hi | rj_lo;
      }
      setRegister(rd_reg(instr), static_cast<int64_t>(alu32_out));
      break;
    }
    case op_alsl_w: {
      uint8_t sa = sa2(instr) + 1;
      alu32_out = (static_cast<int32_t>(rj(instr)) << sa) +
                  static_cast<int32_t>(rk(instr));
      setRegister(rd_reg(instr), alu32_out);
      break;
    }
    case op_alsl_wu: {
      uint8_t sa = sa2(instr) + 1;
      alu32_out = (static_cast<int32_t>(rj(instr)) << sa) +
                  static_cast<int32_t>(rk(instr));
      setRegister(rd_reg(instr), static_cast<uint32_t>(alu32_out));
      break;
    }
    case op_alsl_d: {
      MOZ_ASSERT(instr->bit(17) == 0);
      uint8_t sa = sa2(instr) + 1;
      alu_out = (rj(instr) << sa) + rk(instr);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp16(SimInstruction* instr) {
  int64_t alu_out;
  switch (instr->bits(31, 16) << 16) {
    case op_slli_d: {
      MOZ_ASSERT(instr->bit(17) == 0);
      MOZ_ASSERT(instr->bits(17, 16) == 0b01);
      setRegister(rd_reg(instr), rj(instr) << ui6(instr));
      break;
    }
    case op_srli_d: {
      MOZ_ASSERT(instr->bit(17) == 0);
      setRegister(rd_reg(instr), rj_u(instr) >> ui6(instr));
      break;
    }
    case op_srai_d: {
      MOZ_ASSERT(instr->bit(17) == 0);
      setRegister(rd_reg(instr), rj(instr) >> ui6(instr));
      break;
    }
    case op_rotri_d: {
      MOZ_ASSERT(instr->bit(17) == 0);
      MOZ_ASSERT(instr->bits(17, 16) == 0b01);
      alu_out = static_cast<int64_t>(RotateRight64(rj_u(instr), ui6(instr)));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp17(SimInstruction* instr) {
  int64_t alu_out;
  int32_t alu32_out;

  switch (instr->bits(31, 15) << 15) {
    case op_slli_w: {
      MOZ_ASSERT(instr->bit(17) == 0);
      MOZ_ASSERT(instr->bits(17, 15) == 0b001);
      alu32_out = static_cast<int32_t>(rj(instr)) << ui5(instr);
      setRegister(rd_reg(instr), static_cast<int64_t>(alu32_out));
      break;
    }
    case op_srai_w: {
      MOZ_ASSERT(instr->bit(17) == 0);
      MOZ_ASSERT(instr->bits(17, 15) == 0b001);
      alu32_out = static_cast<int32_t>(rj(instr)) >> ui5(instr);
      setRegister(rd_reg(instr), static_cast<int64_t>(alu32_out));
      break;
    }
    case op_rotri_w: {
      MOZ_ASSERT(instr->bit(17) == 0);
      MOZ_ASSERT(instr->bits(17, 15) == 0b001);
      alu32_out = static_cast<int32_t>(
          RotateRight32(static_cast<const uint32_t>(rj_u(instr)),
                        static_cast<const uint32_t>(ui5(instr))));
      setRegister(rd_reg(instr), static_cast<int64_t>(alu32_out));
      break;
    }
    case op_srli_w: {
      MOZ_ASSERT(instr->bit(17) == 0);
      MOZ_ASSERT(instr->bits(17, 15) == 0b001);
      alu32_out = static_cast<uint32_t>(rj(instr)) >> ui5(instr);
      setRegister(rd_reg(instr), static_cast<int64_t>(alu32_out));
      break;
    }
    case op_add_w: {
      int32_t alu32_out = static_cast<int32_t>(rj(instr) + rk(instr));
      // Sign-extend result of 32bit operation into 64bit register.
      setRegister(rd_reg(instr), static_cast<int64_t>(alu32_out));
      break;
    }
    case op_add_d:
      setRegister(rd_reg(instr), rj(instr) + rk(instr));
      break;
    case op_sub_w: {
      int32_t alu32_out = static_cast<int32_t>(rj(instr) - rk(instr));
      // Sign-extend result of 32bit operation into 64bit register.
      setRegister(rd_reg(instr), static_cast<int64_t>(alu32_out));
      break;
    }
    case op_sub_d:
      setRegister(rd_reg(instr), rj(instr) - rk(instr));
      break;
    case op_slt:
      setRegister(rd_reg(instr), rj(instr) < rk(instr) ? 1 : 0);
      break;
    case op_sltu:
      setRegister(rd_reg(instr), rj_u(instr) < rk_u(instr) ? 1 : 0);
      break;
    case op_maskeqz:
      setRegister(rd_reg(instr), rk(instr) == 0 ? 0 : rj(instr));
      break;
    case op_masknez:
      setRegister(rd_reg(instr), rk(instr) != 0 ? 0 : rj(instr));
      break;
    case op_nor:
      setRegister(rd_reg(instr), ~(rj(instr) | rk(instr)));
      break;
    case op_and:
      setRegister(rd_reg(instr), rj(instr) & rk(instr));
      break;
    case op_or:
      setRegister(rd_reg(instr), rj(instr) | rk(instr));
      break;
    case op_xor:
      setRegister(rd_reg(instr), rj(instr) ^ rk(instr));
      break;
    case op_orn:
      setRegister(rd_reg(instr), rj(instr) | (~rk(instr)));
      break;
    case op_andn:
      setRegister(rd_reg(instr), rj(instr) & (~rk(instr)));
      break;
    case op_sll_w:
      setRegister(rd_reg(instr), (int32_t)rj(instr) << (rk_u(instr) % 32));
      break;
    case op_srl_w: {
      alu_out =
          static_cast<int32_t>((uint32_t)rj_u(instr) >> (rk_u(instr) % 32));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_sra_w:
      setRegister(rd_reg(instr), (int32_t)rj(instr) >> (rk_u(instr) % 32));
      break;
    case op_sll_d:
      setRegister(rd_reg(instr), rj(instr) << (rk_u(instr) % 64));
      break;
    case op_srl_d: {
      alu_out = static_cast<int64_t>(rj_u(instr) >> (rk_u(instr) % 64));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_sra_d:
      setRegister(rd_reg(instr), rj(instr) >> (rk_u(instr) % 64));
      break;
    case op_rotr_w: {
      alu_out = static_cast<int32_t>(
          RotateRight32(static_cast<const uint32_t>(rj_u(instr)),
                        static_cast<const uint32_t>(rk_u(instr) % 32)));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_rotr_d: {
      alu_out = static_cast<int64_t>(
          RotateRight64((rj_u(instr)), (rk_u(instr) % 64)));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_mul_w: {
      alu_out =
          static_cast<int32_t>(rj(instr)) * static_cast<int32_t>(rk(instr));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_mulh_w: {
      int32_t rj_lo = static_cast<int32_t>(rj(instr));
      int32_t rk_lo = static_cast<int32_t>(rk(instr));
      alu_out = static_cast<int64_t>(rj_lo) * static_cast<int64_t>(rk_lo);
      setRegister(rd_reg(instr), alu_out >> 32);
      break;
    }
    case op_mulh_wu: {
      uint32_t rj_lo = static_cast<uint32_t>(rj_u(instr));
      uint32_t rk_lo = static_cast<uint32_t>(rk_u(instr));
      alu_out = static_cast<uint64_t>(rj_lo) * static_cast<uint64_t>(rk_lo);
      setRegister(rd_reg(instr), alu_out >> 32);
      break;
    }
    case op_mul_d:
      setRegister(rd_reg(instr), rj(instr) * rk(instr));
      break;
    case op_mulh_d:
      setRegister(rd_reg(instr), MultiplyHighSigned(rj(instr), rk(instr)));
      break;
    case op_mulh_du:
      setRegister(rd_reg(instr),
                  MultiplyHighUnsigned(rj_u(instr), rk_u(instr)));
      break;
    case op_mulw_d_w: {
      int64_t rj_i32 = static_cast<int32_t>(rj(instr));
      int64_t rk_i32 = static_cast<int32_t>(rk(instr));
      setRegister(rd_reg(instr), rj_i32 * rk_i32);
      break;
    }
    case op_mulw_d_wu: {
      uint64_t rj_u32 = static_cast<uint32_t>(rj_u(instr));
      uint64_t rk_u32 = static_cast<uint32_t>(rk_u(instr));
      setRegister(rd_reg(instr), rj_u32 * rk_u32);
      break;
    }
    case op_div_w: {
      int32_t rj_i32 = static_cast<int32_t>(rj(instr));
      int32_t rk_i32 = static_cast<int32_t>(rk(instr));
      if (rj_i32 == INT_MIN && rk_i32 == -1) {
        setRegister(rd_reg(instr), INT_MIN);
      } else if (rk_i32 != 0) {
        setRegister(rd_reg(instr), rj_i32 / rk_i32);
      }
      break;
    }
    case op_mod_w: {
      int32_t rj_i32 = static_cast<int32_t>(rj(instr));
      int32_t rk_i32 = static_cast<int32_t>(rk(instr));
      if (rj_i32 == INT_MIN && rk_i32 == -1) {
        setRegister(rd_reg(instr), 0);
      } else if (rk_i32 != 0) {
        setRegister(rd_reg(instr), rj_i32 % rk_i32);
      }
      break;
    }
    case op_div_wu: {
      uint32_t rj_u32 = static_cast<uint32_t>(rj(instr));
      uint32_t rk_u32 = static_cast<uint32_t>(rk(instr));
      if (rk_u32 != 0) {
        setRegister(rd_reg(instr), static_cast<int32_t>(rj_u32 / rk_u32));
      }
      break;
    }
    case op_mod_wu: {
      uint32_t rj_u32 = static_cast<uint32_t>(rj(instr));
      uint32_t rk_u32 = static_cast<uint32_t>(rk(instr));
      if (rk_u32 != 0) {
        setRegister(rd_reg(instr), static_cast<int32_t>(rj_u32 % rk_u32));
      }
      break;
    }
    case op_div_d: {
      if (rj(instr) == INT64_MIN && rk(instr) == -1) {
        setRegister(rd_reg(instr), INT64_MIN);
      } else if (rk(instr) != 0) {
        setRegister(rd_reg(instr), rj(instr) / rk(instr));
      }
      break;
    }
    case op_mod_d: {
      if (rj(instr) == LONG_MIN && rk(instr) == -1) {
        setRegister(rd_reg(instr), 0);
      } else if (rk(instr) != 0) {
        setRegister(rd_reg(instr), rj(instr) % rk(instr));
      }
      break;
    }
    case op_div_du: {
      if (rk_u(instr) != 0) {
        setRegister(rd_reg(instr),
                    static_cast<int64_t>(rj_u(instr) / rk_u(instr)));
      }
      break;
    }
    case op_mod_du: {
      if (rk_u(instr) != 0) {
        setRegister(rd_reg(instr),
                    static_cast<int64_t>(rj_u(instr) % rk_u(instr)));
      }
      break;
    }
    case op_break:
      softwareInterrupt(instr);
      break;
    case op_fadd_s: {
      setFpuRegisterFloat(fd_reg(instr), fj_float(instr) + fk_float(instr));
      break;
    }
    case op_fadd_d: {
      setFpuRegisterDouble(fd_reg(instr), fj_double(instr) + fk_double(instr));
      break;
    }
    case op_fsub_s: {
      setFpuRegisterFloat(fd_reg(instr), fj_float(instr) - fk_float(instr));
      break;
    }
    case op_fsub_d: {
      setFpuRegisterDouble(fd_reg(instr), fj_double(instr) - fk_double(instr));
      break;
    }
    case op_fmul_s: {
      setFpuRegisterFloat(fd_reg(instr), fj_float(instr) * fk_float(instr));
      break;
    }
    case op_fmul_d: {
      setFpuRegisterDouble(fd_reg(instr), fj_double(instr) * fk_double(instr));
      break;
    }
    case op_fdiv_s: {
      setFpuRegisterFloat(fd_reg(instr), fj_float(instr) / fk_float(instr));
      break;
    }

    case op_fdiv_d: {
      setFpuRegisterDouble(fd_reg(instr), fj_double(instr) / fk_double(instr));
      break;
    }
    case op_fmax_s: {
      setFpuRegisterFloat(fd_reg(instr),
                          FPUMax(fk_float(instr), fj_float(instr)));
      break;
    }
    case op_fmax_d: {
      setFpuRegisterDouble(fd_reg(instr),
                           FPUMax(fk_double(instr), fj_double(instr)));
      break;
    }
    case op_fmin_s: {
      setFpuRegisterFloat(fd_reg(instr),
                          FPUMin(fk_float(instr), fj_float(instr)));
      break;
    }
    case op_fmin_d: {
      setFpuRegisterDouble(fd_reg(instr),
                           FPUMin(fk_double(instr), fj_double(instr)));
      break;
    }
    case op_fmaxa_s: {
      setFpuRegisterFloat(fd_reg(instr),
                          FPUMaxA(fk_float(instr), fj_float(instr)));
      break;
    }
    case op_fmaxa_d: {
      setFpuRegisterDouble(fd_reg(instr),
                           FPUMaxA(fk_double(instr), fj_double(instr)));
      break;
    }
    case op_fmina_s: {
      setFpuRegisterFloat(fd_reg(instr),
                          FPUMinA(fk_float(instr), fj_float(instr)));
      break;
    }
    case op_fmina_d: {
      setFpuRegisterDouble(fd_reg(instr),
                           FPUMinA(fk_double(instr), fj_double(instr)));
      break;
    }
    case op_ldx_b:
      setRegister(rd_reg(instr), readB(rj(instr) + rk(instr)));
      break;
    case op_ldx_h:
      setRegister(rd_reg(instr), readH(rj(instr) + rk(instr), instr));
      break;
    case op_ldx_w:
      setRegister(rd_reg(instr), readW(rj(instr) + rk(instr), instr));
      break;
    case op_ldx_d:
      setRegister(rd_reg(instr), readDW(rj(instr) + rk(instr), instr));
      break;
    case op_stx_b:
      writeB(rj(instr) + rk(instr), static_cast<int8_t>(rd(instr)));
      break;
    case op_stx_h:
      writeH(rj(instr) + rk(instr), static_cast<int16_t>(rd(instr)), instr);
      break;
    case op_stx_w:
      writeW(rj(instr) + rk(instr), static_cast<int32_t>(rd(instr)), instr);
      break;
    case op_stx_d:
      writeDW(rj(instr) + rk(instr), rd(instr), instr);
      break;
    case op_ldx_bu:
      setRegister(rd_reg(instr), readBU(rj(instr) + rk(instr)));
      break;
    case op_ldx_hu:
      setRegister(rd_reg(instr), readHU(rj(instr) + rk(instr), instr));
      break;
    case op_ldx_wu:
      setRegister(rd_reg(instr), readWU(rj(instr) + rk(instr), instr));
      break;
    case op_fldx_s:
      setFpuRegister(fd_reg(instr), kFPUInvalidResult);  // Trash upper 32 bits.
      setFpuRegisterWord(fd_reg(instr), readW(rj(instr) + rk(instr), instr));
      break;
    case op_fldx_d:
      setFpuRegister(fd_reg(instr), kFPUInvalidResult);  // Trash upper 32 bits.
      setFpuRegisterDouble(fd_reg(instr), readD(rj(instr) + rk(instr), instr));
      break;
    case op_fstx_s: {
      int32_t alu_out_32 = static_cast<int32_t>(getFpuRegister(fd_reg(instr)));
      writeW(rj(instr) + rk(instr), alu_out_32, instr);
      break;
    }
    case op_fstx_d: {
      writeD(rj(instr) + rk(instr), getFpuRegisterDouble(fd_reg(instr)), instr);
      break;
    }
    case op_amswap_w:
      UNIMPLEMENTED();
      break;
    case op_amswap_d:
      UNIMPLEMENTED();
      break;
    case op_amadd_w:
      UNIMPLEMENTED();
      break;
    case op_amadd_d:
      UNIMPLEMENTED();
      break;
    case op_amand_w:
      UNIMPLEMENTED();
      break;
    case op_amand_d:
      UNIMPLEMENTED();
      break;
    case op_amor_w:
      UNIMPLEMENTED();
      break;
    case op_amor_d:
      UNIMPLEMENTED();
      break;
    case op_amxor_w:
      UNIMPLEMENTED();
      break;
    case op_amxor_d:
      UNIMPLEMENTED();
      break;
    case op_ammax_w:
      UNIMPLEMENTED();
      break;
    case op_ammax_d:
      UNIMPLEMENTED();
      break;
    case op_ammin_w:
      UNIMPLEMENTED();
      break;
    case op_ammin_d:
      UNIMPLEMENTED();
      break;
    case op_ammax_wu:
      UNIMPLEMENTED();
      break;
    case op_ammax_du:
      UNIMPLEMENTED();
      break;
    case op_ammin_wu:
      UNIMPLEMENTED();
      break;
    case op_ammin_du:
      UNIMPLEMENTED();
      break;
    case op_amswap_db_w:
      UNIMPLEMENTED();
      break;
    case op_amswap_db_d:
      UNIMPLEMENTED();
      break;
    case op_amadd_db_w:
      UNIMPLEMENTED();
      break;
    case op_amadd_db_d:
      UNIMPLEMENTED();
      break;
    case op_amand_db_w:
      UNIMPLEMENTED();
      break;
    case op_amand_db_d:
      UNIMPLEMENTED();
      break;
    case op_amor_db_w:
      UNIMPLEMENTED();
      break;
    case op_amor_db_d:
      UNIMPLEMENTED();
      break;
    case op_amxor_db_w:
      UNIMPLEMENTED();
      break;
    case op_amxor_db_d:
      UNIMPLEMENTED();
      break;
    case op_ammax_db_w:
      UNIMPLEMENTED();
      break;
    case op_ammax_db_d:
      UNIMPLEMENTED();
      break;
    case op_ammin_db_w:
      UNIMPLEMENTED();
      break;
    case op_ammin_db_d:
      UNIMPLEMENTED();
      break;
    case op_ammax_db_wu:
      UNIMPLEMENTED();
      break;
    case op_ammax_db_du:
      UNIMPLEMENTED();
      break;
    case op_ammin_db_wu:
      UNIMPLEMENTED();
      break;
    case op_ammin_db_du:
      UNIMPLEMENTED();
      break;
    case op_dbar:
      // TODO(loong64): dbar simulation
      break;
    case op_ibar:
      UNIMPLEMENTED();
      break;
    case op_fcopysign_s:
      UNIMPLEMENTED();
      break;
    case op_fcopysign_d:
      UNIMPLEMENTED();
      break;
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp22(SimInstruction* instr) {
  int64_t alu_out;

  switch (instr->bits(31, 10) << 10) {
    case op_clz_w: {
      alu_out = U32(rj_u(instr)) ? __builtin_clz(U32(rj_u(instr))) : 32;
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_ctz_w: {
      alu_out = U32(rj_u(instr)) ? __builtin_ctz(U32(rj_u(instr))) : 32;
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_clz_d: {
      alu_out = U64(rj_u(instr)) ? __builtin_clzll(U64(rj_u(instr))) : 64;
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_ctz_d: {
      alu_out = U64(rj_u(instr)) ? __builtin_ctzll(U64(rj_u(instr))) : 64;
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_revb_2h: {
      uint32_t input = static_cast<uint32_t>(rj(instr));
      uint64_t output = 0;

      uint32_t mask = 0xFF000000;
      for (int i = 0; i < 4; i++) {
        uint32_t tmp = mask & input;
        if (i % 2 == 0) {
          tmp = tmp >> 8;
        } else {
          tmp = tmp << 8;
        }
        output = output | tmp;
        mask = mask >> 8;
      }

      alu_out = static_cast<int64_t>(static_cast<int32_t>(output));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_revb_4h: {
      uint64_t input = rj_u(instr);
      uint64_t output = 0;

      uint64_t mask = 0xFF00000000000000;
      for (int i = 0; i < 8; i++) {
        uint64_t tmp = mask & input;
        if (i % 2 == 0) {
          tmp = tmp >> 8;
        } else {
          tmp = tmp << 8;
        }
        output = output | tmp;
        mask = mask >> 8;
      }

      alu_out = static_cast<int64_t>(output);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_revb_2w: {
      uint64_t input = rj_u(instr);
      uint64_t output = 0;

      uint64_t mask = 0xFF000000FF000000;
      for (int i = 0; i < 4; i++) {
        uint64_t tmp = mask & input;
        if (i <= 1) {
          tmp = tmp >> (24 - i * 16);
        } else {
          tmp = tmp << (i * 16 - 24);
        }
        output = output | tmp;
        mask = mask >> 8;
      }

      alu_out = static_cast<int64_t>(output);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_revb_d: {
      uint64_t input = rj_u(instr);
      uint64_t output = 0;

      uint64_t mask = 0xFF00000000000000;
      for (int i = 0; i < 8; i++) {
        uint64_t tmp = mask & input;
        if (i <= 3) {
          tmp = tmp >> (56 - i * 16);
        } else {
          tmp = tmp << (i * 16 - 56);
        }
        output = output | tmp;
        mask = mask >> 8;
      }

      alu_out = static_cast<int64_t>(output);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_revh_2w: {
      uint64_t input = rj_u(instr);
      uint64_t output = 0;

      uint64_t mask = 0xFFFF000000000000;
      for (int i = 0; i < 4; i++) {
        uint64_t tmp = mask & input;
        if (i % 2 == 0) {
          tmp = tmp >> 16;
        } else {
          tmp = tmp << 16;
        }
        output = output | tmp;
        mask = mask >> 16;
      }

      alu_out = static_cast<int64_t>(output);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_revh_d: {
      uint64_t input = rj_u(instr);
      uint64_t output = 0;

      uint64_t mask = 0xFFFF000000000000;
      for (int i = 0; i < 4; i++) {
        uint64_t tmp = mask & input;
        if (i <= 1) {
          tmp = tmp >> (48 - i * 32);
        } else {
          tmp = tmp << (i * 32 - 48);
        }
        output = output | tmp;
        mask = mask >> 16;
      }

      alu_out = static_cast<int64_t>(output);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_bitrev_4b: {
      uint32_t input = static_cast<uint32_t>(rj(instr));
      uint32_t output = 0;
      uint8_t i_byte, o_byte;

      // Reverse the bit in byte for each individual byte
      for (int i = 0; i < 4; i++) {
        output = output >> 8;
        i_byte = input & 0xFF;

        // Fast way to reverse bits in byte
        // Devised by Sean Anderson, July 13, 2001
        o_byte = static_cast<uint8_t>(((i_byte * 0x0802LU & 0x22110LU) |
                                       (i_byte * 0x8020LU & 0x88440LU)) *
                                          0x10101LU >>
                                      16);

        output = output | (static_cast<uint32_t>(o_byte << 24));
        input = input >> 8;
      }

      alu_out = static_cast<int64_t>(static_cast<int32_t>(output));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_bitrev_8b: {
      uint64_t input = rj_u(instr);
      uint64_t output = 0;
      uint8_t i_byte, o_byte;

      // Reverse the bit in byte for each individual byte
      for (int i = 0; i < 8; i++) {
        output = output >> 8;
        i_byte = input & 0xFF;

        // Fast way to reverse bits in byte
        // Devised by Sean Anderson, July 13, 2001
        o_byte = static_cast<uint8_t>(((i_byte * 0x0802LU & 0x22110LU) |
                                       (i_byte * 0x8020LU & 0x88440LU)) *
                                          0x10101LU >>
                                      16);

        output = output | (static_cast<uint64_t>(o_byte) << 56);
        input = input >> 8;
      }

      alu_out = static_cast<int64_t>(output);
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_bitrev_w: {
      uint32_t input = static_cast<uint32_t>(rj(instr));
      uint32_t output = 0;
      output = ReverseBits(input);
      alu_out = static_cast<int64_t>(static_cast<int32_t>(output));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_bitrev_d: {
      alu_out = static_cast<int64_t>(ReverseBits(rj_u(instr)));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_ext_w_b: {
      uint8_t input = static_cast<uint8_t>(rj(instr));
      alu_out = static_cast<int64_t>(static_cast<int8_t>(input));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_ext_w_h: {
      uint16_t input = static_cast<uint16_t>(rj(instr));
      alu_out = static_cast<int64_t>(static_cast<int16_t>(input));
      setRegister(rd_reg(instr), alu_out);
      break;
    }
    case op_fabs_s: {
      setFpuRegisterFloat(fd_reg(instr), std::abs(fj_float(instr)));
      break;
    }
    case op_fabs_d: {
      setFpuRegisterDouble(fd_reg(instr), std::abs(fj_double(instr)));
      break;
    }
    case op_fneg_s: {
      setFpuRegisterFloat(fd_reg(instr), -fj_float(instr));
      break;
    }
    case op_fneg_d: {
      setFpuRegisterDouble(fd_reg(instr), -fj_double(instr));
      break;
    }
    case op_fsqrt_s: {
      if (fj_float(instr) >= 0) {
        setFpuRegisterFloat(fd_reg(instr), std::sqrt(fj_float(instr)));
      } else {
        setFpuRegisterFloat(fd_reg(instr), std::sqrt(-1));  // qnan
        setFCSRBit(kFCSRInvalidOpFlagBit, true);
      }
      break;
    }
    case op_fsqrt_d: {
      if (fj_double(instr) >= 0) {
        setFpuRegisterDouble(fd_reg(instr), std::sqrt(fj_double(instr)));
      } else {
        setFpuRegisterDouble(fd_reg(instr), std::sqrt(-1));  // qnan
        setFCSRBit(kFCSRInvalidOpFlagBit, true);
      }
      break;
    }
    case op_fmov_s: {
      setFpuRegisterFloat(fd_reg(instr), fj_float(instr));
      break;
    }
    case op_fmov_d: {
      setFpuRegisterDouble(fd_reg(instr), fj_double(instr));
      break;
    }
    case op_movgr2fr_w: {
      setFpuRegisterWord(fd_reg(instr), static_cast<int32_t>(rj(instr)));
      break;
    }
    case op_movgr2fr_d: {
      setFpuRegister(fd_reg(instr), rj(instr));
      break;
    }
    case op_movgr2frh_w: {
      setFpuRegisterHiWord(fd_reg(instr), static_cast<int32_t>(rj(instr)));
      break;
    }
    case op_movfr2gr_s: {
      setRegister(rd_reg(instr),
                  static_cast<int64_t>(getFpuRegisterWord(fj_reg(instr))));
      break;
    }
    case op_movfr2gr_d: {
      setRegister(rd_reg(instr), getFpuRegister(fj_reg(instr)));
      break;
    }
    case op_movfrh2gr_s: {
      setRegister(rd_reg(instr), getFpuRegisterHiWord(fj_reg(instr)));
      break;
    }
    case op_movgr2fcsr: {
      // fcsr could be 0-3
      MOZ_ASSERT(rd_reg(instr) < 4);
      FCSR_ = static_cast<uint32_t>(rj(instr));
      break;
    }
    case op_movfcsr2gr: {
      setRegister(rd_reg(instr), FCSR_);
      break;
    }
    case op_fcvt_s_d: {
      setFpuRegisterFloat(fd_reg(instr), static_cast<float>(fj_double(instr)));
      break;
    }
    case op_fcvt_d_s: {
      setFpuRegisterDouble(fd_reg(instr), static_cast<double>(fj_float(instr)));
      break;
    }
    case op_ftintrm_w_s: {
      float fj = fj_float(instr);
      float rounded = std::floor(fj);
      int32_t result = static_cast<int32_t>(rounded);
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterWordInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrm_w_d: {
      double fj = fj_double(instr);
      double rounded = std::floor(fj);
      int32_t result = static_cast<int32_t>(rounded);
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrm_l_s: {
      float fj = fj_float(instr);
      float rounded = std::floor(fj);
      int64_t result = static_cast<int64_t>(rounded);
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrm_l_d: {
      double fj = fj_double(instr);
      double rounded = std::floor(fj);
      int64_t result = static_cast<int64_t>(rounded);
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrp_w_s: {
      float fj = fj_float(instr);
      float rounded = std::ceil(fj);
      int32_t result = static_cast<int32_t>(rounded);
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterWordInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrp_w_d: {
      double fj = fj_double(instr);
      double rounded = std::ceil(fj);
      int32_t result = static_cast<int32_t>(rounded);
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrp_l_s: {
      float fj = fj_float(instr);
      float rounded = std::ceil(fj);
      int64_t result = static_cast<int64_t>(rounded);
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrp_l_d: {
      double fj = fj_double(instr);
      double rounded = std::ceil(fj);
      int64_t result = static_cast<int64_t>(rounded);
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrz_w_s: {
      float fj = fj_float(instr);
      float rounded = std::trunc(fj);
      int32_t result = static_cast<int32_t>(rounded);
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterWordInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrz_w_d: {
      double fj = fj_double(instr);
      double rounded = std::trunc(fj);
      int32_t result = static_cast<int32_t>(rounded);
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrz_l_s: {
      float fj = fj_float(instr);
      float rounded = std::trunc(fj);
      int64_t result = static_cast<int64_t>(rounded);
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrz_l_d: {
      double fj = fj_double(instr);
      double rounded = std::trunc(fj);
      int64_t result = static_cast<int64_t>(rounded);
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrne_w_s: {
      float fj = fj_float(instr);
      float rounded = std::floor(fj + 0.5);
      int32_t result = static_cast<int32_t>(rounded);
      if ((result & 1) != 0 && result - fj == 0.5) {
        // If the number is halfway between two integers,
        // round to the even one.
        result--;
      }
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterWordInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrne_w_d: {
      double fj = fj_double(instr);
      double rounded = std::floor(fj + 0.5);
      int32_t result = static_cast<int32_t>(rounded);
      if ((result & 1) != 0 && result - fj == 0.5) {
        // If the number is halfway between two integers,
        // round to the even one.
        result--;
      }
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrne_l_s: {
      float fj = fj_float(instr);
      float rounded = std::floor(fj + 0.5);
      int64_t result = static_cast<int64_t>(rounded);
      if ((result & 1) != 0 && result - fj == 0.5) {
        // If the number is halfway between two integers,
        // round to the even one.
        result--;
      }
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftintrne_l_d: {
      double fj = fj_double(instr);
      double rounded = std::floor(fj + 0.5);
      int64_t result = static_cast<int64_t>(rounded);
      if ((result & 1) != 0 && result - fj == 0.5) {
        // If the number is halfway between two integers,
        // round to the even one.
        result--;
      }
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftint_w_s: {
      float fj = fj_float(instr);
      float rounded;
      int32_t result;
      roundAccordingToFCSR<float>(fj, &rounded, &result);
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterWordInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftint_w_d: {
      double fj = fj_double(instr);
      double rounded;
      int32_t result;
      roundAccordingToFCSR<double>(fj, &rounded, &result);
      setFpuRegisterWord(fd_reg(instr), result);
      if (setFCSRRoundError<int32_t>(fj, rounded)) {
        setFpuRegisterWordInvalidResult(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftint_l_s: {
      float fj = fj_float(instr);
      float rounded;
      int64_t result;
      round64AccordingToFCSR<float>(fj, &rounded, &result);
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ftint_l_d: {
      double fj = fj_double(instr);
      double rounded;
      int64_t result;
      round64AccordingToFCSR<double>(fj, &rounded, &result);
      setFpuRegister(fd_reg(instr), result);
      if (setFCSRRoundError<int64_t>(fj, rounded)) {
        setFpuRegisterInvalidResult64(fj, rounded, fd_reg(instr));
      }
      break;
    }
    case op_ffint_s_w: {
      alu_out = getFpuRegisterSignedWord(fj_reg(instr));
      setFpuRegisterFloat(fd_reg(instr), static_cast<float>(alu_out));
      break;
    }
    case op_ffint_s_l: {
      alu_out = getFpuRegister(fj_reg(instr));
      setFpuRegisterFloat(fd_reg(instr), static_cast<float>(alu_out));
      break;
    }
    case op_ffint_d_w: {
      alu_out = getFpuRegisterSignedWord(fj_reg(instr));
      setFpuRegisterDouble(fd_reg(instr), static_cast<double>(alu_out));
      break;
    }
    case op_ffint_d_l: {
      alu_out = getFpuRegister(fj_reg(instr));
      setFpuRegisterDouble(fd_reg(instr), static_cast<double>(alu_out));
      break;
    }
    case op_frint_s: {
      float fj = fj_float(instr);
      float result, temp_result;
      double temp;
      float upper = std::ceil(fj);
      float lower = std::floor(fj);
      switch (getFCSRRoundingMode()) {
        case kRoundToNearest:
          if (upper - fj < fj - lower) {
            result = upper;
          } else if (upper - fj > fj - lower) {
            result = lower;
          } else {
            temp_result = upper / 2;
            float reminder = std::modf(temp_result, &temp);
            if (reminder == 0) {
              result = upper;
            } else {
              result = lower;
            }
          }
          break;
        case kRoundToZero:
          result = (fj > 0 ? lower : upper);
          break;
        case kRoundToPlusInf:
          result = upper;
          break;
        case kRoundToMinusInf:
          result = lower;
          break;
      }
      setFpuRegisterFloat(fd_reg(instr), result);
      if (result != fj) {
        setFCSRBit(kFCSRInexactFlagBit, true);
      }
      break;
    }
    case op_frint_d: {
      double fj = fj_double(instr);
      double result, temp, temp_result;
      double upper = std::ceil(fj);
      double lower = std::floor(fj);
      switch (getFCSRRoundingMode()) {
        case kRoundToNearest:
          if (upper - fj < fj - lower) {
            result = upper;
          } else if (upper - fj > fj - lower) {
            result = lower;
          } else {
            temp_result = upper / 2;
            double reminder = std::modf(temp_result, &temp);
            if (reminder == 0) {
              result = upper;
            } else {
              result = lower;
            }
          }
          break;
        case kRoundToZero:
          result = (fj > 0 ? lower : upper);
          break;
        case kRoundToPlusInf:
          result = upper;
          break;
        case kRoundToMinusInf:
          result = lower;
          break;
      }
      setFpuRegisterDouble(fd_reg(instr), result);
      if (result != fj) {
        setFCSRBit(kFCSRInexactFlagBit, true);
      }
      break;
    }
    case op_movfr2cf:
      printf("Sim UNIMPLEMENTED: MOVFR2CF\n");
      UNIMPLEMENTED();
      break;
    case op_movgr2cf:
      printf("Sim UNIMPLEMENTED: MOVGR2CF\n");
      UNIMPLEMENTED();
      break;
    case op_clo_w:
      printf("Sim UNIMPLEMENTED: FCO_W\n");
      UNIMPLEMENTED();
      break;
    case op_cto_w:
      printf("Sim UNIMPLEMENTED: FTO_W\n");
      UNIMPLEMENTED();
      break;
    case op_clo_d:
      printf("Sim UNIMPLEMENTED: FLO_D\n");
      UNIMPLEMENTED();
      break;
    case op_cto_d:
      printf("Sim UNIMPLEMENTED: FTO_D\n");
      UNIMPLEMENTED();
      break;
    // Unimplemented opcodes raised an error in the configuration step before,
    // so we can use the default here to set the destination register in common
    // cases.
    default:
      UNREACHABLE();
  }
}

void Simulator::decodeTypeOp24(SimInstruction* instr) {
  switch (instr->bits(31, 8) << 8) {
    case op_movcf2fr:
      UNIMPLEMENTED();
      break;
    case op_movcf2gr:
      setRegister(rd_reg(instr), getCFRegister(cj_reg(instr)));
      break;
      UNIMPLEMENTED();
      break;
    default:
      UNREACHABLE();
  }
}

// Executes the current instruction.
void Simulator::instructionDecode(SimInstruction* instr) {
  if (!SimulatorProcess::ICacheCheckingDisableCount) {
    AutoLockSimulatorCache als;
    SimulatorProcess::checkICacheLocked(instr);
  }
  pc_modified_ = false;

  switch (instr->instructionType()) {
    case SimInstruction::kOp6Type:
      decodeTypeOp6(instr);
      break;
    case SimInstruction::kOp7Type:
      decodeTypeOp7(instr);
      break;
    case SimInstruction::kOp8Type:
      decodeTypeOp8(instr);
      break;
    case SimInstruction::kOp10Type:
      decodeTypeOp10(instr);
      break;
    case SimInstruction::kOp11Type:
      decodeTypeOp11(instr);
      break;
    case SimInstruction::kOp12Type:
      decodeTypeOp12(instr);
      break;
    case SimInstruction::kOp14Type:
      decodeTypeOp14(instr);
      break;
    case SimInstruction::kOp15Type:
      decodeTypeOp15(instr);
      break;
    case SimInstruction::kOp16Type:
      decodeTypeOp16(instr);
      break;
    case SimInstruction::kOp17Type:
      decodeTypeOp17(instr);
      break;
    case SimInstruction::kOp22Type:
      decodeTypeOp22(instr);
      break;
    case SimInstruction::kOp24Type:
      decodeTypeOp24(instr);
      break;
    default:
      UNSUPPORTED();
  }
  if (!pc_modified_) {
    setRegister(pc,
                reinterpret_cast<int64_t>(instr) + SimInstruction::kInstrSize);
  }
}

void Simulator::enable_single_stepping(SingleStepCallback cb, void* arg) {
  single_stepping_ = true;
  single_step_callback_ = cb;
  single_step_callback_arg_ = arg;
  single_step_callback_(single_step_callback_arg_, this, (void*)get_pc());
}

void Simulator::disable_single_stepping() {
  if (!single_stepping_) {
    return;
  }
  single_step_callback_(single_step_callback_arg_, this, (void*)get_pc());
  single_stepping_ = false;
  single_step_callback_ = nullptr;
  single_step_callback_arg_ = nullptr;
}

template <bool enableStopSimAt>
void Simulator::execute() {
  if (single_stepping_) {
    single_step_callback_(single_step_callback_arg_, this, nullptr);
  }

  // Get the PC to simulate. Cannot use the accessor here as we need the
  // raw PC value and not the one used as input to arithmetic instructions.
  int64_t program_counter = get_pc();

  while (program_counter != end_sim_pc) {
    if (enableStopSimAt && (icount_ == Simulator::StopSimAt)) {
      loong64Debugger dbg(this);
      dbg.debug();
    } else {
      if (single_stepping_) {
        single_step_callback_(single_step_callback_arg_, this,
                              (void*)program_counter);
      }
      SimInstruction* instr =
          reinterpret_cast<SimInstruction*>(program_counter);
      instructionDecode(instr);
      icount_++;
    }
    program_counter = get_pc();
  }

  if (single_stepping_) {
    single_step_callback_(single_step_callback_arg_, this, nullptr);
  }
}

void Simulator::callInternal(uint8_t* entry) {
  // Prepare to execute the code at entry.
  setRegister(pc, reinterpret_cast<int64_t>(entry));
  // Put down marker for end of simulation. The simulator will stop simulation
  // when the PC reaches this value. By saving the "end simulation" value into
  // the LR the simulation stops when returning to this call point.
  setRegister(ra, end_sim_pc);

  // Remember the values of callee-saved registers.
  // The code below assumes that r9 is not used as sb (static base) in
  // simulator code and therefore is regarded as a callee-saved register.
  int64_t s0_val = getRegister(s0);
  int64_t s1_val = getRegister(s1);
  int64_t s2_val = getRegister(s2);
  int64_t s3_val = getRegister(s3);
  int64_t s4_val = getRegister(s4);
  int64_t s5_val = getRegister(s5);
  int64_t s6_val = getRegister(s6);
  int64_t s7_val = getRegister(s7);
  int64_t s8_val = getRegister(s8);
  int64_t gp_val = getRegister(gp);
  int64_t sp_val = getRegister(sp);
  int64_t tp_val = getRegister(tp);
  int64_t fp_val = getRegister(fp);

  // Set up the callee-saved registers with a known value. To be able to check
  // that they are preserved properly across JS execution.
  int64_t callee_saved_value = icount_;
  setRegister(s0, callee_saved_value);
  setRegister(s1, callee_saved_value);
  setRegister(s2, callee_saved_value);
  setRegister(s3, callee_saved_value);
  setRegister(s4, callee_saved_value);
  setRegister(s5, callee_saved_value);
  setRegister(s6, callee_saved_value);
  setRegister(s7, callee_saved_value);
  setRegister(s8, callee_saved_value);
  setRegister(gp, callee_saved_value);
  setRegister(tp, callee_saved_value);
  setRegister(fp, callee_saved_value);

  // Start the simulation.
  if (Simulator::StopSimAt != -1) {
    execute<true>();
  } else {
    execute<false>();
  }

  // Check that the callee-saved registers have been preserved.
  MOZ_ASSERT(callee_saved_value == getRegister(s0));
  MOZ_ASSERT(callee_saved_value == getRegister(s1));
  MOZ_ASSERT(callee_saved_value == getRegister(s2));
  MOZ_ASSERT(callee_saved_value == getRegister(s3));
  MOZ_ASSERT(callee_saved_value == getRegister(s4));
  MOZ_ASSERT(callee_saved_value == getRegister(s5));
  MOZ_ASSERT(callee_saved_value == getRegister(s6));
  MOZ_ASSERT(callee_saved_value == getRegister(s7));
  MOZ_ASSERT(callee_saved_value == getRegister(s8));
  MOZ_ASSERT(callee_saved_value == getRegister(gp));
  MOZ_ASSERT(callee_saved_value == getRegister(tp));
  MOZ_ASSERT(callee_saved_value == getRegister(fp));

  // Restore callee-saved registers with the original value.
  setRegister(s0, s0_val);
  setRegister(s1, s1_val);
  setRegister(s2, s2_val);
  setRegister(s3, s3_val);
  setRegister(s4, s4_val);
  setRegister(s5, s5_val);
  setRegister(s6, s6_val);
  setRegister(s7, s7_val);
  setRegister(s8, s8_val);
  setRegister(gp, gp_val);
  setRegister(sp, sp_val);
  setRegister(tp, tp_val);
  setRegister(fp, fp_val);
}

int64_t Simulator::call(uint8_t* entry, int argument_count, ...) {
  va_list parameters;
  va_start(parameters, argument_count);

  int64_t original_stack = getRegister(sp);
  // Compute position of stack on entry to generated code.
  int64_t entry_stack = original_stack;
  if (argument_count > kCArgSlotCount) {
    entry_stack = entry_stack - argument_count * sizeof(int64_t);
  } else {
    entry_stack = entry_stack - kCArgsSlotsSize;
  }

  entry_stack &= ~U64(ABIStackAlignment - 1);

  intptr_t* stack_argument = reinterpret_cast<intptr_t*>(entry_stack);

  // Setup the arguments.
  for (int i = 0; i < argument_count; i++) {
    js::jit::Register argReg;
    if (GetIntArgReg(i, &argReg)) {
      setRegister(argReg.code(), va_arg(parameters, int64_t));
    } else {
      stack_argument[i] = va_arg(parameters, int64_t);
    }
  }

  va_end(parameters);
  setRegister(sp, entry_stack);

  callInternal(entry);

  // Pop stack passed arguments.
  MOZ_ASSERT(entry_stack == getRegister(sp));
  setRegister(sp, original_stack);

  int64_t result = getRegister(a0);
  return result;
}

uintptr_t Simulator::pushAddress(uintptr_t address) {
  int new_sp = getRegister(sp) - sizeof(uintptr_t);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(new_sp);
  *stack_slot = address;
  setRegister(sp, new_sp);
  return new_sp;
}

uintptr_t Simulator::popAddress() {
  int current_sp = getRegister(sp);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(current_sp);
  uintptr_t address = *stack_slot;
  setRegister(sp, current_sp + sizeof(uintptr_t));
  return address;
}

}  // namespace jit
}  // namespace js

js::jit::Simulator* JSContext::simulator() const { return simulator_; }

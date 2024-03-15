/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_loong64_Assembler_loong64_h
#define jit_loong64_Assembler_loong64_h

#include "mozilla/Sprintf.h"
#include <iterator>

#include "jit/CompactBuffer.h"
#include "jit/JitCode.h"
#include "jit/JitSpewer.h"
#include "jit/loong64/Architecture-loong64.h"
#include "jit/shared/Assembler-shared.h"
#include "jit/shared/Disassembler-shared.h"
#include "jit/shared/IonAssemblerBuffer.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace jit {

static constexpr Register zero{Registers::zero};
static constexpr Register ra{Registers::ra};
static constexpr Register tp{Registers::tp};
static constexpr Register sp{Registers::sp};
static constexpr Register a0{Registers::a0};
static constexpr Register a1{Registers::a1};
static constexpr Register a2{Registers::a2};
static constexpr Register a3{Registers::a3};
static constexpr Register a4{Registers::a4};
static constexpr Register a5{Registers::a5};
static constexpr Register a6{Registers::a6};
static constexpr Register a7{Registers::a7};
static constexpr Register t0{Registers::t0};
static constexpr Register t1{Registers::t1};
static constexpr Register t2{Registers::t2};
static constexpr Register t3{Registers::t3};
static constexpr Register t4{Registers::t4};
static constexpr Register t5{Registers::t5};
static constexpr Register t6{Registers::t6};
static constexpr Register t7{Registers::t7};
static constexpr Register t8{Registers::t8};
static constexpr Register rx{Registers::rx};
static constexpr Register fp{Registers::fp};
static constexpr Register s0{Registers::s0};
static constexpr Register s1{Registers::s1};
static constexpr Register s2{Registers::s2};
static constexpr Register s3{Registers::s3};
static constexpr Register s4{Registers::s4};
static constexpr Register s5{Registers::s5};
static constexpr Register s6{Registers::s6};
static constexpr Register s7{Registers::s7};
static constexpr Register s8{Registers::s8};

static constexpr FloatRegister f0{FloatRegisters::f0, FloatRegisters::Double};
static constexpr FloatRegister f1{FloatRegisters::f1, FloatRegisters::Double};
static constexpr FloatRegister f2{FloatRegisters::f2, FloatRegisters::Double};
static constexpr FloatRegister f3{FloatRegisters::f3, FloatRegisters::Double};
static constexpr FloatRegister f4{FloatRegisters::f4, FloatRegisters::Double};
static constexpr FloatRegister f5{FloatRegisters::f5, FloatRegisters::Double};
static constexpr FloatRegister f6{FloatRegisters::f6, FloatRegisters::Double};
static constexpr FloatRegister f7{FloatRegisters::f7, FloatRegisters::Double};
static constexpr FloatRegister f8{FloatRegisters::f8, FloatRegisters::Double};
static constexpr FloatRegister f9{FloatRegisters::f9, FloatRegisters::Double};
static constexpr FloatRegister f10{FloatRegisters::f10, FloatRegisters::Double};
static constexpr FloatRegister f11{FloatRegisters::f11, FloatRegisters::Double};
static constexpr FloatRegister f12{FloatRegisters::f12, FloatRegisters::Double};
static constexpr FloatRegister f13{FloatRegisters::f13, FloatRegisters::Double};
static constexpr FloatRegister f14{FloatRegisters::f14, FloatRegisters::Double};
static constexpr FloatRegister f15{FloatRegisters::f15, FloatRegisters::Double};
static constexpr FloatRegister f16{FloatRegisters::f16, FloatRegisters::Double};
static constexpr FloatRegister f17{FloatRegisters::f17, FloatRegisters::Double};
static constexpr FloatRegister f18{FloatRegisters::f18, FloatRegisters::Double};
static constexpr FloatRegister f19{FloatRegisters::f19, FloatRegisters::Double};
static constexpr FloatRegister f20{FloatRegisters::f20, FloatRegisters::Double};
static constexpr FloatRegister f21{FloatRegisters::f21, FloatRegisters::Double};
static constexpr FloatRegister f22{FloatRegisters::f22, FloatRegisters::Double};
static constexpr FloatRegister f23{FloatRegisters::f23, FloatRegisters::Double};
static constexpr FloatRegister f24{FloatRegisters::f24, FloatRegisters::Double};
static constexpr FloatRegister f25{FloatRegisters::f25, FloatRegisters::Double};
static constexpr FloatRegister f26{FloatRegisters::f26, FloatRegisters::Double};
static constexpr FloatRegister f27{FloatRegisters::f27, FloatRegisters::Double};
static constexpr FloatRegister f28{FloatRegisters::f28, FloatRegisters::Double};
static constexpr FloatRegister f29{FloatRegisters::f29, FloatRegisters::Double};
static constexpr FloatRegister f30{FloatRegisters::f30, FloatRegisters::Double};
static constexpr FloatRegister f31{FloatRegisters::f31, FloatRegisters::Double};

static constexpr Register InvalidReg{Registers::Invalid};
static constexpr FloatRegister InvalidFloatReg;

static constexpr Register StackPointer = sp;
static constexpr Register FramePointer = fp;
static constexpr Register ReturnReg = a0;
static constexpr Register64 ReturnReg64(ReturnReg);
static constexpr FloatRegister ReturnFloat32Reg{FloatRegisters::f0,
                                                FloatRegisters::Single};
static constexpr FloatRegister ReturnDoubleReg = f0;
static constexpr FloatRegister ReturnSimd128Reg = InvalidFloatReg;

static constexpr Register ScratchRegister = t7;
static constexpr Register SecondScratchReg = t8;

// Helper classes for ScratchRegister usage. Asserts that only one piece
// of code thinks it has exclusive ownership of each scratch register.
struct ScratchRegisterScope : public AutoRegisterScope {
  explicit ScratchRegisterScope(MacroAssembler& masm)
      : AutoRegisterScope(masm, ScratchRegister) {}
};

struct SecondScratchRegisterScope : public AutoRegisterScope {
  explicit SecondScratchRegisterScope(MacroAssembler& masm)
      : AutoRegisterScope(masm, SecondScratchReg) {}
};

static constexpr FloatRegister ScratchFloat32Reg{FloatRegisters::f23,
                                                 FloatRegisters::Single};
static constexpr FloatRegister ScratchDoubleReg = f23;
static constexpr FloatRegister ScratchSimd128Reg = InvalidFloatReg;

struct ScratchFloat32Scope : public AutoFloatRegisterScope {
  explicit ScratchFloat32Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchFloat32Reg) {}
};

struct ScratchDoubleScope : public AutoFloatRegisterScope {
  explicit ScratchDoubleScope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchDoubleReg) {}
};

// Use arg reg from EnterJIT function as OsrFrameReg.
static constexpr Register OsrFrameReg = a3;
static constexpr Register PreBarrierReg = a1;
static constexpr Register InterpreterPCReg = t0;
static constexpr Register CallTempReg0 = t0;
static constexpr Register CallTempReg1 = t1;
static constexpr Register CallTempReg2 = t2;
static constexpr Register CallTempReg3 = t3;
static constexpr Register CallTempReg4 = t4;
static constexpr Register CallTempReg5 = t5;
static constexpr Register CallTempNonArgRegs[] = {t0, t1, t2, t3};
static const uint32_t NumCallTempNonArgRegs = std::size(CallTempNonArgRegs);

static constexpr Register IntArgReg0 = a0;
static constexpr Register IntArgReg1 = a1;
static constexpr Register IntArgReg2 = a2;
static constexpr Register IntArgReg3 = a3;
static constexpr Register IntArgReg4 = a4;
static constexpr Register IntArgReg5 = a5;
static constexpr Register IntArgReg6 = a6;
static constexpr Register IntArgReg7 = a7;
static constexpr Register HeapReg = s7;

// Registers used by RegExpMatcher and RegExpExecMatch stubs (do not use
// JSReturnOperand).
static constexpr Register RegExpMatcherRegExpReg = CallTempReg0;
static constexpr Register RegExpMatcherStringReg = CallTempReg1;
static constexpr Register RegExpMatcherLastIndexReg = CallTempReg2;

// Registers used by RegExpExecTest stub (do not use ReturnReg).
static constexpr Register RegExpExecTestRegExpReg = CallTempReg0;
static constexpr Register RegExpExecTestStringReg = CallTempReg1;

// Registers used by RegExpSearcher stub (do not use ReturnReg).
static constexpr Register RegExpSearcherRegExpReg = CallTempReg0;
static constexpr Register RegExpSearcherStringReg = CallTempReg1;
static constexpr Register RegExpSearcherLastIndexReg = CallTempReg2;

static constexpr Register JSReturnReg_Type = a3;
static constexpr Register JSReturnReg_Data = a2;
static constexpr Register JSReturnReg = a2;
static constexpr ValueOperand JSReturnOperand = ValueOperand(JSReturnReg);

// These registers may be volatile or nonvolatile.
static constexpr Register ABINonArgReg0 = t0;
static constexpr Register ABINonArgReg1 = t1;
static constexpr Register ABINonArgReg2 = t2;
static constexpr Register ABINonArgReg3 = t3;

// These registers may be volatile or nonvolatile.
// Note: these three registers are all guaranteed to be different
static constexpr Register ABINonArgReturnReg0 = t0;
static constexpr Register ABINonArgReturnReg1 = t1;
static constexpr Register ABINonVolatileReg = s0;

// This register is guaranteed to be clobberable during the prologue and
// epilogue of an ABI call which must preserve both ABI argument, return
// and non-volatile registers.
static constexpr Register ABINonArgReturnVolatileReg = ra;

// This register may be volatile or nonvolatile.
// Avoid f23 which is the scratch register.
static constexpr FloatRegister ABINonArgDoubleReg{FloatRegisters::f21,
                                                  FloatRegisters::Double};

// Instance pointer argument register for WebAssembly functions. This must not
// alias any other register used for passing function arguments or return
// values. Preserved by WebAssembly functions. Must be nonvolatile.
static constexpr Register InstanceReg = s4;

// Registers used for wasm table calls. These registers must be disjoint
// from the ABI argument registers, InstanceReg and each other.
static constexpr Register WasmTableCallScratchReg0 = ABINonArgReg0;
static constexpr Register WasmTableCallScratchReg1 = ABINonArgReg1;
static constexpr Register WasmTableCallSigReg = ABINonArgReg2;
static constexpr Register WasmTableCallIndexReg = ABINonArgReg3;

// Registers used for ref calls.
static constexpr Register WasmCallRefCallScratchReg0 = ABINonArgReg0;
static constexpr Register WasmCallRefCallScratchReg1 = ABINonArgReg1;
static constexpr Register WasmCallRefReg = ABINonArgReg3;

// Register used as a scratch along the return path in the fast js -> wasm stub
// code. This must not overlap ReturnReg, JSReturnOperand, or InstanceReg.
// It must be a volatile register.
static constexpr Register WasmJitEntryReturnScratch = t1;

static constexpr uint32_t ABIStackAlignment = 16;
static constexpr uint32_t CodeAlignment = 16;
static constexpr uint32_t JitStackAlignment = 16;

static constexpr uint32_t JitStackValueAlignment =
    JitStackAlignment / sizeof(Value);
static_assert(JitStackAlignment % sizeof(Value) == 0 &&
                  JitStackValueAlignment >= 1,
              "Stack alignment should be a non-zero multiple of sizeof(Value)");

// TODO(loong64): this is just a filler to prevent a build failure. The
// LoongArch SIMD alignment requirements still need to be explored.
static constexpr uint32_t SimdMemoryAlignment = 16;

static_assert(CodeAlignment % SimdMemoryAlignment == 0,
              "Code alignment should be larger than any of the alignments "
              "which are used for "
              "the constant sections of the code buffer.  Thus it should be "
              "larger than the "
              "alignment for SIMD constants.");

static constexpr uint32_t WasmStackAlignment = SimdMemoryAlignment;
static const uint32_t WasmTrapInstructionLength = 4;

// See comments in wasm::GenerateFunctionPrologue.  The difference between these
// is the size of the largest callable prologue on the platform.
static constexpr uint32_t WasmCheckedCallEntryOffset = 0u;

static constexpr Scale ScalePointer = TimesEight;

// TODO(loong64): Add LoongArch instruction types description.

// LoongArch instruction encoding constants.
static const uint32_t RJShift = 5;
static const uint32_t RJBits = 5;
static const uint32_t RKShift = 10;
static const uint32_t RKBits = 5;
static const uint32_t RDShift = 0;
static const uint32_t RDBits = 5;
static const uint32_t FJShift = 5;
static const uint32_t FJBits = 5;
static const uint32_t FKShift = 10;
static const uint32_t FKBits = 5;
static const uint32_t FDShift = 0;
static const uint32_t FDBits = 5;
static const uint32_t FAShift = 15;
static const uint32_t FABits = 5;
static const uint32_t CJShift = 5;
static const uint32_t CJBits = 3;
static const uint32_t CDShift = 0;
static const uint32_t CDBits = 3;
static const uint32_t CAShift = 15;
static const uint32_t CABits = 3;
static const uint32_t CONDShift = 15;
static const uint32_t CONDBits = 5;

static const uint32_t SAShift = 15;
static const uint32_t SA2Bits = 2;
static const uint32_t SA3Bits = 3;
static const uint32_t LSBWShift = 10;
static const uint32_t LSBWBits = 5;
static const uint32_t LSBDShift = 10;
static const uint32_t LSBDBits = 6;
static const uint32_t MSBWShift = 16;
static const uint32_t MSBWBits = 5;
static const uint32_t MSBDShift = 16;
static const uint32_t MSBDBits = 6;
static const uint32_t Imm5Shift = 10;
static const uint32_t Imm5Bits = 5;
static const uint32_t Imm6Shift = 10;
static const uint32_t Imm6Bits = 6;
static const uint32_t Imm12Shift = 10;
static const uint32_t Imm12Bits = 12;
static const uint32_t Imm14Shift = 10;
static const uint32_t Imm14Bits = 14;
static const uint32_t Imm15Shift = 0;
static const uint32_t Imm15Bits = 15;
static const uint32_t Imm16Shift = 10;
static const uint32_t Imm16Bits = 16;
static const uint32_t Imm20Shift = 5;
static const uint32_t Imm20Bits = 20;
static const uint32_t Imm21Shift = 0;
static const uint32_t Imm21Bits = 21;
static const uint32_t Imm26Shift = 0;
static const uint32_t Imm26Bits = 26;
static const uint32_t CODEShift = 0;
static const uint32_t CODEBits = 15;

// LoongArch instruction field bit masks.
static const uint32_t RJMask = (1 << RJBits) - 1;
static const uint32_t RKMask = (1 << RKBits) - 1;
static const uint32_t RDMask = (1 << RDBits) - 1;
static const uint32_t SA2Mask = (1 << SA2Bits) - 1;
static const uint32_t SA3Mask = (1 << SA3Bits) - 1;
static const uint32_t CONDMask = (1 << CONDBits) - 1;
static const uint32_t LSBWMask = (1 << LSBWBits) - 1;
static const uint32_t LSBDMask = (1 << LSBDBits) - 1;
static const uint32_t MSBWMask = (1 << MSBWBits) - 1;
static const uint32_t MSBDMask = (1 << MSBDBits) - 1;
static const uint32_t CODEMask = (1 << CODEBits) - 1;
static const uint32_t Imm5Mask = (1 << Imm5Bits) - 1;
static const uint32_t Imm6Mask = (1 << Imm6Bits) - 1;
static const uint32_t Imm12Mask = (1 << Imm12Bits) - 1;
static const uint32_t Imm14Mask = (1 << Imm14Bits) - 1;
static const uint32_t Imm15Mask = (1 << Imm15Bits) - 1;
static const uint32_t Imm16Mask = (1 << Imm16Bits) - 1;
static const uint32_t Imm20Mask = (1 << Imm20Bits) - 1;
static const uint32_t Imm21Mask = (1 << Imm21Bits) - 1;
static const uint32_t Imm26Mask = (1 << Imm26Bits) - 1;
static const uint32_t BOffImm16Mask = ((1 << Imm16Bits) - 1) << Imm16Shift;
static const uint32_t BOffImm21Mask = ((1 << Imm21Bits) - 1) << Imm21Shift;
static const uint32_t BOffImm26Mask = ((1 << Imm26Bits) - 1) << Imm26Shift;
static const uint32_t RegMask = Registers::Total - 1;

// TODO(loong64) Change to syscall?
static const uint32_t MAX_BREAK_CODE = 1024 - 1;
static const uint32_t WASM_TRAP = 6;  // BRK_OVERFLOW

// TODO(loong64) Change to LoongArch instruction type.
class Instruction;
class InstReg;
class InstImm;
class InstJump;

uint32_t RJ(Register r);
uint32_t RK(Register r);
uint32_t RD(Register r);
uint32_t FJ(FloatRegister r);
uint32_t FK(FloatRegister r);
uint32_t FD(FloatRegister r);
uint32_t FA(FloatRegister r);
uint32_t SA2(uint32_t value);
uint32_t SA2(FloatRegister r);
uint32_t SA3(uint32_t value);
uint32_t SA3(FloatRegister r);

Register toRK(Instruction& i);
Register toRJ(Instruction& i);
Register toRD(Instruction& i);
Register toR(Instruction& i);

// LoongArch enums for instruction fields
enum OpcodeField {
  op_beqz = 0x10U << 26,
  op_bnez = 0x11U << 26,
  op_bcz = 0x12U << 26,  // bceqz & bcnez
  op_jirl = 0x13U << 26,
  op_b = 0x14U << 26,
  op_bl = 0x15U << 26,
  op_beq = 0x16U << 26,
  op_bne = 0x17U << 26,
  op_blt = 0x18U << 26,
  op_bge = 0x19U << 26,
  op_bltu = 0x1aU << 26,
  op_bgeu = 0x1bU << 26,

  op_addu16i_d = 0x4U << 26,

  op_lu12i_w = 0xaU << 25,
  op_lu32i_d = 0xbU << 25,
  op_pcaddi = 0xcU << 25,
  op_pcalau12i = 0xdU << 25,
  op_pcaddu12i = 0xeU << 25,
  op_pcaddu18i = 0xfU << 25,
  op_ll_w = 0x20U << 24,
  op_sc_w = 0x21U << 24,
  op_ll_d = 0x22U << 24,
  op_sc_d = 0x23U << 24,
  op_ldptr_w = 0x24U << 24,
  op_stptr_w = 0x25U << 24,
  op_ldptr_d = 0x26U << 24,
  op_stptr_d = 0x27U << 24,
  op_bstrins_d = 0x2U << 22,
  op_bstrpick_d = 0x3U << 22,
  op_slti = 0x8U << 22,
  op_sltui = 0x9U << 22,
  op_addi_w = 0xaU << 22,
  op_addi_d = 0xbU << 22,
  op_lu52i_d = 0xcU << 22,
  op_andi = 0xdU << 22,
  op_ori = 0xeU << 22,
  op_xori = 0xfU << 22,
  op_ld_b = 0xa0U << 22,
  op_ld_h = 0xa1U << 22,
  op_ld_w = 0xa2U << 22,
  op_ld_d = 0xa3U << 22,
  op_st_b = 0xa4U << 22,
  op_st_h = 0xa5U << 22,
  op_st_w = 0xa6U << 22,
  op_st_d = 0xa7U << 22,
  op_ld_bu = 0xa8U << 22,
  op_ld_hu = 0xa9U << 22,
  op_ld_wu = 0xaaU << 22,
  op_preld = 0xabU << 22,
  op_fld_s = 0xacU << 22,
  op_fst_s = 0xadU << 22,
  op_fld_d = 0xaeU << 22,
  op_fst_d = 0xafU << 22,
  op_bstr_w = 0x3U << 21,  // BSTRINS_W & BSTRPICK_W
  op_fmadd_s = 0x81U << 20,
  op_fmadd_d = 0x82U << 20,
  op_fmsub_s = 0x85U << 20,
  op_fmsub_d = 0x86U << 20,
  op_fnmadd_s = 0x89U << 20,
  op_fnmadd_d = 0x8aU << 20,
  op_fnmsub_s = 0x8dU << 20,
  op_fnmsub_d = 0x8eU << 20,
  op_fcmp_cond_s = 0xc1U << 20,
  op_fcmp_cond_d = 0xc2U << 20,

  op_bytepick_d = 0x3U << 18,
  op_fsel = 0x340U << 18,

  op_bytepick_w = 0x4U << 17,
  op_alsl_w = 0x2U << 17,
  op_alsl_wu = 0x3U << 17,
  op_alsl_d = 0x16U << 17,

  op_slli_d = 0x41U << 16,
  op_srli_d = 0x45U << 16,
  op_srai_d = 0x49U << 16,

  op_slli_w = 0x81U << 15,
  op_srli_w = 0x89U << 15,
  op_srai_w = 0x91U << 15,
  op_add_w = 0x20U << 15,
  op_add_d = 0x21U << 15,
  op_sub_w = 0x22U << 15,
  op_sub_d = 0x23U << 15,
  op_slt = 0x24U << 15,
  op_sltu = 0x25U << 15,
  op_maskeqz = 0x26U << 15,
  op_masknez = 0x27U << 15,
  op_nor = 0x28U << 15,
  op_and = 0x29U << 15,
  op_or = 0x2aU << 15,
  op_xor = 0x2bU << 15,
  op_orn = 0x2cU << 15,
  op_andn = 0x2dU << 15,
  op_sll_w = 0x2eU << 15,
  op_srl_w = 0x2fU << 15,
  op_sra_w = 0x30U << 15,
  op_sll_d = 0x31U << 15,
  op_srl_d = 0x32U << 15,
  op_sra_d = 0x33U << 15,
  op_rotr_w = 0x36U << 15,
  op_rotr_d = 0x37U << 15,
  op_rotri_w = 0x99U << 15,
  op_rotri_d = 0x4DU << 16,
  op_mul_w = 0x38U << 15,
  op_mulh_w = 0x39U << 15,
  op_mulh_wu = 0x3aU << 15,
  op_mul_d = 0x3bU << 15,
  op_mulh_d = 0x3cU << 15,
  op_mulh_du = 0x3dU << 15,
  op_mulw_d_w = 0x3eU << 15,
  op_mulw_d_wu = 0x3fU << 15,
  op_div_w = 0x40U << 15,
  op_mod_w = 0x41U << 15,
  op_div_wu = 0x42U << 15,
  op_mod_wu = 0x43U << 15,
  op_div_d = 0x44U << 15,
  op_mod_d = 0x45U << 15,
  op_div_du = 0x46U << 15,
  op_mod_du = 0x47U << 15,
  op_break = 0x54U << 15,
  op_syscall = 0x56U << 15,
  op_fadd_s = 0x201U << 15,
  op_fadd_d = 0x202U << 15,
  op_fsub_s = 0x205U << 15,
  op_fsub_d = 0x206U << 15,
  op_fmul_s = 0x209U << 15,
  op_fmul_d = 0x20aU << 15,
  op_fdiv_s = 0x20dU << 15,
  op_fdiv_d = 0x20eU << 15,
  op_fmax_s = 0x211U << 15,
  op_fmax_d = 0x212U << 15,
  op_fmin_s = 0x215U << 15,
  op_fmin_d = 0x216U << 15,
  op_fmaxa_s = 0x219U << 15,
  op_fmaxa_d = 0x21aU << 15,
  op_fmina_s = 0x21dU << 15,
  op_fmina_d = 0x21eU << 15,
  op_fcopysign_s = 0x225U << 15,
  op_fcopysign_d = 0x226U << 15,
  op_ldx_b = 0x7000U << 15,
  op_ldx_h = 0x7008U << 15,
  op_ldx_w = 0x7010U << 15,
  op_ldx_d = 0x7018U << 15,
  op_stx_b = 0x7020U << 15,
  op_stx_h = 0x7028U << 15,
  op_stx_w = 0x7030U << 15,
  op_stx_d = 0x7038U << 15,
  op_ldx_bu = 0x7040U << 15,
  op_ldx_hu = 0x7048U << 15,
  op_ldx_wu = 0x7050U << 15,
  op_fldx_s = 0x7060U << 15,
  op_fldx_d = 0x7068U << 15,
  op_fstx_s = 0x7070U << 15,
  op_fstx_d = 0x7078U << 15,
  op_amswap_w = 0x70c0U << 15,
  op_amswap_d = 0x70c1U << 15,
  op_amadd_w = 0x70c2U << 15,
  op_amadd_d = 0x70c3U << 15,
  op_amand_w = 0x70c4U << 15,
  op_amand_d = 0x70c5U << 15,
  op_amor_w = 0x70c6U << 15,
  op_amor_d = 0x70c7U << 15,
  op_amxor_w = 0x70c8U << 15,
  op_amxor_d = 0x70c9U << 15,
  op_ammax_w = 0x70caU << 15,
  op_ammax_d = 0x70cbU << 15,
  op_ammin_w = 0x70ccU << 15,
  op_ammin_d = 0x70cdU << 15,
  op_ammax_wu = 0x70ceU << 15,
  op_ammax_du = 0x70cfU << 15,
  op_ammin_wu = 0x70d0U << 15,
  op_ammin_du = 0x70d1U << 15,
  op_amswap_db_w = 0x70d2U << 15,
  op_amswap_db_d = 0x70d3U << 15,
  op_amadd_db_w = 0x70d4U << 15,
  op_amadd_db_d = 0x70d5U << 15,
  op_amand_db_w = 0x70d6U << 15,
  op_amand_db_d = 0x70d7U << 15,
  op_amor_db_w = 0x70d8U << 15,
  op_amor_db_d = 0x70d9U << 15,
  op_amxor_db_w = 0x70daU << 15,
  op_amxor_db_d = 0x70dbU << 15,
  op_ammax_db_w = 0x70dcU << 15,
  op_ammax_db_d = 0x70ddU << 15,
  op_ammin_db_w = 0x70deU << 15,
  op_ammin_db_d = 0x70dfU << 15,
  op_ammax_db_wu = 0x70e0U << 15,
  op_ammax_db_du = 0x70e1U << 15,
  op_ammin_db_wu = 0x70e2U << 15,
  op_ammin_db_du = 0x70e3U << 15,
  op_dbar = 0x70e4U << 15,
  op_ibar = 0x70e5U << 15,
  op_clo_w = 0x4U << 10,
  op_clz_w = 0x5U << 10,
  op_cto_w = 0x6U << 10,
  op_ctz_w = 0x7U << 10,
  op_clo_d = 0x8U << 10,
  op_clz_d = 0x9U << 10,
  op_cto_d = 0xaU << 10,
  op_ctz_d = 0xbU << 10,
  op_revb_2h = 0xcU << 10,
  op_revb_4h = 0xdU << 10,
  op_revb_2w = 0xeU << 10,
  op_revb_d = 0xfU << 10,
  op_revh_2w = 0x10U << 10,
  op_revh_d = 0x11U << 10,
  op_bitrev_4b = 0x12U << 10,
  op_bitrev_8b = 0x13U << 10,
  op_bitrev_w = 0x14U << 10,
  op_bitrev_d = 0x15U << 10,
  op_ext_w_h = 0x16U << 10,
  op_ext_w_b = 0x17U << 10,
  op_fabs_s = 0x4501U << 10,
  op_fabs_d = 0x4502U << 10,
  op_fneg_s = 0x4505U << 10,
  op_fneg_d = 0x4506U << 10,
  op_fsqrt_s = 0x4511U << 10,
  op_fsqrt_d = 0x4512U << 10,
  op_fmov_s = 0x4525U << 10,
  op_fmov_d = 0x4526U << 10,
  op_movgr2fr_w = 0x4529U << 10,
  op_movgr2fr_d = 0x452aU << 10,
  op_movgr2frh_w = 0x452bU << 10,
  op_movfr2gr_s = 0x452dU << 10,
  op_movfr2gr_d = 0x452eU << 10,
  op_movfrh2gr_s = 0x452fU << 10,
  op_movgr2fcsr = 0x4530U << 10,
  op_movfcsr2gr = 0x4532U << 10,
  op_movfr2cf = 0x4534U << 10,
  op_movgr2cf = 0x4536U << 10,
  op_fcvt_s_d = 0x4646U << 10,
  op_fcvt_d_s = 0x4649U << 10,
  op_ftintrm_w_s = 0x4681U << 10,
  op_ftintrm_w_d = 0x4682U << 10,
  op_ftintrm_l_s = 0x4689U << 10,
  op_ftintrm_l_d = 0x468aU << 10,
  op_ftintrp_w_s = 0x4691U << 10,
  op_ftintrp_w_d = 0x4692U << 10,
  op_ftintrp_l_s = 0x4699U << 10,
  op_ftintrp_l_d = 0x469aU << 10,
  op_ftintrz_w_s = 0x46a1U << 10,
  op_ftintrz_w_d = 0x46a2U << 10,
  op_ftintrz_l_s = 0x46a9U << 10,
  op_ftintrz_l_d = 0x46aaU << 10,
  op_ftintrne_w_s = 0x46b1U << 10,
  op_ftintrne_w_d = 0x46b2U << 10,
  op_ftintrne_l_s = 0x46b9U << 10,
  op_ftintrne_l_d = 0x46baU << 10,
  op_ftint_w_s = 0x46c1U << 10,
  op_ftint_w_d = 0x46c2U << 10,
  op_ftint_l_s = 0x46c9U << 10,
  op_ftint_l_d = 0x46caU << 10,
  op_ffint_s_w = 0x4744U << 10,
  op_ffint_s_l = 0x4746U << 10,
  op_ffint_d_w = 0x4748U << 10,
  op_ffint_d_l = 0x474aU << 10,
  op_frint_s = 0x4791U << 10,
  op_frint_d = 0x4792U << 10,
  op_movcf2fr = 0x114d4U << 8,
  op_movcf2gr = 0x114dcU << 8,
};

class Operand;

// A BOffImm16 is a 16 bit immediate that is used for branches.
class BOffImm16 {
  uint32_t data;

 public:
  uint32_t encode() {
    MOZ_ASSERT(!isInvalid());
    return data;
  }
  int32_t decode() {
    MOZ_ASSERT(!isInvalid());
    return (int32_t(data << 18) >> 16);
  }

  explicit BOffImm16(int offset) : data((offset) >> 2 & Imm16Mask) {
    MOZ_ASSERT((offset & 0x3) == 0);
    MOZ_ASSERT(IsInRange(offset));
  }
  static bool IsInRange(int offset) {
    if ((offset) < int(unsigned(INT16_MIN) << 2)) {
      return false;
    }
    if ((offset) > (INT16_MAX << 2)) {
      return false;
    }
    return true;
  }
  static const uint32_t INVALID = 0x00020000;
  BOffImm16() : data(INVALID) {}

  bool isInvalid() { return data == INVALID; }
  Instruction* getDest(Instruction* src) const;

  BOffImm16(InstImm inst);
};

// A JOffImm26 is a 26 bit immediate that is used for unconditional jumps.
class JOffImm26 {
  uint32_t data;

 public:
  uint32_t encode() {
    MOZ_ASSERT(!isInvalid());
    return data;
  }
  int32_t decode() {
    MOZ_ASSERT(!isInvalid());
    return (int32_t(data << 8) >> 6);
  }

  explicit JOffImm26(int offset) : data((offset) >> 2 & Imm26Mask) {
    MOZ_ASSERT((offset & 0x3) == 0);
    MOZ_ASSERT(IsInRange(offset));
  }
  static bool IsInRange(int offset) {
    if ((offset) < -536870912) {
      return false;
    }
    if ((offset) > 536870908) {
      return false;
    }
    return true;
  }
  static const uint32_t INVALID = 0x20000000;
  JOffImm26() : data(INVALID) {}

  bool isInvalid() { return data == INVALID; }
  Instruction* getDest(Instruction* src);
};

class Imm16 {
  uint16_t value;

 public:
  Imm16();
  Imm16(uint32_t imm) : value(imm) {}
  uint32_t encode() { return value; }
  int32_t decodeSigned() { return value; }
  uint32_t decodeUnsigned() { return value; }

  static bool IsInSignedRange(int32_t imm) {
    return imm >= INT16_MIN && imm <= INT16_MAX;
  }

  static bool IsInUnsignedRange(uint32_t imm) { return imm <= UINT16_MAX; }
};

class Imm8 {
  uint8_t value;

 public:
  Imm8();
  Imm8(uint32_t imm) : value(imm) {}
  uint32_t encode(uint32_t shift) { return value << shift; }
  int32_t decodeSigned() { return value; }
  uint32_t decodeUnsigned() { return value; }
  static bool IsInSignedRange(int32_t imm) {
    return imm >= INT8_MIN && imm <= INT8_MAX;
  }
  static bool IsInUnsignedRange(uint32_t imm) { return imm <= UINT8_MAX; }
  static Imm8 Lower(Imm16 imm) { return Imm8(imm.decodeSigned() & 0xff); }
  static Imm8 Upper(Imm16 imm) {
    return Imm8((imm.decodeSigned() >> 8) & 0xff);
  }
};

class Operand {
 public:
  enum Tag { REG, FREG, MEM };

 private:
  Tag tag : 3;
  uint32_t reg : 5;
  int32_t offset;

 public:
  Operand(Register reg_) : tag(REG), reg(reg_.code()) {}

  Operand(FloatRegister freg) : tag(FREG), reg(freg.code()) {}

  Operand(Register base, Imm32 off)
      : tag(MEM), reg(base.code()), offset(off.value) {}

  Operand(Register base, int32_t off)
      : tag(MEM), reg(base.code()), offset(off) {}

  Operand(const Address& addr)
      : tag(MEM), reg(addr.base.code()), offset(addr.offset) {}

  Tag getTag() const { return tag; }

  Register toReg() const {
    MOZ_ASSERT(tag == REG);
    return Register::FromCode(reg);
  }

  FloatRegister toFReg() const {
    MOZ_ASSERT(tag == FREG);
    return FloatRegister::FromCode(reg);
  }

  void toAddr(Register* r, Imm32* dest) const {
    MOZ_ASSERT(tag == MEM);
    *r = Register::FromCode(reg);
    *dest = Imm32(offset);
  }
  Address toAddress() const {
    MOZ_ASSERT(tag == MEM);
    return Address(Register::FromCode(reg), offset);
  }
  int32_t disp() const {
    MOZ_ASSERT(tag == MEM);
    return offset;
  }

  int32_t base() const {
    MOZ_ASSERT(tag == MEM);
    return reg;
  }
  Register baseReg() const {
    MOZ_ASSERT(tag == MEM);
    return Register::FromCode(reg);
  }
};

// int check.
inline bool is_intN(int32_t x, unsigned n) {
  MOZ_ASSERT((0 < n) && (n < 64));
  int32_t limit = static_cast<int32_t>(1) << (n - 1);
  return (-limit <= x) && (x < limit);
}

inline bool is_uintN(int32_t x, unsigned n) {
  MOZ_ASSERT((0 < n) && (n < (sizeof(x) * 8)));
  return !(x >> n);
}

inline Imm32 Imm64::firstHalf() const { return low(); }

inline Imm32 Imm64::secondHalf() const { return hi(); }

static constexpr int32_t SliceSize = 1024;
typedef js::jit::AssemblerBuffer<SliceSize, Instruction> LOONGBuffer;

class LOONGBufferWithExecutableCopy : public LOONGBuffer {
 public:
  void executableCopy(uint8_t* buffer) {
    if (this->oom()) {
      return;
    }

    for (Slice* cur = head; cur != nullptr; cur = cur->getNext()) {
      memcpy(buffer, &cur->instructions, cur->length());
      buffer += cur->length();
    }
  }

  bool appendRawCode(const uint8_t* code, size_t numBytes) {
    if (this->oom()) {
      return false;
    }
    while (numBytes > SliceSize) {
      this->putBytes(SliceSize, code);
      numBytes -= SliceSize;
      code += SliceSize;
    }
    this->putBytes(numBytes, code);
    return !this->oom();
  }
};

class AssemblerLOONG64 : public AssemblerShared {
 public:
  // TODO(loong64): Should we remove these conditions here?
  enum Condition {
    Equal,
    NotEqual,
    Above,
    AboveOrEqual,
    Below,
    BelowOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
    GreaterThanOrEqual_Signed,
    GreaterThanOrEqual_NotSigned,
    LessThan,
    LessThan_Signed,
    LessThan_NotSigned,
    LessThanOrEqual,
    Overflow,
    CarrySet,
    CarryClear,
    Signed,
    NotSigned,
    Zero,
    NonZero,
    Always,
  };

  enum DoubleCondition {
    DoubleOrdered,
    DoubleEqual,
    DoubleNotEqual,
    DoubleGreaterThan,
    DoubleGreaterThanOrEqual,
    DoubleLessThan,
    DoubleLessThanOrEqual,
    DoubleUnordered,
    DoubleEqualOrUnordered,
    DoubleNotEqualOrUnordered,
    DoubleGreaterThanOrUnordered,
    DoubleGreaterThanOrEqualOrUnordered,
    DoubleLessThanOrUnordered,
    DoubleLessThanOrEqualOrUnordered
  };

  enum FPUCondition {
    kNoFPUCondition = -1,

    CAF = 0x00,
    SAF = 0x01,
    CLT = 0x02,
    SLT = 0x03,
    CEQ = 0x04,
    SEQ = 0x05,
    CLE = 0x06,
    SLE = 0x07,
    CUN = 0x08,
    SUN = 0x09,
    CULT = 0x0a,
    SULT = 0x0b,
    CUEQ = 0x0c,
    SUEQ = 0x0d,
    CULE = 0x0e,
    SULE = 0x0f,
    CNE = 0x10,
    SNE = 0x11,
    COR = 0x14,
    SOR = 0x15,
    CUNE = 0x18,
    SUNE = 0x19,
  };

  enum FPConditionBit { FCC0 = 0, FCC1, FFC2, FCC3, FCC4, FCC5, FCC6, FCC7 };

  enum FPControl { FCSR = 0 };

  enum FCSRBit { CauseI = 24, CauseU, CauseO, CauseZ, CauseV };

  enum FloatFormat { SingleFloat, DoubleFloat };

  enum JumpOrCall { BranchIsJump, BranchIsCall };

  enum FloatTestKind { TestForTrue, TestForFalse };

  // :( this should be protected, but since CodeGenerator
  // wants to use it, It needs to go out here :(

  BufferOffset nextOffset() { return m_buffer.nextOffset(); }

 protected:
  Instruction* editSrc(BufferOffset bo) { return m_buffer.getInst(bo); }

  // structure for fixing up pc-relative loads/jumps when a the machine code
  // gets moved (executable copy, gc, etc.)
  struct RelativePatch {
    // the offset within the code buffer where the value is loaded that
    // we want to fix-up
    BufferOffset offset;
    void* target;
    RelocationKind kind;

    RelativePatch(BufferOffset offset, void* target, RelocationKind kind)
        : offset(offset), target(target), kind(kind) {}
  };

  js::Vector<RelativePatch, 8, SystemAllocPolicy> jumps_;

  CompactBufferWriter jumpRelocations_;
  CompactBufferWriter dataRelocations_;

  LOONGBufferWithExecutableCopy m_buffer;

#ifdef JS_JITSPEW
  Sprinter* printer;
#endif

 public:
  AssemblerLOONG64()
      : m_buffer(),
#ifdef JS_JITSPEW
        printer(nullptr),
#endif
        isFinished(false) {
  }

  static Condition InvertCondition(Condition cond);
  static DoubleCondition InvertCondition(DoubleCondition cond);
  // This is changing the condition codes for cmp a, b to the same codes for cmp
  // b, a.
  static Condition InvertCmpCondition(Condition cond);

  // As opposed to x86/x64 version, the data relocation has to be executed
  // before to recover the pointer, and not after.
  void writeDataRelocation(ImmGCPtr ptr) {
    // Raw GC pointer relocations and Value relocations both end up in
    // TraceOneDataRelocation.
    if (ptr.value) {
      if (gc::IsInsideNursery(ptr.value)) {
        embedsNurseryPointers_ = true;
      }
      dataRelocations_.writeUnsigned(nextOffset().getOffset());
    }
  }

  void assertNoGCThings() const {
#ifdef DEBUG
    MOZ_ASSERT(dataRelocations_.length() == 0);
    for (auto& j : jumps_) {
      MOZ_ASSERT(j.kind == RelocationKind::HARDCODED);
    }
#endif
  }

 public:
  void setUnlimitedBuffer() { m_buffer.setUnlimited(); }
  bool oom() const;

  void setPrinter(Sprinter* sp) {
#ifdef JS_JITSPEW
    printer = sp;
#endif
  }

#ifdef JS_JITSPEW
  inline void spew(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3) {
    if (MOZ_UNLIKELY(printer || JitSpewEnabled(JitSpew_Codegen))) {
      va_list va;
      va_start(va, fmt);
      spew(fmt, va);
      va_end(va);
    }
  }

  void decodeBranchInstAndSpew(InstImm branch);
#else
  MOZ_ALWAYS_INLINE void spew(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3) {}
#endif

#ifdef JS_JITSPEW
  MOZ_COLD void spew(const char* fmt, va_list va) MOZ_FORMAT_PRINTF(2, 0) {
    // Buffer to hold the formatted string. Note that this may contain
    // '%' characters, so do not pass it directly to printf functions.
    char buf[200];

    int i = VsprintfLiteral(buf, fmt, va);
    if (i > -1) {
      if (printer) {
        printer->printf("%s\n", buf);
      }
      js::jit::JitSpew(js::jit::JitSpew_Codegen, "%s", buf);
    }
  }
#endif

  Register getStackPointer() const { return StackPointer; }

 protected:
  bool isFinished;

 public:
  void finish();
  bool appendRawCode(const uint8_t* code, size_t numBytes);
  bool reserve(size_t size);
  bool swapBuffer(wasm::Bytes& bytes);
  void executableCopy(void* buffer);
  void copyJumpRelocationTable(uint8_t* dest);
  void copyDataRelocationTable(uint8_t* dest);

  // Size of the instruction stream, in bytes.
  size_t size() const;
  // Size of the jump relocation table, in bytes.
  size_t jumpRelocationTableBytes() const;
  size_t dataRelocationTableBytes() const;

  // Size of the data table, in bytes.
  size_t bytesNeeded() const;

  // Write a blob of binary into the instruction stream *OR*
  // into a destination address. If dest is nullptr (the default), then the
  // instruction gets written into the instruction stream. If dest is not null
  // it is interpreted as a pointer to the location that we want the
  // instruction to be written.
  BufferOffset writeInst(uint32_t x, uint32_t* dest = nullptr);
  // A static variant for the cases where we don't want to have an assembler
  // object at all. Normally, you would use the dummy (nullptr) object.
  static void WriteInstStatic(uint32_t x, uint32_t* dest);

 public:
  BufferOffset haltingAlign(int alignment);
  BufferOffset nopAlign(int alignment);
  BufferOffset as_nop() { return as_andi(zero, zero, 0); }

  // Branch and jump instructions
  BufferOffset as_b(JOffImm26 off);
  BufferOffset as_bl(JOffImm26 off);
  BufferOffset as_jirl(Register rd, Register rj, BOffImm16 off);

  InstImm getBranchCode(JumpOrCall jumpOrCall);  // b, bl
  InstImm getBranchCode(Register rd, Register rj,
                        Condition c);  // beq, bne, bge, bgeu, blt, bltu
  InstImm getBranchCode(Register rj, Condition c);  // beqz, bnez
  InstImm getBranchCode(FPConditionBit cj);         // bceqz, bcnez

  // Arithmetic instructions
  BufferOffset as_add_w(Register rd, Register rj, Register rk);
  BufferOffset as_add_d(Register rd, Register rj, Register rk);
  BufferOffset as_sub_w(Register rd, Register rj, Register rk);
  BufferOffset as_sub_d(Register rd, Register rj, Register rk);

  BufferOffset as_addi_w(Register rd, Register rj, int32_t si12);
  BufferOffset as_addi_d(Register rd, Register rj, int32_t si12);
  BufferOffset as_addu16i_d(Register rd, Register rj, int32_t si16);

  BufferOffset as_alsl_w(Register rd, Register rj, Register rk, uint32_t sa2);
  BufferOffset as_alsl_wu(Register rd, Register rj, Register rk, uint32_t sa2);
  BufferOffset as_alsl_d(Register rd, Register rj, Register rk, uint32_t sa2);

  BufferOffset as_lu12i_w(Register rd, int32_t si20);
  BufferOffset as_lu32i_d(Register rd, int32_t si20);
  BufferOffset as_lu52i_d(Register rd, Register rj, int32_t si12);

  BufferOffset as_slt(Register rd, Register rj, Register rk);
  BufferOffset as_sltu(Register rd, Register rj, Register rk);
  BufferOffset as_slti(Register rd, Register rj, int32_t si12);
  BufferOffset as_sltui(Register rd, Register rj, int32_t si12);

  BufferOffset as_pcaddi(Register rd, int32_t si20);
  BufferOffset as_pcaddu12i(Register rd, int32_t si20);
  BufferOffset as_pcaddu18i(Register rd, int32_t si20);
  BufferOffset as_pcalau12i(Register rd, int32_t si20);

  BufferOffset as_mul_w(Register rd, Register rj, Register rk);
  BufferOffset as_mulh_w(Register rd, Register rj, Register rk);
  BufferOffset as_mulh_wu(Register rd, Register rj, Register rk);
  BufferOffset as_mul_d(Register rd, Register rj, Register rk);
  BufferOffset as_mulh_d(Register rd, Register rj, Register rk);
  BufferOffset as_mulh_du(Register rd, Register rj, Register rk);

  BufferOffset as_mulw_d_w(Register rd, Register rj, Register rk);
  BufferOffset as_mulw_d_wu(Register rd, Register rj, Register rk);

  BufferOffset as_div_w(Register rd, Register rj, Register rk);
  BufferOffset as_mod_w(Register rd, Register rj, Register rk);
  BufferOffset as_div_wu(Register rd, Register rj, Register rk);
  BufferOffset as_mod_wu(Register rd, Register rj, Register rk);
  BufferOffset as_div_d(Register rd, Register rj, Register rk);
  BufferOffset as_mod_d(Register rd, Register rj, Register rk);
  BufferOffset as_div_du(Register rd, Register rj, Register rk);
  BufferOffset as_mod_du(Register rd, Register rj, Register rk);

  // Logical instructions
  BufferOffset as_and(Register rd, Register rj, Register rk);
  BufferOffset as_or(Register rd, Register rj, Register rk);
  BufferOffset as_xor(Register rd, Register rj, Register rk);
  BufferOffset as_nor(Register rd, Register rj, Register rk);
  BufferOffset as_andn(Register rd, Register rj, Register rk);
  BufferOffset as_orn(Register rd, Register rj, Register rk);

  BufferOffset as_andi(Register rd, Register rj, int32_t ui12);
  BufferOffset as_ori(Register rd, Register rj, int32_t ui12);
  BufferOffset as_xori(Register rd, Register rj, int32_t ui12);

  // Shift instructions
  BufferOffset as_sll_w(Register rd, Register rj, Register rk);
  BufferOffset as_srl_w(Register rd, Register rj, Register rk);
  BufferOffset as_sra_w(Register rd, Register rj, Register rk);
  BufferOffset as_rotr_w(Register rd, Register rj, Register rk);

  BufferOffset as_slli_w(Register rd, Register rj, int32_t ui5);
  BufferOffset as_srli_w(Register rd, Register rj, int32_t ui5);
  BufferOffset as_srai_w(Register rd, Register rj, int32_t ui5);
  BufferOffset as_rotri_w(Register rd, Register rj, int32_t ui5);

  BufferOffset as_sll_d(Register rd, Register rj, Register rk);
  BufferOffset as_srl_d(Register rd, Register rj, Register rk);
  BufferOffset as_sra_d(Register rd, Register rj, Register rk);
  BufferOffset as_rotr_d(Register rd, Register rj, Register rk);

  BufferOffset as_slli_d(Register rd, Register rj, int32_t ui6);
  BufferOffset as_srli_d(Register rd, Register rj, int32_t ui6);
  BufferOffset as_srai_d(Register rd, Register rj, int32_t ui6);
  BufferOffset as_rotri_d(Register rd, Register rj, int32_t ui6);

  // Bit operation instrucitons
  BufferOffset as_ext_w_b(Register rd, Register rj);
  BufferOffset as_ext_w_h(Register rd, Register rj);

  BufferOffset as_clo_w(Register rd, Register rj);
  BufferOffset as_clz_w(Register rd, Register rj);
  BufferOffset as_cto_w(Register rd, Register rj);
  BufferOffset as_ctz_w(Register rd, Register rj);
  BufferOffset as_clo_d(Register rd, Register rj);
  BufferOffset as_clz_d(Register rd, Register rj);
  BufferOffset as_cto_d(Register rd, Register rj);
  BufferOffset as_ctz_d(Register rd, Register rj);

  BufferOffset as_bytepick_w(Register rd, Register rj, Register rk,
                             int32_t sa2);
  BufferOffset as_bytepick_d(Register rd, Register rj, Register rk,
                             int32_t sa3);

  BufferOffset as_revb_2h(Register rd, Register rj);
  BufferOffset as_revb_4h(Register rd, Register rj);
  BufferOffset as_revb_2w(Register rd, Register rj);
  BufferOffset as_revb_d(Register rd, Register rj);

  BufferOffset as_revh_2w(Register rd, Register rj);
  BufferOffset as_revh_d(Register rd, Register rj);

  BufferOffset as_bitrev_4b(Register rd, Register rj);
  BufferOffset as_bitrev_8b(Register rd, Register rj);

  BufferOffset as_bitrev_w(Register rd, Register rj);
  BufferOffset as_bitrev_d(Register rd, Register rj);

  BufferOffset as_bstrins_w(Register rd, Register rj, int32_t msbw,
                            int32_t lsbw);
  BufferOffset as_bstrins_d(Register rd, Register rj, int32_t msbd,
                            int32_t lsbd);
  BufferOffset as_bstrpick_w(Register rd, Register rj, int32_t msbw,
                             int32_t lsbw);
  BufferOffset as_bstrpick_d(Register rd, Register rj, int32_t msbd,
                             int32_t lsbd);

  BufferOffset as_maskeqz(Register rd, Register rj, Register rk);
  BufferOffset as_masknez(Register rd, Register rj, Register rk);

  // Load and store instructions
  BufferOffset as_ld_b(Register rd, Register rj, int32_t si12);
  BufferOffset as_ld_h(Register rd, Register rj, int32_t si12);
  BufferOffset as_ld_w(Register rd, Register rj, int32_t si12);
  BufferOffset as_ld_d(Register rd, Register rj, int32_t si12);
  BufferOffset as_ld_bu(Register rd, Register rj, int32_t si12);
  BufferOffset as_ld_hu(Register rd, Register rj, int32_t si12);
  BufferOffset as_ld_wu(Register rd, Register rj, int32_t si12);
  BufferOffset as_st_b(Register rd, Register rj, int32_t si12);
  BufferOffset as_st_h(Register rd, Register rj, int32_t si12);
  BufferOffset as_st_w(Register rd, Register rj, int32_t si12);
  BufferOffset as_st_d(Register rd, Register rj, int32_t si12);

  BufferOffset as_ldx_b(Register rd, Register rj, Register rk);
  BufferOffset as_ldx_h(Register rd, Register rj, Register rk);
  BufferOffset as_ldx_w(Register rd, Register rj, Register rk);
  BufferOffset as_ldx_d(Register rd, Register rj, Register rk);
  BufferOffset as_ldx_bu(Register rd, Register rj, Register rk);
  BufferOffset as_ldx_hu(Register rd, Register rj, Register rk);
  BufferOffset as_ldx_wu(Register rd, Register rj, Register rk);
  BufferOffset as_stx_b(Register rd, Register rj, Register rk);
  BufferOffset as_stx_h(Register rd, Register rj, Register rk);
  BufferOffset as_stx_w(Register rd, Register rj, Register rk);
  BufferOffset as_stx_d(Register rd, Register rj, Register rk);

  BufferOffset as_ldptr_w(Register rd, Register rj, int32_t si14);
  BufferOffset as_ldptr_d(Register rd, Register rj, int32_t si14);
  BufferOffset as_stptr_w(Register rd, Register rj, int32_t si14);
  BufferOffset as_stptr_d(Register rd, Register rj, int32_t si14);

  BufferOffset as_preld(int32_t hint, Register rj, int32_t si12);

  // Atomic instructions
  BufferOffset as_amswap_w(Register rd, Register rj, Register rk);
  BufferOffset as_amswap_d(Register rd, Register rj, Register rk);
  BufferOffset as_amadd_w(Register rd, Register rj, Register rk);
  BufferOffset as_amadd_d(Register rd, Register rj, Register rk);
  BufferOffset as_amand_w(Register rd, Register rj, Register rk);
  BufferOffset as_amand_d(Register rd, Register rj, Register rk);
  BufferOffset as_amor_w(Register rd, Register rj, Register rk);
  BufferOffset as_amor_d(Register rd, Register rj, Register rk);
  BufferOffset as_amxor_w(Register rd, Register rj, Register rk);
  BufferOffset as_amxor_d(Register rd, Register rj, Register rk);
  BufferOffset as_ammax_w(Register rd, Register rj, Register rk);
  BufferOffset as_ammax_d(Register rd, Register rj, Register rk);
  BufferOffset as_ammin_w(Register rd, Register rj, Register rk);
  BufferOffset as_ammin_d(Register rd, Register rj, Register rk);
  BufferOffset as_ammax_wu(Register rd, Register rj, Register rk);
  BufferOffset as_ammax_du(Register rd, Register rj, Register rk);
  BufferOffset as_ammin_wu(Register rd, Register rj, Register rk);
  BufferOffset as_ammin_du(Register rd, Register rj, Register rk);

  BufferOffset as_amswap_db_w(Register rd, Register rj, Register rk);
  BufferOffset as_amswap_db_d(Register rd, Register rj, Register rk);
  BufferOffset as_amadd_db_w(Register rd, Register rj, Register rk);
  BufferOffset as_amadd_db_d(Register rd, Register rj, Register rk);
  BufferOffset as_amand_db_w(Register rd, Register rj, Register rk);
  BufferOffset as_amand_db_d(Register rd, Register rj, Register rk);
  BufferOffset as_amor_db_w(Register rd, Register rj, Register rk);
  BufferOffset as_amor_db_d(Register rd, Register rj, Register rk);
  BufferOffset as_amxor_db_w(Register rd, Register rj, Register rk);
  BufferOffset as_amxor_db_d(Register rd, Register rj, Register rk);
  BufferOffset as_ammax_db_w(Register rd, Register rj, Register rk);
  BufferOffset as_ammax_db_d(Register rd, Register rj, Register rk);
  BufferOffset as_ammin_db_w(Register rd, Register rj, Register rk);
  BufferOffset as_ammin_db_d(Register rd, Register rj, Register rk);
  BufferOffset as_ammax_db_wu(Register rd, Register rj, Register rk);
  BufferOffset as_ammax_db_du(Register rd, Register rj, Register rk);
  BufferOffset as_ammin_db_wu(Register rd, Register rj, Register rk);
  BufferOffset as_ammin_db_du(Register rd, Register rj, Register rk);

  BufferOffset as_ll_w(Register rd, Register rj, int32_t si14);
  BufferOffset as_ll_d(Register rd, Register rj, int32_t si14);
  BufferOffset as_sc_w(Register rd, Register rj, int32_t si14);
  BufferOffset as_sc_d(Register rd, Register rj, int32_t si14);

  // Barrier instructions
  BufferOffset as_dbar(int32_t hint);
  BufferOffset as_ibar(int32_t hint);

  // FP Arithmetic instructions
  BufferOffset as_fadd_s(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fadd_d(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fsub_s(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fsub_d(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fmul_s(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fmul_d(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fdiv_s(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fdiv_d(FloatRegister fd, FloatRegister fj, FloatRegister fk);

  BufferOffset as_fmadd_s(FloatRegister fd, FloatRegister fj, FloatRegister fk,
                          FloatRegister fa);
  BufferOffset as_fmadd_d(FloatRegister fd, FloatRegister fj, FloatRegister fk,
                          FloatRegister fa);
  BufferOffset as_fmsub_s(FloatRegister fd, FloatRegister fj, FloatRegister fk,
                          FloatRegister fa);
  BufferOffset as_fmsub_d(FloatRegister fd, FloatRegister fj, FloatRegister fk,
                          FloatRegister fa);
  BufferOffset as_fnmadd_s(FloatRegister fd, FloatRegister fj, FloatRegister fk,
                           FloatRegister fa);
  BufferOffset as_fnmadd_d(FloatRegister fd, FloatRegister fj, FloatRegister fk,
                           FloatRegister fa);
  BufferOffset as_fnmsub_s(FloatRegister fd, FloatRegister fj, FloatRegister fk,
                           FloatRegister fa);
  BufferOffset as_fnmsub_d(FloatRegister fd, FloatRegister fj, FloatRegister fk,
                           FloatRegister fa);

  BufferOffset as_fmax_s(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fmax_d(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fmin_s(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fmin_d(FloatRegister fd, FloatRegister fj, FloatRegister fk);

  BufferOffset as_fmaxa_s(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fmaxa_d(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fmina_s(FloatRegister fd, FloatRegister fj, FloatRegister fk);
  BufferOffset as_fmina_d(FloatRegister fd, FloatRegister fj, FloatRegister fk);

  BufferOffset as_fabs_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_fabs_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_fneg_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_fneg_d(FloatRegister fd, FloatRegister fj);

  BufferOffset as_fsqrt_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_fsqrt_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_fcopysign_s(FloatRegister fd, FloatRegister fj,
                              FloatRegister fk);
  BufferOffset as_fcopysign_d(FloatRegister fd, FloatRegister fj,
                              FloatRegister fk);

  // FP compare instructions (fcmp.cond.s fcmp.cond.d)
  BufferOffset as_fcmp_cor(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                           FPConditionBit cd);
  BufferOffset as_fcmp_ceq(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                           FPConditionBit cd);
  BufferOffset as_fcmp_cne(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                           FPConditionBit cd);
  BufferOffset as_fcmp_cle(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                           FPConditionBit cd);
  BufferOffset as_fcmp_clt(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                           FPConditionBit cd);
  BufferOffset as_fcmp_cun(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                           FPConditionBit cd);
  BufferOffset as_fcmp_cueq(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                            FPConditionBit cd);
  BufferOffset as_fcmp_cune(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                            FPConditionBit cd);
  BufferOffset as_fcmp_cule(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                            FPConditionBit cd);
  BufferOffset as_fcmp_cult(FloatFormat fmt, FloatRegister fj, FloatRegister fk,
                            FPConditionBit cd);

  // FP conversion instructions
  BufferOffset as_fcvt_s_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_fcvt_d_s(FloatRegister fd, FloatRegister fj);

  BufferOffset as_ffint_s_w(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ffint_s_l(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ffint_d_w(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ffint_d_l(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftint_w_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftint_w_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftint_l_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftint_l_d(FloatRegister fd, FloatRegister fj);

  BufferOffset as_ftintrm_w_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrm_w_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrm_l_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrm_l_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrp_w_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrp_w_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrp_l_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrp_l_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrz_w_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrz_w_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrz_l_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrz_l_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrne_w_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrne_w_d(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrne_l_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_ftintrne_l_d(FloatRegister fd, FloatRegister fj);

  BufferOffset as_frint_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_frint_d(FloatRegister fd, FloatRegister fj);

  // FP mov instructions
  BufferOffset as_fmov_s(FloatRegister fd, FloatRegister fj);
  BufferOffset as_fmov_d(FloatRegister fd, FloatRegister fj);

  BufferOffset as_fsel(FloatRegister fd, FloatRegister fj, FloatRegister fk,
                       FPConditionBit ca);

  BufferOffset as_movgr2fr_w(FloatRegister fd, Register rj);
  BufferOffset as_movgr2fr_d(FloatRegister fd, Register rj);
  BufferOffset as_movgr2frh_w(FloatRegister fd, Register rj);

  BufferOffset as_movfr2gr_s(Register rd, FloatRegister fj);
  BufferOffset as_movfr2gr_d(Register rd, FloatRegister fj);
  BufferOffset as_movfrh2gr_s(Register rd, FloatRegister fj);

  BufferOffset as_movgr2fcsr(Register rj);
  BufferOffset as_movfcsr2gr(Register rd);

  BufferOffset as_movfr2cf(FPConditionBit cd, FloatRegister fj);
  BufferOffset as_movcf2fr(FloatRegister fd, FPConditionBit cj);

  BufferOffset as_movgr2cf(FPConditionBit cd, Register rj);
  BufferOffset as_movcf2gr(Register rd, FPConditionBit cj);

  // FP load/store instructions
  BufferOffset as_fld_s(FloatRegister fd, Register rj, int32_t si12);
  BufferOffset as_fld_d(FloatRegister fd, Register rj, int32_t si12);
  BufferOffset as_fst_s(FloatRegister fd, Register rj, int32_t si12);
  BufferOffset as_fst_d(FloatRegister fd, Register rj, int32_t si12);

  BufferOffset as_fldx_s(FloatRegister fd, Register rj, Register rk);
  BufferOffset as_fldx_d(FloatRegister fd, Register rj, Register rk);
  BufferOffset as_fstx_s(FloatRegister fd, Register rj, Register rk);
  BufferOffset as_fstx_d(FloatRegister fd, Register rj, Register rk);

  // label operations
  void bind(Label* label, BufferOffset boff = BufferOffset());
  virtual void bind(InstImm* inst, uintptr_t branch, uintptr_t target) = 0;
  void bind(CodeLabel* label) { label->target()->bind(currentOffset()); }
  uint32_t currentOffset() { return nextOffset().getOffset(); }
  void retarget(Label* label, Label* target);

  void call(Label* label);
  void call(void* target);

  void as_break(uint32_t code);

 public:
  static bool SupportsFloatingPoint() {
#if defined(__loongarch_hard_float) || defined(JS_SIMULATOR_LOONG64)
    return true;
#else
    return false;
#endif
  }
  static bool SupportsUnalignedAccesses() { return true; }
  static bool SupportsFastUnalignedFPAccesses() { return true; }

  static bool HasRoundInstruction(RoundingMode mode) { return false; }

 protected:
  InstImm invertBranch(InstImm branch, BOffImm16 skipOffset);
  void addPendingJump(BufferOffset src, ImmPtr target, RelocationKind kind) {
    enoughMemory_ &= jumps_.append(RelativePatch(src, target.value, kind));
    if (kind == RelocationKind::JITCODE) {
      jumpRelocations_.writeUnsigned(src.getOffset());
    }
  }

  void addLongJump(BufferOffset src, BufferOffset dst) {
    CodeLabel cl;
    cl.patchAt()->bind(src.getOffset());
    cl.target()->bind(dst.getOffset());
    cl.setLinkMode(CodeLabel::JumpImmediate);
    addCodeLabel(std::move(cl));
  }

 public:
  void flushBuffer() {}

  void comment(const char* msg) { spew("; %s", msg); }

  static uint32_t NopSize() { return 4; }

  static void PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm);

  static uint8_t* NextInstruction(uint8_t* instruction,
                                  uint32_t* count = nullptr);

  static void ToggleToJmp(CodeLocationLabel inst_);
  static void ToggleToCmp(CodeLocationLabel inst_);

  void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                   const Disassembler::HeapAccess& heapAccess) {
    // Implement this if we implement a disassembler.
  }
};  // AssemblerLOONG64

// andi r0, r0, 0
const uint32_t NopInst = 0x03400000;

// An Instruction is a structure for both encoding and decoding any and all
// LoongArch instructions.
class Instruction {
 public:
  uint32_t data;

 protected:
  // Standard constructor
  Instruction(uint32_t data_) : data(data_) {}
  // You should never create an instruction directly.  You should create a
  // more specific instruction which will eventually call one of these
  // constructors for you.

 public:
  uint32_t encode() const { return data; }

  void makeNop() { data = NopInst; }

  void setData(uint32_t data) { this->data = data; }

  const Instruction& operator=(const Instruction& src) {
    data = src.data;
    return *this;
  }

  // Extract the one particular bit.
  uint32_t extractBit(uint32_t bit) { return (encode() >> bit) & 1; }
  // Extract a bit field out of the instruction
  uint32_t extractBitField(uint32_t hi, uint32_t lo) {
    return (encode() >> lo) & ((2 << (hi - lo)) - 1);
  }

  // Get the next instruction in the instruction stream.
  // This does neat things like ignoreconstant pools and their guards.
  Instruction* next();

  // Sometimes, an api wants a uint32_t (or a pointer to it) rather than
  // an instruction.  raw() just coerces this into a pointer to a uint32_t
  const uint32_t* raw() const { return &data; }
  uint32_t size() const { return 4; }
};  // Instruction

// make sure that it is the right size
static_assert(sizeof(Instruction) == 4,
              "Size of Instruction class has to be 4 bytes.");

class InstNOP : public Instruction {
 public:
  InstNOP() : Instruction(NopInst) {}
};

// Class for register type instructions.
class InstReg : public Instruction {
 public:
  InstReg(OpcodeField op, Register rj, Register rd)
      : Instruction(op | RJ(rj) | RD(rd)) {}
  InstReg(OpcodeField op, Register rk, Register rj, Register rd)
      : Instruction(op | RK(rk) | RJ(rj) | RD(rd)) {}
  InstReg(OpcodeField op, uint32_t sa, Register rk, Register rj, Register rd,
          uint32_t sa_bit)
      : Instruction(sa_bit == 2 ? op | SA2(sa) | RK(rk) | RJ(rj) | RD(rd)
                                : op | SA3(sa) | RK(rk) | RJ(rj) | RD(rd)) {
    MOZ_ASSERT(sa_bit == 2 || sa_bit == 3);
  }
  InstReg(OpcodeField op, Register rj, Register rd, bool HasRd)
      : Instruction(HasRd ? op | RJ(rj) | RD(rd) : op | RK(rj) | RJ(rd)) {}

  // For floating-point
  InstReg(OpcodeField op, Register rj, FloatRegister fd)
      : Instruction(op | RJ(rj) | FD(fd)) {}
  InstReg(OpcodeField op, FloatRegister fj, FloatRegister fd)
      : Instruction(op | FJ(fj) | FD(fd)) {}
  InstReg(OpcodeField op, FloatRegister fk, FloatRegister fj, FloatRegister fd)
      : Instruction(op | FK(fk) | FJ(fj) | FD(fd)) {}
  InstReg(OpcodeField op, Register rk, Register rj, FloatRegister fd)
      : Instruction(op | RK(rk) | RJ(rj) | FD(fd)) {}
  InstReg(OpcodeField op, FloatRegister fa, FloatRegister fk, FloatRegister fj,
          FloatRegister fd)
      : Instruction(op | FA(fa) | FK(fk) | FJ(fj) | FD(fd)) {}
  InstReg(OpcodeField op, AssemblerLOONG64::FPConditionBit ca, FloatRegister fk,
          FloatRegister fj, FloatRegister fd)
      : Instruction(op | ca << CAShift | FK(fk) | FJ(fj) | FD(fd)) {
    MOZ_ASSERT(op == op_fsel);
  }
  InstReg(OpcodeField op, FloatRegister fj, Register rd)
      : Instruction(op | FJ(fj) | RD(rd)) {
    MOZ_ASSERT((op == op_movfr2gr_s) || (op == op_movfr2gr_d) ||
               (op == op_movfrh2gr_s));
  }
  InstReg(OpcodeField op, Register rj, uint32_t fd)
      : Instruction(op | RJ(rj) | fd) {
    MOZ_ASSERT(op == op_movgr2fcsr);
  }
  InstReg(OpcodeField op, uint32_t fj, Register rd)
      : Instruction(op | (fj << FJShift) | RD(rd)) {
    MOZ_ASSERT(op == op_movfcsr2gr);
  }
  InstReg(OpcodeField op, FloatRegister fj, AssemblerLOONG64::FPConditionBit cd)
      : Instruction(op | FJ(fj) | cd) {
    MOZ_ASSERT(op == op_movfr2cf);
  }
  InstReg(OpcodeField op, AssemblerLOONG64::FPConditionBit cj, FloatRegister fd)
      : Instruction(op | (cj << CJShift) | FD(fd)) {
    MOZ_ASSERT(op == op_movcf2fr);
  }
  InstReg(OpcodeField op, Register rj, AssemblerLOONG64::FPConditionBit cd)
      : Instruction(op | RJ(rj) | cd) {
    MOZ_ASSERT(op == op_movgr2cf);
  }
  InstReg(OpcodeField op, AssemblerLOONG64::FPConditionBit cj, Register rd)
      : Instruction(op | (cj << CJShift) | RD(rd)) {
    MOZ_ASSERT(op == op_movcf2gr);
  }
  InstReg(OpcodeField op, int32_t cond, FloatRegister fk, FloatRegister fj,
          AssemblerLOONG64::FPConditionBit cd)
      : Instruction(op | (cond & CONDMask) << CONDShift | FK(fk) | FJ(fj) |
                    (cd & RDMask)) {
    MOZ_ASSERT(is_uintN(cond, 5));
  }

  uint32_t extractRK() {
    return extractBitField(RKShift + RKBits - 1, RKShift);
  }
  uint32_t extractRJ() {
    return extractBitField(RJShift + RJBits - 1, RJShift);
  }
  uint32_t extractRD() {
    return extractBitField(RDShift + RDBits - 1, RDShift);
  }
  uint32_t extractSA2() {
    return extractBitField(SAShift + SA2Bits - 1, SAShift);
  }
  uint32_t extractSA3() {
    return extractBitField(SAShift + SA3Bits - 1, SAShift);
  }
};

// Class for branch, load and store instructions with immediate offset.
class InstImm : public Instruction {
 public:
  void extractImm16(BOffImm16* dest);
  uint32_t genImm(int32_t value, uint32_t value_bits) {
    uint32_t imm = value & Imm5Mask;
    if (value_bits == 6) {
      imm = value & Imm6Mask;
    } else if (value_bits == 12) {
      imm = value & Imm12Mask;
    } else if (value_bits == 14) {
      imm = value & Imm14Mask;
    }

    return imm;
  }

  InstImm(OpcodeField op, int32_t value, Register rj, Register rd,
          uint32_t value_bits)
      : Instruction(op | genImm(value, value_bits) << RKShift | RJ(rj) |
                    RD(rd)) {
    MOZ_ASSERT(value_bits == 5 || value_bits == 6 || value_bits == 12 ||
               value_bits == 14);
  }
  InstImm(OpcodeField op, BOffImm16 off, Register rj, Register rd)
      : Instruction(op | (off.encode() & Imm16Mask) << Imm16Shift | RJ(rj) |
                    RD(rd)) {}
  InstImm(OpcodeField op, int32_t si21, Register rj, bool NotHasRd)
      : Instruction(NotHasRd ? op | (si21 & Imm16Mask) << RKShift | RJ(rj) |
                                   (si21 & Imm21Mask) >> 16
                             : op | (si21 & Imm20Mask) << Imm20Shift | RD(rj)) {
    if (NotHasRd) {
      MOZ_ASSERT(op == op_beqz || op == op_bnez);
      MOZ_ASSERT(is_intN(si21, 21));
    } else {
      MOZ_ASSERT(op == op_lu12i_w || op == op_lu32i_d || op == op_pcaddi ||
                 op == op_pcaddu12i || op == op_pcaddu18i ||
                 op == op_pcalau12i);
      // si20
      MOZ_ASSERT(is_intN(si21, 20) || is_uintN(si21, 20));
    }
  }
  InstImm(OpcodeField op, int32_t si21, AssemblerLOONG64::FPConditionBit cj,
          bool isNotEqual)
      : Instruction(isNotEqual
                        ? op | (si21 & Imm16Mask) << RKShift |
                              (cj + 8) << CJShift | (si21 & Imm21Mask) >> 16
                        : op | (si21 & Imm16Mask) << RKShift | cj << CJShift |
                              (si21 & Imm21Mask) >> 16) {
    MOZ_ASSERT(is_intN(si21, 21));
    MOZ_ASSERT(op == op_bcz);
    MOZ_ASSERT(cj >= 0 && cj <= 7);
  }
  InstImm(OpcodeField op, Imm16 off, Register rj, Register rd)
      : Instruction(op | (off.encode() & Imm16Mask) << Imm16Shift | RJ(rj) |
                    RD(rd)) {}
  InstImm(OpcodeField op, int32_t bit15)
      : Instruction(op | (bit15 & Imm15Mask)) {
    MOZ_ASSERT(is_uintN(bit15, 15));
  }

  InstImm(OpcodeField op, int32_t bit26, bool jump)
      : Instruction(op | (bit26 & Imm16Mask) << Imm16Shift |
                    (bit26 & Imm26Mask) >> 16) {
    MOZ_ASSERT(is_intN(bit26, 26));
  }
  InstImm(OpcodeField op, int32_t si12, Register rj, int32_t hint)
      : Instruction(op | (si12 & Imm12Mask) << Imm12Shift | RJ(rj) |
                    (hint & RDMask)) {
    MOZ_ASSERT(op == op_preld);
  }
  InstImm(OpcodeField op, int32_t msb, int32_t lsb, Register rj, Register rd,
          uint32_t sb_bits)
      : Instruction((sb_bits == 5)
                        ? op | (msb & MSBWMask) << MSBWShift |
                              (lsb & LSBWMask) << LSBWShift | RJ(rj) | RD(rd)
                        : op | (msb & MSBDMask) << MSBDShift |
                              (lsb & LSBDMask) << LSBDShift | RJ(rj) | RD(rd)) {
    MOZ_ASSERT(sb_bits == 5 || sb_bits == 6);
    MOZ_ASSERT(op == op_bstr_w || op == op_bstrins_d || op == op_bstrpick_d);
  }
  InstImm(OpcodeField op, int32_t msb, int32_t lsb, Register rj, Register rd)
      : Instruction(op | (msb & MSBWMask) << MSBWShift |
                    ((lsb + 0x20) & LSBDMask) << LSBWShift | RJ(rj) | RD(rd)) {
    MOZ_ASSERT(op == op_bstr_w);
  }

  // For floating-point loads and stores.
  InstImm(OpcodeField op, int32_t si12, Register rj, FloatRegister fd)
      : Instruction(op | (si12 & Imm12Mask) << Imm12Shift | RJ(rj) | FD(fd)) {
    MOZ_ASSERT(is_intN(si12, 12));
  }

  void setOpcode(OpcodeField op, uint32_t opBits) {
    // opBits not greater than 24.
    MOZ_ASSERT(opBits < 25);
    uint32_t OpcodeShift = 32 - opBits;
    uint32_t OpcodeMask = ((1 << opBits) - 1) << OpcodeShift;
    data = (data & ~OpcodeMask) | op;
  }
  uint32_t extractRK() {
    return extractBitField(RKShift + RKBits - 1, RKShift);
  }
  uint32_t extractRJ() {
    return extractBitField(RJShift + RJBits - 1, RJShift);
  }
  void setRJ(uint32_t rj) { data = (data & ~RJMask) | (rj << RJShift); }
  uint32_t extractRD() {
    return extractBitField(RDShift + RDBits - 1, RDShift);
  }
  uint32_t extractImm16Value() {
    return extractBitField(Imm16Shift + Imm16Bits - 1, Imm16Shift);
  }
  void setBOffImm16(BOffImm16 off) {
    // Reset immediate field and replace it
    data = (data & ~BOffImm16Mask) | (off.encode() << Imm16Shift);
  }
  void setImm21(int32_t off) {
    // Reset immediate field and replace it
    uint32_t low16 = (off >> 2) & Imm16Mask;
    int32_t high5 = (off >> 18) & Imm5Mask;
    uint32_t fcc_info = (data >> 5) & 0x1F;
    data = (data & ~BOffImm26Mask) | (low16 << Imm16Shift) | high5 |
           (fcc_info << 5);
  }
};

// Class for Jump type instructions.
class InstJump : public Instruction {
 public:
  InstJump(OpcodeField op, JOffImm26 off)
      : Instruction(op | (off.encode() & Imm16Mask) << Imm16Shift |
                    (off.encode() & Imm26Mask) >> 16) {
    MOZ_ASSERT(op == op_b || op == op_bl);
  }

  void setJOffImm26(JOffImm26 off) {
    // Reset immediate field and replace it
    data = (data & ~BOffImm26Mask) |
           ((off.encode() & Imm16Mask) << Imm16Shift) |
           ((off.encode() >> 16) & 0x3ff);
  }
  uint32_t extractImm26Value() {
    return extractBitField(Imm26Shift + Imm26Bits - 1, Imm26Shift);
  }
};

class ABIArgGenerator {
 public:
  ABIArgGenerator()
      : intRegIndex_(0), floatRegIndex_(0), stackOffset_(0), current_() {}

  ABIArg next(MIRType argType);
  ABIArg& current() { return current_; }
  uint32_t stackBytesConsumedSoFar() const { return stackOffset_; }
  void increaseStackOffset(uint32_t bytes) { stackOffset_ += bytes; }

 protected:
  unsigned intRegIndex_;
  unsigned floatRegIndex_;
  uint32_t stackOffset_;
  ABIArg current_;
};

class Assembler : public AssemblerLOONG64 {
 public:
  Assembler() : AssemblerLOONG64() {}

  static uintptr_t GetPointer(uint8_t*);

  using AssemblerLOONG64::bind;

  static void Bind(uint8_t* rawCode, const CodeLabel& label);

  void processCodeLabels(uint8_t* rawCode);

  static void TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                   CompactBufferReader& reader);
  static void TraceDataRelocations(JSTracer* trc, JitCode* code,
                                   CompactBufferReader& reader);

  void bind(InstImm* inst, uintptr_t branch, uintptr_t target);

  // Copy the assembly code to the given buffer, and perform any pending
  // relocations relying on the target address.
  void executableCopy(uint8_t* buffer);

  static uint32_t PatchWrite_NearCallSize();

  static uint64_t ExtractLoad64Value(Instruction* inst0);
  static void UpdateLoad64Value(Instruction* inst0, uint64_t value);
  static void WriteLoad64Instructions(Instruction* inst0, Register reg,
                                      uint64_t value);

  static void PatchWrite_NearCall(CodeLocationLabel start,
                                  CodeLocationLabel toCall);
  static void PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue,
                                      ImmPtr expectedValue);
  static void PatchDataWithValueCheck(CodeLocationLabel label,
                                      PatchedImmPtr newValue,
                                      PatchedImmPtr expectedValue);

  static uint64_t ExtractInstructionImmediate(uint8_t* code);

  static void ToggleCall(CodeLocationLabel inst_, bool enabled);
};  // Assembler

static const uint32_t NumIntArgRegs = 8;
static const uint32_t NumFloatArgRegs = 8;

static inline bool GetIntArgReg(uint32_t usedIntArgs, Register* out) {
  if (usedIntArgs < NumIntArgRegs) {
    *out = Register::FromCode(a0.code() + usedIntArgs);
    return true;
  }
  return false;
}

static inline bool GetFloatArgReg(uint32_t usedFloatArgs, FloatRegister* out) {
  if (usedFloatArgs < NumFloatArgRegs) {
    *out = FloatRegister::FromCode(f0.code() + usedFloatArgs);
    return true;
  }
  return false;
}

// Get a register in which we plan to put a quantity that will be used as an
// integer argument. This differs from GetIntArgReg in that if we have no more
// actual argument registers to use we will fall back on using whatever
// CallTempReg* don't overlap the argument registers, and only fail once those
// run out too.
static inline bool GetTempRegForIntArg(uint32_t usedIntArgs,
                                       uint32_t usedFloatArgs, Register* out) {
  // NOTE: We can't properly determine which regs are used if there are
  // float arguments. If this is needed, we will have to guess.
  MOZ_ASSERT(usedFloatArgs == 0);

  if (GetIntArgReg(usedIntArgs, out)) {
    return true;
  }
  // Unfortunately, we have to assume things about the point at which
  // GetIntArgReg returns false, because we need to know how many registers it
  // can allocate.
  usedIntArgs -= NumIntArgRegs;
  if (usedIntArgs >= NumCallTempNonArgRegs) {
    return false;
  }
  *out = CallTempNonArgRegs[usedIntArgs];
  return true;
}

}  // namespace jit
}  // namespace js

#endif /* jit_loong64_Assembler_loong64_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_Assembler_mips_shared_h
#define jit_mips_shared_Assembler_mips_shared_h

#include "mozilla/Attributes.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Sprintf.h"

#include "jit/CompactBuffer.h"
#include "jit/JitCode.h"
#include "jit/JitSpewer.h"
#include "jit/mips-shared/Architecture-mips-shared.h"
#include "jit/shared/Assembler-shared.h"
#include "jit/shared/IonAssemblerBuffer.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace jit {

static constexpr Register zero{Registers::zero};
static constexpr Register at{Registers::at};
static constexpr Register v0{Registers::v0};
static constexpr Register v1{Registers::v1};
static constexpr Register a0{Registers::a0};
static constexpr Register a1{Registers::a1};
static constexpr Register a2{Registers::a2};
static constexpr Register a3{Registers::a3};
static constexpr Register a4{Registers::ta0};
static constexpr Register a5{Registers::ta1};
static constexpr Register a6{Registers::ta2};
static constexpr Register a7{Registers::ta3};
static constexpr Register t0{Registers::t0};
static constexpr Register t1{Registers::t1};
static constexpr Register t2{Registers::t2};
static constexpr Register t3{Registers::t3};
static constexpr Register t4{Registers::ta0};
static constexpr Register t5{Registers::ta1};
static constexpr Register t6{Registers::ta2};
static constexpr Register t7{Registers::ta3};
static constexpr Register s0{Registers::s0};
static constexpr Register s1{Registers::s1};
static constexpr Register s2{Registers::s2};
static constexpr Register s3{Registers::s3};
static constexpr Register s4{Registers::s4};
static constexpr Register s5{Registers::s5};
static constexpr Register s6{Registers::s6};
static constexpr Register s7{Registers::s7};
static constexpr Register t8{Registers::t8};
static constexpr Register t9{Registers::t9};
static constexpr Register k0{Registers::k0};
static constexpr Register k1{Registers::k1};
static constexpr Register gp{Registers::gp};
static constexpr Register sp{Registers::sp};
static constexpr Register fp{Registers::fp};
static constexpr Register ra{Registers::ra};

static constexpr Register ScratchRegister = at;
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

// Use arg reg from EnterJIT function as OsrFrameReg.
static constexpr Register OsrFrameReg = a3;
static constexpr Register CallTempReg0 = t0;
static constexpr Register CallTempReg1 = t1;
static constexpr Register CallTempReg2 = t2;
static constexpr Register CallTempReg3 = t3;

static constexpr Register IntArgReg0 = a0;
static constexpr Register IntArgReg1 = a1;
static constexpr Register IntArgReg2 = a2;
static constexpr Register IntArgReg3 = a3;
static constexpr Register IntArgReg4 = a4;
static constexpr Register IntArgReg5 = a5;
static constexpr Register IntArgReg6 = a6;
static constexpr Register IntArgReg7 = a7;
static constexpr Register GlobalReg = s6;  // used by Odin
static constexpr Register HeapReg = s7;    // used by Odin

static constexpr Register PreBarrierReg = a1;

static constexpr Register InvalidReg{Registers::invalid_reg};
static constexpr FloatRegister InvalidFloatReg;

static constexpr Register StackPointer = sp;
static constexpr Register FramePointer = fp;
static constexpr Register ReturnReg = v0;
static constexpr FloatRegister ReturnSimd128Reg = InvalidFloatReg;
static constexpr FloatRegister ScratchSimd128Reg = InvalidFloatReg;

// A bias applied to the GlobalReg to allow the use of instructions with small
// negative immediate offsets which doubles the range of global data that can be
// accessed with a single instruction.
static const int32_t WasmGlobalRegBias = 32768;

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

static constexpr uint32_t CodeAlignment = 8;

/* clang-format off */
// MIPS instruction types
//                +---------------------------------------------------------------+
//                |    6      |    5    |    5    |    5    |    5    |    6      |
//                +---------------------------------------------------------------+
// Register type  |  Opcode   |    Rs   |    Rt   |    Rd   |    Sa   | Function  |
//                +---------------------------------------------------------------+
//                |    6      |    5    |    5    |               16              |
//                +---------------------------------------------------------------+
// Immediate type |  Opcode   |    Rs   |    Rt   |    2's complement constant    |
//                +---------------------------------------------------------------+
//                |    6      |                        26                         |
//                +---------------------------------------------------------------+
// Jump type      |  Opcode   |                    jump_target                    |
//                +---------------------------------------------------------------+
//                31 bit                                                      bit 0
/* clang-format on */

// MIPS instruction encoding constants.
static const uint32_t OpcodeShift = 26;
static const uint32_t OpcodeBits = 6;
static const uint32_t RSShift = 21;
static const uint32_t RSBits = 5;
static const uint32_t RTShift = 16;
static const uint32_t RTBits = 5;
static const uint32_t RDShift = 11;
static const uint32_t RDBits = 5;
static const uint32_t RZShift = 0;
static const uint32_t RZBits = 5;
static const uint32_t SAShift = 6;
static const uint32_t SABits = 5;
static const uint32_t FunctionShift = 0;
static const uint32_t FunctionBits = 6;
static const uint32_t Imm16Shift = 0;
static const uint32_t Imm16Bits = 16;
static const uint32_t Imm26Shift = 0;
static const uint32_t Imm26Bits = 26;
static const uint32_t Imm28Shift = 0;
static const uint32_t Imm28Bits = 28;
static const uint32_t ImmFieldShift = 2;
static const uint32_t FRBits = 5;
static const uint32_t FRShift = 21;
static const uint32_t FSShift = 11;
static const uint32_t FSBits = 5;
static const uint32_t FTShift = 16;
static const uint32_t FTBits = 5;
static const uint32_t FDShift = 6;
static const uint32_t FDBits = 5;
static const uint32_t FCccShift = 8;
static const uint32_t FCccBits = 3;
static const uint32_t FBccShift = 18;
static const uint32_t FBccBits = 3;
static const uint32_t FBtrueShift = 16;
static const uint32_t FBtrueBits = 1;
static const uint32_t FccMask = 0x7;
static const uint32_t FccShift = 2;

// MIPS instruction  field bit masks.
static const uint32_t OpcodeMask = ((1 << OpcodeBits) - 1) << OpcodeShift;
static const uint32_t Imm16Mask = ((1 << Imm16Bits) - 1) << Imm16Shift;
static const uint32_t Imm26Mask = ((1 << Imm26Bits) - 1) << Imm26Shift;
static const uint32_t Imm28Mask = ((1 << Imm28Bits) - 1) << Imm28Shift;
static const uint32_t RSMask = ((1 << RSBits) - 1) << RSShift;
static const uint32_t RTMask = ((1 << RTBits) - 1) << RTShift;
static const uint32_t RDMask = ((1 << RDBits) - 1) << RDShift;
static const uint32_t SAMask = ((1 << SABits) - 1) << SAShift;
static const uint32_t FunctionMask = ((1 << FunctionBits) - 1) << FunctionShift;
static const uint32_t RegMask = Registers::Total - 1;

static const uint32_t BREAK_STACK_UNALIGNED = 1;
static const uint32_t MAX_BREAK_CODE = 1024 - 1;
static const uint32_t WASM_TRAP = 6;  // BRK_OVERFLOW

class Instruction;
class InstReg;
class InstImm;
class InstJump;

uint32_t RS(Register r);
uint32_t RT(Register r);
uint32_t RT(FloatRegister r);
uint32_t RD(Register r);
uint32_t RD(FloatRegister r);
uint32_t RZ(Register r);
uint32_t RZ(FloatRegister r);
uint32_t SA(uint32_t value);
uint32_t SA(FloatRegister r);
uint32_t FS(uint32_t value);

Register toRS(Instruction& i);
Register toRT(Instruction& i);
Register toRD(Instruction& i);
Register toR(Instruction& i);

// MIPS enums for instruction fields
enum OpcodeField {
  op_special = 0 << OpcodeShift,
  op_regimm = 1 << OpcodeShift,

  op_j = 2 << OpcodeShift,
  op_jal = 3 << OpcodeShift,
  op_beq = 4 << OpcodeShift,
  op_bne = 5 << OpcodeShift,
  op_blez = 6 << OpcodeShift,
  op_bgtz = 7 << OpcodeShift,

  op_addi = 8 << OpcodeShift,
  op_addiu = 9 << OpcodeShift,
  op_slti = 10 << OpcodeShift,
  op_sltiu = 11 << OpcodeShift,
  op_andi = 12 << OpcodeShift,
  op_ori = 13 << OpcodeShift,
  op_xori = 14 << OpcodeShift,
  op_lui = 15 << OpcodeShift,

  op_cop1 = 17 << OpcodeShift,
  op_cop1x = 19 << OpcodeShift,

  op_beql = 20 << OpcodeShift,
  op_bnel = 21 << OpcodeShift,
  op_blezl = 22 << OpcodeShift,
  op_bgtzl = 23 << OpcodeShift,

  op_daddi = 24 << OpcodeShift,
  op_daddiu = 25 << OpcodeShift,

  op_ldl = 26 << OpcodeShift,
  op_ldr = 27 << OpcodeShift,

  op_special2 = 28 << OpcodeShift,
  op_special3 = 31 << OpcodeShift,

  op_lb = 32 << OpcodeShift,
  op_lh = 33 << OpcodeShift,
  op_lwl = 34 << OpcodeShift,
  op_lw = 35 << OpcodeShift,
  op_lbu = 36 << OpcodeShift,
  op_lhu = 37 << OpcodeShift,
  op_lwr = 38 << OpcodeShift,
  op_lwu = 39 << OpcodeShift,
  op_sb = 40 << OpcodeShift,
  op_sh = 41 << OpcodeShift,
  op_swl = 42 << OpcodeShift,
  op_sw = 43 << OpcodeShift,
  op_sdl = 44 << OpcodeShift,
  op_sdr = 45 << OpcodeShift,
  op_swr = 46 << OpcodeShift,

  op_ll = 48 << OpcodeShift,
  op_lwc1 = 49 << OpcodeShift,
  op_lwc2 = 50 << OpcodeShift,
  op_lld = 52 << OpcodeShift,
  op_ldc1 = 53 << OpcodeShift,
  op_ldc2 = 54 << OpcodeShift,
  op_ld = 55 << OpcodeShift,

  op_sc = 56 << OpcodeShift,
  op_swc1 = 57 << OpcodeShift,
  op_swc2 = 58 << OpcodeShift,
  op_scd = 60 << OpcodeShift,
  op_sdc1 = 61 << OpcodeShift,
  op_sdc2 = 62 << OpcodeShift,
  op_sd = 63 << OpcodeShift,
};

enum RSField {
  rs_zero = 0 << RSShift,
  // cop1 encoding of RS field.
  rs_mfc1 = 0 << RSShift,
  rs_one = 1 << RSShift,
  rs_dmfc1 = 1 << RSShift,
  rs_cfc1 = 2 << RSShift,
  rs_mfhc1 = 3 << RSShift,
  rs_mtc1 = 4 << RSShift,
  rs_dmtc1 = 5 << RSShift,
  rs_ctc1 = 6 << RSShift,
  rs_mthc1 = 7 << RSShift,
  rs_bc1 = 8 << RSShift,
  rs_f = 0x9 << RSShift,
  rs_t = 0xd << RSShift,
  rs_s_r6 = 20 << RSShift,
  rs_d_r6 = 21 << RSShift,
  rs_s = 16 << RSShift,
  rs_d = 17 << RSShift,
  rs_w = 20 << RSShift,
  rs_l = 21 << RSShift,
  rs_ps = 22 << RSShift
};

enum RTField {
  rt_zero = 0 << RTShift,
  // regimm  encoding of RT field.
  rt_bltz = 0 << RTShift,
  rt_bgez = 1 << RTShift,
  rt_bltzal = 16 << RTShift,
  rt_bgezal = 17 << RTShift
};

enum FunctionField {
  // special encoding of function field.
  ff_sll = 0,
  ff_movci = 1,
  ff_srl = 2,
  ff_sra = 3,
  ff_sllv = 4,
  ff_srlv = 6,
  ff_srav = 7,

  ff_jr = 8,
  ff_jalr = 9,
  ff_movz = 10,
  ff_movn = 11,
  ff_break = 13,
  ff_sync = 15,

  ff_mfhi = 16,
  ff_mflo = 18,

  ff_dsllv = 20,
  ff_dsrlv = 22,
  ff_dsrav = 23,

  ff_mult = 24,
  ff_multu = 25,

  ff_mulu = 25,
  ff_muh = 24,
  ff_muhu = 25,
  ff_dmul = 28,
  ff_dmulu = 29,
  ff_dmuh = 28,
  ff_dmuhu = 29,

  ff_div = 26,
  ff_mod = 26,
  ff_divu = 27,
  ff_modu = 27,
  ff_dmult = 28,
  ff_dmultu = 29,
  ff_ddiv = 30,
  ff_dmod = 30,
  ff_ddivu = 31,
  ff_dmodu = 31,

  ff_add = 32,
  ff_addu = 33,
  ff_sub = 34,
  ff_subu = 35,
  ff_and = 36,
  ff_or = 37,
  ff_xor = 38,
  ff_nor = 39,

  ff_slt = 42,
  ff_sltu = 43,
  ff_dadd = 44,
  ff_daddu = 45,
  ff_dsub = 46,
  ff_dsubu = 47,

  ff_tge = 48,
  ff_tgeu = 49,
  ff_tlt = 50,
  ff_tltu = 51,
  ff_teq = 52,
  ff_seleqz = 53,
  ff_tne = 54,
  ff_selnez = 55,
  ff_dsll = 56,
  ff_dsrl = 58,
  ff_dsra = 59,
  ff_dsll32 = 60,
  ff_dsrl32 = 62,
  ff_dsra32 = 63,

  // special2 encoding of function field.
  ff_madd = 0,
  ff_maddu = 1,
#ifdef MIPSR6
  ff_clz = 16,
  ff_dclz = 18,
  ff_mul = 24,
#else
  ff_mul = 2,
  ff_clz = 32,
  ff_dclz = 36,
#endif
  ff_clo = 33,

  // special3 encoding of function field.
  ff_ext = 0,
  ff_dextm = 1,
  ff_dextu = 2,
  ff_dext = 3,
  ff_ins = 4,
  ff_dinsm = 5,
  ff_dinsu = 6,
  ff_dins = 7,
  ff_bshfl = 32,
  ff_dbshfl = 36,
  ff_sc = 38,
  ff_scd = 39,
  ff_ll = 54,
  ff_lld = 55,

  // cop1 encoding of function field.
  ff_add_fmt = 0,
  ff_sub_fmt = 1,
  ff_mul_fmt = 2,
  ff_div_fmt = 3,
  ff_sqrt_fmt = 4,
  ff_abs_fmt = 5,
  ff_mov_fmt = 6,
  ff_neg_fmt = 7,

  ff_round_l_fmt = 8,
  ff_trunc_l_fmt = 9,
  ff_ceil_l_fmt = 10,
  ff_floor_l_fmt = 11,

  ff_round_w_fmt = 12,
  ff_trunc_w_fmt = 13,
  ff_ceil_w_fmt = 14,
  ff_floor_w_fmt = 15,

  ff_movf_fmt = 17,
  ff_movz_fmt = 18,
  ff_movn_fmt = 19,

  ff_min = 28,
  ff_max = 30,

  ff_cvt_s_fmt = 32,
  ff_cvt_d_fmt = 33,
  ff_cvt_w_fmt = 36,
  ff_cvt_l_fmt = 37,
  ff_cvt_ps_s = 38,

#ifdef MIPSR6
  ff_c_f_fmt = 0,
  ff_c_un_fmt = 1,
  ff_c_eq_fmt = 2,
  ff_c_ueq_fmt = 3,
  ff_c_olt_fmt = 4,
  ff_c_ult_fmt = 5,
  ff_c_ole_fmt = 6,
  ff_c_ule_fmt = 7,
#else
  ff_c_f_fmt = 48,
  ff_c_un_fmt = 49,
  ff_c_eq_fmt = 50,
  ff_c_ueq_fmt = 51,
  ff_c_olt_fmt = 52,
  ff_c_ult_fmt = 53,
  ff_c_ole_fmt = 54,
  ff_c_ule_fmt = 55,
#endif

  ff_madd_s = 32,
  ff_madd_d = 33,

  // Loongson encoding of function field.
  ff_gsxbx = 0,
  ff_gsxhx = 1,
  ff_gsxwx = 2,
  ff_gsxdx = 3,
  ff_gsxwlc1 = 4,
  ff_gsxwrc1 = 5,
  ff_gsxdlc1 = 6,
  ff_gsxdrc1 = 7,
  ff_gsxwxc1 = 6,
  ff_gsxdxc1 = 7,
  ff_gsxq = 0x20,
  ff_gsxqc1 = 0x8020,

  ff_null = 0
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
    return (int32_t(data << 18) >> 16) + 4;
  }

  explicit BOffImm16(int offset) : data((offset - 4) >> 2 & Imm16Mask) {
    MOZ_ASSERT((offset & 0x3) == 0);
    MOZ_ASSERT(IsInRange(offset));
  }
  static bool IsInRange(int offset) {
    if ((offset - 4) < int(unsigned(INT16_MIN) << 2)) {
      return false;
    }
    if ((offset - 4) > (INT16_MAX << 2)) {
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
    return (int32_t(data << 8) >> 6) + 4;
  }

  explicit JOffImm26(int offset) : data((offset - 4) >> 2 & Imm26Mask) {
    MOZ_ASSERT((offset & 0x3) == 0);
    MOZ_ASSERT(IsInRange(offset));
  }
  static bool IsInRange(int offset) {
    if ((offset - 4) < -536870912) {
      return false;
    }
    if ((offset - 4) > 536870908) {
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
  static Imm16 Lower(Imm32 imm) { return Imm16(imm.value & 0xffff); }
  static Imm16 Upper(Imm32 imm) { return Imm16((imm.value >> 16) & 0xffff); }
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

class GSImm13 {
  uint16_t value;

 public:
  GSImm13();
  GSImm13(uint32_t imm) : value(imm & ~0xf) {}
  uint32_t encode(uint32_t shift) { return ((value >> 4) & 0x1ff) << shift; }
  int32_t decodeSigned() { return value; }
  uint32_t decodeUnsigned() { return value; }
  static bool IsInRange(int32_t imm) {
    return imm >= int32_t(uint32_t(-256) << 4) && imm <= (255 << 4);
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

inline Imm32 Imm64::firstHalf() const { return low(); }

inline Imm32 Imm64::secondHalf() const { return hi(); }

static constexpr int32_t SliceSize = 1024;
typedef js::jit::AssemblerBuffer<SliceSize, Instruction> MIPSBuffer;

class MIPSBufferWithExecutableCopy : public MIPSBuffer {
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

class AssemblerMIPSShared : public AssemblerShared {
 public:
  enum Condition {
    Equal,
    NotEqual,
    Above,
    AboveOrEqual,
    Below,
    BelowOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
    LessThan,
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
    // These conditions will only evaluate to true if the comparison is ordered
    // - i.e. neither operand is NaN.
    DoubleOrdered,
    DoubleEqual,
    DoubleNotEqual,
    DoubleGreaterThan,
    DoubleGreaterThanOrEqual,
    DoubleLessThan,
    DoubleLessThanOrEqual,
    // If either operand is NaN, these conditions always evaluate to true.
    DoubleUnordered,
    DoubleEqualOrUnordered,
    DoubleNotEqualOrUnordered,
    DoubleGreaterThanOrUnordered,
    DoubleGreaterThanOrEqualOrUnordered,
    DoubleLessThanOrUnordered,
    DoubleLessThanOrEqualOrUnordered
  };

  enum FPConditionBit { FCC0 = 0, FCC1, FCC2, FCC3, FCC4, FCC5, FCC6, FCC7 };

  enum FPControl {
    FIR = 0,
    UFR,
    UNFR = 4,
    FCCR = 25,
    FEXR,
    FENR = 28,
    FCSR = 31
  };

  enum FCSRBit { CauseI = 12, CauseU, CauseO, CauseZ, CauseV };

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

  MIPSBufferWithExecutableCopy m_buffer;

#ifdef JS_JITSPEW
  Sprinter* printer;
#endif

 public:
  AssemblerMIPSShared()
      : m_buffer(),
#ifdef JS_JITSPEW
        printer(nullptr),
#endif
        isFinished(false) {
  }

  static Condition InvertCondition(Condition cond);
  static DoubleCondition InvertCondition(DoubleCondition cond);

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
  BufferOffset as_nop();

  // Branch and jump instructions
  BufferOffset as_bal(BOffImm16 off);
  BufferOffset as_b(BOffImm16 off);

  InstImm getBranchCode(JumpOrCall jumpOrCall);
  InstImm getBranchCode(Register s, Register t, Condition c);
  InstImm getBranchCode(Register s, Condition c);
  InstImm getBranchCode(FloatTestKind testKind, FPConditionBit fcc);

  BufferOffset as_j(JOffImm26 off);
  BufferOffset as_jal(JOffImm26 off);

  BufferOffset as_jr(Register rs);
  BufferOffset as_jalr(Register rs);

  // Arithmetic instructions
  BufferOffset as_addu(Register rd, Register rs, Register rt);
  BufferOffset as_addiu(Register rd, Register rs, int32_t j);
  BufferOffset as_daddu(Register rd, Register rs, Register rt);
  BufferOffset as_daddiu(Register rd, Register rs, int32_t j);
  BufferOffset as_subu(Register rd, Register rs, Register rt);
  BufferOffset as_dsubu(Register rd, Register rs, Register rt);
  BufferOffset as_mult(Register rs, Register rt);
  BufferOffset as_multu(Register rs, Register rt);
  BufferOffset as_dmult(Register rs, Register rt);
  BufferOffset as_dmultu(Register rs, Register rt);
  BufferOffset as_div(Register rs, Register rt);
  BufferOffset as_divu(Register rs, Register rt);
  BufferOffset as_mul(Register rd, Register rs, Register rt);
  BufferOffset as_madd(Register rs, Register rt);
  BufferOffset as_maddu(Register rs, Register rt);
  BufferOffset as_ddiv(Register rs, Register rt);
  BufferOffset as_ddivu(Register rs, Register rt);

  BufferOffset as_muh(Register rd, Register rs, Register rt);
  BufferOffset as_muhu(Register rd, Register rs, Register rt);
  BufferOffset as_mulu(Register rd, Register rs, Register rt);
  BufferOffset as_dmuh(Register rd, Register rs, Register rt);
  BufferOffset as_dmuhu(Register rd, Register rs, Register rt);
  BufferOffset as_dmul(Register rd, Register rs, Register rt);
  BufferOffset as_dmulu(Register rd, Register rs, Register rt);
  BufferOffset as_div(Register rd, Register rs, Register rt);
  BufferOffset as_divu(Register rd, Register rs, Register rt);
  BufferOffset as_mod(Register rd, Register rs, Register rt);
  BufferOffset as_modu(Register rd, Register rs, Register rt);
  BufferOffset as_ddiv(Register rd, Register rs, Register rt);
  BufferOffset as_ddivu(Register rd, Register rs, Register rt);
  BufferOffset as_dmod(Register rd, Register rs, Register rt);
  BufferOffset as_dmodu(Register rd, Register rs, Register rt);

  // Logical instructions
  BufferOffset as_and(Register rd, Register rs, Register rt);
  BufferOffset as_or(Register rd, Register rs, Register rt);
  BufferOffset as_xor(Register rd, Register rs, Register rt);
  BufferOffset as_nor(Register rd, Register rs, Register rt);

  BufferOffset as_andi(Register rd, Register rs, int32_t j);
  BufferOffset as_ori(Register rd, Register rs, int32_t j);
  BufferOffset as_xori(Register rd, Register rs, int32_t j);
  BufferOffset as_lui(Register rd, int32_t j);

  // Shift instructions
  // as_sll(zero, zero, x) instructions are reserved as nop
  BufferOffset as_sll(Register rd, Register rt, uint16_t sa);
  BufferOffset as_dsll(Register rd, Register rt, uint16_t sa);
  BufferOffset as_dsll32(Register rd, Register rt, uint16_t sa);
  BufferOffset as_sllv(Register rd, Register rt, Register rs);
  BufferOffset as_dsllv(Register rd, Register rt, Register rs);
  BufferOffset as_srl(Register rd, Register rt, uint16_t sa);
  BufferOffset as_dsrl(Register rd, Register rt, uint16_t sa);
  BufferOffset as_dsrl32(Register rd, Register rt, uint16_t sa);
  BufferOffset as_srlv(Register rd, Register rt, Register rs);
  BufferOffset as_dsrlv(Register rd, Register rt, Register rs);
  BufferOffset as_sra(Register rd, Register rt, uint16_t sa);
  BufferOffset as_dsra(Register rd, Register rt, uint16_t sa);
  BufferOffset as_dsra32(Register rd, Register rt, uint16_t sa);
  BufferOffset as_srav(Register rd, Register rt, Register rs);
  BufferOffset as_rotr(Register rd, Register rt, uint16_t sa);
  BufferOffset as_rotrv(Register rd, Register rt, Register rs);
  BufferOffset as_dsrav(Register rd, Register rt, Register rs);
  BufferOffset as_drotr(Register rd, Register rt, uint16_t sa);
  BufferOffset as_drotr32(Register rd, Register rt, uint16_t sa);
  BufferOffset as_drotrv(Register rd, Register rt, Register rs);

  // Load and store instructions
  BufferOffset as_lb(Register rd, Register rs, int16_t off);
  BufferOffset as_lbu(Register rd, Register rs, int16_t off);
  BufferOffset as_lh(Register rd, Register rs, int16_t off);
  BufferOffset as_lhu(Register rd, Register rs, int16_t off);
  BufferOffset as_lw(Register rd, Register rs, int16_t off);
  BufferOffset as_lwu(Register rd, Register rs, int16_t off);
  BufferOffset as_lwl(Register rd, Register rs, int16_t off);
  BufferOffset as_lwr(Register rd, Register rs, int16_t off);
  BufferOffset as_ll(Register rd, Register rs, int16_t off);
  BufferOffset as_lld(Register rd, Register rs, int16_t off);
  BufferOffset as_ld(Register rd, Register rs, int16_t off);
  BufferOffset as_ldl(Register rd, Register rs, int16_t off);
  BufferOffset as_ldr(Register rd, Register rs, int16_t off);
  BufferOffset as_sb(Register rd, Register rs, int16_t off);
  BufferOffset as_sh(Register rd, Register rs, int16_t off);
  BufferOffset as_sw(Register rd, Register rs, int16_t off);
  BufferOffset as_swl(Register rd, Register rs, int16_t off);
  BufferOffset as_swr(Register rd, Register rs, int16_t off);
  BufferOffset as_sc(Register rd, Register rs, int16_t off);
  BufferOffset as_scd(Register rd, Register rs, int16_t off);
  BufferOffset as_sd(Register rd, Register rs, int16_t off);
  BufferOffset as_sdl(Register rd, Register rs, int16_t off);
  BufferOffset as_sdr(Register rd, Register rs, int16_t off);

  // Loongson-specific load and store instructions
  BufferOffset as_gslbx(Register rd, Register rs, Register ri, int16_t off);
  BufferOffset as_gssbx(Register rd, Register rs, Register ri, int16_t off);
  BufferOffset as_gslhx(Register rd, Register rs, Register ri, int16_t off);
  BufferOffset as_gsshx(Register rd, Register rs, Register ri, int16_t off);
  BufferOffset as_gslwx(Register rd, Register rs, Register ri, int16_t off);
  BufferOffset as_gsswx(Register rd, Register rs, Register ri, int16_t off);
  BufferOffset as_gsldx(Register rd, Register rs, Register ri, int16_t off);
  BufferOffset as_gssdx(Register rd, Register rs, Register ri, int16_t off);
  BufferOffset as_gslq(Register rh, Register rl, Register rs, int16_t off);
  BufferOffset as_gssq(Register rh, Register rl, Register rs, int16_t off);

  // Move from HI/LO register.
  BufferOffset as_mfhi(Register rd);
  BufferOffset as_mflo(Register rd);

  // Set on less than.
  BufferOffset as_slt(Register rd, Register rs, Register rt);
  BufferOffset as_sltu(Register rd, Register rs, Register rt);
  BufferOffset as_slti(Register rd, Register rs, int32_t j);
  BufferOffset as_sltiu(Register rd, Register rs, uint32_t j);

  // Conditional move.
  BufferOffset as_movz(Register rd, Register rs, Register rt);
  BufferOffset as_movn(Register rd, Register rs, Register rt);
  BufferOffset as_movt(Register rd, Register rs, uint16_t cc = 0);
  BufferOffset as_movf(Register rd, Register rs, uint16_t cc = 0);
  BufferOffset as_seleqz(Register rd, Register rs, Register rt);
  BufferOffset as_selnez(Register rd, Register rs, Register rt);

  // Bit twiddling.
  BufferOffset as_clz(Register rd, Register rs);
  BufferOffset as_dclz(Register rd, Register rs);
  BufferOffset as_wsbh(Register rd, Register rt);
  BufferOffset as_dsbh(Register rd, Register rt);
  BufferOffset as_dshd(Register rd, Register rt);
  BufferOffset as_ins(Register rt, Register rs, uint16_t pos, uint16_t size);
  BufferOffset as_dins(Register rt, Register rs, uint16_t pos, uint16_t size);
  BufferOffset as_dinsm(Register rt, Register rs, uint16_t pos, uint16_t size);
  BufferOffset as_dinsu(Register rt, Register rs, uint16_t pos, uint16_t size);
  BufferOffset as_ext(Register rt, Register rs, uint16_t pos, uint16_t size);
  BufferOffset as_dext(Register rt, Register rs, uint16_t pos, uint16_t size);
  BufferOffset as_dextm(Register rt, Register rs, uint16_t pos, uint16_t size);
  BufferOffset as_dextu(Register rt, Register rs, uint16_t pos, uint16_t size);

  // Sign extend
  BufferOffset as_seb(Register rd, Register rt);
  BufferOffset as_seh(Register rd, Register rt);

  // FP instructions

  BufferOffset as_ldc1(FloatRegister ft, Register base, int32_t off);
  BufferOffset as_sdc1(FloatRegister ft, Register base, int32_t off);

  BufferOffset as_lwc1(FloatRegister ft, Register base, int32_t off);
  BufferOffset as_swc1(FloatRegister ft, Register base, int32_t off);

  // Loongson-specific FP load and store instructions
  BufferOffset as_gsldl(FloatRegister fd, Register base, int32_t off);
  BufferOffset as_gsldr(FloatRegister fd, Register base, int32_t off);
  BufferOffset as_gssdl(FloatRegister fd, Register base, int32_t off);
  BufferOffset as_gssdr(FloatRegister fd, Register base, int32_t off);
  BufferOffset as_gslsl(FloatRegister fd, Register base, int32_t off);
  BufferOffset as_gslsr(FloatRegister fd, Register base, int32_t off);
  BufferOffset as_gsssl(FloatRegister fd, Register base, int32_t off);
  BufferOffset as_gsssr(FloatRegister fd, Register base, int32_t off);
  BufferOffset as_gslsx(FloatRegister fd, Register rs, Register ri,
                        int16_t off);
  BufferOffset as_gsssx(FloatRegister fd, Register rs, Register ri,
                        int16_t off);
  BufferOffset as_gsldx(FloatRegister fd, Register rs, Register ri,
                        int16_t off);
  BufferOffset as_gssdx(FloatRegister fd, Register rs, Register ri,
                        int16_t off);
  BufferOffset as_gslq(FloatRegister rh, FloatRegister rl, Register rs,
                       int16_t off);
  BufferOffset as_gssq(FloatRegister rh, FloatRegister rl, Register rs,
                       int16_t off);

  BufferOffset as_movs(FloatRegister fd, FloatRegister fs);
  BufferOffset as_movd(FloatRegister fd, FloatRegister fs);

  BufferOffset as_ctc1(Register rt, FPControl fc);
  BufferOffset as_cfc1(Register rt, FPControl fc);

  BufferOffset as_mtc1(Register rt, FloatRegister fs);
  BufferOffset as_mfc1(Register rt, FloatRegister fs);

  BufferOffset as_mthc1(Register rt, FloatRegister fs);
  BufferOffset as_mfhc1(Register rt, FloatRegister fs);
  BufferOffset as_dmtc1(Register rt, FloatRegister fs);
  BufferOffset as_dmfc1(Register rt, FloatRegister fs);

 public:
  // FP convert instructions
  BufferOffset as_ceilws(FloatRegister fd, FloatRegister fs);
  BufferOffset as_floorws(FloatRegister fd, FloatRegister fs);
  BufferOffset as_roundws(FloatRegister fd, FloatRegister fs);
  BufferOffset as_truncws(FloatRegister fd, FloatRegister fs);
  BufferOffset as_truncls(FloatRegister fd, FloatRegister fs);

  BufferOffset as_ceilwd(FloatRegister fd, FloatRegister fs);
  BufferOffset as_floorwd(FloatRegister fd, FloatRegister fs);
  BufferOffset as_roundwd(FloatRegister fd, FloatRegister fs);
  BufferOffset as_truncwd(FloatRegister fd, FloatRegister fs);
  BufferOffset as_truncld(FloatRegister fd, FloatRegister fs);

  BufferOffset as_cvtdl(FloatRegister fd, FloatRegister fs);
  BufferOffset as_cvtds(FloatRegister fd, FloatRegister fs);
  BufferOffset as_cvtdw(FloatRegister fd, FloatRegister fs);
  BufferOffset as_cvtld(FloatRegister fd, FloatRegister fs);
  BufferOffset as_cvtls(FloatRegister fd, FloatRegister fs);
  BufferOffset as_cvtsd(FloatRegister fd, FloatRegister fs);
  BufferOffset as_cvtsl(FloatRegister fd, FloatRegister fs);
  BufferOffset as_cvtsw(FloatRegister fd, FloatRegister fs);
  BufferOffset as_cvtwd(FloatRegister fd, FloatRegister fs);
  BufferOffset as_cvtws(FloatRegister fd, FloatRegister fs);

  // FP arithmetic instructions
  BufferOffset as_adds(FloatRegister fd, FloatRegister fs, FloatRegister ft);
  BufferOffset as_addd(FloatRegister fd, FloatRegister fs, FloatRegister ft);
  BufferOffset as_subs(FloatRegister fd, FloatRegister fs, FloatRegister ft);
  BufferOffset as_subd(FloatRegister fd, FloatRegister fs, FloatRegister ft);

  BufferOffset as_abss(FloatRegister fd, FloatRegister fs);
  BufferOffset as_absd(FloatRegister fd, FloatRegister fs);
  BufferOffset as_negs(FloatRegister fd, FloatRegister fs);
  BufferOffset as_negd(FloatRegister fd, FloatRegister fs);

  BufferOffset as_muls(FloatRegister fd, FloatRegister fs, FloatRegister ft);
  BufferOffset as_muld(FloatRegister fd, FloatRegister fs, FloatRegister ft);
  BufferOffset as_divs(FloatRegister fd, FloatRegister fs, FloatRegister ft);
  BufferOffset as_divd(FloatRegister fd, FloatRegister fs, FloatRegister ft);
  BufferOffset as_sqrts(FloatRegister fd, FloatRegister fs);
  BufferOffset as_sqrtd(FloatRegister fd, FloatRegister fs);

  BufferOffset as_max(FloatFormat fmt, FloatRegister fd, FloatRegister fs,
                      FloatRegister ft);
  BufferOffset as_min(FloatFormat fmt, FloatRegister fd, FloatRegister fs,
                      FloatRegister ft);

  // FP compare instructions
  BufferOffset as_cf(FloatFormat fmt, FloatRegister fs, FloatRegister ft,
                     FPConditionBit fcc = FCC0);
  BufferOffset as_cun(FloatFormat fmt, FloatRegister fs, FloatRegister ft,
                      FPConditionBit fcc = FCC0);
  BufferOffset as_ceq(FloatFormat fmt, FloatRegister fs, FloatRegister ft,
                      FPConditionBit fcc = FCC0);
  BufferOffset as_cueq(FloatFormat fmt, FloatRegister fs, FloatRegister ft,
                       FPConditionBit fcc = FCC0);
  BufferOffset as_colt(FloatFormat fmt, FloatRegister fs, FloatRegister ft,
                       FPConditionBit fcc = FCC0);
  BufferOffset as_cult(FloatFormat fmt, FloatRegister fs, FloatRegister ft,
                       FPConditionBit fcc = FCC0);
  BufferOffset as_cole(FloatFormat fmt, FloatRegister fs, FloatRegister ft,
                       FPConditionBit fcc = FCC0);
  BufferOffset as_cule(FloatFormat fmt, FloatRegister fs, FloatRegister ft,
                       FPConditionBit fcc = FCC0);

  // FP conditional move.
  BufferOffset as_movt(FloatFormat fmt, FloatRegister fd, FloatRegister fs,
                       FPConditionBit fcc = FCC0);
  BufferOffset as_movf(FloatFormat fmt, FloatRegister fd, FloatRegister fs,
                       FPConditionBit fcc = FCC0);
  BufferOffset as_movz(FloatFormat fmt, FloatRegister fd, FloatRegister fs,
                       Register rt);
  BufferOffset as_movn(FloatFormat fmt, FloatRegister fd, FloatRegister fs,
                       Register rt);

  // Conditional trap operations
  BufferOffset as_tge(Register rs, Register rt, uint32_t code = 0);
  BufferOffset as_tgeu(Register rs, Register rt, uint32_t code = 0);
  BufferOffset as_tlt(Register rs, Register rt, uint32_t code = 0);
  BufferOffset as_tltu(Register rs, Register rt, uint32_t code = 0);
  BufferOffset as_teq(Register rs, Register rt, uint32_t code = 0);
  BufferOffset as_tne(Register rs, Register rt, uint32_t code = 0);

  // label operations
  void bind(Label* label, BufferOffset boff = BufferOffset());
  virtual void bind(InstImm* inst, uintptr_t branch, uintptr_t target) = 0;
  void bind(CodeLabel* label) { label->target()->bind(currentOffset()); }
  uint32_t currentOffset() { return nextOffset().getOffset(); }
  void retarget(Label* label, Label* target);

  void call(Label* label);
  void call(void* target);

  void as_break(uint32_t code);
  void as_sync(uint32_t stype = 0);

 public:
  static bool SupportsFloatingPoint() {
#if (defined(__mips_hard_float) && !defined(__mips_single_float)) || \
    defined(JS_SIMULATOR_MIPS32) || defined(JS_SIMULATOR_MIPS64)
    return true;
#else
    return false;
#endif
  }
  static bool SupportsUnalignedAccesses() { return true; }
  static bool SupportsFastUnalignedFPAccesses() { return false; }

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

  static uint32_t AlignDoubleArg(uint32_t offset) {
    return (offset + 1U) & ~1U;
  }

  static uint8_t* NextInstruction(uint8_t* instruction,
                                  uint32_t* count = nullptr);

  static void ToggleToJmp(CodeLocationLabel inst_);
  static void ToggleToCmp(CodeLocationLabel inst_);

  static void UpdateLuiOriValue(Instruction* inst0, Instruction* inst1,
                                uint32_t value);

  void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                   const Disassembler::HeapAccess& heapAccess) {
    // Implement this if we implement a disassembler.
  }
};  // AssemblerMIPSShared

// sll zero, zero, 0
const uint32_t NopInst = 0x00000000;

// An Instruction is a structure for both encoding and decoding any and all
// MIPS instructions.
class Instruction {
 protected:
  uint32_t data;

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
  // Since all MIPS instructions have opcode, the opcode
  // extractor resides in the base class.
  uint32_t extractOpcode() {
    return extractBitField(OpcodeShift + OpcodeBits - 1, OpcodeShift);
  }
  // Return the fields at their original place in the instruction encoding.
  OpcodeField OpcodeFieldRaw() const {
    return static_cast<OpcodeField>(encode() & OpcodeMask);
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
  InstReg(OpcodeField op, Register rd, FunctionField ff)
      : Instruction(op | RD(rd) | ff) {}
  InstReg(OpcodeField op, Register rs, Register rt, FunctionField ff)
      : Instruction(op | RS(rs) | RT(rt) | ff) {}
  InstReg(OpcodeField op, Register rs, Register rt, Register rd,
          FunctionField ff)
      : Instruction(op | RS(rs) | RT(rt) | RD(rd) | ff) {}
  InstReg(OpcodeField op, Register rs, Register rt, Register rd, uint32_t sa,
          FunctionField ff)
      : Instruction(op | RS(rs) | RT(rt) | RD(rd) | SA(sa) | ff) {}
  InstReg(OpcodeField op, RSField rs, Register rt, Register rd, uint32_t sa,
          FunctionField ff)
      : Instruction(op | rs | RT(rt) | RD(rd) | SA(sa) | ff) {}
  InstReg(OpcodeField op, Register rs, RTField rt, Register rd, uint32_t sa,
          FunctionField ff)
      : Instruction(op | RS(rs) | rt | RD(rd) | SA(sa) | ff) {}
  InstReg(OpcodeField op, Register rs, uint32_t cc, Register rd, uint32_t sa,
          FunctionField ff)
      : Instruction(op | RS(rs) | cc | RD(rd) | SA(sa) | ff) {}
  InstReg(OpcodeField op, uint32_t code, FunctionField ff)
      : Instruction(op | code | ff) {}
  // for float point
  InstReg(OpcodeField op, RSField rs, Register rt, uint32_t fs)
      : Instruction(op | rs | RT(rt) | FS(fs)) {}
  InstReg(OpcodeField op, RSField rs, Register rt, FloatRegister rd)
      : Instruction(op | rs | RT(rt) | RD(rd)) {}
  InstReg(OpcodeField op, RSField rs, Register rt, FloatRegister rd,
          uint32_t sa, FunctionField ff)
      : Instruction(op | rs | RT(rt) | RD(rd) | SA(sa) | ff) {}
  InstReg(OpcodeField op, RSField rs, Register rt, FloatRegister fs,
          FloatRegister fd, FunctionField ff)
      : Instruction(op | rs | RT(rt) | RD(fs) | SA(fd) | ff) {}
  InstReg(OpcodeField op, RSField rs, FloatRegister ft, FloatRegister fs,
          FloatRegister fd, FunctionField ff)
      : Instruction(op | rs | RT(ft) | RD(fs) | SA(fd) | ff) {}
  InstReg(OpcodeField op, RSField rs, FloatRegister ft, FloatRegister fd,
          uint32_t sa, FunctionField ff)
      : Instruction(op | rs | RT(ft) | RD(fd) | SA(sa) | ff) {}

  uint32_t extractRS() {
    return extractBitField(RSShift + RSBits - 1, RSShift);
  }
  uint32_t extractRT() {
    return extractBitField(RTShift + RTBits - 1, RTShift);
  }
  uint32_t extractRD() {
    return extractBitField(RDShift + RDBits - 1, RDShift);
  }
  uint32_t extractSA() {
    return extractBitField(SAShift + SABits - 1, SAShift);
  }
  uint32_t extractFunctionField() {
    return extractBitField(FunctionShift + FunctionBits - 1, FunctionShift);
  }
};

// Class for branch, load and store instructions with immediate offset.
class InstImm : public Instruction {
 public:
  void extractImm16(BOffImm16* dest);

  InstImm(OpcodeField op, Register rs, Register rt, BOffImm16 off)
      : Instruction(op | RS(rs) | RT(rt) | off.encode()) {}
  InstImm(OpcodeField op, Register rs, RTField rt, BOffImm16 off)
      : Instruction(op | RS(rs) | rt | off.encode()) {}
  InstImm(OpcodeField op, RSField rs, uint32_t cc, BOffImm16 off)
      : Instruction(op | rs | cc | off.encode()) {}
  InstImm(OpcodeField op, Register rs, Register rt, Imm16 off)
      : Instruction(op | RS(rs) | RT(rt) | off.encode()) {}
  InstImm(uint32_t raw) : Instruction(raw) {}
  // For floating-point loads and stores.
  InstImm(OpcodeField op, Register rs, FloatRegister rt, Imm16 off)
      : Instruction(op | RS(rs) | RT(rt) | off.encode()) {}

  uint32_t extractOpcode() {
    return extractBitField(OpcodeShift + OpcodeBits - 1, OpcodeShift);
  }
  void setOpcode(OpcodeField op) { data = (data & ~OpcodeMask) | op; }
  uint32_t extractRS() {
    return extractBitField(RSShift + RSBits - 1, RSShift);
  }
  uint32_t extractRT() {
    return extractBitField(RTShift + RTBits - 1, RTShift);
  }
  void setRT(RTField rt) { data = (data & ~RTMask) | rt; }
  uint32_t extractImm16Value() {
    return extractBitField(Imm16Shift + Imm16Bits - 1, Imm16Shift);
  }
  void setBOffImm16(BOffImm16 off) {
    // Reset immediate field and replace it
    data = (data & ~Imm16Mask) | off.encode();
  }
  void setImm16(Imm16 off) {
    // Reset immediate field and replace it
    data = (data & ~Imm16Mask) | off.encode();
  }
};

// Class for Jump type instructions.
class InstJump : public Instruction {
 public:
  InstJump(OpcodeField op, JOffImm26 off) : Instruction(op | off.encode()) {}

  uint32_t extractImm26Value() {
    return extractBitField(Imm26Shift + Imm26Bits - 1, Imm26Shift);
  }
};

// Class for Loongson-specific instructions
class InstGS : public Instruction {
 public:
  // For indexed loads and stores.
  InstGS(OpcodeField op, Register rs, Register rt, Register rd, Imm8 off,
         FunctionField ff)
      : Instruction(op | RS(rs) | RT(rt) | RD(rd) | off.encode(3) | ff) {}
  InstGS(OpcodeField op, Register rs, FloatRegister rt, Register rd, Imm8 off,
         FunctionField ff)
      : Instruction(op | RS(rs) | RT(rt) | RD(rd) | off.encode(3) | ff) {}
  // For quad-word loads and stores.
  InstGS(OpcodeField op, Register rs, Register rt, Register rz, GSImm13 off,
         FunctionField ff)
      : Instruction(op | RS(rs) | RT(rt) | RZ(rz) | off.encode(6) | ff) {}
  InstGS(OpcodeField op, Register rs, FloatRegister rt, FloatRegister rz,
         GSImm13 off, FunctionField ff)
      : Instruction(op | RS(rs) | RT(rt) | RZ(rz) | off.encode(6) | ff) {}
  InstGS(uint32_t raw) : Instruction(raw) {}
  // For floating-point unaligned loads and stores.
  InstGS(OpcodeField op, Register rs, FloatRegister rt, Imm8 off,
         FunctionField ff)
      : Instruction(op | RS(rs) | RT(rt) | off.encode(6) | ff) {}
};

inline bool IsUnaligned(const wasm::MemoryAccessDesc& access) {
  if (!access.align()) {
    return false;
  }

#ifdef JS_CODEGEN_MIPS32
  if (access.type() == Scalar::Int64 && access.align() >= 4) {
    return false;
  }
#endif

  return access.align() < access.byteSize();
}

}  // namespace jit
}  // namespace js

#endif /* jit_mips_shared_Assembler_mips_shared_h */

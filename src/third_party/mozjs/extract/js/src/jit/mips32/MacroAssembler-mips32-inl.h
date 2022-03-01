/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_MacroAssembler_mips32_inl_h
#define jit_mips32_MacroAssembler_mips32_inl_h

#include "jit/mips32/MacroAssembler-mips32.h"

#include "vm/BigIntType.h"  // JS::BigInt

#include "jit/mips-shared/MacroAssembler-mips-shared-inl.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style

void MacroAssembler::move64(Register64 src, Register64 dest) {
  move32(src.low, dest.low);
  move32(src.high, dest.high);
}

void MacroAssembler::move64(Imm64 imm, Register64 dest) {
  move32(Imm32(imm.value & 0xFFFFFFFFL), dest.low);
  move32(Imm32((imm.value >> 32) & 0xFFFFFFFFL), dest.high);
}

void MacroAssembler::moveDoubleToGPR64(FloatRegister src, Register64 dest) {
  moveFromDoubleHi(src, dest.high);
  moveFromDoubleLo(src, dest.low);
}

void MacroAssembler::moveGPR64ToDouble(Register64 src, FloatRegister dest) {
  moveToDoubleHi(src.high, dest);
  moveToDoubleLo(src.low, dest);
}

void MacroAssembler::move64To32(Register64 src, Register dest) {
  if (src.low != dest) {
    move32(src.low, dest);
  }
}

void MacroAssembler::move32To64ZeroExtend(Register src, Register64 dest) {
  if (src != dest.low) {
    move32(src, dest.low);
  }
  move32(Imm32(0), dest.high);
}

void MacroAssembler::move8To64SignExtend(Register src, Register64 dest) {
  move8SignExtend(src, dest.low);
  move32To64SignExtend(dest.low, dest);
}

void MacroAssembler::move16To64SignExtend(Register src, Register64 dest) {
  move16SignExtend(src, dest.low);
  move32To64SignExtend(dest.low, dest);
}

void MacroAssembler::move32To64SignExtend(Register src, Register64 dest) {
  if (src != dest.low) {
    move32(src, dest.low);
  }
  ma_sra(dest.high, dest.low, Imm32(31));
}

void MacroAssembler::move32SignExtendToPtr(Register src, Register dest) {
  move32(src, dest);
}

void MacroAssembler::move32ZeroExtendToPtr(Register src, Register dest) {
  move32(src, dest);
}

// ===============================================================
// Load instructions

void MacroAssembler::load32SignExtendToPtr(const Address& src, Register dest) {
  load32(src, dest);
}

// ===============================================================
// Logical instructions

void MacroAssembler::notPtr(Register reg) { ma_not(reg, reg); }

void MacroAssembler::andPtr(Register src, Register dest) { ma_and(dest, src); }

void MacroAssembler::andPtr(Imm32 imm, Register dest) { ma_and(dest, imm); }

void MacroAssembler::and64(Imm64 imm, Register64 dest) {
  if (imm.low().value != int32_t(0xFFFFFFFF)) {
    and32(imm.low(), dest.low);
  }
  if (imm.hi().value != int32_t(0xFFFFFFFF)) {
    and32(imm.hi(), dest.high);
  }
}

void MacroAssembler::and64(Register64 src, Register64 dest) {
  and32(src.low, dest.low);
  and32(src.high, dest.high);
}

void MacroAssembler::or64(Imm64 imm, Register64 dest) {
  if (imm.low().value) {
    or32(imm.low(), dest.low);
  }
  if (imm.hi().value) {
    or32(imm.hi(), dest.high);
  }
}

void MacroAssembler::xor64(Imm64 imm, Register64 dest) {
  if (imm.low().value) {
    xor32(imm.low(), dest.low);
  }
  if (imm.hi().value) {
    xor32(imm.hi(), dest.high);
  }
}

void MacroAssembler::orPtr(Register src, Register dest) { ma_or(dest, src); }

void MacroAssembler::orPtr(Imm32 imm, Register dest) { ma_or(dest, imm); }

void MacroAssembler::or64(Register64 src, Register64 dest) {
  or32(src.low, dest.low);
  or32(src.high, dest.high);
}

void MacroAssembler::xor64(Register64 src, Register64 dest) {
  ma_xor(dest.low, src.low);
  ma_xor(dest.high, src.high);
}

void MacroAssembler::xorPtr(Register src, Register dest) { ma_xor(dest, src); }

void MacroAssembler::xorPtr(Imm32 imm, Register dest) { ma_xor(dest, imm); }

// ===============================================================
// Swap instructions

void MacroAssembler::byteSwap64(Register64 reg) {
  byteSwap32(reg.high);
  byteSwap32(reg.low);

  // swap reg.high and reg.low.
  ma_xor(reg.high, reg.low);
  ma_xor(reg.low, reg.high);
  ma_xor(reg.high, reg.low);
}

// ===============================================================
// Arithmetic functions

void MacroAssembler::addPtr(Register src, Register dest) { ma_addu(dest, src); }

void MacroAssembler::addPtr(Imm32 imm, Register dest) { ma_addu(dest, imm); }

void MacroAssembler::addPtr(ImmWord imm, Register dest) {
  addPtr(Imm32(imm.value), dest);
}

void MacroAssembler::add64(Register64 src, Register64 dest) {
  if (dest.low == src.low) {
    as_sltu(ScratchRegister, src.low, zero);
    as_addu(dest.low, dest.low, src.low);
  } else {
    as_addu(dest.low, dest.low, src.low);
    as_sltu(ScratchRegister, dest.low, src.low);
  }
  as_addu(dest.high, dest.high, src.high);
  as_addu(dest.high, dest.high, ScratchRegister);
}

void MacroAssembler::add64(Imm32 imm, Register64 dest) {
  if (Imm16::IsInSignedRange(imm.value)) {
    as_addiu(dest.low, dest.low, imm.value);
    as_sltiu(ScratchRegister, dest.low, imm.value);
  } else {
    ma_li(ScratchRegister, imm);
    as_addu(dest.low, dest.low, ScratchRegister);
    as_sltu(ScratchRegister, dest.low, ScratchRegister);
  }
  as_addu(dest.high, dest.high, ScratchRegister);
}

void MacroAssembler::add64(Imm64 imm, Register64 dest) {
  add64(imm.low(), dest);
  ma_addu(dest.high, dest.high, imm.hi());
}

CodeOffset MacroAssembler::sub32FromStackPtrWithPatch(Register dest) {
  CodeOffset offset = CodeOffset(currentOffset());
  ma_liPatchable(dest, Imm32(0));
  as_subu(dest, StackPointer, dest);
  return offset;
}

void MacroAssembler::patchSub32FromStackPtr(CodeOffset offset, Imm32 imm) {
  Instruction* lui =
      (Instruction*)m_buffer.getInst(BufferOffset(offset.offset()));
  MOZ_ASSERT(lui->extractOpcode() == ((uint32_t)op_lui >> OpcodeShift));
  MOZ_ASSERT(lui->next()->extractOpcode() == ((uint32_t)op_ori >> OpcodeShift));

  UpdateLuiOriValue(lui, lui->next(), imm.value);
}

void MacroAssembler::subPtr(Register src, Register dest) {
  as_subu(dest, dest, src);
}

void MacroAssembler::subPtr(Imm32 imm, Register dest) {
  ma_subu(dest, dest, imm);
}

void MacroAssembler::sub64(Register64 src, Register64 dest) {
  MOZ_ASSERT(dest.low != src.high);
  MOZ_ASSERT(dest.high != src.low);
  MOZ_ASSERT(dest.high != src.high);

  as_sltu(ScratchRegister, dest.low, src.low);
  as_subu(dest.high, dest.high, ScratchRegister);
  as_subu(dest.low, dest.low, src.low);
  as_subu(dest.high, dest.high, src.high);
}

void MacroAssembler::sub64(Imm64 imm, Register64 dest) {
  if (Imm16::IsInSignedRange(imm.low().value) &&
      Imm16::IsInSignedRange(-imm.value)) {
    as_sltiu(ScratchRegister, dest.low, imm.low().value);
    as_subu(dest.high, dest.high, ScratchRegister);
    as_addiu(dest.low, dest.low, -imm.value);
  } else {
    ma_li(SecondScratchReg, imm.low());
    as_sltu(ScratchRegister, dest.low, SecondScratchReg);
    as_subu(dest.high, dest.high, ScratchRegister);
    as_subu(dest.low, dest.low, SecondScratchReg);
  }
  ma_subu(dest.high, dest.high, imm.hi());
}

void MacroAssembler::mulPtr(Register rhs, Register srcDest) {
  as_mul(srcDest, srcDest, rhs);
}

void MacroAssembler::mul64(Imm64 imm, const Register64& dest) {
  // LOW32  = LOW(LOW(dest) * LOW(imm));
  // HIGH32 = LOW(HIGH(dest) * LOW(imm)) [multiply imm into upper bits]
  //        + LOW(LOW(dest) * HIGH(imm)) [multiply dest into upper bits]
  //        + HIGH(LOW(dest) * LOW(imm)) [carry]

  if (imm.low().value == 5) {
    // Optimized case for Math.random().
    as_sll(ScratchRegister, dest.low, 2);
    as_srl(SecondScratchReg, dest.low, 32 - 2);
    as_addu(dest.low, ScratchRegister, dest.low);
    as_sltu(ScratchRegister, dest.low, ScratchRegister);
    as_addu(ScratchRegister, ScratchRegister, SecondScratchReg);
    as_sll(SecondScratchReg, dest.high, 2);
    as_addu(SecondScratchReg, SecondScratchReg, dest.high);
    as_addu(dest.high, ScratchRegister, SecondScratchReg);
  } else {
    // HIGH32 = LOW(HIGH(dest) * LOW(imm)) [multiply imm into upper bits]
    //        + LOW(LOW(dest) * HIGH(imm)) [multiply dest into upper bits]
    ma_li(ScratchRegister, imm.low());
    as_mult(dest.high, ScratchRegister);
    ma_li(ScratchRegister, imm.hi());
    as_madd(dest.low, ScratchRegister);
    as_mflo(dest.high);
    //        + HIGH(LOW(dest) * LOW(imm)) [carry]
    // LOW32  = LOW(LOW(dest) * LOW(imm));
    ma_li(ScratchRegister, imm.low());
    as_multu(dest.low, ScratchRegister);
    as_mfhi(ScratchRegister);
    as_mflo(dest.low);
    as_addu(dest.high, dest.high, ScratchRegister);
  }
}

void MacroAssembler::mul64(Imm64 imm, const Register64& dest,
                           const Register temp) {
  // LOW32  = LOW(LOW(dest) * LOW(imm));
  // HIGH32 = LOW(HIGH(dest) * LOW(imm)) [multiply imm into upper bits]
  //        + LOW(LOW(dest) * HIGH(imm)) [multiply dest into upper bits]
  //        + HIGH(LOW(dest) * LOW(imm)) [carry]

  MOZ_ASSERT(temp != dest.high && temp != dest.low);

  // HIGH32 = LOW(HIGH(dest) * LOW(imm)) [multiply imm into upper bits]
  //        + LOW(LOW(dest) * HIGH(imm)) [multiply dest into upper bits]
  ma_li(ScratchRegister, imm.low());
  as_mult(dest.high, ScratchRegister);
  ma_li(temp, imm.hi());
  as_madd(dest.low, temp);
  as_mflo(dest.high);
  //        + HIGH(LOW(dest) * LOW(imm)) [carry]
  // LOW32  = LOW(LOW(dest) * LOW(imm));
  as_multu(dest.low, ScratchRegister);
  as_mfhi(ScratchRegister);
  as_mflo(dest.low);
  as_addu(dest.high, dest.high, ScratchRegister);
}

void MacroAssembler::mul64(const Register64& src, const Register64& dest,
                           const Register temp) {
  // LOW32  = LOW(LOW(dest) * LOW(imm));
  // HIGH32 = LOW(HIGH(dest) * LOW(imm)) [multiply imm into upper bits]
  //        + LOW(LOW(dest) * HIGH(imm)) [multiply dest into upper bits]
  //        + HIGH(LOW(dest) * LOW(imm)) [carry]

  MOZ_ASSERT(dest != src);
  MOZ_ASSERT(dest.low != src.high && dest.high != src.low);

  // HIGH32 = LOW(HIGH(dest) * LOW(src)) [multiply src into upper bits]
  //        + LOW(LOW(dest) * HIGH(src)) [multiply dest into upper bits]
  as_mult(dest.high, src.low);
  as_madd(dest.low, src.high);
  as_mflo(dest.high);
  //        + HIGH(LOW(dest) * LOW(src)) [carry]
  // LOW32  = LOW(LOW(dest) * LOW(src));
  as_multu(dest.low, src.low);
  as_mfhi(ScratchRegister);
  as_mflo(dest.low);
  as_addu(dest.high, dest.high, ScratchRegister);
}

void MacroAssembler::neg64(Register64 reg) {
  as_subu(ScratchRegister, zero, reg.low);
  as_sltu(ScratchRegister, reg.low, ScratchRegister);
  as_subu(reg.high, zero, reg.high);
  as_subu(reg.high, reg.high, ScratchRegister);
}

void MacroAssembler::negPtr(Register reg) { as_subu(reg, zero, reg); }

void MacroAssembler::mulBy3(Register src, Register dest) {
  MOZ_ASSERT(src != ScratchRegister);
  as_addu(ScratchRegister, src, src);
  as_addu(dest, ScratchRegister, src);
}

void MacroAssembler::inc64(AbsoluteAddress dest) {
  ma_li(ScratchRegister, Imm32((int32_t)dest.addr));
  as_lw(SecondScratchReg, ScratchRegister, 0);

  as_addiu(SecondScratchReg, SecondScratchReg, 1);
  as_sw(SecondScratchReg, ScratchRegister, 0);

  as_sltiu(SecondScratchReg, SecondScratchReg, 1);
  as_lw(ScratchRegister, ScratchRegister, 4);

  as_addu(SecondScratchReg, ScratchRegister, SecondScratchReg);

  ma_li(ScratchRegister, Imm32((int32_t)dest.addr));
  as_sw(SecondScratchReg, ScratchRegister, 4);
}

// ===============================================================
// Shift functions

void MacroAssembler::lshiftPtr(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 32);
  ma_sll(dest, dest, imm);
}

void MacroAssembler::lshiftPtr(Register src, Register dest) {
  ma_sll(dest, dest, src);
}

void MacroAssembler::lshift64(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  ScratchRegisterScope scratch(*this);

  if (imm.value == 0) {
    return;
  } else if (imm.value < 32) {
    as_sll(dest.high, dest.high, imm.value);
    as_srl(scratch, dest.low, (32 - imm.value) % 32);
    as_or(dest.high, dest.high, scratch);
    as_sll(dest.low, dest.low, imm.value);
  } else {
    as_sll(dest.high, dest.low, imm.value - 32);
    move32(Imm32(0), dest.low);
  }
}

void MacroAssembler::lshift64(Register unmaskedShift, Register64 dest) {
  Label done;
  ScratchRegisterScope shift(*this);

  ma_and(shift, unmaskedShift, Imm32(0x3f));
  ma_b(shift, Imm32(0), &done, Equal);

  mov(dest.low, SecondScratchReg);
  ma_sll(dest.low, dest.low, shift);
  as_nor(shift, zero, shift);
  as_srl(SecondScratchReg, SecondScratchReg, 1);
  ma_srl(SecondScratchReg, SecondScratchReg, shift);
  ma_and(shift, unmaskedShift, Imm32(0x3f));
  ma_sll(dest.high, dest.high, shift);
  as_or(dest.high, dest.high, SecondScratchReg);

  ma_and(SecondScratchReg, shift, Imm32(0x20));
  as_movn(dest.high, dest.low, SecondScratchReg);
  as_movn(dest.low, zero, SecondScratchReg);

  bind(&done);
}

void MacroAssembler::rshiftPtr(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 32);
  ma_srl(dest, dest, imm);
}

void MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 32);
  ma_sra(dest, dest, imm);
}

void MacroAssembler::rshiftPtr(Register src, Register dest) {
  ma_srl(dest, dest, src);
}

void MacroAssembler::rshift64(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  ScratchRegisterScope scratch(*this);

  if (imm.value == 0) {
    return;
  } else if (imm.value < 32) {
    as_srl(dest.low, dest.low, imm.value);
    as_sll(scratch, dest.high, (32 - imm.value) % 32);
    as_or(dest.low, dest.low, scratch);
    as_srl(dest.high, dest.high, imm.value);
  } else if (imm.value == 32) {
    ma_move(dest.low, dest.high);
    move32(Imm32(0), dest.high);
  } else {
    ma_srl(dest.low, dest.high, Imm32(imm.value - 32));
    move32(Imm32(0), dest.high);
  }
}

void MacroAssembler::rshift64(Register unmaskedShift, Register64 dest) {
  Label done;
  ScratchRegisterScope shift(*this);

  ma_and(shift, unmaskedShift, Imm32(0x3f));
  ma_b(shift, Imm32(0), &done, Equal);

  mov(dest.high, SecondScratchReg);
  ma_srl(dest.high, dest.high, shift);
  as_nor(shift, zero, shift);
  as_sll(SecondScratchReg, SecondScratchReg, 1);
  ma_sll(SecondScratchReg, SecondScratchReg, shift);
  ma_and(shift, unmaskedShift, Imm32(0x3f));
  ma_srl(dest.low, dest.low, shift);
  as_or(dest.low, dest.low, SecondScratchReg);
  ma_and(SecondScratchReg, shift, Imm32(0x20));
  as_movn(dest.low, dest.high, SecondScratchReg);
  as_movn(dest.high, zero, SecondScratchReg);

  bind(&done);
}

void MacroAssembler::rshift64Arithmetic(Imm32 imm, Register64 dest) {
  MOZ_ASSERT(0 <= imm.value && imm.value < 64);
  ScratchRegisterScope scratch(*this);

  if (imm.value == 0) {
    return;
  } else if (imm.value < 32) {
    as_srl(dest.low, dest.low, imm.value);
    as_sll(scratch, dest.high, (32 - imm.value) % 32);
    as_or(dest.low, dest.low, scratch);
    as_sra(dest.high, dest.high, imm.value);
  } else if (imm.value == 32) {
    ma_move(dest.low, dest.high);
    as_sra(dest.high, dest.high, 31);
  } else {
    as_sra(dest.low, dest.high, imm.value - 32);
    as_sra(dest.high, dest.high, 31);
  }
}

void MacroAssembler::rshift64Arithmetic(Register unmaskedShift,
                                        Register64 dest) {
  Label done;

  ScratchRegisterScope shift(*this);
  ma_and(shift, unmaskedShift, Imm32(0x3f));
  ma_b(shift, Imm32(0), &done, Equal);

  mov(dest.high, SecondScratchReg);
  ma_sra(dest.high, dest.high, shift);
  as_nor(shift, zero, shift);
  as_sll(SecondScratchReg, SecondScratchReg, 1);
  ma_sll(SecondScratchReg, SecondScratchReg, shift);
  ma_and(shift, unmaskedShift, Imm32(0x3f));
  ma_srl(dest.low, dest.low, shift);
  as_or(dest.low, dest.low, SecondScratchReg);
  ma_and(SecondScratchReg, shift, Imm32(0x20));
  as_sra(shift, dest.high, 31);
  as_movn(dest.low, dest.high, SecondScratchReg);
  as_movn(dest.high, shift, SecondScratchReg);

  bind(&done);
}

// ===============================================================
// Rotation functions

void MacroAssembler::rotateLeft64(Imm32 count, Register64 input,
                                  Register64 dest, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  MOZ_ASSERT(input.low != dest.high && input.high != dest.low);

  int32_t amount = count.value & 0x3f;
  if (amount > 32) {
    rotateRight64(Imm32(64 - amount), input, dest, temp);
  } else {
    ScratchRegisterScope scratch(*this);
    if (amount == 0) {
      ma_move(dest.low, input.low);
      ma_move(dest.high, input.high);
    } else if (amount == 32) {
      ma_move(scratch, input.low);
      ma_move(dest.low, input.high);
      ma_move(dest.high, scratch);
    } else {
      MOZ_ASSERT(0 < amount && amount < 32);
      ma_move(scratch, input.high);
      ma_sll(dest.high, input.high, Imm32(amount));
      ma_srl(SecondScratchReg, input.low, Imm32(32 - amount));
      as_or(dest.high, dest.high, SecondScratchReg);
      ma_sll(dest.low, input.low, Imm32(amount));
      ma_srl(SecondScratchReg, scratch, Imm32(32 - amount));
      as_or(dest.low, dest.low, SecondScratchReg);
    }
  }
}

void MacroAssembler::rotateLeft64(Register shift, Register64 src,
                                  Register64 dest, Register temp) {
  MOZ_ASSERT(temp != src.low && temp != src.high);
  MOZ_ASSERT(shift != src.low && shift != src.high);
  MOZ_ASSERT(temp != InvalidReg);

  ScratchRegisterScope scratch(*this);

  ma_and(scratch, shift, Imm32(0x3f));
  as_nor(SecondScratchReg, zero, scratch);
  ma_sll(temp, src.low, scratch);
  ma_move(scratch, src.low);
  as_srl(dest.low, src.high, 1);
  ma_srl(dest.low, dest.low, SecondScratchReg);
  as_or(dest.low, dest.low, temp);
  ma_move(SecondScratchReg, src.high);
  as_srl(dest.high, scratch, 1);
  ma_and(scratch, shift, Imm32(0x3f));
  ma_sll(temp, SecondScratchReg, scratch);
  as_nor(SecondScratchReg, zero, scratch);
  ma_srl(dest.high, dest.high, SecondScratchReg);
  as_or(dest.high, dest.high, temp);
  ma_and(temp, scratch, Imm32(32));
  as_movn(SecondScratchReg, dest.high, temp);
  as_movn(dest.high, dest.low, temp);
  as_movn(dest.low, SecondScratchReg, temp);
}

void MacroAssembler::rotateRight64(Imm32 count, Register64 input,
                                   Register64 dest, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  MOZ_ASSERT(input.low != dest.high && input.high != dest.low);

  int32_t amount = count.value & 0x3f;
  if (amount > 32) {
    rotateLeft64(Imm32(64 - amount), input, dest, temp);
  } else {
    ScratchRegisterScope scratch(*this);
    if (amount == 0) {
      ma_move(dest.low, input.low);
      ma_move(dest.high, input.high);
    } else if (amount == 32) {
      ma_move(scratch, input.low);
      ma_move(dest.low, input.high);
      ma_move(dest.high, scratch);
    } else {
      MOZ_ASSERT(0 < amount && amount < 32);
      ma_move(scratch, input.high);
      ma_srl(dest.high, input.high, Imm32(amount));
      ma_sll(SecondScratchReg, input.low, Imm32(32 - amount));
      as_or(dest.high, dest.high, SecondScratchReg);
      ma_srl(dest.low, input.low, Imm32(amount));
      ma_sll(SecondScratchReg, scratch, Imm32(32 - amount));
      as_or(dest.low, dest.low, SecondScratchReg);
    }
  }
}

void MacroAssembler::rotateRight64(Register shift, Register64 src,
                                   Register64 dest, Register temp) {
  MOZ_ASSERT(temp != src.low && temp != src.high);
  MOZ_ASSERT(shift != src.low && shift != src.high);
  MOZ_ASSERT(temp != InvalidReg);

  ScratchRegisterScope scratch(*this);

  ma_and(scratch, shift, Imm32(0x3f));
  as_nor(SecondScratchReg, zero, scratch);
  ma_srl(temp, src.low, scratch);
  ma_move(scratch, src.low);
  as_sll(dest.low, src.high, 1);
  ma_sll(dest.low, dest.low, SecondScratchReg);
  as_or(dest.low, dest.low, temp);
  ma_move(SecondScratchReg, src.high);
  as_sll(dest.high, scratch, 1);
  ma_and(scratch, shift, Imm32(0x3f));
  ma_srl(temp, SecondScratchReg, scratch);
  as_nor(SecondScratchReg, zero, scratch);
  ma_sll(dest.high, dest.high, SecondScratchReg);
  as_or(dest.high, dest.high, temp);
  ma_and(temp, scratch, Imm32(32));
  as_movn(SecondScratchReg, dest.high, temp);
  as_movn(dest.high, dest.low, temp);
  as_movn(dest.low, SecondScratchReg, temp);
}

template <typename T1, typename T2>
void MacroAssembler::cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest) {
  ma_cmp_set(dest, lhs, rhs, cond);
}

template <typename T1, typename T2>
void MacroAssembler::cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest) {
  ma_cmp_set(dest, lhs, rhs, cond);
}

// ===============================================================
// Bit counting functions

void MacroAssembler::clz64(Register64 src, Register dest) {
  as_clz(ScratchRegister, src.high);
  as_clz(SecondScratchReg, src.low);
  as_movn(SecondScratchReg, zero, src.high);
  as_addu(dest, ScratchRegister, SecondScratchReg);
}

void MacroAssembler::ctz64(Register64 src, Register dest) {
  as_movz(SecondScratchReg, src.high, src.low);
  as_movn(SecondScratchReg, src.low, src.low);
  ma_ctz(SecondScratchReg, SecondScratchReg);
  ma_li(ScratchRegister, Imm32(0x20));
  as_movn(ScratchRegister, zero, src.low);
  as_addu(dest, SecondScratchReg, ScratchRegister);
}

void MacroAssembler::popcnt64(Register64 src, Register64 dest, Register tmp) {
  MOZ_ASSERT(dest.low != tmp);
  MOZ_ASSERT(dest.high != tmp);
  MOZ_ASSERT(dest.low != dest.high);

  as_srl(tmp, src.low, 1);
  as_srl(SecondScratchReg, src.high, 1);
  ma_li(ScratchRegister, Imm32(0x55555555));
  as_and(tmp, tmp, ScratchRegister);
  as_subu(tmp, src.low, tmp);
  as_and(SecondScratchReg, SecondScratchReg, ScratchRegister);
  as_subu(SecondScratchReg, src.high, SecondScratchReg);
  ma_li(ScratchRegister, Imm32(0x33333333));
  as_and(dest.low, tmp, ScratchRegister);
  as_srl(tmp, tmp, 2);
  as_and(tmp, tmp, ScratchRegister);
  as_addu(tmp, dest.low, tmp);
  as_and(dest.high, SecondScratchReg, ScratchRegister);
  as_srl(SecondScratchReg, SecondScratchReg, 2);
  as_and(SecondScratchReg, SecondScratchReg, ScratchRegister);
  as_addu(SecondScratchReg, dest.high, SecondScratchReg);
  ma_li(ScratchRegister, Imm32(0x0F0F0F0F));
  as_addu(tmp, SecondScratchReg, tmp);
  as_srl(dest.low, tmp, 4);
  as_and(dest.low, dest.low, ScratchRegister);
  as_and(tmp, tmp, ScratchRegister);
  as_addu(dest.low, dest.low, tmp);
  ma_mul(dest.low, dest.low, Imm32(0x01010101));
  as_srl(dest.low, dest.low, 24);
  ma_move(dest.high, zero);
}

// ===============================================================
// Branch functions

void MacroAssembler::branch64(Condition cond, const Address& lhs, Imm64 val,
                              Label* label) {
  MOZ_ASSERT(cond == Assembler::NotEqual,
             "other condition codes not supported");

  branch32(cond, lhs, val.firstHalf(), label);
  branch32(cond, Address(lhs.base, lhs.offset + sizeof(uint32_t)),
           val.secondHalf(), label);
}

void MacroAssembler::branch64(Condition cond, const Address& lhs,
                              const Address& rhs, Register scratch,
                              Label* label) {
  MOZ_ASSERT(cond == Assembler::NotEqual,
             "other condition codes not supported");
  MOZ_ASSERT(lhs.base != scratch);
  MOZ_ASSERT(rhs.base != scratch);

  load32(rhs, scratch);
  branch32(cond, lhs, scratch, label);

  load32(Address(rhs.base, rhs.offset + sizeof(uint32_t)), scratch);
  branch32(cond, Address(lhs.base, lhs.offset + sizeof(uint32_t)), scratch,
           label);
}

void MacroAssembler::branch64(Condition cond, Register64 lhs, Imm64 val,
                              Label* success, Label* fail) {
  if (val.value == 0) {
    switch (cond) {
      case Assembler::Equal:
      case Assembler::BelowOrEqual:
      case Assembler::NotEqual:
      case Assembler::Above:
        as_or(ScratchRegister, lhs.high, lhs.low);
        ma_b(ScratchRegister, ScratchRegister, success,
             (cond == Assembler::Equal || cond == Assembler::BelowOrEqual)
                 ? Assembler::Zero
                 : Assembler::NonZero);
        break;
      case Assembler::LessThan:
      case Assembler::GreaterThanOrEqual:
        ma_b(lhs.high, Imm32(0), success, cond);
        break;
      case Assembler::LessThanOrEqual:
      case Assembler::GreaterThan:
        as_or(SecondScratchReg, lhs.high, lhs.low);
        as_sra(ScratchRegister, lhs.high, 31);
        as_sltu(ScratchRegister, ScratchRegister, SecondScratchReg);
        ma_b(ScratchRegister, ScratchRegister, success,
             (cond == Assembler::LessThanOrEqual) ? Assembler::Zero
                                                  : Assembler::NonZero);
        break;
      case Assembler::Below:
        // This condition is always false. No branch required.
        break;
      case Assembler::AboveOrEqual:
        ma_b(success);
        break;
      default:
        MOZ_CRASH("Condition code not supported");
    }
    return;
  }

  Condition c = ma_cmp64(cond, lhs, val, SecondScratchReg);
  ma_b(SecondScratchReg, SecondScratchReg, success, c);
  if (fail) {
    jump(fail);
  }
}

void MacroAssembler::branch64(Condition cond, Register64 lhs, Register64 rhs,
                              Label* success, Label* fail) {
  Condition c = ma_cmp64(cond, lhs, rhs, SecondScratchReg);
  ma_b(SecondScratchReg, SecondScratchReg, success, c);
  if (fail) {
    jump(fail);
  }
}

void MacroAssembler::branchPrivatePtr(Condition cond, const Address& lhs,
                                      Register rhs, Label* label) {
  branchPtr(cond, lhs, rhs, label);
}

template <class L>
void MacroAssembler::branchTest64(Condition cond, Register64 lhs,
                                  Register64 rhs, Register temp, L label) {
  if (cond == Assembler::Zero || cond == Assembler::NonZero) {
    MOZ_ASSERT(lhs.low == rhs.low);
    MOZ_ASSERT(lhs.high == rhs.high);
    as_or(ScratchRegister, lhs.low, lhs.high);
    ma_b(ScratchRegister, ScratchRegister, label, cond);
  } else if (cond == Assembler::Signed || cond == Assembler::NotSigned) {
    branchTest32(cond, lhs.high, rhs.high, label);
  } else {
    MOZ_CRASH("Unsupported condition");
  }
}

void MacroAssembler::branchTestUndefined(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  branchTestUndefined(cond, value.typeReg(), label);
}

void MacroAssembler::branchTestInt32(Condition cond, const ValueOperand& value,
                                     Label* label) {
  branchTestInt32(cond, value.typeReg(), label);
}

void MacroAssembler::branchTestInt32Truthy(bool b, const ValueOperand& value,
                                           Label* label) {
  ScratchRegisterScope scratch(*this);
  as_and(scratch, value.payloadReg(), value.payloadReg());
  ma_b(scratch, scratch, label, b ? NonZero : Zero);
}

void MacroAssembler::branchTestDouble(Condition cond, Register tag,
                                      Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  Condition actual = (cond == Equal) ? Below : AboveOrEqual;
  ma_b(tag, ImmTag(JSVAL_TAG_CLEAR), label, actual);
}

void MacroAssembler::branchTestDouble(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestDouble(cond, value.typeReg(), label);
}

void MacroAssembler::branchTestNumber(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestNumber(cond, value.typeReg(), label);
}

void MacroAssembler::branchTestBoolean(Condition cond,
                                       const ValueOperand& value,
                                       Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_b(value.typeReg(), ImmType(JSVAL_TYPE_BOOLEAN), label, cond);
}

void MacroAssembler::branchTestBooleanTruthy(bool b, const ValueOperand& value,
                                             Label* label) {
  ma_b(value.payloadReg(), value.payloadReg(), label, b ? NonZero : Zero);
}

void MacroAssembler::branchTestString(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestString(cond, value.typeReg(), label);
}

void MacroAssembler::branchTestStringTruthy(bool b, const ValueOperand& value,
                                            Label* label) {
  Register string = value.payloadReg();
  SecondScratchRegisterScope scratch2(*this);
  ma_lw(scratch2, Address(string, JSString::offsetOfLength()));
  ma_b(scratch2, Imm32(0), label, b ? NotEqual : Equal);
}

void MacroAssembler::branchTestSymbol(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestSymbol(cond, value.typeReg(), label);
}

void MacroAssembler::branchTestBigInt(Condition cond, const BaseIndex& address,
                                      Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  Register tag = extractTag(address, scratch2);
  branchTestBigInt(cond, tag, label);
}

void MacroAssembler::branchTestBigInt(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestBigInt(cond, value.typeReg(), label);
}

void MacroAssembler::branchTestBigIntTruthy(bool b, const ValueOperand& value,
                                            Label* label) {
  Register bi = value.payloadReg();
  SecondScratchRegisterScope scratch2(*this);
  ma_lw(scratch2, Address(bi, BigInt::offsetOfDigitLength()));
  ma_b(scratch2, Imm32(0), label, b ? NotEqual : Equal);
}

void MacroAssembler::branchTestNull(Condition cond, const ValueOperand& value,
                                    Label* label) {
  branchTestNull(cond, value.typeReg(), label);
}

void MacroAssembler::branchTestObject(Condition cond, const ValueOperand& value,
                                      Label* label) {
  branchTestObject(cond, value.typeReg(), label);
}

void MacroAssembler::branchTestPrimitive(Condition cond,
                                         const ValueOperand& value,
                                         Label* label) {
  branchTestPrimitive(cond, value.typeReg(), label);
}

template <class L>
void MacroAssembler::branchTestMagic(Condition cond, const ValueOperand& value,
                                     L label) {
  ma_b(value.typeReg(), ImmTag(JSVAL_TAG_MAGIC), label, cond);
}

void MacroAssembler::branchTestMagic(Condition cond, const Address& valaddr,
                                     JSWhyMagic why, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  Label notMagic;
  if (cond == Assembler::Equal) {
    branchTestMagic(Assembler::NotEqual, valaddr, &notMagic);
  } else {
    branchTestMagic(Assembler::NotEqual, valaddr, label);
  }

  branch32(cond, ToPayload(valaddr), Imm32(why), label);
  bind(&notMagic);
}

void MacroAssembler::branchTruncateDoubleMaybeModUint32(FloatRegister src,
                                                        Register dest,
                                                        Label* fail) {
  as_truncwd(ScratchFloat32Reg, src);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromFloat32(ScratchFloat32Reg, dest);
  ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
  ma_b(ScratchRegister, Imm32(0), fail, Assembler::NotEqual);
}

void MacroAssembler::branchTruncateFloat32MaybeModUint32(FloatRegister src,
                                                         Register dest,
                                                         Label* fail) {
  as_truncws(ScratchFloat32Reg, src);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromFloat32(ScratchFloat32Reg, dest);
  ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
  ma_b(ScratchRegister, Imm32(0), fail, Assembler::NotEqual);
}

//}}} check_macroassembler_style
// ===============================================================

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs, Register rhs,
                                   Register src, Register dest) {
  cmp32Move32(cond, lhs, rhs, src, dest);
}

void MacroAssembler::cmpPtrMovePtr(Condition cond, Register lhs,
                                   const Address& rhs, Register src,
                                   Register dest) {
  cmp32Move32(cond, lhs, rhs, src, dest);
}

void MacroAssemblerMIPSCompat::incrementInt32Value(const Address& addr) {
  asMasm().add32(Imm32(1), ToPayload(addr));
}

void MacroAssemblerMIPSCompat::computeEffectiveAddress(const BaseIndex& address,
                                                       Register dest) {
  computeScaledAddress(address, dest);
  if (address.offset) {
    asMasm().addPtr(Imm32(address.offset), dest);
  }
}

void MacroAssemblerMIPSCompat::retn(Imm32 n) {
  // pc <- [sp]; sp += n
  loadPtr(Address(StackPointer, 0), ra);
  asMasm().addPtr(n, StackPointer);
  as_jr(ra);
  as_nop();
}

}  // namespace jit
}  // namespace js

#endif /* jit_mips32_MacroAssembler_mips32_inl_h */

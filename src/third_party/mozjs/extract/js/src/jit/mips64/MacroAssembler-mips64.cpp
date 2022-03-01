/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips64/MacroAssembler-mips64.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/MacroAssembler.h"
#include "jit/mips64/Simulator-mips64.h"
#include "jit/MoveEmitter.h"
#include "jit/SharedICRegisters.h"
#include "util/Memory.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace jit;

using mozilla::Abs;

static_assert(sizeof(intptr_t) == 8, "Not 32-bit clean.");

void MacroAssemblerMIPS64Compat::convertBoolToInt32(Register src,
                                                    Register dest) {
  // Note that C++ bool is only 1 byte, so zero extend it to clear the
  // higher-order bits.
  ma_and(dest, src, Imm32(0xff));
}

void MacroAssemblerMIPS64Compat::convertInt32ToDouble(Register src,
                                                      FloatRegister dest) {
  as_mtc1(src, dest);
  as_cvtdw(dest, dest);
}

void MacroAssemblerMIPS64Compat::convertInt32ToDouble(const Address& src,
                                                      FloatRegister dest) {
  ma_ls(dest, src);
  as_cvtdw(dest, dest);
}

void MacroAssemblerMIPS64Compat::convertInt32ToDouble(const BaseIndex& src,
                                                      FloatRegister dest) {
  computeScaledAddress(src, ScratchRegister);
  convertInt32ToDouble(Address(ScratchRegister, src.offset), dest);
}

void MacroAssemblerMIPS64Compat::convertUInt32ToDouble(Register src,
                                                       FloatRegister dest) {
  ma_dext(ScratchRegister, src, Imm32(0), Imm32(32));
  asMasm().convertInt64ToDouble(Register64(ScratchRegister), dest);
}

void MacroAssemblerMIPS64Compat::convertUInt64ToDouble(Register src,
                                                       FloatRegister dest) {
  Label positive, done;
  ma_b(src, src, &positive, NotSigned, ShortJump);

  MOZ_ASSERT(src != ScratchRegister);
  MOZ_ASSERT(src != SecondScratchReg);

  ma_and(ScratchRegister, src, Imm32(1));
  ma_dsrl(SecondScratchReg, src, Imm32(1));
  ma_or(ScratchRegister, SecondScratchReg);
  as_dmtc1(ScratchRegister, dest);
  as_cvtdl(dest, dest);
  asMasm().addDouble(dest, dest);
  ma_b(&done, ShortJump);

  bind(&positive);
  as_dmtc1(src, dest);
  as_cvtdl(dest, dest);

  bind(&done);
}

void MacroAssemblerMIPS64Compat::convertUInt32ToFloat32(Register src,
                                                        FloatRegister dest) {
  ma_dext(ScratchRegister, src, Imm32(0), Imm32(32));
  asMasm().convertInt64ToFloat32(Register64(ScratchRegister), dest);
}

void MacroAssemblerMIPS64Compat::convertDoubleToFloat32(FloatRegister src,
                                                        FloatRegister dest) {
  as_cvtsd(dest, src);
}

const int CauseBitPos = int(Assembler::CauseI);
const int CauseBitCount = 1 + int(Assembler::CauseV) - int(Assembler::CauseI);
const int CauseIOrVMask = ((1 << int(Assembler::CauseI)) |
                           (1 << int(Assembler::CauseV))) >>
                          int(Assembler::CauseI);

// Checks whether a double is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void MacroAssemblerMIPS64Compat::convertDoubleToInt32(FloatRegister src,
                                                      Register dest,
                                                      Label* fail,
                                                      bool negativeZeroCheck) {
  if (negativeZeroCheck) {
    moveFromDouble(src, dest);
    ma_drol(dest, dest, Imm32(1));
    ma_b(dest, Imm32(1), fail, Assembler::Equal);
  }

  // Truncate double to int ; if result is inexact or invalid fail.
  as_truncwd(ScratchFloat32Reg, src);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromFloat32(ScratchFloat32Reg, dest);
  ma_ext(ScratchRegister, ScratchRegister, CauseBitPos, CauseBitCount);
  as_andi(ScratchRegister, ScratchRegister,
          CauseIOrVMask);  // masking for Inexact and Invalid flag.
  ma_b(ScratchRegister, Imm32(0), fail, Assembler::NotEqual);
}

void MacroAssemblerMIPS64Compat::convertDoubleToPtr(FloatRegister src,
                                                    Register dest, Label* fail,
                                                    bool negativeZeroCheck) {
  if (negativeZeroCheck) {
    moveFromDouble(src, dest);
    ma_drol(dest, dest, Imm32(1));
    ma_b(dest, Imm32(1), fail, Assembler::Equal);
  }
  as_truncld(ScratchDoubleReg, src);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromDouble(ScratchDoubleReg, dest);
  ma_ext(ScratchRegister, ScratchRegister, CauseBitPos, CauseBitCount);
  as_andi(ScratchRegister, ScratchRegister, CauseIOrVMask);
  ma_b(ScratchRegister, Imm32(0), fail, Assembler::NotEqual);
}

// Checks whether a float32 is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void MacroAssemblerMIPS64Compat::convertFloat32ToInt32(FloatRegister src,
                                                       Register dest,
                                                       Label* fail,
                                                       bool negativeZeroCheck) {
  if (negativeZeroCheck) {
    moveFromFloat32(src, dest);
    ma_b(dest, Imm32(INT32_MIN), fail, Assembler::Equal);
  }

  as_truncws(ScratchFloat32Reg, src);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromFloat32(ScratchFloat32Reg, dest);
  ma_ext(ScratchRegister, ScratchRegister, CauseBitPos, CauseBitCount);
  as_andi(ScratchRegister, ScratchRegister, CauseIOrVMask);
  ma_b(ScratchRegister, Imm32(0), fail, Assembler::NotEqual);
}

void MacroAssemblerMIPS64Compat::convertFloat32ToDouble(FloatRegister src,
                                                        FloatRegister dest) {
  as_cvtds(dest, src);
}

void MacroAssemblerMIPS64Compat::convertInt32ToFloat32(Register src,
                                                       FloatRegister dest) {
  as_mtc1(src, dest);
  as_cvtsw(dest, dest);
}

void MacroAssemblerMIPS64Compat::convertInt32ToFloat32(const Address& src,
                                                       FloatRegister dest) {
  ma_ls(dest, src);
  as_cvtsw(dest, dest);
}

void MacroAssembler::convertIntPtrToDouble(Register src, FloatRegister dest) {
  convertInt64ToDouble(Register64(src), dest);
}

void MacroAssemblerMIPS64Compat::movq(Register rs, Register rd) {
  ma_move(rd, rs);
}

void MacroAssemblerMIPS64::ma_li(Register dest, CodeLabel* label) {
  BufferOffset bo = m_buffer.nextOffset();
  ma_liPatchable(dest, ImmWord(/* placeholder */ 0));
  label->patchAt()->bind(bo.getOffset());
  label->setLinkMode(CodeLabel::MoveImmediate);
}

void MacroAssemblerMIPS64::ma_li(Register dest, ImmWord imm) {
  int64_t value = imm.value;

  if (-1 == (value >> 15) || 0 == (value >> 15)) {
    as_addiu(dest, zero, value);
    return;
  }
  if (0 == (value >> 16)) {
    as_ori(dest, zero, value);
    return;
  }

  if (-1 == (value >> 31) || 0 == (value >> 31)) {
    as_lui(dest, uint16_t(value >> 16));
  } else if (0 == (value >> 32)) {
    as_lui(dest, uint16_t(value >> 16));
    as_dinsu(dest, zero, 32, 32);
  } else if (-1 == (value >> 47) || 0 == (value >> 47)) {
    as_lui(dest, uint16_t(value >> 32));
    if (uint16_t(value >> 16)) {
      as_ori(dest, dest, uint16_t(value >> 16));
    }
    as_dsll(dest, dest, 16);
  } else if (0 == (value >> 48)) {
    as_lui(dest, uint16_t(value >> 32));
    as_dinsu(dest, zero, 32, 32);
    if (uint16_t(value >> 16)) {
      as_ori(dest, dest, uint16_t(value >> 16));
    }
    as_dsll(dest, dest, 16);
  } else {
    as_lui(dest, uint16_t(value >> 48));
    if (uint16_t(value >> 32)) {
      as_ori(dest, dest, uint16_t(value >> 32));
    }
    if (uint16_t(value >> 16)) {
      as_dsll(dest, dest, 16);
      as_ori(dest, dest, uint16_t(value >> 16));
      as_dsll(dest, dest, 16);
    } else {
      as_dsll32(dest, dest, 32);
    }
  }
  if (uint16_t(value)) {
    as_ori(dest, dest, uint16_t(value));
  }
}

// This method generates lui, dsll and ori instruction block that can be
// modified by UpdateLoad64Value, either during compilation (eg.
// Assembler::bind), or during execution (eg. jit::PatchJump).
void MacroAssemblerMIPS64::ma_liPatchable(Register dest, ImmPtr imm) {
  return ma_liPatchable(dest, ImmWord(uintptr_t(imm.value)));
}

void MacroAssemblerMIPS64::ma_liPatchable(Register dest, ImmWord imm,
                                          LiFlags flags) {
  if (Li64 == flags) {
    m_buffer.ensureSpace(6 * sizeof(uint32_t));
    as_lui(dest, Imm16::Upper(Imm32(imm.value >> 32)).encode());
    as_ori(dest, dest, Imm16::Lower(Imm32(imm.value >> 32)).encode());
    as_dsll(dest, dest, 16);
    as_ori(dest, dest, Imm16::Upper(Imm32(imm.value)).encode());
    as_dsll(dest, dest, 16);
    as_ori(dest, dest, Imm16::Lower(Imm32(imm.value)).encode());
  } else {
    m_buffer.ensureSpace(4 * sizeof(uint32_t));
    as_lui(dest, Imm16::Lower(Imm32(imm.value >> 32)).encode());
    as_ori(dest, dest, Imm16::Upper(Imm32(imm.value)).encode());
    as_drotr32(dest, dest, 48);
    as_ori(dest, dest, Imm16::Lower(Imm32(imm.value)).encode());
  }
}

void MacroAssemblerMIPS64::ma_dnegu(Register rd, Register rs) {
  as_dsubu(rd, zero, rs);
}

// Shifts
void MacroAssemblerMIPS64::ma_dsll(Register rd, Register rt, Imm32 shift) {
  if (31 < shift.value) {
    as_dsll32(rd, rt, shift.value);
  } else {
    as_dsll(rd, rt, shift.value);
  }
}

void MacroAssemblerMIPS64::ma_dsrl(Register rd, Register rt, Imm32 shift) {
  if (31 < shift.value) {
    as_dsrl32(rd, rt, shift.value);
  } else {
    as_dsrl(rd, rt, shift.value);
  }
}

void MacroAssemblerMIPS64::ma_dsra(Register rd, Register rt, Imm32 shift) {
  if (31 < shift.value) {
    as_dsra32(rd, rt, shift.value);
  } else {
    as_dsra(rd, rt, shift.value);
  }
}

void MacroAssemblerMIPS64::ma_dror(Register rd, Register rt, Imm32 shift) {
  if (31 < shift.value) {
    as_drotr32(rd, rt, shift.value);
  } else {
    as_drotr(rd, rt, shift.value);
  }
}

void MacroAssemblerMIPS64::ma_drol(Register rd, Register rt, Imm32 shift) {
  uint32_t s = 64 - shift.value;

  if (31 < s) {
    as_drotr32(rd, rt, s);
  } else {
    as_drotr(rd, rt, s);
  }
}

void MacroAssemblerMIPS64::ma_dsll(Register rd, Register rt, Register shift) {
  as_dsllv(rd, rt, shift);
}

void MacroAssemblerMIPS64::ma_dsrl(Register rd, Register rt, Register shift) {
  as_dsrlv(rd, rt, shift);
}

void MacroAssemblerMIPS64::ma_dsra(Register rd, Register rt, Register shift) {
  as_dsrav(rd, rt, shift);
}

void MacroAssemblerMIPS64::ma_dror(Register rd, Register rt, Register shift) {
  as_drotrv(rd, rt, shift);
}

void MacroAssemblerMIPS64::ma_drol(Register rd, Register rt, Register shift) {
  as_dsubu(ScratchRegister, zero, shift);
  as_drotrv(rd, rt, ScratchRegister);
}

void MacroAssemblerMIPS64::ma_dins(Register rt, Register rs, Imm32 pos,
                                   Imm32 size) {
  if (pos.value >= 0 && pos.value < 32) {
    if (pos.value + size.value > 32) {
      as_dinsm(rt, rs, pos.value, size.value);
    } else {
      as_dins(rt, rs, pos.value, size.value);
    }
  } else {
    as_dinsu(rt, rs, pos.value, size.value);
  }
}

void MacroAssemblerMIPS64::ma_dext(Register rt, Register rs, Imm32 pos,
                                   Imm32 size) {
  if (pos.value >= 0 && pos.value < 32) {
    if (size.value > 32) {
      as_dextm(rt, rs, pos.value, size.value);
    } else {
      as_dext(rt, rs, pos.value, size.value);
    }
  } else {
    as_dextu(rt, rs, pos.value, size.value);
  }
}

void MacroAssemblerMIPS64::ma_dsbh(Register rd, Register rt) {
  as_dsbh(rd, rt);
}

void MacroAssemblerMIPS64::ma_dshd(Register rd, Register rt) {
  as_dshd(rd, rt);
}

void MacroAssemblerMIPS64::ma_dctz(Register rd, Register rs) {
  ma_dnegu(ScratchRegister, rs);
  as_and(rd, ScratchRegister, rs);
  as_dclz(rd, rd);
  ma_dnegu(SecondScratchReg, rd);
  ma_daddu(SecondScratchReg, Imm32(0x3f));
#ifdef MIPS64
  as_selnez(SecondScratchReg, SecondScratchReg, ScratchRegister);
  as_seleqz(rd, rd, ScratchRegister);
  as_or(rd, rd, SecondScratchReg);
#else
  as_movn(rd, SecondScratchReg, ScratchRegister);
#endif
}

// Arithmetic-based ops.

// Add.
void MacroAssemblerMIPS64::ma_daddu(Register rd, Register rs, Imm32 imm) {
  if (Imm16::IsInSignedRange(imm.value)) {
    as_daddiu(rd, rs, imm.value);
  } else {
    ma_li(ScratchRegister, imm);
    as_daddu(rd, rs, ScratchRegister);
  }
}

void MacroAssemblerMIPS64::ma_daddu(Register rd, Register rs) {
  as_daddu(rd, rd, rs);
}

void MacroAssemblerMIPS64::ma_daddu(Register rd, Imm32 imm) {
  ma_daddu(rd, rd, imm);
}

void MacroAssemblerMIPS64::ma_add32TestOverflow(Register rd, Register rs,
                                                Register rt, Label* overflow) {
  as_daddu(SecondScratchReg, rs, rt);
  as_addu(rd, rs, rt);
  ma_b(rd, SecondScratchReg, overflow, Assembler::NotEqual);
}

void MacroAssemblerMIPS64::ma_add32TestOverflow(Register rd, Register rs,
                                                Imm32 imm, Label* overflow) {
  // Check for signed range because of as_daddiu
  if (Imm16::IsInSignedRange(imm.value)) {
    as_daddiu(SecondScratchReg, rs, imm.value);
    as_addiu(rd, rs, imm.value);
    ma_b(rd, SecondScratchReg, overflow, Assembler::NotEqual);
  } else {
    ma_li(ScratchRegister, imm);
    ma_add32TestOverflow(rd, rs, ScratchRegister, overflow);
  }
}

void MacroAssemblerMIPS64::ma_addPtrTestOverflow(Register rd, Register rs,
                                                 Register rt, Label* overflow) {
  SecondScratchRegisterScope scratch2(asMasm());
  MOZ_ASSERT(rd != rt);
  MOZ_ASSERT(rd != scratch2);

  if (rs == rt) {
    as_daddu(rd, rs, rs);
    as_xor(scratch2, rs, rd);
  } else {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(rs != scratch2);
    MOZ_ASSERT(rt != scratch2);

    // If the sign of rs and rt are different, no overflow
    as_xor(scratch2, rs, rt);
    as_nor(scratch2, scratch2, zero);

    as_daddu(rd, rs, rt);
    as_xor(scratch, rd, rt);
    as_and(scratch, scratch, scratch2);
  }

  ma_b(scratch2, zero, overflow, Assembler::LessThan);
}

void MacroAssemblerMIPS64::ma_addPtrTestOverflow(Register rd, Register rs,
                                                 Imm32 imm, Label* overflow) {
  ma_li(ScratchRegister, imm);
  ma_addPtrTestOverflow(rd, rs, ScratchRegister, overflow);
}

void MacroAssemblerMIPS64::ma_addPtrTestCarry(Condition cond, Register rd,
                                              Register rs, Register rt,
                                              Label* overflow) {
  SecondScratchRegisterScope scratch2(asMasm());
  as_daddu(rd, rs, rt);
  as_sltu(scratch2, rd, rt);
  ma_b(scratch2, scratch2, overflow,
       cond == Assembler::CarrySet ? Assembler::NonZero : Assembler::Zero);
}

void MacroAssemblerMIPS64::ma_addPtrTestCarry(Condition cond, Register rd,
                                              Register rs, Imm32 imm,
                                              Label* overflow) {
  // Check for signed range because of as_daddiu
  if (Imm16::IsInSignedRange(imm.value)) {
    SecondScratchRegisterScope scratch2(asMasm());
    as_daddiu(rd, rs, imm.value);
    as_sltiu(scratch2, rd, imm.value);
    ma_b(scratch2, scratch2, overflow,
         cond == Assembler::CarrySet ? Assembler::NonZero : Assembler::Zero);
  } else {
    ma_li(ScratchRegister, imm);
    ma_addPtrTestCarry(cond, rd, rs, ScratchRegister, overflow);
  }
}

// Subtract.
void MacroAssemblerMIPS64::ma_dsubu(Register rd, Register rs, Imm32 imm) {
  if (Imm16::IsInSignedRange(-imm.value)) {
    as_daddiu(rd, rs, -imm.value);
  } else {
    ma_li(ScratchRegister, imm);
    as_dsubu(rd, rs, ScratchRegister);
  }
}

void MacroAssemblerMIPS64::ma_dsubu(Register rd, Register rs) {
  as_dsubu(rd, rd, rs);
}

void MacroAssemblerMIPS64::ma_dsubu(Register rd, Imm32 imm) {
  ma_dsubu(rd, rd, imm);
}

void MacroAssemblerMIPS64::ma_sub32TestOverflow(Register rd, Register rs,
                                                Register rt, Label* overflow) {
  as_dsubu(SecondScratchReg, rs, rt);
  as_subu(rd, rs, rt);
  ma_b(rd, SecondScratchReg, overflow, Assembler::NotEqual);
}

void MacroAssemblerMIPS64::ma_subPtrTestOverflow(Register rd, Register rs,
                                                 Register rt, Label* overflow) {
  SecondScratchRegisterScope scratch2(asMasm());
  MOZ_ASSERT_IF(rs == rd, rs != rt);
  MOZ_ASSERT(rd != rt);
  MOZ_ASSERT(rs != scratch2);
  MOZ_ASSERT(rt != scratch2);
  MOZ_ASSERT(rd != scratch2);

  Register rs_copy = rs;

  if (rs == rd) {
    ma_move(scratch2, rs);
    rs_copy = scratch2;
  }

  {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(rd != scratch);

    as_dsubu(rd, rs, rt);
    // If the sign of rs and rt are the same, no overflow
    as_xor(scratch, rs_copy, rt);
    // Check if the sign of rd and rs are the same
    as_xor(scratch2, rd, rs_copy);
    as_and(scratch2, scratch2, scratch);
  }

  ma_b(scratch2, zero, overflow, Assembler::LessThan);
}

void MacroAssemblerMIPS64::ma_subPtrTestOverflow(Register rd, Register rs,
                                                 Imm32 imm, Label* overflow) {
  ma_li(ScratchRegister, imm);
  ma_subPtrTestOverflow(rd, rs, ScratchRegister, overflow);
}

void MacroAssemblerMIPS64::ma_dmult(Register rs, Imm32 imm) {
  ma_li(ScratchRegister, imm);
#ifdef MIPSR6
  as_dmul(rs, ScratchRegister, SecondScratchReg);
  as_dmuh(rs, ScratchRegister, rs);
  ma_move(rs, SecondScratchReg);
#else
  as_dmult(rs, ScratchRegister);
#endif
}

void MacroAssemblerMIPS64::ma_mulPtrTestOverflow(Register rd, Register rs,
                                                 Register rt, Label* overflow) {
#ifdef MIPSR6
  if (rd == rs) {
    ma_move(SecondScratchReg, rs);
    rs = SecondScratchReg;
  }
  as_dmul(rd, rs, rt);
  as_dmuh(SecondScratchReg, rs, rt);
#else
  as_dmult(rs, rt);
  as_mflo(rd);
  as_mfhi(SecondScratchReg);
#endif
  as_dsra32(ScratchRegister, rd, 63);
  ma_b(ScratchRegister, SecondScratchReg, overflow, Assembler::NotEqual);
}

// Memory.
void MacroAssemblerMIPS64::ma_load(Register dest, Address address,
                                   LoadStoreSize size,
                                   LoadStoreExtension extension) {
  int16_t encodedOffset;
  Register base;

  if (isLoongson() && ZeroExtend != extension &&
      !Imm16::IsInSignedRange(address.offset)) {
    ma_li(ScratchRegister, Imm32(address.offset));
    base = address.base;

    switch (size) {
      case SizeByte:
        as_gslbx(dest, base, ScratchRegister, 0);
        break;
      case SizeHalfWord:
        as_gslhx(dest, base, ScratchRegister, 0);
        break;
      case SizeWord:
        as_gslwx(dest, base, ScratchRegister, 0);
        break;
      case SizeDouble:
        as_gsldx(dest, base, ScratchRegister, 0);
        break;
      default:
        MOZ_CRASH("Invalid argument for ma_load");
    }
    return;
  }

  if (!Imm16::IsInSignedRange(address.offset)) {
    ma_li(ScratchRegister, Imm32(address.offset));
    as_daddu(ScratchRegister, address.base, ScratchRegister);
    base = ScratchRegister;
    encodedOffset = Imm16(0).encode();
  } else {
    encodedOffset = Imm16(address.offset).encode();
    base = address.base;
  }

  switch (size) {
    case SizeByte:
      if (ZeroExtend == extension) {
        as_lbu(dest, base, encodedOffset);
      } else {
        as_lb(dest, base, encodedOffset);
      }
      break;
    case SizeHalfWord:
      if (ZeroExtend == extension) {
        as_lhu(dest, base, encodedOffset);
      } else {
        as_lh(dest, base, encodedOffset);
      }
      break;
    case SizeWord:
      if (ZeroExtend == extension) {
        as_lwu(dest, base, encodedOffset);
      } else {
        as_lw(dest, base, encodedOffset);
      }
      break;
    case SizeDouble:
      as_ld(dest, base, encodedOffset);
      break;
    default:
      MOZ_CRASH("Invalid argument for ma_load");
  }
}

void MacroAssemblerMIPS64::ma_store(Register data, Address address,
                                    LoadStoreSize size,
                                    LoadStoreExtension extension) {
  int16_t encodedOffset;
  Register base;

  if (isLoongson() && !Imm16::IsInSignedRange(address.offset)) {
    ma_li(ScratchRegister, Imm32(address.offset));
    base = address.base;

    switch (size) {
      case SizeByte:
        as_gssbx(data, base, ScratchRegister, 0);
        break;
      case SizeHalfWord:
        as_gsshx(data, base, ScratchRegister, 0);
        break;
      case SizeWord:
        as_gsswx(data, base, ScratchRegister, 0);
        break;
      case SizeDouble:
        as_gssdx(data, base, ScratchRegister, 0);
        break;
      default:
        MOZ_CRASH("Invalid argument for ma_store");
    }
    return;
  }

  if (!Imm16::IsInSignedRange(address.offset)) {
    ma_li(ScratchRegister, Imm32(address.offset));
    as_daddu(ScratchRegister, address.base, ScratchRegister);
    base = ScratchRegister;
    encodedOffset = Imm16(0).encode();
  } else {
    encodedOffset = Imm16(address.offset).encode();
    base = address.base;
  }

  switch (size) {
    case SizeByte:
      as_sb(data, base, encodedOffset);
      break;
    case SizeHalfWord:
      as_sh(data, base, encodedOffset);
      break;
    case SizeWord:
      as_sw(data, base, encodedOffset);
      break;
    case SizeDouble:
      as_sd(data, base, encodedOffset);
      break;
    default:
      MOZ_CRASH("Invalid argument for ma_store");
  }
}

void MacroAssemblerMIPS64Compat::computeScaledAddress(const BaseIndex& address,
                                                      Register dest) {
  int32_t shift = Imm32::ShiftOf(address.scale).value;
  if (shift) {
    ma_dsll(ScratchRegister, address.index, Imm32(shift));
    as_daddu(dest, address.base, ScratchRegister);
  } else {
    as_daddu(dest, address.base, address.index);
  }
}

void MacroAssemblerMIPS64Compat::computeEffectiveAddress(
    const BaseIndex& address, Register dest) {
  computeScaledAddress(address, dest);
  if (address.offset) {
    asMasm().addPtr(Imm32(address.offset), dest);
  }
}

// Shortcut for when we know we're transferring 32 bits of data.
void MacroAssemblerMIPS64::ma_pop(Register r) {
  as_ld(r, StackPointer, 0);
  as_daddiu(StackPointer, StackPointer, sizeof(intptr_t));
}

void MacroAssemblerMIPS64::ma_push(Register r) {
  if (r == sp) {
    // Pushing sp requires one more instruction.
    ma_move(ScratchRegister, sp);
    r = ScratchRegister;
  }

  as_daddiu(StackPointer, StackPointer, (int32_t) - sizeof(intptr_t));
  as_sd(r, StackPointer, 0);
}

// Branches when done from within mips-specific code.
void MacroAssemblerMIPS64::ma_b(Register lhs, ImmWord imm, Label* label,
                                Condition c, JumpKind jumpKind) {
  if (imm.value <= INT32_MAX) {
    ma_b(lhs, Imm32(uint32_t(imm.value)), label, c, jumpKind);
  } else {
    MOZ_ASSERT(lhs != ScratchRegister);
    ma_li(ScratchRegister, imm);
    ma_b(lhs, ScratchRegister, label, c, jumpKind);
  }
}

void MacroAssemblerMIPS64::ma_b(Register lhs, Address addr, Label* label,
                                Condition c, JumpKind jumpKind) {
  MOZ_ASSERT(lhs != ScratchRegister);
  ma_load(ScratchRegister, addr, SizeDouble);
  ma_b(lhs, ScratchRegister, label, c, jumpKind);
}

void MacroAssemblerMIPS64::ma_b(Address addr, Imm32 imm, Label* label,
                                Condition c, JumpKind jumpKind) {
  ma_load(SecondScratchReg, addr, SizeDouble);
  ma_b(SecondScratchReg, imm, label, c, jumpKind);
}

void MacroAssemblerMIPS64::ma_b(Address addr, ImmGCPtr imm, Label* label,
                                Condition c, JumpKind jumpKind) {
  ma_load(SecondScratchReg, addr, SizeDouble);
  ma_b(SecondScratchReg, imm, label, c, jumpKind);
}

void MacroAssemblerMIPS64::ma_bal(Label* label, DelaySlotFill delaySlotFill) {
  spew("branch .Llabel %p\n", label);
  if (label->bound()) {
    // Generate the long jump for calls because return address has to be
    // the address after the reserved block.
    addLongJump(nextOffset(), BufferOffset(label->offset()));
    ma_liPatchable(ScratchRegister, ImmWord(LabelBase::INVALID_OFFSET));
    as_jalr(ScratchRegister);
    if (delaySlotFill == FillDelaySlot) {
      as_nop();
    }
    return;
  }

  // Second word holds a pointer to the next branch in label's chain.
  uint32_t nextInChain =
      label->used() ? label->offset() : LabelBase::INVALID_OFFSET;

  // Make the whole branch continous in the buffer. The '6'
  // instructions are writing at below (contain delay slot).
  m_buffer.ensureSpace(6 * sizeof(uint32_t));

  spew("bal .Llabel %p\n", label);
  BufferOffset bo = writeInst(getBranchCode(BranchIsCall).encode());
  writeInst(nextInChain);
  if (!oom()) {
    label->use(bo.getOffset());
  }
  // Leave space for long jump.
  as_nop();
  as_nop();
  as_nop();
  if (delaySlotFill == FillDelaySlot) {
    as_nop();
  }
}

void MacroAssemblerMIPS64::branchWithCode(InstImm code, Label* label,
                                          JumpKind jumpKind) {
  // simply output the pointer of one label as its id,
  // notice that after one label destructor, the pointer will be reused.
  spew("branch .Llabel %p", label);
  MOZ_ASSERT(code.encode() !=
             InstImm(op_regimm, zero, rt_bgezal, BOffImm16(0)).encode());
  InstImm inst_beq = InstImm(op_beq, zero, zero, BOffImm16(0));

  if (label->bound()) {
    int32_t offset = label->offset() - m_buffer.nextOffset().getOffset();

    if (BOffImm16::IsInRange(offset)) {
      jumpKind = ShortJump;
    }

    if (jumpKind == ShortJump) {
      MOZ_ASSERT(BOffImm16::IsInRange(offset));
      code.setBOffImm16(BOffImm16(offset));
#ifdef JS_JITSPEW
      decodeBranchInstAndSpew(code);
#endif
      writeInst(code.encode());
      as_nop();
      return;
    }

    if (code.encode() == inst_beq.encode()) {
      // Handle long jump
      addLongJump(nextOffset(), BufferOffset(label->offset()));
      ma_liPatchable(ScratchRegister, ImmWord(LabelBase::INVALID_OFFSET));
      as_jr(ScratchRegister);
      as_nop();
      return;
    }

    // Handle long conditional branch, the target offset is based on self,
    // point to next instruction of nop at below.
    spew("invert branch .Llabel %p", label);
    InstImm code_r = invertBranch(code, BOffImm16(7 * sizeof(uint32_t)));
#ifdef JS_JITSPEW
    decodeBranchInstAndSpew(code_r);
#endif
    writeInst(code_r.encode());
    // No need for a "nop" here because we can clobber scratch.
    addLongJump(nextOffset(), BufferOffset(label->offset()));
    ma_liPatchable(ScratchRegister, ImmWord(LabelBase::INVALID_OFFSET));
    as_jr(ScratchRegister);
    as_nop();
    return;
  }

  // Generate open jump and link it to a label.

  // Second word holds a pointer to the next branch in label's chain.
  uint32_t nextInChain =
      label->used() ? label->offset() : LabelBase::INVALID_OFFSET;

  if (jumpKind == ShortJump) {
    // Make the whole branch continous in the buffer.
    m_buffer.ensureSpace(2 * sizeof(uint32_t));

    // Indicate that this is short jump with offset 4.
    code.setBOffImm16(BOffImm16(4));
#ifdef JS_JITSPEW
    decodeBranchInstAndSpew(code);
#endif
    BufferOffset bo = writeInst(code.encode());
    writeInst(nextInChain);
    if (!oom()) {
      label->use(bo.getOffset());
    }
    return;
  }

  bool conditional = code.encode() != inst_beq.encode();

  // Make the whole branch continous in the buffer. The '7'
  // instructions are writing at below (contain conditional nop).
  m_buffer.ensureSpace(7 * sizeof(uint32_t));

#ifdef JS_JITSPEW
  decodeBranchInstAndSpew(code);
#endif
  BufferOffset bo = writeInst(code.encode());
  writeInst(nextInChain);
  if (!oom()) {
    label->use(bo.getOffset());
  }
  // Leave space for potential long jump.
  as_nop();
  as_nop();
  as_nop();
  as_nop();
  if (conditional) {
    as_nop();
  }
}

void MacroAssemblerMIPS64::ma_cmp_set(Register rd, Register rs, ImmWord imm,
                                      Condition c) {
  if (imm.value <= INT32_MAX) {
    ma_cmp_set(rd, rs, Imm32(uint32_t(imm.value)), c);
  } else {
    ma_li(ScratchRegister, imm);
    ma_cmp_set(rd, rs, ScratchRegister, c);
  }
}

void MacroAssemblerMIPS64::ma_cmp_set(Register rd, Register rs, ImmPtr imm,
                                      Condition c) {
  ma_cmp_set(rd, rs, ImmWord(uintptr_t(imm.value)), c);
}

void MacroAssemblerMIPS64::ma_cmp_set(Register rd, Address address, Imm32 imm,
                                      Condition c) {
  ma_load(ScratchRegister, address, SizeWord, SignExtend);
  ma_cmp_set(rd, ScratchRegister, imm, c);
}

// fp instructions
void MacroAssemblerMIPS64::ma_lid(FloatRegister dest, double value) {
  ImmWord imm(mozilla::BitwiseCast<uint64_t>(value));

  if (imm.value != 0) {
    ma_li(ScratchRegister, imm);
    moveToDouble(ScratchRegister, dest);
  } else {
    moveToDouble(zero, dest);
  }
}

void MacroAssemblerMIPS64::ma_mv(FloatRegister src, ValueOperand dest) {
  as_dmfc1(dest.valueReg(), src);
}

void MacroAssemblerMIPS64::ma_mv(ValueOperand src, FloatRegister dest) {
  as_dmtc1(src.valueReg(), dest);
}

void MacroAssemblerMIPS64::ma_ls(FloatRegister ft, Address address) {
  if (Imm16::IsInSignedRange(address.offset)) {
    as_lwc1(ft, address.base, address.offset);
  } else {
    MOZ_ASSERT(address.base != ScratchRegister);
    ma_li(ScratchRegister, Imm32(address.offset));
    if (isLoongson()) {
      as_gslsx(ft, address.base, ScratchRegister, 0);
    } else {
      as_daddu(ScratchRegister, address.base, ScratchRegister);
      as_lwc1(ft, ScratchRegister, 0);
    }
  }
}

void MacroAssemblerMIPS64::ma_ld(FloatRegister ft, Address address) {
  if (Imm16::IsInSignedRange(address.offset)) {
    as_ldc1(ft, address.base, address.offset);
  } else {
    MOZ_ASSERT(address.base != ScratchRegister);
    ma_li(ScratchRegister, Imm32(address.offset));
    if (isLoongson()) {
      as_gsldx(ft, address.base, ScratchRegister, 0);
    } else {
      as_daddu(ScratchRegister, address.base, ScratchRegister);
      as_ldc1(ft, ScratchRegister, 0);
    }
  }
}

void MacroAssemblerMIPS64::ma_sd(FloatRegister ft, Address address) {
  if (Imm16::IsInSignedRange(address.offset)) {
    as_sdc1(ft, address.base, address.offset);
  } else {
    MOZ_ASSERT(address.base != ScratchRegister);
    ma_li(ScratchRegister, Imm32(address.offset));
    if (isLoongson()) {
      as_gssdx(ft, address.base, ScratchRegister, 0);
    } else {
      as_daddu(ScratchRegister, address.base, ScratchRegister);
      as_sdc1(ft, ScratchRegister, 0);
    }
  }
}

void MacroAssemblerMIPS64::ma_ss(FloatRegister ft, Address address) {
  if (Imm16::IsInSignedRange(address.offset)) {
    as_swc1(ft, address.base, address.offset);
  } else {
    MOZ_ASSERT(address.base != ScratchRegister);
    ma_li(ScratchRegister, Imm32(address.offset));
    if (isLoongson()) {
      as_gsssx(ft, address.base, ScratchRegister, 0);
    } else {
      as_daddu(ScratchRegister, address.base, ScratchRegister);
      as_swc1(ft, ScratchRegister, 0);
    }
  }
}

void MacroAssemblerMIPS64::ma_pop(FloatRegister f) {
  as_ldc1(f, StackPointer, 0);
  as_daddiu(StackPointer, StackPointer, sizeof(double));
}

void MacroAssemblerMIPS64::ma_push(FloatRegister f) {
  as_daddiu(StackPointer, StackPointer, (int32_t) - sizeof(double));
  as_sdc1(f, StackPointer, 0);
}

bool MacroAssemblerMIPS64Compat::buildOOLFakeExitFrame(void* fakeReturnAddr) {
  uint32_t descriptor = MakeFrameDescriptor(
      asMasm().framePushed(), FrameType::IonJS, ExitFrameLayout::Size());

  asMasm().Push(Imm32(descriptor));  // descriptor_
  asMasm().Push(ImmPtr(fakeReturnAddr));

  return true;
}

void MacroAssemblerMIPS64Compat::move32(Imm32 imm, Register dest) {
  ma_li(dest, imm);
}

void MacroAssemblerMIPS64Compat::move32(Register src, Register dest) {
  ma_move(dest, src);
}

void MacroAssemblerMIPS64Compat::movePtr(Register src, Register dest) {
  ma_move(dest, src);
}
void MacroAssemblerMIPS64Compat::movePtr(ImmWord imm, Register dest) {
  ma_li(dest, imm);
}

void MacroAssemblerMIPS64Compat::movePtr(ImmGCPtr imm, Register dest) {
  ma_li(dest, imm);
}

void MacroAssemblerMIPS64Compat::movePtr(ImmPtr imm, Register dest) {
  movePtr(ImmWord(uintptr_t(imm.value)), dest);
}
void MacroAssemblerMIPS64Compat::movePtr(wasm::SymbolicAddress imm,
                                         Register dest) {
  append(wasm::SymbolicAccess(CodeOffset(nextOffset().getOffset()), imm));
  ma_liPatchable(dest, ImmWord(-1));
}

void MacroAssemblerMIPS64Compat::load8ZeroExtend(const Address& address,
                                                 Register dest) {
  ma_load(dest, address, SizeByte, ZeroExtend);
}

void MacroAssemblerMIPS64Compat::load8ZeroExtend(const BaseIndex& src,
                                                 Register dest) {
  ma_load(dest, src, SizeByte, ZeroExtend);
}

void MacroAssemblerMIPS64Compat::load8SignExtend(const Address& address,
                                                 Register dest) {
  ma_load(dest, address, SizeByte, SignExtend);
}

void MacroAssemblerMIPS64Compat::load8SignExtend(const BaseIndex& src,
                                                 Register dest) {
  ma_load(dest, src, SizeByte, SignExtend);
}

void MacroAssemblerMIPS64Compat::load16ZeroExtend(const Address& address,
                                                  Register dest) {
  ma_load(dest, address, SizeHalfWord, ZeroExtend);
}

void MacroAssemblerMIPS64Compat::load16ZeroExtend(const BaseIndex& src,
                                                  Register dest) {
  ma_load(dest, src, SizeHalfWord, ZeroExtend);
}

void MacroAssemblerMIPS64Compat::load16SignExtend(const Address& address,
                                                  Register dest) {
  ma_load(dest, address, SizeHalfWord, SignExtend);
}

void MacroAssemblerMIPS64Compat::load16SignExtend(const BaseIndex& src,
                                                  Register dest) {
  ma_load(dest, src, SizeHalfWord, SignExtend);
}

void MacroAssemblerMIPS64Compat::load32(const Address& address, Register dest) {
  ma_load(dest, address, SizeWord);
}

void MacroAssemblerMIPS64Compat::load32(const BaseIndex& address,
                                        Register dest) {
  ma_load(dest, address, SizeWord);
}

void MacroAssemblerMIPS64Compat::load32(AbsoluteAddress address,
                                        Register dest) {
  movePtr(ImmPtr(address.addr), ScratchRegister);
  load32(Address(ScratchRegister, 0), dest);
}

void MacroAssemblerMIPS64Compat::load32(wasm::SymbolicAddress address,
                                        Register dest) {
  movePtr(address, ScratchRegister);
  load32(Address(ScratchRegister, 0), dest);
}

void MacroAssemblerMIPS64Compat::loadPtr(const Address& address,
                                         Register dest) {
  ma_load(dest, address, SizeDouble);
}

void MacroAssemblerMIPS64Compat::loadPtr(const BaseIndex& src, Register dest) {
  ma_load(dest, src, SizeDouble);
}

void MacroAssemblerMIPS64Compat::loadPtr(AbsoluteAddress address,
                                         Register dest) {
  movePtr(ImmPtr(address.addr), ScratchRegister);
  loadPtr(Address(ScratchRegister, 0), dest);
}

void MacroAssemblerMIPS64Compat::loadPtr(wasm::SymbolicAddress address,
                                         Register dest) {
  movePtr(address, ScratchRegister);
  loadPtr(Address(ScratchRegister, 0), dest);
}

void MacroAssemblerMIPS64Compat::loadPrivate(const Address& address,
                                             Register dest) {
  loadPtr(address, dest);
}

void MacroAssemblerMIPS64Compat::loadUnalignedDouble(
    const wasm::MemoryAccessDesc& access, const BaseIndex& src, Register temp,
    FloatRegister dest) {
  computeScaledAddress(src, SecondScratchReg);
  BufferOffset load;
  if (Imm16::IsInSignedRange(src.offset) &&
      Imm16::IsInSignedRange(src.offset + 7)) {
    load = as_ldl(temp, SecondScratchReg, src.offset + 7);
    as_ldr(temp, SecondScratchReg, src.offset);
  } else {
    ma_li(ScratchRegister, Imm32(src.offset));
    as_daddu(ScratchRegister, SecondScratchReg, ScratchRegister);
    load = as_ldl(temp, ScratchRegister, 7);
    as_ldr(temp, ScratchRegister, 0);
  }
  append(access, load.getOffset());
  moveToDouble(temp, dest);
}

void MacroAssemblerMIPS64Compat::loadUnalignedFloat32(
    const wasm::MemoryAccessDesc& access, const BaseIndex& src, Register temp,
    FloatRegister dest) {
  computeScaledAddress(src, SecondScratchReg);
  BufferOffset load;
  if (Imm16::IsInSignedRange(src.offset) &&
      Imm16::IsInSignedRange(src.offset + 3)) {
    load = as_lwl(temp, SecondScratchReg, src.offset + 3);
    as_lwr(temp, SecondScratchReg, src.offset);
  } else {
    ma_li(ScratchRegister, Imm32(src.offset));
    as_daddu(ScratchRegister, SecondScratchReg, ScratchRegister);
    load = as_lwl(temp, ScratchRegister, 3);
    as_lwr(temp, ScratchRegister, 0);
  }
  append(access, load.getOffset());
  moveToFloat32(temp, dest);
}

void MacroAssemblerMIPS64Compat::store8(Imm32 imm, const Address& address) {
  ma_li(SecondScratchReg, imm);
  ma_store(SecondScratchReg, address, SizeByte);
}

void MacroAssemblerMIPS64Compat::store8(Register src, const Address& address) {
  ma_store(src, address, SizeByte);
}

void MacroAssemblerMIPS64Compat::store8(Imm32 imm, const BaseIndex& dest) {
  ma_store(imm, dest, SizeByte);
}

void MacroAssemblerMIPS64Compat::store8(Register src, const BaseIndex& dest) {
  ma_store(src, dest, SizeByte);
}

void MacroAssemblerMIPS64Compat::store16(Imm32 imm, const Address& address) {
  ma_li(SecondScratchReg, imm);
  ma_store(SecondScratchReg, address, SizeHalfWord);
}

void MacroAssemblerMIPS64Compat::store16(Register src, const Address& address) {
  ma_store(src, address, SizeHalfWord);
}

void MacroAssemblerMIPS64Compat::store16(Imm32 imm, const BaseIndex& dest) {
  ma_store(imm, dest, SizeHalfWord);
}

void MacroAssemblerMIPS64Compat::store16(Register src,
                                         const BaseIndex& address) {
  ma_store(src, address, SizeHalfWord);
}

void MacroAssemblerMIPS64Compat::store32(Register src,
                                         AbsoluteAddress address) {
  movePtr(ImmPtr(address.addr), ScratchRegister);
  store32(src, Address(ScratchRegister, 0));
}

void MacroAssemblerMIPS64Compat::store32(Register src, const Address& address) {
  ma_store(src, address, SizeWord);
}

void MacroAssemblerMIPS64Compat::store32(Imm32 src, const Address& address) {
  move32(src, SecondScratchReg);
  ma_store(SecondScratchReg, address, SizeWord);
}

void MacroAssemblerMIPS64Compat::store32(Imm32 imm, const BaseIndex& dest) {
  ma_store(imm, dest, SizeWord);
}

void MacroAssemblerMIPS64Compat::store32(Register src, const BaseIndex& dest) {
  ma_store(src, dest, SizeWord);
}

template <typename T>
void MacroAssemblerMIPS64Compat::storePtr(ImmWord imm, T address) {
  ma_li(SecondScratchReg, imm);
  ma_store(SecondScratchReg, address, SizeDouble);
}

template void MacroAssemblerMIPS64Compat::storePtr<Address>(ImmWord imm,
                                                            Address address);
template void MacroAssemblerMIPS64Compat::storePtr<BaseIndex>(
    ImmWord imm, BaseIndex address);

template <typename T>
void MacroAssemblerMIPS64Compat::storePtr(ImmPtr imm, T address) {
  storePtr(ImmWord(uintptr_t(imm.value)), address);
}

template void MacroAssemblerMIPS64Compat::storePtr<Address>(ImmPtr imm,
                                                            Address address);
template void MacroAssemblerMIPS64Compat::storePtr<BaseIndex>(
    ImmPtr imm, BaseIndex address);

template <typename T>
void MacroAssemblerMIPS64Compat::storePtr(ImmGCPtr imm, T address) {
  movePtr(imm, SecondScratchReg);
  storePtr(SecondScratchReg, address);
}

template void MacroAssemblerMIPS64Compat::storePtr<Address>(ImmGCPtr imm,
                                                            Address address);
template void MacroAssemblerMIPS64Compat::storePtr<BaseIndex>(
    ImmGCPtr imm, BaseIndex address);

void MacroAssemblerMIPS64Compat::storePtr(Register src,
                                          const Address& address) {
  ma_store(src, address, SizeDouble);
}

void MacroAssemblerMIPS64Compat::storePtr(Register src,
                                          const BaseIndex& address) {
  ma_store(src, address, SizeDouble);
}

void MacroAssemblerMIPS64Compat::storePtr(Register src, AbsoluteAddress dest) {
  movePtr(ImmPtr(dest.addr), ScratchRegister);
  storePtr(src, Address(ScratchRegister, 0));
}

void MacroAssemblerMIPS64Compat::storeUnalignedFloat32(
    const wasm::MemoryAccessDesc& access, FloatRegister src, Register temp,
    const BaseIndex& dest) {
  computeScaledAddress(dest, SecondScratchReg);
  moveFromFloat32(src, temp);
  BufferOffset store;
  if (Imm16::IsInSignedRange(dest.offset) &&
      Imm16::IsInSignedRange(dest.offset + 3)) {
    store = as_swl(temp, SecondScratchReg, dest.offset + 3);
    as_swr(temp, SecondScratchReg, dest.offset);
  } else {
    ma_li(ScratchRegister, Imm32(dest.offset));
    as_daddu(ScratchRegister, SecondScratchReg, ScratchRegister);
    store = as_swl(temp, ScratchRegister, 3);
    as_swr(temp, ScratchRegister, 0);
  }
  append(access, store.getOffset());
}

void MacroAssemblerMIPS64Compat::storeUnalignedDouble(
    const wasm::MemoryAccessDesc& access, FloatRegister src, Register temp,
    const BaseIndex& dest) {
  computeScaledAddress(dest, SecondScratchReg);
  moveFromDouble(src, temp);

  BufferOffset store;
  if (Imm16::IsInSignedRange(dest.offset) &&
      Imm16::IsInSignedRange(dest.offset + 7)) {
    store = as_sdl(temp, SecondScratchReg, dest.offset + 7);
    as_sdr(temp, SecondScratchReg, dest.offset);
  } else {
    ma_li(ScratchRegister, Imm32(dest.offset));
    as_daddu(ScratchRegister, SecondScratchReg, ScratchRegister);
    store = as_sdl(temp, ScratchRegister, 7);
    as_sdr(temp, ScratchRegister, 0);
  }
  append(access, store.getOffset());
}

void MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output) {
  as_roundwd(ScratchDoubleReg, input);
  ma_li(ScratchRegister, Imm32(255));
  as_mfc1(output, ScratchDoubleReg);
#ifdef MIPSR6
  as_slti(SecondScratchReg, output, 0);
  as_seleqz(output, output, SecondScratchReg);
  as_sltiu(SecondScratchReg, output, 255);
  as_selnez(output, output, SecondScratchReg);
  as_seleqz(ScratchRegister, ScratchRegister, SecondScratchReg);
  as_or(output, output, ScratchRegister);
#else
  zeroDouble(ScratchDoubleReg);
  as_sltiu(SecondScratchReg, output, 255);
  as_colt(DoubleFloat, ScratchDoubleReg, input);
  // if res > 255; res = 255;
  as_movz(output, ScratchRegister, SecondScratchReg);
  // if !(input > 0); res = 0;
  as_movf(output, zero);
#endif
}

void MacroAssemblerMIPS64Compat::testNullSet(Condition cond,
                                             const ValueOperand& value,
                                             Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  splitTag(value, SecondScratchReg);
  ma_cmp_set(dest, SecondScratchReg, ImmTag(JSVAL_TAG_NULL), cond);
}

void MacroAssemblerMIPS64Compat::testObjectSet(Condition cond,
                                               const ValueOperand& value,
                                               Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  splitTag(value, SecondScratchReg);
  ma_cmp_set(dest, SecondScratchReg, ImmTag(JSVAL_TAG_OBJECT), cond);
}

void MacroAssemblerMIPS64Compat::testUndefinedSet(Condition cond,
                                                  const ValueOperand& value,
                                                  Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  splitTag(value, SecondScratchReg);
  ma_cmp_set(dest, SecondScratchReg, ImmTag(JSVAL_TAG_UNDEFINED), cond);
}

void MacroAssemblerMIPS64Compat::unboxInt32(const ValueOperand& operand,
                                            Register dest) {
  ma_sll(dest, operand.valueReg(), Imm32(0));
}

void MacroAssemblerMIPS64Compat::unboxInt32(Register src, Register dest) {
  ma_sll(dest, src, Imm32(0));
}

void MacroAssemblerMIPS64Compat::unboxInt32(const Address& src, Register dest) {
  load32(Address(src.base, src.offset), dest);
}

void MacroAssemblerMIPS64Compat::unboxInt32(const BaseIndex& src,
                                            Register dest) {
  computeScaledAddress(src, SecondScratchReg);
  load32(Address(SecondScratchReg, src.offset), dest);
}

void MacroAssemblerMIPS64Compat::unboxBoolean(const ValueOperand& operand,
                                              Register dest) {
  ma_dext(dest, operand.valueReg(), Imm32(0), Imm32(32));
}

void MacroAssemblerMIPS64Compat::unboxBoolean(Register src, Register dest) {
  ma_dext(dest, src, Imm32(0), Imm32(32));
}

void MacroAssemblerMIPS64Compat::unboxBoolean(const Address& src,
                                              Register dest) {
  ma_load(dest, Address(src.base, src.offset), SizeWord, ZeroExtend);
}

void MacroAssemblerMIPS64Compat::unboxBoolean(const BaseIndex& src,
                                              Register dest) {
  computeScaledAddress(src, SecondScratchReg);
  ma_load(dest, Address(SecondScratchReg, src.offset), SizeWord, ZeroExtend);
}

void MacroAssemblerMIPS64Compat::unboxDouble(const ValueOperand& operand,
                                             FloatRegister dest) {
  as_dmtc1(operand.valueReg(), dest);
}

void MacroAssemblerMIPS64Compat::unboxDouble(const Address& src,
                                             FloatRegister dest) {
  ma_ld(dest, Address(src.base, src.offset));
}
void MacroAssemblerMIPS64Compat::unboxDouble(const BaseIndex& src,
                                             FloatRegister dest) {
  SecondScratchRegisterScope scratch(asMasm());
  loadPtr(src, scratch);
  unboxDouble(ValueOperand(scratch), dest);
}

void MacroAssemblerMIPS64Compat::unboxString(const ValueOperand& operand,
                                             Register dest) {
  unboxNonDouble(operand, dest, JSVAL_TYPE_STRING);
}

void MacroAssemblerMIPS64Compat::unboxString(Register src, Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
}

void MacroAssemblerMIPS64Compat::unboxString(const Address& src,
                                             Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
}

void MacroAssemblerMIPS64Compat::unboxSymbol(const ValueOperand& operand,
                                             Register dest) {
  unboxNonDouble(operand, dest, JSVAL_TYPE_SYMBOL);
}

void MacroAssemblerMIPS64Compat::unboxSymbol(Register src, Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
}

void MacroAssemblerMIPS64Compat::unboxSymbol(const Address& src,
                                             Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
}

void MacroAssemblerMIPS64Compat::unboxBigInt(const ValueOperand& operand,
                                             Register dest) {
  unboxNonDouble(operand, dest, JSVAL_TYPE_BIGINT);
}

void MacroAssemblerMIPS64Compat::unboxBigInt(Register src, Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_BIGINT);
}

void MacroAssemblerMIPS64Compat::unboxBigInt(const Address& src,
                                             Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_BIGINT);
}

void MacroAssemblerMIPS64Compat::unboxObject(const ValueOperand& src,
                                             Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
}

void MacroAssemblerMIPS64Compat::unboxObject(Register src, Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
}

void MacroAssemblerMIPS64Compat::unboxObject(const Address& src,
                                             Register dest) {
  unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
}

void MacroAssemblerMIPS64Compat::unboxValue(const ValueOperand& src,
                                            AnyRegister dest,
                                            JSValueType type) {
  if (dest.isFloat()) {
    Label notInt32, end;
    asMasm().branchTestInt32(Assembler::NotEqual, src, &notInt32);
    convertInt32ToDouble(src.valueReg(), dest.fpu());
    ma_b(&end, ShortJump);
    bind(&notInt32);
    unboxDouble(src, dest.fpu());
    bind(&end);
  } else {
    unboxNonDouble(src, dest.gpr(), type);
  }
}

void MacroAssemblerMIPS64Compat::boxDouble(FloatRegister src,
                                           const ValueOperand& dest,
                                           FloatRegister) {
  as_dmfc1(dest.valueReg(), src);
}

void MacroAssemblerMIPS64Compat::boxNonDouble(JSValueType type, Register src,
                                              const ValueOperand& dest) {
  MOZ_ASSERT(src != dest.valueReg());
  boxValue(type, src, dest.valueReg());
}

void MacroAssemblerMIPS64Compat::boolValueToDouble(const ValueOperand& operand,
                                                   FloatRegister dest) {
  convertBoolToInt32(operand.valueReg(), ScratchRegister);
  convertInt32ToDouble(ScratchRegister, dest);
}

void MacroAssemblerMIPS64Compat::int32ValueToDouble(const ValueOperand& operand,
                                                    FloatRegister dest) {
  convertInt32ToDouble(operand.valueReg(), dest);
}

void MacroAssemblerMIPS64Compat::boolValueToFloat32(const ValueOperand& operand,
                                                    FloatRegister dest) {
  convertBoolToInt32(operand.valueReg(), ScratchRegister);
  convertInt32ToFloat32(ScratchRegister, dest);
}

void MacroAssemblerMIPS64Compat::int32ValueToFloat32(
    const ValueOperand& operand, FloatRegister dest) {
  convertInt32ToFloat32(operand.valueReg(), dest);
}

void MacroAssemblerMIPS64Compat::loadConstantFloat32(float f,
                                                     FloatRegister dest) {
  ma_lis(dest, f);
}

void MacroAssemblerMIPS64Compat::loadInt32OrDouble(const Address& src,
                                                   FloatRegister dest) {
  Label notInt32, end;
  // If it's an int, convert it to double.
  loadPtr(Address(src.base, src.offset), ScratchRegister);
  ma_dsrl(SecondScratchReg, ScratchRegister, Imm32(JSVAL_TAG_SHIFT));
  asMasm().branchTestInt32(Assembler::NotEqual, SecondScratchReg, &notInt32);
  loadPtr(Address(src.base, src.offset), SecondScratchReg);
  convertInt32ToDouble(SecondScratchReg, dest);
  ma_b(&end, ShortJump);

  // Not an int, just load as double.
  bind(&notInt32);
  unboxDouble(src, dest);
  bind(&end);
}

void MacroAssemblerMIPS64Compat::loadInt32OrDouble(const BaseIndex& addr,
                                                   FloatRegister dest) {
  Label notInt32, end;

  // If it's an int, convert it to double.
  computeScaledAddress(addr, SecondScratchReg);
  // Since we only have one scratch, we need to stomp over it with the tag.
  loadPtr(Address(SecondScratchReg, 0), ScratchRegister);
  ma_dsrl(SecondScratchReg, ScratchRegister, Imm32(JSVAL_TAG_SHIFT));
  asMasm().branchTestInt32(Assembler::NotEqual, SecondScratchReg, &notInt32);

  computeScaledAddress(addr, SecondScratchReg);
  loadPtr(Address(SecondScratchReg, 0), SecondScratchReg);
  convertInt32ToDouble(SecondScratchReg, dest);
  ma_b(&end, ShortJump);

  // Not an int, just load as double.
  bind(&notInt32);
  // First, recompute the offset that had been stored in the scratch register
  // since the scratch register was overwritten loading in the type.
  computeScaledAddress(addr, SecondScratchReg);
  unboxDouble(Address(SecondScratchReg, 0), dest);
  bind(&end);
}

void MacroAssemblerMIPS64Compat::loadConstantDouble(double dp,
                                                    FloatRegister dest) {
  ma_lid(dest, dp);
}

Register MacroAssemblerMIPS64Compat::extractObject(const Address& address,
                                                   Register scratch) {
  loadPtr(Address(address.base, address.offset), scratch);
  ma_dext(scratch, scratch, Imm32(0), Imm32(JSVAL_TAG_SHIFT));
  return scratch;
}

Register MacroAssemblerMIPS64Compat::extractTag(const Address& address,
                                                Register scratch) {
  loadPtr(Address(address.base, address.offset), scratch);
  ma_dext(scratch, scratch, Imm32(JSVAL_TAG_SHIFT),
          Imm32(64 - JSVAL_TAG_SHIFT));
  return scratch;
}

Register MacroAssemblerMIPS64Compat::extractTag(const BaseIndex& address,
                                                Register scratch) {
  computeScaledAddress(address, scratch);
  return extractTag(Address(scratch, address.offset), scratch);
}

/////////////////////////////////////////////////////////////////
// X86/X64-common/ARM/MIPS interface.
/////////////////////////////////////////////////////////////////
void MacroAssemblerMIPS64Compat::storeValue(ValueOperand val, Operand dst) {
  storeValue(val, Address(Register::FromCode(dst.base()), dst.disp()));
}

void MacroAssemblerMIPS64Compat::storeValue(ValueOperand val,
                                            const BaseIndex& dest) {
  computeScaledAddress(dest, SecondScratchReg);
  storeValue(val, Address(SecondScratchReg, dest.offset));
}

void MacroAssemblerMIPS64Compat::storeValue(JSValueType type, Register reg,
                                            BaseIndex dest) {
  computeScaledAddress(dest, ScratchRegister);

  int32_t offset = dest.offset;
  if (!Imm16::IsInSignedRange(offset)) {
    ma_li(SecondScratchReg, Imm32(offset));
    as_daddu(ScratchRegister, ScratchRegister, SecondScratchReg);
    offset = 0;
  }

  storeValue(type, reg, Address(ScratchRegister, offset));
}

void MacroAssemblerMIPS64Compat::storeValue(ValueOperand val,
                                            const Address& dest) {
  storePtr(val.valueReg(), Address(dest.base, dest.offset));
}

void MacroAssemblerMIPS64Compat::storeValue(JSValueType type, Register reg,
                                            Address dest) {
  MOZ_ASSERT(dest.base != SecondScratchReg);

  if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
    store32(reg, dest);
    JSValueShiftedTag tag = (JSValueShiftedTag)JSVAL_TYPE_TO_SHIFTED_TAG(type);
    store32(((Imm64(tag)).secondHalf()), Address(dest.base, dest.offset + 4));
  } else {
    ma_li(SecondScratchReg, ImmTag(JSVAL_TYPE_TO_TAG(type)));
    ma_dsll(SecondScratchReg, SecondScratchReg, Imm32(JSVAL_TAG_SHIFT));
    ma_dins(SecondScratchReg, reg, Imm32(0), Imm32(JSVAL_TAG_SHIFT));
    storePtr(SecondScratchReg, Address(dest.base, dest.offset));
  }
}

void MacroAssemblerMIPS64Compat::storeValue(const Value& val, Address dest) {
  if (val.isGCThing()) {
    writeDataRelocation(val);
    movWithPatch(ImmWord(val.asRawBits()), SecondScratchReg);
  } else {
    ma_li(SecondScratchReg, ImmWord(val.asRawBits()));
  }
  storePtr(SecondScratchReg, Address(dest.base, dest.offset));
}

void MacroAssemblerMIPS64Compat::storeValue(const Value& val, BaseIndex dest) {
  computeScaledAddress(dest, ScratchRegister);

  int32_t offset = dest.offset;
  if (!Imm16::IsInSignedRange(offset)) {
    ma_li(SecondScratchReg, Imm32(offset));
    as_daddu(ScratchRegister, ScratchRegister, SecondScratchReg);
    offset = 0;
  }
  storeValue(val, Address(ScratchRegister, offset));
}

void MacroAssemblerMIPS64Compat::loadValue(const BaseIndex& addr,
                                           ValueOperand val) {
  computeScaledAddress(addr, SecondScratchReg);
  loadValue(Address(SecondScratchReg, addr.offset), val);
}

void MacroAssemblerMIPS64Compat::loadValue(Address src, ValueOperand val) {
  loadPtr(Address(src.base, src.offset), val.valueReg());
}

void MacroAssemblerMIPS64Compat::tagValue(JSValueType type, Register payload,
                                          ValueOperand dest) {
  MOZ_ASSERT(dest.valueReg() != ScratchRegister);
  if (payload != dest.valueReg()) {
    ma_move(dest.valueReg(), payload);
  }
  ma_li(ScratchRegister, ImmTag(JSVAL_TYPE_TO_TAG(type)));
  ma_dins(dest.valueReg(), ScratchRegister, Imm32(JSVAL_TAG_SHIFT),
          Imm32(64 - JSVAL_TAG_SHIFT));
  if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
    ma_dins(dest.valueReg(), zero, Imm32(32), Imm32(JSVAL_TAG_SHIFT - 32));
  }
}

void MacroAssemblerMIPS64Compat::pushValue(ValueOperand val) {
  // Allocate stack slots for Value. One for each.
  asMasm().subPtr(Imm32(sizeof(Value)), StackPointer);
  // Store Value
  storeValue(val, Address(StackPointer, 0));
}

void MacroAssemblerMIPS64Compat::pushValue(const Address& addr) {
  // Load value before allocate stack, addr.base may be is sp.
  loadPtr(Address(addr.base, addr.offset), ScratchRegister);
  ma_dsubu(StackPointer, StackPointer, Imm32(sizeof(Value)));
  storePtr(ScratchRegister, Address(StackPointer, 0));
}

void MacroAssemblerMIPS64Compat::popValue(ValueOperand val) {
  as_ld(val.valueReg(), StackPointer, 0);
  as_daddiu(StackPointer, StackPointer, sizeof(Value));
}

void MacroAssemblerMIPS64Compat::breakpoint() { as_break(0); }

void MacroAssemblerMIPS64Compat::ensureDouble(const ValueOperand& source,
                                              FloatRegister dest,
                                              Label* failure) {
  Label isDouble, done;
  {
    ScratchTagScope tag(asMasm(), source);
    splitTagForTest(source, tag);
    asMasm().branchTestDouble(Assembler::Equal, tag, &isDouble);
    asMasm().branchTestInt32(Assembler::NotEqual, tag, failure);
  }

  unboxInt32(source, ScratchRegister);
  convertInt32ToDouble(ScratchRegister, dest);
  jump(&done);

  bind(&isDouble);
  unboxDouble(source, dest);

  bind(&done);
}

void MacroAssemblerMIPS64Compat::checkStackAlignment() {
#ifdef DEBUG
  Label aligned;
  as_andi(ScratchRegister, sp, ABIStackAlignment - 1);
  ma_b(ScratchRegister, zero, &aligned, Equal, ShortJump);
  as_break(BREAK_STACK_UNALIGNED);
  bind(&aligned);
#endif
}

void MacroAssemblerMIPS64Compat::handleFailureWithHandlerTail(
    Label* profilerExitTail) {
  // Reserve space for exception information.
  int size = (sizeof(ResumeFromException) + ABIStackAlignment) &
             ~(ABIStackAlignment - 1);
  asMasm().subPtr(Imm32(size), StackPointer);
  ma_move(a0, StackPointer);  // Use a0 since it is a first function argument

  // Call the handler.
  using Fn = void (*)(ResumeFromException * rfe);
  asMasm().setupUnalignedABICall(a1);
  asMasm().passABIArg(a0);
  asMasm().callWithABI<Fn, HandleException>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  Label entryFrame;
  Label catch_;
  Label finally;
  Label return_;
  Label bailout;
  Label wasm;
  Label wasmCatch;

  // Already clobbered a0, so use it...
  load32(Address(StackPointer, offsetof(ResumeFromException, kind)), a0);
  asMasm().branch32(Assembler::Equal, a0,
                    Imm32(ResumeFromException::RESUME_ENTRY_FRAME),
                    &entryFrame);
  asMasm().branch32(Assembler::Equal, a0,
                    Imm32(ResumeFromException::RESUME_CATCH), &catch_);
  asMasm().branch32(Assembler::Equal, a0,
                    Imm32(ResumeFromException::RESUME_FINALLY), &finally);
  asMasm().branch32(Assembler::Equal, a0,
                    Imm32(ResumeFromException::RESUME_FORCED_RETURN), &return_);
  asMasm().branch32(Assembler::Equal, a0,
                    Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);
  asMasm().branch32(Assembler::Equal, a0,
                    Imm32(ResumeFromException::RESUME_WASM), &wasm);
  asMasm().branch32(Assembler::Equal, a0,
                    Imm32(ResumeFromException::RESUME_WASM_CATCH), &wasmCatch);

  breakpoint();  // Invalid kind.

  // No exception handler. Load the error value, load the new stack pointer
  // and return from the entry frame.
  bind(&entryFrame);
  asMasm().moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)),
          StackPointer);

  // We're going to be returning by the ion calling convention
  ma_pop(ra);
  as_jr(ra);
  as_nop();

  // If we found a catch handler, this must be a baseline frame. Restore
  // state and jump to the catch block.
  bind(&catch_);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, target)), a0);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)),
          BaselineFrameReg);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)),
          StackPointer);
  jump(a0);

  // If we found a finally block, this must be a baseline frame. Push
  // two values expected by JSOp::Retsub: BooleanValue(true) and the
  // exception.
  bind(&finally);
  ValueOperand exception = ValueOperand(a1);
  loadValue(Address(sp, offsetof(ResumeFromException, exception)), exception);

  loadPtr(Address(sp, offsetof(ResumeFromException, target)), a0);
  loadPtr(Address(sp, offsetof(ResumeFromException, framePointer)),
          BaselineFrameReg);
  loadPtr(Address(sp, offsetof(ResumeFromException, stackPointer)), sp);

  pushValue(BooleanValue(true));
  pushValue(exception);
  jump(a0);

  // Only used in debug mode. Return BaselineFrame->returnValue() to the
  // caller.
  bind(&return_);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)),
          BaselineFrameReg);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)),
          StackPointer);
  loadValue(
      Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue()),
      JSReturnOperand);
  ma_move(StackPointer, BaselineFrameReg);
  pop(BaselineFrameReg);

  // If profiling is enabled, then update the lastProfilingFrame to refer to
  // caller frame before returning.
  {
    Label skipProfilingInstrumentation;
    // Test if profiler enabled.
    AbsoluteAddress addressOfEnabled(
        GetJitContext()->runtime->geckoProfiler().addressOfEnabled());
    asMasm().branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                      &skipProfilingInstrumentation);
    jump(profilerExitTail);
    bind(&skipProfilingInstrumentation);
  }

  ret();

  // If we are bailing out to baseline to handle an exception, jump to
  // the bailout tail stub. Load 1 (true) in ReturnReg to indicate success.
  bind(&bailout);
  loadPtr(Address(sp, offsetof(ResumeFromException, bailoutInfo)), a2);
  ma_li(ReturnReg, Imm32(1));
  loadPtr(Address(sp, offsetof(ResumeFromException, target)), a1);
  jump(a1);

  // If we are throwing and the innermost frame was a wasm frame, reset SP and
  // FP; SP is pointing to the unwound return address to the wasm entry, so
  // we can just ret().
  bind(&wasm);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)),
          FramePointer);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)),
          StackPointer);
  ret();

  // Found a wasm catch handler, restore state and jump to it.
  bind(&wasmCatch);
  loadPtr(Address(sp, offsetof(ResumeFromException, target)), a1);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)),
          FramePointer);
  loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)),
          StackPointer);
  jump(a1);
}

CodeOffset MacroAssemblerMIPS64Compat::toggledJump(Label* label) {
  CodeOffset ret(nextOffset().getOffset());
  ma_b(label);
  return ret;
}

CodeOffset MacroAssemblerMIPS64Compat::toggledCall(JitCode* target,
                                                   bool enabled) {
  BufferOffset bo = nextOffset();
  CodeOffset offset(bo.getOffset());
  addPendingJump(bo, ImmPtr(target->raw()), RelocationKind::JITCODE);
  ma_liPatchable(ScratchRegister, ImmPtr(target->raw()));
  if (enabled) {
    as_jalr(ScratchRegister);
    as_nop();
  } else {
    as_nop();
    as_nop();
  }
  MOZ_ASSERT_IF(!oom(), nextOffset().getOffset() - offset.offset() ==
                            ToggledCallSize(nullptr));
  return offset;
}

void MacroAssemblerMIPS64Compat::profilerEnterFrame(Register framePtr,
                                                    Register scratch) {
  asMasm().loadJSContext(scratch);
  loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
  storePtr(framePtr,
           Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
  storePtr(ImmPtr(nullptr),
           Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void MacroAssemblerMIPS64Compat::profilerExitFrame() {
  jump(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
}

void MacroAssembler::subFromStackPtr(Imm32 imm32) {
  if (imm32.value) {
    asMasm().subPtr(imm32, StackPointer);
  }
}

//{{{ check_macroassembler_style
// ===============================================================
// Stack manipulation functions.

size_t MacroAssembler::PushRegsInMaskSizeInBytes(LiveRegisterSet set) {
  return set.gprs().size() * sizeof(intptr_t) + set.fpus().getPushSizeInBytes();
}

void MacroAssembler::PushRegsInMask(LiveRegisterSet set) {
  int32_t diff =
      set.gprs().size() * sizeof(intptr_t) + set.fpus().getPushSizeInBytes();
  const int32_t reserved = diff;

  reserveStack(reserved);
  for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
    diff -= sizeof(intptr_t);
    storePtr(*iter, Address(StackPointer, diff));
  }

#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  for (FloatRegisterBackwardIterator iter(set.fpus().reduceSetForPush());
       iter.more(); ++iter) {
    diff -= sizeof(double);
    storeDouble(*iter, Address(StackPointer, diff));
  }
  MOZ_ASSERT(diff == 0);
}

void MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set,
                                         LiveRegisterSet ignore) {
  int32_t diff =
      set.gprs().size() * sizeof(intptr_t) + set.fpus().getPushSizeInBytes();
  const int32_t reserved = diff;

  for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
    diff -= sizeof(intptr_t);
    if (!ignore.has(*iter)) {
      loadPtr(Address(StackPointer, diff), *iter);
    }
  }

#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  for (FloatRegisterBackwardIterator iter(set.fpus().reduceSetForPush());
       iter.more(); ++iter) {
    diff -= sizeof(double);
    if (!ignore.has(*iter)) {
      loadDouble(Address(StackPointer, diff), *iter);
    }
  }
  MOZ_ASSERT(diff == 0);
  freeStack(reserved);
}

void MacroAssembler::storeRegsInMask(LiveRegisterSet set, Address dest,
                                     Register) {
  FloatRegisterSet fpuSet(set.fpus().reduceSetForPush());
  unsigned numFpu = fpuSet.size();
  int32_t diffF = fpuSet.getPushSizeInBytes();
  int32_t diffG = set.gprs().size() * sizeof(intptr_t);

  MOZ_ASSERT(dest.offset >= diffG + diffF);

  for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
    diffG -= sizeof(intptr_t);
    dest.offset -= sizeof(intptr_t);
    storePtr(*iter, dest);
  }
  MOZ_ASSERT(diffG == 0);

#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  for (FloatRegisterBackwardIterator iter(fpuSet); iter.more(); ++iter) {
    FloatRegister reg = *iter;
    diffF -= reg.size();
    numFpu -= 1;
    dest.offset -= reg.size();
    if (reg.isDouble()) {
      storeDouble(reg, dest);
    } else if (reg.isSingle()) {
      storeFloat32(reg, dest);
    } else {
      MOZ_CRASH("Unknown register type.");
    }
  }
  MOZ_ASSERT(numFpu == 0);
  diffF -= diffF % sizeof(uintptr_t);
  MOZ_ASSERT(diffF == 0);
}
// ===============================================================
// ABI function calls.

void MacroAssembler::setupUnalignedABICall(Register scratch) {
  MOZ_ASSERT(!IsCompilingWasm(), "wasm should only use aligned ABI calls");
  setupNativeABICall();
  dynamicAlignment_ = true;

  ma_move(scratch, StackPointer);

  // Force sp to be aligned
  asMasm().subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
  ma_and(StackPointer, StackPointer, Imm32(~(ABIStackAlignment - 1)));
  storePtr(scratch, Address(StackPointer, 0));
}

void MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromWasm) {
  MOZ_ASSERT(inCall_);
  uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

  // Reserve place for $ra.
  stackForCall += sizeof(intptr_t);

  if (dynamicAlignment_) {
    stackForCall += ComputeByteAlignment(stackForCall, ABIStackAlignment);
  } else {
    uint32_t alignmentAtPrologue = callFromWasm ? sizeof(wasm::Frame) : 0;
    stackForCall += ComputeByteAlignment(
        stackForCall + framePushed() + alignmentAtPrologue, ABIStackAlignment);
  }

  *stackAdjust = stackForCall;
  reserveStack(stackForCall);

  // Save $ra because call is going to clobber it. Restore it in
  // callWithABIPost. NOTE: This is needed for calls from SharedIC.
  // Maybe we can do this differently.
  storePtr(ra, Address(StackPointer, stackForCall - sizeof(intptr_t)));

  // Position all arguments.
  {
    enoughMemory_ &= moveResolver_.resolve();
    if (!enoughMemory_) {
      return;
    }

    MoveEmitter emitter(*this);
    emitter.emit(moveResolver_);
    emitter.finish();
  }

  assertStackAlignment(ABIStackAlignment);
}

void MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result,
                                     bool callFromWasm) {
  // Restore ra value (as stored in callWithABIPre()).
  loadPtr(Address(StackPointer, stackAdjust - sizeof(intptr_t)), ra);

  if (dynamicAlignment_) {
    // Restore sp value from stack (as stored in setupUnalignedABICall()).
    loadPtr(Address(StackPointer, stackAdjust), StackPointer);
    // Use adjustFrame instead of freeStack because we already restored sp.
    adjustFrame(-stackAdjust);
  } else {
    freeStack(stackAdjust);
  }

#ifdef DEBUG
  MOZ_ASSERT(inCall_);
  inCall_ = false;
#endif
}

void MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result) {
  // Load the callee in t9, no instruction between the lw and call
  // should clobber it. Note that we can't use fun.base because it may
  // be one of the IntArg registers clobbered before the call.
  ma_move(t9, fun);
  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(t9);
  callWithABIPost(stackAdjust, result);
}

void MacroAssembler::callWithABINoProfiler(const Address& fun,
                                           MoveOp::Type result) {
  // Load the callee in t9, as above.
  loadPtr(Address(fun.base, fun.offset), t9);
  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);
  call(t9);
  callWithABIPost(stackAdjust, result);
}

// ===============================================================
// Move

void MacroAssembler::moveValue(const TypedOrValueRegister& src,
                               const ValueOperand& dest) {
  if (src.hasValue()) {
    moveValue(src.valueReg(), dest);
    return;
  }

  MIRType type = src.type();
  AnyRegister reg = src.typedReg();

  if (!IsFloatingPointType(type)) {
    boxNonDouble(ValueTypeFromMIRType(type), reg.gpr(), dest);
    return;
  }

  FloatRegister scratch = ScratchDoubleReg;
  FloatRegister freg = reg.fpu();
  if (type == MIRType::Float32) {
    convertFloat32ToDouble(freg, scratch);
    freg = scratch;
  }
  boxDouble(freg, dest, scratch);
}

void MacroAssembler::moveValue(const ValueOperand& src,
                               const ValueOperand& dest) {
  if (src == dest) {
    return;
  }
  movePtr(src.valueReg(), dest.valueReg());
}

void MacroAssembler::moveValue(const Value& src, const ValueOperand& dest) {
  if (!src.isGCThing()) {
    ma_li(dest.valueReg(), ImmWord(src.asRawBits()));
    return;
  }

  writeDataRelocation(src);
  movWithPatch(ImmWord(src.asRawBits()), dest.valueReg());
}

// ===============================================================
// Branch functions

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              const Address& address,
                                              Register temp, Label* label) {
  branchValueIsNurseryCellImpl(cond, address, temp, label);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              ValueOperand value, Register temp,
                                              Label* label) {
  branchValueIsNurseryCellImpl(cond, value, temp, label);
}

template <typename T>
void MacroAssembler::branchValueIsNurseryCellImpl(Condition cond,
                                                  const T& value, Register temp,
                                                  Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
  Label done;
  branchTestGCThing(Assembler::NotEqual, value,
                    cond == Assembler::Equal ? &done : label);

  // temp may be InvalidReg, use scratch2 instead.
  SecondScratchRegisterScope scratch2(*this);

  unboxGCThingForGCBarrier(value, scratch2);
  orPtr(Imm32(gc::ChunkMask), scratch2);
  loadPtr(Address(scratch2, gc::ChunkStoreBufferOffsetFromLastByte), scratch2);
  branchPtr(InvertCondition(cond), scratch2, ImmWord(0), label);

  bind(&done);
}

void MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                     const Value& rhs, Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(*this);
  MOZ_ASSERT(lhs.valueReg() != scratch);
  moveValue(rhs, ValueOperand(scratch));
  ma_b(lhs.valueReg(), scratch, label, cond);
}

// ========================================================================
// Memory access primitives.
template <typename T>
void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                       MIRType valueType, const T& dest,
                                       MIRType slotType) {
  if (valueType == MIRType::Double) {
    boxDouble(value.reg().typedReg().fpu(), dest);
    return;
  }

  // For known integers and booleans, we can just store the unboxed value if
  // the slot has the same type.
  if ((valueType == MIRType::Int32 || valueType == MIRType::Boolean) &&
      slotType == valueType) {
    if (value.constant()) {
      Value val = value.value();
      if (valueType == MIRType::Int32) {
        store32(Imm32(val.toInt32()), dest);
      } else {
        store32(Imm32(val.toBoolean() ? 1 : 0), dest);
      }
    } else {
      store32(value.reg().typedReg().gpr(), dest);
    }
    return;
  }

  if (value.constant()) {
    storeValue(value.value(), dest);
  } else {
    storeValue(ValueTypeFromMIRType(valueType), value.reg().typedReg().gpr(),
               dest);
  }
}

template void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                                MIRType valueType,
                                                const Address& dest,
                                                MIRType slotType);
template void MacroAssembler::storeUnboxedValue(
    const ConstantOrRegister& value, MIRType valueType,
    const BaseObjectElementIndex& dest, MIRType slotType);

void MacroAssembler::PushBoxed(FloatRegister reg) {
  subFromStackPtr(Imm32(sizeof(double)));
  boxDouble(reg, Address(getStackPointer(), 0));
  adjustFrame(sizeof(double));
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Register boundsCheckLimit,
                                       Label* label) {
  ma_b(index, boundsCheckLimit, label, cond);
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Address boundsCheckLimit, Label* label) {
  SecondScratchRegisterScope scratch2(*this);
  load32(boundsCheckLimit, SecondScratchReg);
  ma_b(index, SecondScratchReg, label, cond);
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Register64 boundsCheckLimit,
                                       Label* label) {
  MOZ_CRASH("IMPLEMENTME");
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Address boundsCheckLimit, Label* label) {
  MOZ_CRASH("IMPLEMENTME");
}

void MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input,
                                                Register output,
                                                bool isSaturating,
                                                Label* oolEntry) {
  as_truncld(ScratchDoubleReg, input);
  moveFromDouble(ScratchDoubleReg, output);
  ma_dsrl(ScratchRegister, output, Imm32(32));
  as_sll(output, output, 0);
  ma_b(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);
}

void MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input,
                                                 Register output,
                                                 bool isSaturating,
                                                 Label* oolEntry) {
  as_truncls(ScratchDoubleReg, input);
  moveFromDouble(ScratchDoubleReg, output);
  ma_dsrl(ScratchRegister, output, Imm32(32));
  as_sll(output, output, 0);
  ma_b(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);
}

void MacroAssembler::wasmLoadI64(const wasm::MemoryAccessDesc& access,
                                 Register memoryBase, Register ptr,
                                 Register ptrScratch, Register64 output) {
  wasmLoadI64Impl(access, memoryBase, ptr, ptrScratch, output, InvalidReg);
}

void MacroAssembler::wasmUnalignedLoadI64(const wasm::MemoryAccessDesc& access,
                                          Register memoryBase, Register ptr,
                                          Register ptrScratch,
                                          Register64 output, Register tmp) {
  wasmLoadI64Impl(access, memoryBase, ptr, ptrScratch, output, tmp);
}

void MacroAssembler::wasmStoreI64(const wasm::MemoryAccessDesc& access,
                                  Register64 value, Register memoryBase,
                                  Register ptr, Register ptrScratch) {
  wasmStoreI64Impl(access, value, memoryBase, ptr, ptrScratch, InvalidReg);
}

void MacroAssembler::wasmUnalignedStoreI64(const wasm::MemoryAccessDesc& access,
                                           Register64 value,
                                           Register memoryBase, Register ptr,
                                           Register ptrScratch, Register tmp) {
  wasmStoreI64Impl(access, value, memoryBase, ptr, ptrScratch, tmp);
}

void MacroAssembler::wasmTruncateDoubleToInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempDouble) {
  MOZ_ASSERT(tempDouble.isInvalid());

  as_truncld(ScratchDoubleReg, input);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromDouble(ScratchDoubleReg, output.reg);
  ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
  ma_b(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);

  if (isSaturating) {
    bind(oolRejoin);
  }
}

void MacroAssembler::wasmTruncateDoubleToUInt64(
    FloatRegister input, Register64 output_, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempDouble) {
  MOZ_ASSERT(tempDouble.isInvalid());
  Register output = output_.reg;

  Label done;

  as_truncld(ScratchDoubleReg, input);
  // ma_li INT64_MAX
  ma_li(SecondScratchReg, Imm32(-1));
  ma_dext(SecondScratchReg, SecondScratchReg, Imm32(0), Imm32(63));
  moveFromDouble(ScratchDoubleReg, output);
  // For numbers in  -1.[ : ]INT64_MAX range do nothing more
  ma_b(output, SecondScratchReg, &done, Assembler::Below, ShortJump);

  loadConstantDouble(double(INT64_MAX + 1ULL), ScratchDoubleReg);
  // ma_li INT64_MIN
  ma_daddu(SecondScratchReg, Imm32(1));
  as_subd(ScratchDoubleReg, input, ScratchDoubleReg);
  as_truncld(ScratchDoubleReg, ScratchDoubleReg);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromDouble(ScratchDoubleReg, output);
  ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
  ma_daddu(output, SecondScratchReg);

  // Guard against negative values that result in 0 due the precision loss.
  as_sltiu(SecondScratchReg, output, 1);
  ma_or(ScratchRegister, SecondScratchReg);

  ma_b(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);

  bind(&done);

  if (isSaturating) {
    bind(oolRejoin);
  }
}

void MacroAssembler::wasmTruncateFloat32ToInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempFloat) {
  MOZ_ASSERT(tempFloat.isInvalid());

  as_truncls(ScratchDoubleReg, input);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromDouble(ScratchDoubleReg, output.reg);
  ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
  ma_b(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);

  if (isSaturating) {
    bind(oolRejoin);
  }
}

void MacroAssembler::wasmTruncateFloat32ToUInt64(
    FloatRegister input, Register64 output_, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempFloat) {
  MOZ_ASSERT(tempFloat.isInvalid());
  Register output = output_.reg;

  Label done;

  as_truncls(ScratchDoubleReg, input);
  // ma_li INT64_MAX
  ma_li(SecondScratchReg, Imm32(-1));
  ma_dext(SecondScratchReg, SecondScratchReg, Imm32(0), Imm32(63));
  moveFromDouble(ScratchDoubleReg, output);
  // For numbers in  -1.[ : ]INT64_MAX range do nothing more
  ma_b(output, SecondScratchReg, &done, Assembler::Below, ShortJump);

  loadConstantFloat32(float(INT64_MAX + 1ULL), ScratchFloat32Reg);
  // ma_li INT64_MIN
  ma_daddu(SecondScratchReg, Imm32(1));
  as_subs(ScratchFloat32Reg, input, ScratchFloat32Reg);
  as_truncls(ScratchDoubleReg, ScratchFloat32Reg);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromDouble(ScratchDoubleReg, output);
  ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
  ma_daddu(output, SecondScratchReg);

  // Guard against negative values that result in 0 due the precision loss.
  as_sltiu(SecondScratchReg, output, 1);
  ma_or(ScratchRegister, SecondScratchReg);

  ma_b(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);

  bind(&done);

  if (isSaturating) {
    bind(oolRejoin);
  }
}

void MacroAssemblerMIPS64Compat::wasmLoadI64Impl(
    const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
    Register ptrScratch, Register64 output, Register tmp) {
  uint32_t offset = access.offset();
  MOZ_ASSERT(offset < asMasm().wasmMaxOffsetGuardLimit());
  MOZ_ASSERT_IF(offset, ptrScratch != InvalidReg);

  MOZ_ASSERT(!access.isZeroExtendSimd128Load());
  MOZ_ASSERT(!access.isSplatSimd128Load());
  MOZ_ASSERT(!access.isWidenSimd128Load());

  // Maybe add the offset.
  if (offset) {
    asMasm().addPtr(Imm32(offset), ptrScratch);
    ptr = ptrScratch;
  }

  unsigned byteSize = access.byteSize();
  bool isSigned;

  switch (access.type()) {
    case Scalar::Int8:
      isSigned = true;
      break;
    case Scalar::Uint8:
      isSigned = false;
      break;
    case Scalar::Int16:
      isSigned = true;
      break;
    case Scalar::Uint16:
      isSigned = false;
      break;
    case Scalar::Int32:
      isSigned = true;
      break;
    case Scalar::Uint32:
      isSigned = false;
      break;
    case Scalar::Int64:
      isSigned = true;
      break;
    default:
      MOZ_CRASH("unexpected array type");
  }

  BaseIndex address(memoryBase, ptr, TimesOne);
  if (IsUnaligned(access)) {
    MOZ_ASSERT(tmp != InvalidReg);
    asMasm().ma_load_unaligned(access, output.reg, address, tmp,
                               static_cast<LoadStoreSize>(8 * byteSize),
                               isSigned ? SignExtend : ZeroExtend);
    return;
  }

  asMasm().memoryBarrierBefore(access.sync());
  asMasm().ma_load(output.reg, address,
                   static_cast<LoadStoreSize>(8 * byteSize),
                   isSigned ? SignExtend : ZeroExtend);
  asMasm().append(access, asMasm().size() - 4);
  asMasm().memoryBarrierAfter(access.sync());
}

void MacroAssemblerMIPS64Compat::wasmStoreI64Impl(
    const wasm::MemoryAccessDesc& access, Register64 value, Register memoryBase,
    Register ptr, Register ptrScratch, Register tmp) {
  uint32_t offset = access.offset();
  MOZ_ASSERT(offset < asMasm().wasmMaxOffsetGuardLimit());
  MOZ_ASSERT_IF(offset, ptrScratch != InvalidReg);

  // Maybe add the offset.
  if (offset) {
    asMasm().addPtr(Imm32(offset), ptrScratch);
    ptr = ptrScratch;
  }

  unsigned byteSize = access.byteSize();
  bool isSigned;
  switch (access.type()) {
    case Scalar::Int8:
      isSigned = true;
      break;
    case Scalar::Uint8:
      isSigned = false;
      break;
    case Scalar::Int16:
      isSigned = true;
      break;
    case Scalar::Uint16:
      isSigned = false;
      break;
    case Scalar::Int32:
      isSigned = true;
      break;
    case Scalar::Uint32:
      isSigned = false;
      break;
    case Scalar::Int64:
      isSigned = true;
      break;
    default:
      MOZ_CRASH("unexpected array type");
  }

  BaseIndex address(memoryBase, ptr, TimesOne);

  if (IsUnaligned(access)) {
    MOZ_ASSERT(tmp != InvalidReg);
    asMasm().ma_store_unaligned(access, value.reg, address, tmp,
                                static_cast<LoadStoreSize>(8 * byteSize),
                                isSigned ? SignExtend : ZeroExtend);
    return;
  }

  asMasm().memoryBarrierBefore(access.sync());
  asMasm().ma_store(value.reg, address,
                    static_cast<LoadStoreSize>(8 * byteSize),
                    isSigned ? SignExtend : ZeroExtend);
  asMasm().append(access, asMasm().size() - 4);
  asMasm().memoryBarrierAfter(access.sync());
}

template <typename T>
static void CompareExchange64(MacroAssembler& masm,
                              const wasm::MemoryAccessDesc* access,
                              const Synchronization& sync, const T& mem,
                              Register64 expect, Register64 replace,
                              Register64 output) {
  MOZ_ASSERT(expect != output && replace != output);
  masm.computeEffectiveAddress(mem, SecondScratchReg);

  Label tryAgain;
  Label exit;

  masm.memoryBarrierBefore(sync);

  masm.bind(&tryAgain);

  if (access) {
    masm.append(*access, masm.size());
  }
  masm.as_lld(output.reg, SecondScratchReg, 0);

  masm.ma_b(output.reg, expect.reg, &exit, Assembler::NotEqual, ShortJump);
  masm.movePtr(replace.reg, ScratchRegister);
  masm.as_scd(ScratchRegister, SecondScratchReg, 0);
  masm.ma_b(ScratchRegister, ScratchRegister, &tryAgain, Assembler::Zero,
            ShortJump);

  masm.memoryBarrierAfter(sync);

  masm.bind(&exit);
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const Address& mem,
                                           Register64 expect,
                                           Register64 replace,
                                           Register64 output) {
  CompareExchange64(*this, &access, access.sync(), mem, expect, replace,
                    output);
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const BaseIndex& mem,
                                           Register64 expect,
                                           Register64 replace,
                                           Register64 output) {
  CompareExchange64(*this, &access, access.sync(), mem, expect, replace,
                    output);
}

void MacroAssembler::compareExchange64(const Synchronization& sync,
                                       const Address& mem, Register64 expect,
                                       Register64 replace, Register64 output) {
  CompareExchange64(*this, nullptr, sync, mem, expect, replace, output);
}

void MacroAssembler::compareExchange64(const Synchronization& sync,
                                       const BaseIndex& mem, Register64 expect,
                                       Register64 replace, Register64 output) {
  CompareExchange64(*this, nullptr, sync, mem, expect, replace, output);
}

template <typename T>
static void AtomicExchange64(MacroAssembler& masm,
                             const wasm::MemoryAccessDesc* access,
                             const Synchronization& sync, const T& mem,
                             Register64 value, Register64 output) {
  MOZ_ASSERT(value != output);
  masm.computeEffectiveAddress(mem, SecondScratchReg);

  Label tryAgain;

  masm.memoryBarrierBefore(sync);

  masm.bind(&tryAgain);

  if (access) {
    masm.append(*access, masm.size());
  }

  masm.as_lld(output.reg, SecondScratchReg, 0);
  masm.movePtr(value.reg, ScratchRegister);
  masm.as_scd(ScratchRegister, SecondScratchReg, 0);
  masm.ma_b(ScratchRegister, ScratchRegister, &tryAgain, Assembler::Zero,
            ShortJump);

  masm.memoryBarrierAfter(sync);
}

template <typename T>
static void WasmAtomicExchange64(MacroAssembler& masm,
                                 const wasm::MemoryAccessDesc& access,
                                 const T& mem, Register64 value,
                                 Register64 output) {
  AtomicExchange64(masm, &access, access.sync(), mem, value, output);
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const Address& mem, Register64 src,
                                          Register64 output) {
  WasmAtomicExchange64(*this, access, mem, src, output);
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const BaseIndex& mem, Register64 src,
                                          Register64 output) {
  WasmAtomicExchange64(*this, access, mem, src, output);
}

void MacroAssembler::atomicExchange64(const Synchronization& sync,
                                      const Address& mem, Register64 value,
                                      Register64 output) {
  AtomicExchange64(*this, nullptr, sync, mem, value, output);
}

void MacroAssembler::atomicExchange64(const Synchronization& sync,
                                      const BaseIndex& mem, Register64 value,
                                      Register64 output) {
  AtomicExchange64(*this, nullptr, sync, mem, value, output);
}

template <typename T>
static void AtomicFetchOp64(MacroAssembler& masm,
                            const wasm::MemoryAccessDesc* access,
                            const Synchronization& sync, AtomicOp op,
                            Register64 value, const T& mem, Register64 temp,
                            Register64 output) {
  MOZ_ASSERT(value != output);
  MOZ_ASSERT(value != temp);
  masm.computeEffectiveAddress(mem, SecondScratchReg);

  Label tryAgain;

  masm.memoryBarrierBefore(sync);

  masm.bind(&tryAgain);
  if (access) {
    masm.append(*access, masm.size());
  }

  masm.as_lld(output.reg, SecondScratchReg, 0);

  switch (op) {
    case AtomicFetchAddOp:
      masm.as_daddu(temp.reg, output.reg, value.reg);
      break;
    case AtomicFetchSubOp:
      masm.as_dsubu(temp.reg, output.reg, value.reg);
      break;
    case AtomicFetchAndOp:
      masm.as_and(temp.reg, output.reg, value.reg);
      break;
    case AtomicFetchOrOp:
      masm.as_or(temp.reg, output.reg, value.reg);
      break;
    case AtomicFetchXorOp:
      masm.as_xor(temp.reg, output.reg, value.reg);
      break;
    default:
      MOZ_CRASH();
  }

  masm.as_scd(temp.reg, SecondScratchReg, 0);
  masm.ma_b(temp.reg, temp.reg, &tryAgain, Assembler::Zero, ShortJump);

  masm.memoryBarrierAfter(sync);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const Address& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp64(*this, &access, access.sync(), op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const BaseIndex& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp64(*this, &access, access.sync(), op, value, mem, temp, output);
}

void MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                                     Register64 value, const Address& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp64(*this, nullptr, sync, op, value, mem, temp, output);
}

void MacroAssembler::atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                                     Register64 value, const BaseIndex& mem,
                                     Register64 temp, Register64 output) {
  AtomicFetchOp64(*this, nullptr, sync, op, value, mem, temp, output);
}

void MacroAssembler::atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                                      Register64 value, const Address& mem,
                                      Register64 temp) {
  AtomicFetchOp64(*this, nullptr, sync, op, value, mem, temp, temp);
}

void MacroAssembler::atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                                      Register64 value, const BaseIndex& mem,
                                      Register64 temp) {
  AtomicFetchOp64(*this, nullptr, sync, op, value, mem, temp, temp);
}

// ========================================================================
// Convert floating point.

void MacroAssembler::convertInt64ToDouble(Register64 src, FloatRegister dest) {
  as_dmtc1(src.reg, dest);
  as_cvtdl(dest, dest);
}

void MacroAssembler::convertInt64ToFloat32(Register64 src, FloatRegister dest) {
  as_dmtc1(src.reg, dest);
  as_cvtsl(dest, dest);
}

bool MacroAssembler::convertUInt64ToDoubleNeedsTemp() { return false; }

void MacroAssembler::convertUInt64ToDouble(Register64 src, FloatRegister dest,
                                           Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  MacroAssemblerSpecific::convertUInt64ToDouble(src.reg, dest);
}

void MacroAssembler::convertUInt64ToFloat32(Register64 src_, FloatRegister dest,
                                            Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());

  Register src = src_.reg;
  Label positive, done;
  ma_b(src, src, &positive, NotSigned, ShortJump);

  MOZ_ASSERT(src != ScratchRegister);
  MOZ_ASSERT(src != SecondScratchReg);

  ma_and(ScratchRegister, src, Imm32(1));
  ma_dsrl(SecondScratchReg, src, Imm32(1));
  ma_or(ScratchRegister, SecondScratchReg);
  as_dmtc1(ScratchRegister, dest);
  as_cvtsl(dest, dest);
  addFloat32(dest, dest);
  ma_b(&done, ShortJump);

  bind(&positive);
  as_dmtc1(src, dest);
  as_cvtsl(dest, dest);

  bind(&done);
}

//}}} check_macroassembler_style

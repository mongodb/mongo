/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips32/MacroAssembler-mips32.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/MacroAssembler.h"
#include "jit/mips32/Simulator-mips32.h"
#include "jit/MoveEmitter.h"
#include "jit/SharedICRegisters.h"
#include "util/Memory.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace jit;

using mozilla::Abs;

static const int32_t PAYLOAD_OFFSET = NUNBOX32_PAYLOAD_OFFSET;
static const int32_t TAG_OFFSET = NUNBOX32_TYPE_OFFSET;

static_assert(sizeof(intptr_t) == 4, "Not 64-bit clean.");

void MacroAssemblerMIPSCompat::convertBoolToInt32(Register src, Register dest) {
  // Note that C++ bool is only 1 byte, so zero extend it to clear the
  // higher-order bits.
  ma_and(dest, src, Imm32(0xff));
}

void MacroAssemblerMIPSCompat::convertInt32ToDouble(Register src,
                                                    FloatRegister dest) {
  as_mtc1(src, dest);
  as_cvtdw(dest, dest);
}

void MacroAssemblerMIPSCompat::convertInt32ToDouble(const Address& src,
                                                    FloatRegister dest) {
  ma_ls(dest, src);
  as_cvtdw(dest, dest);
}

void MacroAssemblerMIPSCompat::convertInt32ToDouble(const BaseIndex& src,
                                                    FloatRegister dest) {
  computeScaledAddress(src, ScratchRegister);
  convertInt32ToDouble(Address(ScratchRegister, src.offset), dest);
}

void MacroAssemblerMIPSCompat::convertUInt32ToDouble(Register src,
                                                     FloatRegister dest) {
  Label positive, done;
  ma_b(src, src, &positive, NotSigned, ShortJump);

  const uint32_t kExponentShift =
      mozilla::FloatingPoint<double>::kExponentShift - 32;
  const uint32_t kExponent =
      (31 + mozilla::FloatingPoint<double>::kExponentBias);

  ma_ext(SecondScratchReg, src, 31 - kExponentShift, kExponentShift);
  ma_li(ScratchRegister, Imm32(kExponent << kExponentShift));
  ma_or(SecondScratchReg, ScratchRegister);
  ma_sll(ScratchRegister, src, Imm32(kExponentShift + 1));
  moveToDoubleHi(SecondScratchReg, dest);
  moveToDoubleLo(ScratchRegister, dest);

  ma_b(&done, ShortJump);

  bind(&positive);
  convertInt32ToDouble(src, dest);

  bind(&done);
}

void MacroAssemblerMIPSCompat::convertUInt32ToFloat32(Register src,
                                                      FloatRegister dest) {
  Label positive, done;
  ma_b(src, src, &positive, NotSigned, ShortJump);

  const uint32_t kExponentShift =
      mozilla::FloatingPoint<double>::kExponentShift - 32;
  const uint32_t kExponent =
      (31 + mozilla::FloatingPoint<double>::kExponentBias);

  ma_ext(SecondScratchReg, src, 31 - kExponentShift, kExponentShift);
  ma_li(ScratchRegister, Imm32(kExponent << kExponentShift));
  ma_or(SecondScratchReg, ScratchRegister);
  ma_sll(ScratchRegister, src, Imm32(kExponentShift + 1));
  FloatRegister destDouble = dest.asDouble();
  moveToDoubleHi(SecondScratchReg, destDouble);
  moveToDoubleLo(ScratchRegister, destDouble);

  convertDoubleToFloat32(destDouble, dest);

  ma_b(&done, ShortJump);

  bind(&positive);
  convertInt32ToFloat32(src, dest);

  bind(&done);
}

void MacroAssemblerMIPSCompat::convertDoubleToFloat32(FloatRegister src,
                                                      FloatRegister dest) {
  as_cvtsd(dest, src);
}

void MacroAssemblerMIPSCompat::convertDoubleToPtr(FloatRegister src,
                                                  Register dest, Label* fail,
                                                  bool negativeZeroCheck) {
  convertDoubleToInt32(src, dest, fail, negativeZeroCheck);
}

const int CauseBitPos = int(Assembler::CauseI);
const int CauseBitCount = 1 + int(Assembler::CauseV) - int(Assembler::CauseI);
const int CauseIOrVMask = ((1 << int(Assembler::CauseI)) |
                           (1 << int(Assembler::CauseV))) >>
                          int(Assembler::CauseI);

// Checks whether a double is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void MacroAssemblerMIPSCompat::convertDoubleToInt32(FloatRegister src,
                                                    Register dest, Label* fail,
                                                    bool negativeZeroCheck) {
  if (negativeZeroCheck) {
    moveFromDoubleHi(src, dest);
    moveFromDoubleLo(src, SecondScratchReg);
    ma_xor(dest, Imm32(INT32_MIN));
    ma_or(dest, SecondScratchReg);
    ma_b(dest, Imm32(0), fail, Assembler::Equal);
  }

  // Truncate double to int ; if result is inexact or invalid fail.
  as_truncwd(ScratchFloat32Reg, src);
  as_cfc1(ScratchRegister, Assembler::FCSR);
  moveFromFloat32(ScratchFloat32Reg, dest);
  ma_ext(ScratchRegister, ScratchRegister, CauseBitPos, CauseBitCount);
  // Here adding the masking andi instruction just for a precaution.
  // For the instruction of trunc.*.*, the Floating Point Exceptions can be
  // only Inexact, Invalid Operation, Unimplemented Operation.
  // Leaving it maybe is also ok.
  as_andi(ScratchRegister, ScratchRegister, CauseIOrVMask);
  ma_b(ScratchRegister, Imm32(0), fail, Assembler::NotEqual);
}

// Checks whether a float32 is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void MacroAssemblerMIPSCompat::convertFloat32ToInt32(FloatRegister src,
                                                     Register dest, Label* fail,
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

void MacroAssemblerMIPSCompat::convertFloat32ToDouble(FloatRegister src,
                                                      FloatRegister dest) {
  as_cvtds(dest, src);
}

void MacroAssemblerMIPSCompat::convertInt32ToFloat32(Register src,
                                                     FloatRegister dest) {
  as_mtc1(src, dest);
  as_cvtsw(dest, dest);
}

void MacroAssemblerMIPSCompat::convertInt32ToFloat32(const Address& src,
                                                     FloatRegister dest) {
  ma_ls(dest, src);
  as_cvtsw(dest, dest);
}

void MacroAssemblerMIPS::ma_li(Register dest, CodeLabel* label) {
  BufferOffset bo = m_buffer.nextOffset();
  ma_liPatchable(dest, ImmWord(/* placeholder */ 0));
  label->patchAt()->bind(bo.getOffset());
  label->setLinkMode(CodeLabel::MoveImmediate);
}

void MacroAssemblerMIPS::ma_li(Register dest, ImmWord imm) {
  ma_li(dest, Imm32(uint32_t(imm.value)));
}

void MacroAssemblerMIPS::ma_liPatchable(Register dest, ImmPtr imm) {
  ma_liPatchable(dest, ImmWord(uintptr_t(imm.value)));
}

void MacroAssemblerMIPS::ma_liPatchable(Register dest, ImmWord imm) {
  ma_liPatchable(dest, Imm32(int32_t(imm.value)));
}

// Arithmetic-based ops.

// Add.
void MacroAssemblerMIPS::ma_add32TestOverflow(Register rd, Register rs,
                                              Register rt, Label* overflow) {
  MOZ_ASSERT_IF(rs == rd, rs != rt);
  MOZ_ASSERT(rs != ScratchRegister);
  MOZ_ASSERT(rt != ScratchRegister);
  MOZ_ASSERT(rd != rt);
  MOZ_ASSERT(rd != ScratchRegister);
  MOZ_ASSERT(rd != SecondScratchReg);

  if (rs == rt) {
    as_addu(rd, rs, rs);
    as_xor(SecondScratchReg, rs, rd);
    ma_b(SecondScratchReg, Imm32(0), overflow, Assembler::LessThan);
    return;
  }

  // If different sign, no overflow
  as_xor(ScratchRegister, rs, rt);

  as_addu(rd, rs, rt);
  as_nor(ScratchRegister, ScratchRegister, zero);
  // If different sign, then overflow
  as_xor(SecondScratchReg, rt, rd);
  as_and(SecondScratchReg, SecondScratchReg, ScratchRegister);
  ma_b(SecondScratchReg, Imm32(0), overflow, Assembler::LessThan);
}

void MacroAssemblerMIPS::ma_add32TestOverflow(Register rd, Register rs,
                                              Imm32 imm, Label* overflow) {
  MOZ_ASSERT(rs != ScratchRegister);
  MOZ_ASSERT(rs != SecondScratchReg);
  MOZ_ASSERT(rd != ScratchRegister);
  MOZ_ASSERT(rd != SecondScratchReg);

  Register rs_copy = rs;

  if (imm.value > 0) {
    as_nor(ScratchRegister, rs, zero);
  } else if (rs == rd) {
    ma_move(ScratchRegister, rs);
    rs_copy = ScratchRegister;
  }

  if (Imm16::IsInSignedRange(imm.value)) {
    as_addiu(rd, rs, imm.value);
  } else {
    ma_li(SecondScratchReg, imm);
    as_addu(rd, rs, SecondScratchReg);
  }

  if (imm.value > 0) {
    as_and(ScratchRegister, ScratchRegister, rd);
  } else {
    as_nor(SecondScratchReg, rd, zero);
    as_and(ScratchRegister, rs_copy, SecondScratchReg);
  }

  ma_b(ScratchRegister, Imm32(0), overflow, Assembler::LessThan);
}

// Subtract.
void MacroAssemblerMIPS::ma_sub32TestOverflow(Register rd, Register rs,
                                              Register rt, Label* overflow) {
  // The rs == rt case should probably be folded at MIR stage.
  // Happens for Number_isInteger*. Not worth specializing here.
  MOZ_ASSERT_IF(rs == rd, rs != rt);
  MOZ_ASSERT(rs != SecondScratchReg);
  MOZ_ASSERT(rt != SecondScratchReg);
  MOZ_ASSERT(rd != rt);
  MOZ_ASSERT(rd != ScratchRegister);
  MOZ_ASSERT(rd != SecondScratchReg);

  Register rs_copy = rs;

  if (rs == rd) {
    ma_move(SecondScratchReg, rs);
    rs_copy = SecondScratchReg;
  }

  as_subu(rd, rs, rt);
  // If same sign, no overflow
  as_xor(ScratchRegister, rs_copy, rt);
  // If different sign, then overflow
  as_xor(SecondScratchReg, rs_copy, rd);
  as_and(SecondScratchReg, SecondScratchReg, ScratchRegister);
  ma_b(SecondScratchReg, Imm32(0), overflow, Assembler::LessThan);
}

// Memory.

void MacroAssemblerMIPS::ma_load(Register dest, Address address,
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
    as_addu(ScratchRegister, address.base, ScratchRegister);
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
      as_lw(dest, base, encodedOffset);
      break;
    default:
      MOZ_CRASH("Invalid argument for ma_load");
  }
}

void MacroAssemblerMIPS::ma_store(Register data, Address address,
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
    as_addu(ScratchRegister, address.base, ScratchRegister);
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
    default:
      MOZ_CRASH("Invalid argument for ma_store");
  }
}

void MacroAssemblerMIPSCompat::computeScaledAddress(const BaseIndex& address,
                                                    Register dest) {
  int32_t shift = Imm32::ShiftOf(address.scale).value;
  if (shift) {
    ma_sll(ScratchRegister, address.index, Imm32(shift));
    as_addu(dest, address.base, ScratchRegister);
  } else {
    as_addu(dest, address.base, address.index);
  }
}

// Shortcut for when we know we're transferring 32 bits of data.
void MacroAssemblerMIPS::ma_lw(Register data, Address address) {
  ma_load(data, address, SizeWord);
}

void MacroAssemblerMIPS::ma_sw(Register data, Address address) {
  ma_store(data, address, SizeWord);
}

void MacroAssemblerMIPS::ma_sw(Imm32 imm, Address address) {
  MOZ_ASSERT(address.base != ScratchRegister);
  ma_li(ScratchRegister, imm);

  if (Imm16::IsInSignedRange(address.offset)) {
    as_sw(ScratchRegister, address.base, address.offset);
  } else {
    MOZ_ASSERT(address.base != SecondScratchReg);

    ma_li(SecondScratchReg, Imm32(address.offset));
    as_addu(SecondScratchReg, address.base, SecondScratchReg);
    as_sw(ScratchRegister, SecondScratchReg, 0);
  }
}

void MacroAssemblerMIPS::ma_sw(Register data, BaseIndex& address) {
  ma_store(data, address, SizeWord);
}

void MacroAssemblerMIPS::ma_pop(Register r) {
  as_lw(r, StackPointer, 0);
  as_addiu(StackPointer, StackPointer, sizeof(intptr_t));
}

void MacroAssemblerMIPS::ma_push(Register r) {
  if (r == sp) {
    // Pushing sp requires one more instruction.
    ma_move(ScratchRegister, sp);
    r = ScratchRegister;
  }

  as_addiu(StackPointer, StackPointer, -sizeof(intptr_t));
  as_sw(r, StackPointer, 0);
}

// Branches when done from within mips-specific code.
void MacroAssemblerMIPS::ma_b(Register lhs, Address addr, Label* label,
                              Condition c, JumpKind jumpKind) {
  MOZ_ASSERT(lhs != ScratchRegister);
  ma_lw(ScratchRegister, addr);
  ma_b(lhs, ScratchRegister, label, c, jumpKind);
}

void MacroAssemblerMIPS::ma_b(Address addr, Imm32 imm, Label* label,
                              Condition c, JumpKind jumpKind) {
  ma_lw(SecondScratchReg, addr);
  ma_b(SecondScratchReg, imm, label, c, jumpKind);
}

void MacroAssemblerMIPS::ma_b(Address addr, ImmGCPtr imm, Label* label,
                              Condition c, JumpKind jumpKind) {
  ma_lw(SecondScratchReg, addr);
  ma_b(SecondScratchReg, imm, label, c, jumpKind);
}

void MacroAssemblerMIPS::ma_bal(Label* label, DelaySlotFill delaySlotFill) {
  spew("branch .Llabel %p\n", label);
  if (label->bound()) {
    // Generate the long jump for calls because return address has to be
    // the address after the reserved block.
    addLongJump(nextOffset(), BufferOffset(label->offset()));
    ma_liPatchable(ScratchRegister, Imm32(LabelBase::INVALID_OFFSET));
    as_jalr(ScratchRegister);
    if (delaySlotFill == FillDelaySlot) {
      as_nop();
    }
    return;
  }

  // Second word holds a pointer to the next branch in label's chain.
  uint32_t nextInChain =
      label->used() ? label->offset() : LabelBase::INVALID_OFFSET;

  // Make the whole branch continous in the buffer.
  m_buffer.ensureSpace(4 * sizeof(uint32_t));

  spew("bal .Llabel %p\n", label);
  BufferOffset bo = writeInst(getBranchCode(BranchIsCall).encode());
  writeInst(nextInChain);
  if (!oom()) {
    label->use(bo.getOffset());
  }
  // Leave space for long jump.
  as_nop();
  if (delaySlotFill == FillDelaySlot) {
    as_nop();
  }
}

void MacroAssemblerMIPS::branchWithCode(InstImm code, Label* label,
                                        JumpKind jumpKind) {
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
      ma_liPatchable(ScratchRegister, Imm32(LabelBase::INVALID_OFFSET));
      as_jr(ScratchRegister);
      as_nop();
      return;
    }

    // Handle long conditional branch
    spew("invert branch .Llabel %p", label);
    InstImm code_r = invertBranch(code, BOffImm16(5 * sizeof(uint32_t)));
#ifdef JS_JITSPEW
    decodeBranchInstAndSpew(code_r);
#endif
    writeInst(code_r.encode());

    // No need for a "nop" here because we can clobber scratch.
    addLongJump(nextOffset(), BufferOffset(label->offset()));
    ma_liPatchable(ScratchRegister, Imm32(LabelBase::INVALID_OFFSET));
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

  // Make the whole branch continous in the buffer.
  m_buffer.ensureSpace((conditional ? 5 : 4) * sizeof(uint32_t));

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
  if (conditional) {
    as_nop();
  }
}

void MacroAssemblerMIPSCompat::cmp64Set(Condition cond, Register64 lhs,
                                        Imm64 val, Register dest) {
  if (val.value == 0) {
    switch (cond) {
      case Assembler::Equal:
      case Assembler::BelowOrEqual:
        as_or(dest, lhs.high, lhs.low);
        as_sltiu(dest, dest, 1);
        break;
      case Assembler::NotEqual:
      case Assembler::Above:
        as_or(dest, lhs.high, lhs.low);
        as_sltu(dest, zero, dest);
        break;
      case Assembler::LessThan:
      case Assembler::GreaterThanOrEqual:
        as_slt(dest, lhs.high, zero);
        if (cond == Assembler::GreaterThanOrEqual) {
          as_xori(dest, dest, 1);
        }
        break;
      case Assembler::GreaterThan:
      case Assembler::LessThanOrEqual:
        as_or(SecondScratchReg, lhs.high, lhs.low);
        as_sra(ScratchRegister, lhs.high, 31);
        as_sltu(dest, ScratchRegister, SecondScratchReg);
        if (cond == Assembler::LessThanOrEqual) {
          as_xori(dest, dest, 1);
        }
        break;
      case Assembler::Below:
      case Assembler::AboveOrEqual:
        as_ori(dest, zero, cond == Assembler::AboveOrEqual ? 1 : 0);
        break;
      default:
        MOZ_CRASH("Condition code not supported");
        break;
    }
    return;
  }

  Condition c = ma_cmp64(cond, lhs, val, dest);

  switch (cond) {
    // For Equal/NotEqual cond ma_cmp64 dest holds non boolean result.
    case Assembler::Equal:
      as_sltiu(dest, dest, 1);
      break;
    case Assembler::NotEqual:
      as_sltu(dest, zero, dest);
      break;
    default:
      if (c == Assembler::Zero) as_xori(dest, dest, 1);
      break;
  }
}

void MacroAssemblerMIPSCompat::cmp64Set(Condition cond, Register64 lhs,
                                        Register64 rhs, Register dest) {
  Condition c = ma_cmp64(cond, lhs, rhs, dest);

  switch (cond) {
    // For Equal/NotEqual cond ma_cmp64 dest holds non boolean result.
    case Assembler::Equal:
      as_sltiu(dest, dest, 1);
      break;
    case Assembler::NotEqual:
      as_sltu(dest, zero, dest);
      break;
    default:
      if (c == Assembler::Zero) as_xori(dest, dest, 1);
      break;
  }
}

Assembler::Condition MacroAssemblerMIPSCompat::ma_cmp64(Condition cond,
                                                        Register64 lhs,
                                                        Register64 rhs,
                                                        Register dest) {
  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
      as_xor(SecondScratchReg, lhs.high, rhs.high);
      as_xor(ScratchRegister, lhs.low, rhs.low);
      as_or(dest, SecondScratchReg, ScratchRegister);
      return (cond == Assembler::Equal) ? Assembler::Zero : Assembler::NonZero;
      break;
    case Assembler::LessThan:
    case Assembler::GreaterThanOrEqual:
      as_slt(SecondScratchReg, rhs.high, lhs.high);
      as_sltu(ScratchRegister, lhs.low, rhs.low);
      as_slt(SecondScratchReg, SecondScratchReg, ScratchRegister);
      as_slt(ScratchRegister, lhs.high, rhs.high);
      as_or(dest, ScratchRegister, SecondScratchReg);
      return (cond == Assembler::GreaterThanOrEqual) ? Assembler::Zero
                                                     : Assembler::NonZero;
      break;
    case Assembler::GreaterThan:
    case Assembler::LessThanOrEqual:
      as_slt(SecondScratchReg, lhs.high, rhs.high);
      as_sltu(ScratchRegister, rhs.low, lhs.low);
      as_slt(SecondScratchReg, SecondScratchReg, ScratchRegister);
      as_slt(ScratchRegister, rhs.high, lhs.high);
      as_or(dest, ScratchRegister, SecondScratchReg);
      return (cond == Assembler::LessThanOrEqual) ? Assembler::Zero
                                                  : Assembler::NonZero;
      break;
    case Assembler::Below:
    case Assembler::AboveOrEqual:
      as_sltu(SecondScratchReg, rhs.high, lhs.high);
      as_sltu(ScratchRegister, lhs.low, rhs.low);
      as_slt(SecondScratchReg, SecondScratchReg, ScratchRegister);
      as_sltu(ScratchRegister, lhs.high, rhs.high);
      as_or(dest, ScratchRegister, SecondScratchReg);
      return (cond == Assembler::AboveOrEqual) ? Assembler::Zero
                                               : Assembler::NonZero;
      break;
    case Assembler::Above:
    case Assembler::BelowOrEqual:
      as_sltu(SecondScratchReg, lhs.high, rhs.high);
      as_sltu(ScratchRegister, rhs.low, lhs.low);
      as_slt(SecondScratchReg, SecondScratchReg, ScratchRegister);
      as_sltu(ScratchRegister, rhs.high, lhs.high);
      as_or(dest, ScratchRegister, SecondScratchReg);
      return (cond == Assembler::BelowOrEqual) ? Assembler::Zero
                                               : Assembler::NonZero;
      break;
    default:
      MOZ_CRASH("Condition code not supported");
      break;
  }
}

Assembler::Condition MacroAssemblerMIPSCompat::ma_cmp64(Condition cond,
                                                        Register64 lhs,
                                                        Imm64 val,
                                                        Register dest) {
  MOZ_ASSERT(val.value != 0);

  switch (cond) {
    case Assembler::Equal:
    case Assembler::NotEqual:
      ma_xor(SecondScratchReg, lhs.high, val.hi());
      ma_xor(ScratchRegister, lhs.low, val.low());
      as_or(dest, SecondScratchReg, ScratchRegister);
      return (cond == Assembler::Equal) ? Assembler::Zero : Assembler::NonZero;
      break;
    case Assembler::LessThan:
    case Assembler::GreaterThanOrEqual:
      ma_li(SecondScratchReg, val.hi());
      as_slt(ScratchRegister, lhs.high, SecondScratchReg);
      as_slt(SecondScratchReg, SecondScratchReg, lhs.high);
      as_subu(SecondScratchReg, SecondScratchReg, ScratchRegister);
      ma_li(ScratchRegister, val.low());
      as_sltu(ScratchRegister, lhs.low, ScratchRegister);
      as_slt(dest, SecondScratchReg, ScratchRegister);
      return (cond == Assembler::GreaterThanOrEqual) ? Assembler::Zero
                                                     : Assembler::NonZero;
      break;
    case Assembler::GreaterThan:
    case Assembler::LessThanOrEqual:
      ma_li(SecondScratchReg, val.hi());
      as_slt(ScratchRegister, SecondScratchReg, lhs.high);
      as_slt(SecondScratchReg, lhs.high, SecondScratchReg);
      as_subu(SecondScratchReg, SecondScratchReg, ScratchRegister);
      ma_li(ScratchRegister, val.low());
      as_sltu(ScratchRegister, ScratchRegister, lhs.low);
      as_slt(dest, SecondScratchReg, ScratchRegister);
      return (cond == Assembler::LessThanOrEqual) ? Assembler::Zero
                                                  : Assembler::NonZero;
      break;
    case Assembler::Below:
    case Assembler::AboveOrEqual:
      ma_li(SecondScratchReg, val.hi());
      as_sltu(ScratchRegister, lhs.high, SecondScratchReg);
      as_sltu(SecondScratchReg, SecondScratchReg, lhs.high);
      as_subu(SecondScratchReg, SecondScratchReg, ScratchRegister);
      ma_li(ScratchRegister, val.low());
      as_sltu(ScratchRegister, lhs.low, ScratchRegister);
      as_slt(dest, SecondScratchReg, ScratchRegister);
      return (cond == Assembler::AboveOrEqual) ? Assembler::Zero
                                               : Assembler::NonZero;
      break;
    case Assembler::Above:
    case Assembler::BelowOrEqual:
      ma_li(SecondScratchReg, val.hi());
      as_sltu(ScratchRegister, SecondScratchReg, lhs.high);
      as_sltu(SecondScratchReg, lhs.high, SecondScratchReg);
      as_subu(SecondScratchReg, SecondScratchReg, ScratchRegister);
      ma_li(ScratchRegister, val.low());
      as_sltu(ScratchRegister, ScratchRegister, lhs.low);
      as_slt(dest, SecondScratchReg, ScratchRegister);
      return (cond == Assembler::BelowOrEqual) ? Assembler::Zero
                                               : Assembler::NonZero;
      break;
    default:
      MOZ_CRASH("Condition code not supported");
      break;
  }
}

// fp instructions
void MacroAssemblerMIPS::ma_lid(FloatRegister dest, double value) {
  struct DoubleStruct {
    uint32_t lo;
    uint32_t hi;
  };
  DoubleStruct intStruct = mozilla::BitwiseCast<DoubleStruct>(value);
#if MOZ_BIG_ENDIAN()
  std::swap(intStruct.hi, intStruct.lo);
#endif

  // put hi part of 64 bit value into the odd register
  if (intStruct.hi == 0) {
    moveToDoubleHi(zero, dest);
  } else {
    ma_li(ScratchRegister, Imm32(intStruct.hi));
    moveToDoubleHi(ScratchRegister, dest);
  }

  // put low part of 64 bit value into the even register
  if (intStruct.lo == 0) {
    moveToDoubleLo(zero, dest);
  } else {
    ma_li(ScratchRegister, Imm32(intStruct.lo));
    moveToDoubleLo(ScratchRegister, dest);
  }
}

void MacroAssemblerMIPS::ma_mv(FloatRegister src, ValueOperand dest) {
  moveFromDoubleLo(src, dest.payloadReg());
  moveFromDoubleHi(src, dest.typeReg());
}

void MacroAssemblerMIPS::ma_mv(ValueOperand src, FloatRegister dest) {
  moveToDoubleLo(src.payloadReg(), dest);
  moveToDoubleHi(src.typeReg(), dest);
}

void MacroAssemblerMIPS::ma_ls(FloatRegister ft, Address address) {
  if (Imm16::IsInSignedRange(address.offset)) {
    as_lwc1(ft, address.base, address.offset);
  } else {
    MOZ_ASSERT(address.base != ScratchRegister);
    ma_li(ScratchRegister, Imm32(address.offset));
    if (isLoongson()) {
      as_gslsx(ft, address.base, ScratchRegister, 0);
    } else {
      as_addu(ScratchRegister, address.base, ScratchRegister);
      as_lwc1(ft, ScratchRegister, 0);
    }
  }
}

void MacroAssemblerMIPS::ma_ld(FloatRegister ft, Address address) {
  if (Imm16::IsInSignedRange(address.offset)) {
    as_ldc1(ft, address.base, address.offset);
  } else {
    MOZ_ASSERT(address.base != ScratchRegister);
    ma_li(ScratchRegister, Imm32(address.offset));
    if (isLoongson()) {
      as_gsldx(ft, address.base, ScratchRegister, 0);
    } else {
      as_addu(ScratchRegister, address.base, ScratchRegister);
      as_ldc1(ft, ScratchRegister, 0);
    }
  }
}

void MacroAssemblerMIPS::ma_sd(FloatRegister ft, Address address) {
  if (Imm16::IsInSignedRange(address.offset)) {
    as_sdc1(ft, address.base, address.offset);
  } else {
    MOZ_ASSERT(address.base != ScratchRegister);
    ma_li(ScratchRegister, Imm32(address.offset));
    if (isLoongson()) {
      as_gssdx(ft, address.base, ScratchRegister, 0);
    } else {
      as_addu(ScratchRegister, address.base, ScratchRegister);
      as_sdc1(ft, ScratchRegister, 0);
    }
  }
}

void MacroAssemblerMIPS::ma_ss(FloatRegister ft, Address address) {
  if (Imm16::IsInSignedRange(address.offset)) {
    as_swc1(ft, address.base, address.offset);
  } else {
    MOZ_ASSERT(address.base != ScratchRegister);
    ma_li(ScratchRegister, Imm32(address.offset));
    if (isLoongson()) {
      as_gsssx(ft, address.base, ScratchRegister, 0);
    } else {
      as_addu(ScratchRegister, address.base, ScratchRegister);
      as_swc1(ft, ScratchRegister, 0);
    }
  }
}

void MacroAssemblerMIPS::ma_ldc1WordAligned(FloatRegister ft, Register base,
                                            int32_t off) {
  MOZ_ASSERT(Imm16::IsInSignedRange(off + PAYLOAD_OFFSET) &&
             Imm16::IsInSignedRange(off + TAG_OFFSET));

  as_lwc1(ft, base, off + PAYLOAD_OFFSET);
  as_lwc1(getOddPair(ft), base, off + TAG_OFFSET);
}

void MacroAssemblerMIPS::ma_sdc1WordAligned(FloatRegister ft, Register base,
                                            int32_t off) {
  MOZ_ASSERT(Imm16::IsInSignedRange(off + PAYLOAD_OFFSET) &&
             Imm16::IsInSignedRange(off + TAG_OFFSET));

  as_swc1(ft, base, off + PAYLOAD_OFFSET);
  as_swc1(getOddPair(ft), base, off + TAG_OFFSET);
}

void MacroAssemblerMIPS::ma_pop(FloatRegister f) {
  if (f.isDouble()) {
    ma_ldc1WordAligned(f, StackPointer, 0);
  } else {
    as_lwc1(f, StackPointer, 0);
  }

  as_addiu(StackPointer, StackPointer, f.size());
}

void MacroAssemblerMIPS::ma_push(FloatRegister f) {
  as_addiu(StackPointer, StackPointer, -f.size());

  if (f.isDouble()) {
    ma_sdc1WordAligned(f, StackPointer, 0);
  } else {
    as_swc1(f, StackPointer, 0);
  }
}

bool MacroAssemblerMIPSCompat::buildOOLFakeExitFrame(void* fakeReturnAddr) {
  uint32_t descriptor = MakeFrameDescriptor(
      asMasm().framePushed(), FrameType::IonJS, ExitFrameLayout::Size());

  asMasm().Push(Imm32(descriptor));  // descriptor_
  asMasm().Push(ImmPtr(fakeReturnAddr));

  return true;
}

void MacroAssemblerMIPSCompat::move32(Imm32 imm, Register dest) {
  ma_li(dest, imm);
}

void MacroAssemblerMIPSCompat::move32(Register src, Register dest) {
  ma_move(dest, src);
}

void MacroAssemblerMIPSCompat::movePtr(Register src, Register dest) {
  ma_move(dest, src);
}
void MacroAssemblerMIPSCompat::movePtr(ImmWord imm, Register dest) {
  ma_li(dest, imm);
}

void MacroAssemblerMIPSCompat::movePtr(ImmGCPtr imm, Register dest) {
  ma_li(dest, imm);
}

void MacroAssemblerMIPSCompat::movePtr(ImmPtr imm, Register dest) {
  movePtr(ImmWord(uintptr_t(imm.value)), dest);
}
void MacroAssemblerMIPSCompat::movePtr(wasm::SymbolicAddress imm,
                                       Register dest) {
  append(wasm::SymbolicAccess(CodeOffset(nextOffset().getOffset()), imm));
  ma_liPatchable(dest, ImmWord(-1));
}

void MacroAssemblerMIPSCompat::load8ZeroExtend(const Address& address,
                                               Register dest) {
  ma_load(dest, address, SizeByte, ZeroExtend);
}

void MacroAssemblerMIPSCompat::load8ZeroExtend(const BaseIndex& src,
                                               Register dest) {
  ma_load(dest, src, SizeByte, ZeroExtend);
}

void MacroAssemblerMIPSCompat::load8SignExtend(const Address& address,
                                               Register dest) {
  ma_load(dest, address, SizeByte, SignExtend);
}

void MacroAssemblerMIPSCompat::load8SignExtend(const BaseIndex& src,
                                               Register dest) {
  ma_load(dest, src, SizeByte, SignExtend);
}

void MacroAssemblerMIPSCompat::load16ZeroExtend(const Address& address,
                                                Register dest) {
  ma_load(dest, address, SizeHalfWord, ZeroExtend);
}

void MacroAssemblerMIPSCompat::load16ZeroExtend(const BaseIndex& src,
                                                Register dest) {
  ma_load(dest, src, SizeHalfWord, ZeroExtend);
}

void MacroAssemblerMIPSCompat::load16SignExtend(const Address& address,
                                                Register dest) {
  ma_load(dest, address, SizeHalfWord, SignExtend);
}

void MacroAssemblerMIPSCompat::load16SignExtend(const BaseIndex& src,
                                                Register dest) {
  ma_load(dest, src, SizeHalfWord, SignExtend);
}

void MacroAssemblerMIPSCompat::load32(const Address& address, Register dest) {
  ma_load(dest, address, SizeWord);
}

void MacroAssemblerMIPSCompat::load32(const BaseIndex& address, Register dest) {
  ma_load(dest, address, SizeWord);
}

void MacroAssemblerMIPSCompat::load32(AbsoluteAddress address, Register dest) {
  movePtr(ImmPtr(address.addr), ScratchRegister);
  load32(Address(ScratchRegister, 0), dest);
}

void MacroAssemblerMIPSCompat::load32(wasm::SymbolicAddress address,
                                      Register dest) {
  movePtr(address, ScratchRegister);
  load32(Address(ScratchRegister, 0), dest);
}

void MacroAssemblerMIPSCompat::loadPtr(const Address& address, Register dest) {
  ma_load(dest, address, SizeWord);
}

void MacroAssemblerMIPSCompat::loadPtr(const BaseIndex& src, Register dest) {
  ma_load(dest, src, SizeWord);
}

void MacroAssemblerMIPSCompat::loadPtr(AbsoluteAddress address, Register dest) {
  movePtr(ImmPtr(address.addr), ScratchRegister);
  loadPtr(Address(ScratchRegister, 0), dest);
}

void MacroAssemblerMIPSCompat::loadPtr(wasm::SymbolicAddress address,
                                       Register dest) {
  movePtr(address, ScratchRegister);
  loadPtr(Address(ScratchRegister, 0), dest);
}

void MacroAssemblerMIPSCompat::loadPrivate(const Address& address,
                                           Register dest) {
  ma_lw(dest, Address(address.base, address.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::loadUnalignedDouble(
    const wasm::MemoryAccessDesc& access, const BaseIndex& src, Register temp,
    FloatRegister dest) {
  MOZ_ASSERT(MOZ_LITTLE_ENDIAN(), "Wasm-only; wasm is disabled on big-endian.");
  computeScaledAddress(src, SecondScratchReg);

  BufferOffset load;
  if (Imm16::IsInSignedRange(src.offset) &&
      Imm16::IsInSignedRange(src.offset + 7)) {
    load = as_lwl(temp, SecondScratchReg, src.offset + INT64LOW_OFFSET + 3);
    as_lwr(temp, SecondScratchReg, src.offset + INT64LOW_OFFSET);
    append(access, load.getOffset());
    moveToDoubleLo(temp, dest);
    load = as_lwl(temp, SecondScratchReg, src.offset + INT64HIGH_OFFSET + 3);
    as_lwr(temp, SecondScratchReg, src.offset + INT64HIGH_OFFSET);
    append(access, load.getOffset());
    moveToDoubleHi(temp, dest);
  } else {
    ma_li(ScratchRegister, Imm32(src.offset));
    as_daddu(ScratchRegister, SecondScratchReg, ScratchRegister);
    load = as_lwl(temp, ScratchRegister, INT64LOW_OFFSET + 3);
    as_lwr(temp, ScratchRegister, INT64LOW_OFFSET);
    append(access, load.getOffset());
    moveToDoubleLo(temp, dest);
    load = as_lwl(temp, ScratchRegister, INT64HIGH_OFFSET + 3);
    as_lwr(temp, ScratchRegister, INT64HIGH_OFFSET);
    append(access, load.getOffset());
    moveToDoubleHi(temp, dest);
  }
}

void MacroAssemblerMIPSCompat::loadUnalignedFloat32(
    const wasm::MemoryAccessDesc& access, const BaseIndex& src, Register temp,
    FloatRegister dest) {
  MOZ_ASSERT(MOZ_LITTLE_ENDIAN(), "Wasm-only; wasm is disabled on big-endian.");
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

void MacroAssemblerMIPSCompat::store8(Imm32 imm, const Address& address) {
  ma_li(SecondScratchReg, imm);
  ma_store(SecondScratchReg, address, SizeByte);
}

void MacroAssemblerMIPSCompat::store8(Register src, const Address& address) {
  ma_store(src, address, SizeByte);
}

void MacroAssemblerMIPSCompat::store8(Imm32 imm, const BaseIndex& dest) {
  ma_store(imm, dest, SizeByte);
}

void MacroAssemblerMIPSCompat::store8(Register src, const BaseIndex& dest) {
  ma_store(src, dest, SizeByte);
}

void MacroAssemblerMIPSCompat::store16(Imm32 imm, const Address& address) {
  ma_li(SecondScratchReg, imm);
  ma_store(SecondScratchReg, address, SizeHalfWord);
}

void MacroAssemblerMIPSCompat::store16(Register src, const Address& address) {
  ma_store(src, address, SizeHalfWord);
}

void MacroAssemblerMIPSCompat::store16(Imm32 imm, const BaseIndex& dest) {
  ma_store(imm, dest, SizeHalfWord);
}

void MacroAssemblerMIPSCompat::store16(Register src, const BaseIndex& address) {
  ma_store(src, address, SizeHalfWord);
}

void MacroAssemblerMIPSCompat::store32(Register src, AbsoluteAddress address) {
  movePtr(ImmPtr(address.addr), ScratchRegister);
  store32(src, Address(ScratchRegister, 0));
}

void MacroAssemblerMIPSCompat::store32(Register src, const Address& address) {
  ma_store(src, address, SizeWord);
}

void MacroAssemblerMIPSCompat::store32(Imm32 src, const Address& address) {
  move32(src, SecondScratchReg);
  ma_store(SecondScratchReg, address, SizeWord);
}

void MacroAssemblerMIPSCompat::store32(Imm32 imm, const BaseIndex& dest) {
  ma_store(imm, dest, SizeWord);
}

void MacroAssemblerMIPSCompat::store32(Register src, const BaseIndex& dest) {
  ma_store(src, dest, SizeWord);
}

template <typename T>
void MacroAssemblerMIPSCompat::storePtr(ImmWord imm, T address) {
  ma_li(SecondScratchReg, imm);
  ma_store(SecondScratchReg, address, SizeWord);
}

template void MacroAssemblerMIPSCompat::storePtr<Address>(ImmWord imm,
                                                          Address address);
template void MacroAssemblerMIPSCompat::storePtr<BaseIndex>(ImmWord imm,
                                                            BaseIndex address);

template <typename T>
void MacroAssemblerMIPSCompat::storePtr(ImmPtr imm, T address) {
  storePtr(ImmWord(uintptr_t(imm.value)), address);
}

template void MacroAssemblerMIPSCompat::storePtr<Address>(ImmPtr imm,
                                                          Address address);
template void MacroAssemblerMIPSCompat::storePtr<BaseIndex>(ImmPtr imm,
                                                            BaseIndex address);

template <typename T>
void MacroAssemblerMIPSCompat::storePtr(ImmGCPtr imm, T address) {
  movePtr(imm, SecondScratchReg);
  storePtr(SecondScratchReg, address);
}

template void MacroAssemblerMIPSCompat::storePtr<Address>(ImmGCPtr imm,
                                                          Address address);
template void MacroAssemblerMIPSCompat::storePtr<BaseIndex>(ImmGCPtr imm,
                                                            BaseIndex address);

void MacroAssemblerMIPSCompat::storePtr(Register src, const Address& address) {
  ma_store(src, address, SizeWord);
}

void MacroAssemblerMIPSCompat::storePtr(Register src,
                                        const BaseIndex& address) {
  ma_store(src, address, SizeWord);
}

void MacroAssemblerMIPSCompat::storePtr(Register src, AbsoluteAddress dest) {
  movePtr(ImmPtr(dest.addr), ScratchRegister);
  storePtr(src, Address(ScratchRegister, 0));
}

void MacroAssemblerMIPSCompat::storeUnalignedFloat32(
    const wasm::MemoryAccessDesc& access, FloatRegister src, Register temp,
    const BaseIndex& dest) {
  MOZ_ASSERT(MOZ_LITTLE_ENDIAN(), "Wasm-only; wasm is disabled on big-endian.");
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

void MacroAssemblerMIPSCompat::storeUnalignedDouble(
    const wasm::MemoryAccessDesc& access, FloatRegister src, Register temp,
    const BaseIndex& dest) {
  MOZ_ASSERT(MOZ_LITTLE_ENDIAN(), "Wasm-only; wasm is disabled on big-endian.");
  computeScaledAddress(dest, SecondScratchReg);

  BufferOffset store;
  if (Imm16::IsInSignedRange(dest.offset) &&
      Imm16::IsInSignedRange(dest.offset + 7)) {
    moveFromDoubleHi(src, temp);
    store = as_swl(temp, SecondScratchReg, dest.offset + INT64HIGH_OFFSET + 3);
    as_swr(temp, SecondScratchReg, dest.offset + INT64HIGH_OFFSET);
    moveFromDoubleLo(src, temp);
    as_swl(temp, SecondScratchReg, dest.offset + INT64LOW_OFFSET + 3);
    as_swr(temp, SecondScratchReg, dest.offset + INT64LOW_OFFSET);

  } else {
    ma_li(ScratchRegister, Imm32(dest.offset));
    as_daddu(ScratchRegister, SecondScratchReg, ScratchRegister);
    moveFromDoubleHi(src, temp);
    store = as_swl(temp, ScratchRegister, INT64HIGH_OFFSET + 3);
    as_swr(temp, ScratchRegister, INT64HIGH_OFFSET);
    moveFromDoubleLo(src, temp);
    as_swl(temp, ScratchRegister, INT64LOW_OFFSET + 3);
    as_swr(temp, ScratchRegister, INT64LOW_OFFSET);
  }
  append(access, store.getOffset());
}

void MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output) {
  as_roundwd(ScratchDoubleReg, input);
  ma_li(ScratchRegister, Imm32(255));
  as_mfc1(output, ScratchDoubleReg);
  zeroDouble(ScratchDoubleReg);
  as_sltiu(SecondScratchReg, output, 255);
  as_colt(DoubleFloat, ScratchDoubleReg, input);
  // if res > 255; res = 255;
  as_movz(output, ScratchRegister, SecondScratchReg);
  // if !(input > 0); res = 0;
  as_movf(output, zero);
}

// higher level tag testing code
Operand MacroAssemblerMIPSCompat::ToPayload(Operand base) {
  return Operand(Register::FromCode(base.base()), base.disp() + PAYLOAD_OFFSET);
}

Operand MacroAssemblerMIPSCompat::ToType(Operand base) {
  return Operand(Register::FromCode(base.base()), base.disp() + TAG_OFFSET);
}

void MacroAssemblerMIPSCompat::testNullSet(Condition cond,
                                           const ValueOperand& value,
                                           Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp_set(dest, value.typeReg(), ImmType(JSVAL_TYPE_NULL), cond);
}

void MacroAssemblerMIPSCompat::testObjectSet(Condition cond,
                                             const ValueOperand& value,
                                             Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp_set(dest, value.typeReg(), ImmType(JSVAL_TYPE_OBJECT), cond);
}

void MacroAssemblerMIPSCompat::testUndefinedSet(Condition cond,
                                                const ValueOperand& value,
                                                Register dest) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ma_cmp_set(dest, value.typeReg(), ImmType(JSVAL_TYPE_UNDEFINED), cond);
}

// unboxing code
void MacroAssemblerMIPSCompat::unboxNonDouble(const ValueOperand& operand,
                                              Register dest, JSValueType) {
  if (operand.payloadReg() != dest) {
    ma_move(dest, operand.payloadReg());
  }
}

void MacroAssemblerMIPSCompat::unboxNonDouble(const Address& src, Register dest,
                                              JSValueType) {
  ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::unboxNonDouble(const BaseIndex& src,
                                              Register dest, JSValueType) {
  computeScaledAddress(src, SecondScratchReg);
  ma_lw(dest, Address(SecondScratchReg, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::unboxInt32(const ValueOperand& operand,
                                          Register dest) {
  ma_move(dest, operand.payloadReg());
}

void MacroAssemblerMIPSCompat::unboxInt32(const Address& src, Register dest) {
  ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::unboxBoolean(const ValueOperand& operand,
                                            Register dest) {
  ma_move(dest, operand.payloadReg());
}

void MacroAssemblerMIPSCompat::unboxBoolean(const Address& src, Register dest) {
  ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::unboxDouble(const ValueOperand& operand,
                                           FloatRegister dest) {
  moveToDoubleLo(operand.payloadReg(), dest);
  moveToDoubleHi(operand.typeReg(), dest);
}

void MacroAssemblerMIPSCompat::unboxDouble(const Address& src,
                                           FloatRegister dest) {
  ma_lw(ScratchRegister, Address(src.base, src.offset + PAYLOAD_OFFSET));
  moveToDoubleLo(ScratchRegister, dest);
  ma_lw(ScratchRegister, Address(src.base, src.offset + TAG_OFFSET));
  moveToDoubleHi(ScratchRegister, dest);
}

void MacroAssemblerMIPSCompat::unboxDouble(const BaseIndex& src,
                                           FloatRegister dest) {
  loadDouble(src, dest);
}

void MacroAssemblerMIPSCompat::unboxString(const ValueOperand& operand,
                                           Register dest) {
  ma_move(dest, operand.payloadReg());
}

void MacroAssemblerMIPSCompat::unboxString(const Address& src, Register dest) {
  ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::unboxBigInt(const ValueOperand& operand,
                                           Register dest) {
  ma_move(dest, operand.payloadReg());
}

void MacroAssemblerMIPSCompat::unboxBigInt(const Address& src, Register dest) {
  ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::unboxObject(const ValueOperand& src,
                                           Register dest) {
  ma_move(dest, src.payloadReg());
}

void MacroAssemblerMIPSCompat::unboxObject(const Address& src, Register dest) {
  ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::unboxObjectOrNull(const Address& src,
                                                 Register dest) {
  ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::unboxValue(const ValueOperand& src,
                                          AnyRegister dest, JSValueType) {
  if (dest.isFloat()) {
    Label notInt32, end;
    asMasm().branchTestInt32(Assembler::NotEqual, src, &notInt32);
    convertInt32ToDouble(src.payloadReg(), dest.fpu());
    ma_b(&end, ShortJump);
    bind(&notInt32);
    unboxDouble(src, dest.fpu());
    bind(&end);
  } else if (src.payloadReg() != dest.gpr()) {
    ma_move(dest.gpr(), src.payloadReg());
  }
}

void MacroAssemblerMIPSCompat::boxDouble(FloatRegister src,
                                         const ValueOperand& dest,
                                         FloatRegister) {
  moveFromDoubleLo(src, dest.payloadReg());
  moveFromDoubleHi(src, dest.typeReg());
}

void MacroAssemblerMIPSCompat::boxNonDouble(JSValueType type, Register src,
                                            const ValueOperand& dest) {
  if (src != dest.payloadReg()) {
    ma_move(dest.payloadReg(), src);
  }
  ma_li(dest.typeReg(), ImmType(type));
}

void MacroAssemblerMIPSCompat::boolValueToDouble(const ValueOperand& operand,
                                                 FloatRegister dest) {
  convertBoolToInt32(operand.payloadReg(), ScratchRegister);
  convertInt32ToDouble(ScratchRegister, dest);
}

void MacroAssemblerMIPSCompat::int32ValueToDouble(const ValueOperand& operand,
                                                  FloatRegister dest) {
  convertInt32ToDouble(operand.payloadReg(), dest);
}

void MacroAssemblerMIPSCompat::boolValueToFloat32(const ValueOperand& operand,
                                                  FloatRegister dest) {
  convertBoolToInt32(operand.payloadReg(), ScratchRegister);
  convertInt32ToFloat32(ScratchRegister, dest);
}

void MacroAssemblerMIPSCompat::int32ValueToFloat32(const ValueOperand& operand,
                                                   FloatRegister dest) {
  convertInt32ToFloat32(operand.payloadReg(), dest);
}

void MacroAssemblerMIPSCompat::loadConstantFloat32(float f,
                                                   FloatRegister dest) {
  ma_lis(dest, f);
}

void MacroAssemblerMIPSCompat::loadInt32OrDouble(const Address& src,
                                                 FloatRegister dest) {
  Label notInt32, end;
  // If it's an int, convert it to double.
  ma_lw(SecondScratchReg, Address(src.base, src.offset + TAG_OFFSET));
  asMasm().branchTestInt32(Assembler::NotEqual, SecondScratchReg, &notInt32);
  ma_lw(SecondScratchReg, Address(src.base, src.offset + PAYLOAD_OFFSET));
  convertInt32ToDouble(SecondScratchReg, dest);
  ma_b(&end, ShortJump);

  // Not an int, just load as double.
  bind(&notInt32);
  ma_ld(dest, src);
  bind(&end);
}

void MacroAssemblerMIPSCompat::loadInt32OrDouble(Register base, Register index,
                                                 FloatRegister dest,
                                                 int32_t shift) {
  Label notInt32, end;

  // If it's an int, convert it to double.

  computeScaledAddress(BaseIndex(base, index, ShiftToScale(shift)),
                       SecondScratchReg);
  // Since we only have one scratch, we need to stomp over it with the tag.
  load32(Address(SecondScratchReg, TAG_OFFSET), SecondScratchReg);
  asMasm().branchTestInt32(Assembler::NotEqual, SecondScratchReg, &notInt32);

  computeScaledAddress(BaseIndex(base, index, ShiftToScale(shift)),
                       SecondScratchReg);
  load32(Address(SecondScratchReg, PAYLOAD_OFFSET), SecondScratchReg);
  convertInt32ToDouble(SecondScratchReg, dest);
  ma_b(&end, ShortJump);

  // Not an int, just load as double.
  bind(&notInt32);
  // First, recompute the offset that had been stored in the scratch register
  // since the scratch register was overwritten loading in the type.
  computeScaledAddress(BaseIndex(base, index, ShiftToScale(shift)),
                       SecondScratchReg);
  loadDouble(Address(SecondScratchReg, 0), dest);
  bind(&end);
}

void MacroAssemblerMIPSCompat::loadConstantDouble(double dp,
                                                  FloatRegister dest) {
  ma_lid(dest, dp);
}

Register MacroAssemblerMIPSCompat::extractObject(const Address& address,
                                                 Register scratch) {
  ma_lw(scratch, Address(address.base, address.offset + PAYLOAD_OFFSET));
  return scratch;
}

Register MacroAssemblerMIPSCompat::extractTag(const Address& address,
                                              Register scratch) {
  ma_lw(scratch, Address(address.base, address.offset + TAG_OFFSET));
  return scratch;
}

Register MacroAssemblerMIPSCompat::extractTag(const BaseIndex& address,
                                              Register scratch) {
  computeScaledAddress(address, scratch);
  return extractTag(Address(scratch, address.offset), scratch);
}

uint32_t MacroAssemblerMIPSCompat::getType(const Value& val) {
  return val.toNunboxTag();
}

void MacroAssemblerMIPSCompat::moveData(const Value& val, Register data) {
  if (val.isGCThing()) {
    ma_li(data, ImmGCPtr(val.toGCThing()));
  } else {
    ma_li(data, Imm32(val.toNunboxPayload()));
  }
}

/////////////////////////////////////////////////////////////////
// X86/X64-common/ARM/MIPS interface.
/////////////////////////////////////////////////////////////////
void MacroAssemblerMIPSCompat::storeValue(ValueOperand val, Operand dst) {
  storeValue(val, Address(Register::FromCode(dst.base()), dst.disp()));
}

void MacroAssemblerMIPSCompat::storeValue(ValueOperand val,
                                          const BaseIndex& dest) {
  computeScaledAddress(dest, SecondScratchReg);
  storeValue(val, Address(SecondScratchReg, dest.offset));
}

void MacroAssemblerMIPSCompat::storeValue(JSValueType type, Register reg,
                                          BaseIndex dest) {
  computeScaledAddress(dest, ScratchRegister);

  // Make sure that ma_sw doesn't clobber ScratchRegister
  int32_t offset = dest.offset;
  if (!Imm16::IsInSignedRange(offset)) {
    ma_li(SecondScratchReg, Imm32(offset));
    as_addu(ScratchRegister, ScratchRegister, SecondScratchReg);
    offset = 0;
  }

  storeValue(type, reg, Address(ScratchRegister, offset));
}

void MacroAssemblerMIPSCompat::storeValue(ValueOperand val,
                                          const Address& dest) {
  ma_sw(val.payloadReg(), Address(dest.base, dest.offset + PAYLOAD_OFFSET));
  ma_sw(val.typeReg(), Address(dest.base, dest.offset + TAG_OFFSET));
}

void MacroAssemblerMIPSCompat::storeValue(JSValueType type, Register reg,
                                          Address dest) {
  MOZ_ASSERT(dest.base != SecondScratchReg);

  ma_sw(reg, Address(dest.base, dest.offset + PAYLOAD_OFFSET));
  ma_li(SecondScratchReg, ImmTag(JSVAL_TYPE_TO_TAG(type)));
  ma_sw(SecondScratchReg, Address(dest.base, dest.offset + TAG_OFFSET));
}

void MacroAssemblerMIPSCompat::storeValue(const Value& val, Address dest) {
  MOZ_ASSERT(dest.base != SecondScratchReg);

  ma_li(SecondScratchReg, Imm32(getType(val)));
  ma_sw(SecondScratchReg, Address(dest.base, dest.offset + TAG_OFFSET));
  moveData(val, SecondScratchReg);
  ma_sw(SecondScratchReg, Address(dest.base, dest.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::storeValue(const Value& val, BaseIndex dest) {
  computeScaledAddress(dest, ScratchRegister);

  // Make sure that ma_sw doesn't clobber ScratchRegister
  int32_t offset = dest.offset;
  if (!Imm16::IsInSignedRange(offset)) {
    ma_li(SecondScratchReg, Imm32(offset));
    as_addu(ScratchRegister, ScratchRegister, SecondScratchReg);
    offset = 0;
  }
  storeValue(val, Address(ScratchRegister, offset));
}

void MacroAssemblerMIPSCompat::loadValue(const BaseIndex& addr,
                                         ValueOperand val) {
  computeScaledAddress(addr, SecondScratchReg);
  loadValue(Address(SecondScratchReg, addr.offset), val);
}

void MacroAssemblerMIPSCompat::loadValue(Address src, ValueOperand val) {
  // Ensure that loading the payload does not erase the pointer to the
  // Value in memory.
  if (src.base != val.payloadReg()) {
    ma_lw(val.payloadReg(), Address(src.base, src.offset + PAYLOAD_OFFSET));
    ma_lw(val.typeReg(), Address(src.base, src.offset + TAG_OFFSET));
  } else {
    ma_lw(val.typeReg(), Address(src.base, src.offset + TAG_OFFSET));
    ma_lw(val.payloadReg(), Address(src.base, src.offset + PAYLOAD_OFFSET));
  }
}

void MacroAssemblerMIPSCompat::tagValue(JSValueType type, Register payload,
                                        ValueOperand dest) {
  MOZ_ASSERT(dest.typeReg() != dest.payloadReg());
  if (payload != dest.payloadReg()) {
    ma_move(dest.payloadReg(), payload);
  }
  ma_li(dest.typeReg(), ImmType(type));
}

void MacroAssemblerMIPSCompat::pushValue(ValueOperand val) {
  // Allocate stack slots for type and payload. One for each.
  asMasm().subPtr(Imm32(sizeof(Value)), StackPointer);
  // Store type and payload.
  storeValue(val, Address(StackPointer, 0));
}

void MacroAssemblerMIPSCompat::pushValue(const Address& addr) {
  // Allocate stack slots for type and payload. One for each.
  ma_subu(StackPointer, StackPointer, Imm32(sizeof(Value)));
  // If address is based on StackPointer its offset needs to be adjusted
  // to accommodate for previous stack allocation.
  int32_t offset =
      addr.base != StackPointer ? addr.offset : addr.offset + sizeof(Value);
  // Store type and payload.
  ma_lw(ScratchRegister, Address(addr.base, offset + TAG_OFFSET));
  ma_sw(ScratchRegister, Address(StackPointer, TAG_OFFSET));
  ma_lw(ScratchRegister, Address(addr.base, offset + PAYLOAD_OFFSET));
  ma_sw(ScratchRegister, Address(StackPointer, PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::popValue(ValueOperand val) {
  // Load payload and type.
  as_lw(val.payloadReg(), StackPointer, PAYLOAD_OFFSET);
  as_lw(val.typeReg(), StackPointer, TAG_OFFSET);
  // Free stack.
  as_addiu(StackPointer, StackPointer, sizeof(Value));
}

void MacroAssemblerMIPSCompat::storePayload(const Value& val, Address dest) {
  moveData(val, SecondScratchReg);
  ma_sw(SecondScratchReg, Address(dest.base, dest.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerMIPSCompat::storePayload(Register src, Address dest) {
  ma_sw(src, Address(dest.base, dest.offset + PAYLOAD_OFFSET));
  return;
}

void MacroAssemblerMIPSCompat::storePayload(const Value& val,
                                            const BaseIndex& dest) {
  MOZ_ASSERT(dest.offset == 0);

  computeScaledAddress(dest, SecondScratchReg);

  moveData(val, ScratchRegister);

  as_sw(ScratchRegister, SecondScratchReg, NUNBOX32_PAYLOAD_OFFSET);
}

void MacroAssemblerMIPSCompat::storePayload(Register src,
                                            const BaseIndex& dest) {
  MOZ_ASSERT(dest.offset == 0);

  computeScaledAddress(dest, SecondScratchReg);
  as_sw(src, SecondScratchReg, NUNBOX32_PAYLOAD_OFFSET);
}

void MacroAssemblerMIPSCompat::storeTypeTag(ImmTag tag, Address dest) {
  ma_li(SecondScratchReg, tag);
  ma_sw(SecondScratchReg, Address(dest.base, dest.offset + TAG_OFFSET));
}

void MacroAssemblerMIPSCompat::storeTypeTag(ImmTag tag, const BaseIndex& dest) {
  MOZ_ASSERT(dest.offset == 0);

  computeScaledAddress(dest, SecondScratchReg);
  ma_li(ScratchRegister, tag);
  as_sw(ScratchRegister, SecondScratchReg, TAG_OFFSET);
}

void MacroAssemblerMIPSCompat::breakpoint() { as_break(0); }

void MacroAssemblerMIPSCompat::ensureDouble(const ValueOperand& source,
                                            FloatRegister dest,
                                            Label* failure) {
  Label isDouble, done;
  asMasm().branchTestDouble(Assembler::Equal, source.typeReg(), &isDouble);
  asMasm().branchTestInt32(Assembler::NotEqual, source.typeReg(), failure);

  convertInt32ToDouble(source.payloadReg(), dest);
  jump(&done);

  bind(&isDouble);
  unboxDouble(source, dest);

  bind(&done);
}

void MacroAssemblerMIPSCompat::checkStackAlignment() {
#ifdef DEBUG
  Label aligned;
  as_andi(ScratchRegister, sp, ABIStackAlignment - 1);
  ma_b(ScratchRegister, zero, &aligned, Equal, ShortJump);
  as_break(BREAK_STACK_UNALIGNED);
  bind(&aligned);
#endif
}

void MacroAssemblerMIPSCompat::alignStackPointer() {
  movePtr(StackPointer, SecondScratchReg);
  asMasm().subPtr(Imm32(sizeof(intptr_t)), StackPointer);
  asMasm().andPtr(Imm32(~(ABIStackAlignment - 1)), StackPointer);
  storePtr(SecondScratchReg, Address(StackPointer, 0));
}

void MacroAssemblerMIPSCompat::restoreStackPointer() {
  loadPtr(Address(StackPointer, 0), StackPointer);
}

void MacroAssemblerMIPSCompat::handleFailureWithHandlerTail(
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
  ValueOperand exception = ValueOperand(a1, a2);
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

CodeOffset MacroAssemblerMIPSCompat::toggledJump(Label* label) {
  CodeOffset ret(nextOffset().getOffset());
  ma_b(label);
  return ret;
}

CodeOffset MacroAssemblerMIPSCompat::toggledCall(JitCode* target,
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

void MacroAssemblerMIPSCompat::profilerEnterFrame(Register framePtr,
                                                  Register scratch) {
  asMasm().loadJSContext(scratch);
  loadPtr(Address(scratch, offsetof(JSContext, profilingActivation_)), scratch);
  storePtr(framePtr,
           Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
  storePtr(ImmPtr(nullptr),
           Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void MacroAssemblerMIPSCompat::profilerExitFrame() {
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
  int32_t diffF = set.fpus().getPushSizeInBytes();
  int32_t diffG = set.gprs().size() * sizeof(intptr_t);

  reserveStack(diffG);
  for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
    diffG -= sizeof(intptr_t);
    storePtr(*iter, Address(StackPointer, diffG));
  }
  MOZ_ASSERT(diffG == 0);

#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  if (diffF > 0) {
    // Double values have to be aligned. We reserve extra space so that we can
    // start writing from the first aligned location.
    // We reserve a whole extra double so that the buffer has even size.
    ma_and(SecondScratchReg, sp, Imm32(~(ABIStackAlignment - 1)));
    reserveStack(diffF);

    diffF -= sizeof(double);

    for (FloatRegisterForwardIterator iter(set.fpus().reduceSetForPush());
         iter.more(); ++iter) {
      as_sdc1(*iter, SecondScratchReg, -diffF);
      diffF -= sizeof(double);
    }

    MOZ_ASSERT(diffF == 0);
  }
}

void MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set,
                                         LiveRegisterSet ignore) {
  int32_t diffG = set.gprs().size() * sizeof(intptr_t);
  int32_t diffF = set.fpus().getPushSizeInBytes();
  const int32_t reservedG = diffG;
  const int32_t reservedF = diffF;

#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  if (reservedF > 0) {
    // Read the buffer form the first aligned location.
    ma_addu(SecondScratchReg, sp, Imm32(reservedF));
    ma_and(SecondScratchReg, SecondScratchReg, Imm32(~(ABIStackAlignment - 1)));

    diffF -= sizeof(double);

    LiveFloatRegisterSet fpignore(ignore.fpus().reduceSetForPush());
    for (FloatRegisterForwardIterator iter(set.fpus().reduceSetForPush());
         iter.more(); ++iter) {
      if (!ignore.has(*iter)) {
        as_ldc1(*iter, SecondScratchReg, -diffF);
      }
      diffF -= sizeof(double);
    }
    freeStack(reservedF);
    MOZ_ASSERT(diffF == 0);
  }

  for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
    diffG -= sizeof(intptr_t);
    if (!ignore.has(*iter)) {
      loadPtr(Address(StackPointer, diffG), *iter);
    }
  }
  freeStack(reservedG);
  MOZ_ASSERT(diffG == 0);
}

void MacroAssembler::storeRegsInMask(LiveRegisterSet set, Address dest,
                                     Register scratch) {
  int32_t diffF = set.fpus().getPushSizeInBytes();
  int32_t diffG = set.gprs().size() * sizeof(intptr_t);

  MOZ_ASSERT(dest.offset >= diffG + diffF);
  MOZ_ASSERT(dest.base == StackPointer);

  for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
    diffG -= sizeof(intptr_t);
    dest.offset -= sizeof(intptr_t);
    storePtr(*iter, dest);
  }
  MOZ_ASSERT(diffG == 0);

#ifdef ENABLE_WASM_SIMD
#  error "Needs more careful logic if SIMD is enabled"
#endif

  if (diffF > 0) {
    computeEffectiveAddress(dest, scratch);
    ma_and(scratch, scratch, Imm32(~(ABIStackAlignment - 1)));

    diffF -= sizeof(double);

    for (FloatRegisterForwardIterator iter(set.fpus().reduceSetForPush());
         iter.more(); ++iter) {
      as_sdc1(*iter, scratch, -diffF);
      diffF -= sizeof(double);
    }
    MOZ_ASSERT(diffF == 0);
  }
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
// Move instructions

void MacroAssembler::moveValue(const TypedOrValueRegister& src,
                               const ValueOperand& dest) {
  if (src.hasValue()) {
    moveValue(src.valueReg(), dest);
    return;
  }

  MIRType type = src.type();
  AnyRegister reg = src.typedReg();

  if (!IsFloatingPointType(type)) {
    if (reg.gpr() != dest.payloadReg()) {
      move32(reg.gpr(), dest.payloadReg());
    }
    mov(ImmWord(MIRTypeToTag(type)), dest.typeReg());
    return;
  }

  ScratchDoubleScope scratch(*this);
  FloatRegister freg = reg.fpu();
  if (type == MIRType::Float32) {
    convertFloat32ToDouble(freg, scratch);
    freg = scratch;
  }
  boxDouble(freg, dest, scratch);
}

void MacroAssembler::moveValue(const ValueOperand& src,
                               const ValueOperand& dest) {
  Register s0 = src.typeReg();
  Register s1 = src.payloadReg();
  Register d0 = dest.typeReg();
  Register d1 = dest.payloadReg();

  // Either one or both of the source registers could be the same as a
  // destination register.
  if (s1 == d0) {
    if (s0 == d1) {
      // If both are, this is just a swap of two registers.
      ScratchRegisterScope scratch(*this);
      MOZ_ASSERT(d1 != scratch);
      MOZ_ASSERT(d0 != scratch);
      move32(d1, scratch);
      move32(d0, d1);
      move32(scratch, d0);
      return;
    }
    // If only one is, copy that source first.
    std::swap(s0, s1);
    std::swap(d0, d1);
  }

  if (s0 != d0) {
    move32(s0, d0);
  }
  if (s1 != d1) {
    move32(s1, d1);
  }
}

void MacroAssembler::moveValue(const Value& src, const ValueOperand& dest) {
  move32(Imm32(src.toNunboxTag()), dest.typeReg());
  if (src.isGCThing()) {
    movePtr(ImmGCPtr(src.toGCThing()), dest.payloadReg());
  } else {
    move32(Imm32(src.toNunboxPayload()), dest.payloadReg());
  }
}

// ===============================================================
// Branch functions

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              const Address& address,
                                              Register temp, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  Label done;

  branchTestGCThing(Assembler::NotEqual, address,
                    cond == Assembler::Equal ? &done : label);

  loadPtr(address, temp);
  branchPtrInNurseryChunk(cond, temp, InvalidReg, label);

  bind(&done);
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              ValueOperand value, Register temp,
                                              Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  Label done;

  branchTestGCThing(Assembler::NotEqual, value,
                    cond == Assembler::Equal ? &done : label);
  branchPtrInNurseryChunk(cond, value.payloadReg(), temp, label);

  bind(&done);
}

void MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                     const Value& rhs, Label* label) {
  MOZ_ASSERT(cond == Equal || cond == NotEqual);
  ScratchRegisterScope scratch(*this);
  moveData(rhs, scratch);

  if (cond == Equal) {
    Label done;
    ma_b(lhs.payloadReg(), scratch, &done, NotEqual, ShortJump);
    { ma_b(lhs.typeReg(), Imm32(getType(rhs)), label, Equal); }
    bind(&done);
  } else {
    ma_b(lhs.payloadReg(), scratch, label, NotEqual);

    ma_b(lhs.typeReg(), Imm32(getType(rhs)), label, NotEqual);
  }
}

// ========================================================================
// Memory access primitives.
template <typename T>
void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                       MIRType valueType, const T& dest,
                                       MIRType slotType) {
  if (valueType == MIRType::Double) {
    storeDouble(value.reg().typedReg().fpu(), dest);
    return;
  }

  // Store the type tag if needed.
  if (valueType != slotType) {
    storeTypeTag(ImmType(ValueTypeFromMIRType(valueType)), dest);
  }

  // Store the payload.
  if (value.constant()) {
    storePayload(value.value(), dest);
  } else {
    storePayload(value.reg().typedReg().gpr(), dest);
  }
}

template void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                                MIRType valueType,
                                                const Address& dest,
                                                MIRType slotType);
template void MacroAssembler::storeUnboxedValue(
    const ConstantOrRegister& value, MIRType valueType,
    const BaseObjectElementIndex& dest, MIRType slotType);

void MacroAssembler::PushBoxed(FloatRegister reg) { Push(reg); }

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

void MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input,
                                                Register output,
                                                bool isSaturating,
                                                Label* oolEntry) {
  Label done;

  as_truncwd(ScratchFloat32Reg, input);
  ma_li(ScratchRegister, Imm32(INT32_MAX));
  moveFromFloat32(ScratchFloat32Reg, output);

  // For numbers in  -1.[ : ]INT32_MAX range do nothing more
  ma_b(output, ScratchRegister, &done, Assembler::Below, ShortJump);

  loadConstantDouble(double(INT32_MAX + 1ULL), ScratchDoubleReg);
  ma_li(ScratchRegister, Imm32(INT32_MIN));
  as_subd(ScratchDoubleReg, input, ScratchDoubleReg);
  as_truncwd(ScratchFloat32Reg, ScratchDoubleReg);
  as_cfc1(SecondScratchReg, Assembler::FCSR);
  moveFromFloat32(ScratchFloat32Reg, output);
  ma_ext(SecondScratchReg, SecondScratchReg, Assembler::CauseV, 1);
  ma_addu(output, ScratchRegister);

  ma_b(SecondScratchReg, Imm32(0), oolEntry, Assembler::NotEqual);

  bind(&done);
}

void MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input,
                                                 Register output,
                                                 bool isSaturating,
                                                 Label* oolEntry) {
  Label done;

  as_truncws(ScratchFloat32Reg, input);
  ma_li(ScratchRegister, Imm32(INT32_MAX));
  moveFromFloat32(ScratchFloat32Reg, output);
  // For numbers in  -1.[ : ]INT32_MAX range do nothing more
  ma_b(output, ScratchRegister, &done, Assembler::Below, ShortJump);

  loadConstantFloat32(float(INT32_MAX + 1ULL), ScratchFloat32Reg);
  ma_li(ScratchRegister, Imm32(INT32_MIN));
  as_subs(ScratchFloat32Reg, input, ScratchFloat32Reg);
  as_truncws(ScratchFloat32Reg, ScratchFloat32Reg);
  as_cfc1(SecondScratchReg, Assembler::FCSR);
  moveFromFloat32(ScratchFloat32Reg, output);
  ma_ext(SecondScratchReg, SecondScratchReg, Assembler::CauseV, 1);
  ma_addu(output, ScratchRegister);

  // Guard against negative values that result in 0 due the precision loss.
  as_sltiu(ScratchRegister, output, 1);
  ma_or(SecondScratchReg, ScratchRegister);

  ma_b(SecondScratchReg, Imm32(0), oolEntry, Assembler::NotEqual);

  bind(&done);
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

void MacroAssemblerMIPSCompat::wasmLoadI64Impl(
    const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
    Register ptrScratch, Register64 output, Register tmp) {
  uint32_t offset = access.offset();
  MOZ_ASSERT_IF(offset, ptrScratch != InvalidReg);

  MOZ_ASSERT(!access.isZeroExtendSimd128Load());
  MOZ_ASSERT(!access.isSplatSimd128Load());
  MOZ_ASSERT(!access.isWidenSimd128Load());

  // Maybe add the offset.
  if (offset) {
    asMasm().movePtr(ptr, ptrScratch);
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
  MOZ_ASSERT(INT64LOW_OFFSET == 0);
  if (IsUnaligned(access)) {
    MOZ_ASSERT(tmp != InvalidReg);
    if (byteSize <= 4) {
      asMasm().ma_load_unaligned(access, output.low, address, tmp,
                                 static_cast<LoadStoreSize>(8 * byteSize),
                                 isSigned ? SignExtend : ZeroExtend);
      if (!isSigned) {
        asMasm().move32(Imm32(0), output.high);
      } else {
        asMasm().ma_sra(output.high, output.low, Imm32(31));
      }
    } else {
      MOZ_ASSERT(output.low != ptr);
      asMasm().ma_load_unaligned(access, output.low, address, tmp, SizeWord,
                                 ZeroExtend);
      asMasm().ma_load_unaligned(
          access, output.high,
          BaseIndex(HeapReg, ptr, TimesOne, INT64HIGH_OFFSET), tmp, SizeWord,
          SignExtend);
    }
    return;
  }

  asMasm().memoryBarrierBefore(access.sync());
  if (byteSize <= 4) {
    asMasm().ma_load(output.low, address,
                     static_cast<LoadStoreSize>(8 * byteSize),
                     isSigned ? SignExtend : ZeroExtend);
    asMasm().append(access, asMasm().size() - 4);
    if (!isSigned) {
      asMasm().move32(Imm32(0), output.high);
    } else {
      asMasm().ma_sra(output.high, output.low, Imm32(31));
    }
  } else {
    MOZ_ASSERT(output.low != ptr);
    asMasm().ma_load(output.low, BaseIndex(HeapReg, ptr, TimesOne), SizeWord);
    asMasm().append(access, asMasm().size() - 4);
    asMasm().ma_load(output.high,
                     BaseIndex(HeapReg, ptr, TimesOne, INT64HIGH_OFFSET),
                     SizeWord);
    asMasm().append(access, asMasm().size() - 4);
  }
  asMasm().memoryBarrierAfter(access.sync());
}

void MacroAssemblerMIPSCompat::wasmStoreI64Impl(
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

  MOZ_ASSERT(INT64LOW_OFFSET == 0);
  BaseIndex address(memoryBase, ptr, TimesOne);
  if (IsUnaligned(access)) {
    MOZ_ASSERT(tmp != InvalidReg);
    if (byteSize <= 4) {
      asMasm().ma_store_unaligned(access, value.low, address, tmp,
                                  static_cast<LoadStoreSize>(8 * byteSize),
                                  isSigned ? SignExtend : ZeroExtend);
    } else {
      asMasm().ma_store_unaligned(
          access, value.high,
          BaseIndex(HeapReg, ptr, TimesOne, INT64HIGH_OFFSET), tmp, SizeWord,
          SignExtend);
      asMasm().ma_store_unaligned(access, value.low, address, tmp, SizeWord,
                                  ZeroExtend);
    }
    return;
  }

  asMasm().memoryBarrierBefore(access.sync());
  if (byteSize <= 4) {
    asMasm().ma_store(value.low, address,
                      static_cast<LoadStoreSize>(8 * byteSize));
    asMasm().append(access, asMasm().size() - 4);
  } else {
    asMasm().ma_store(value.high,
                      BaseIndex(HeapReg, ptr, TimesOne, INT64HIGH_OFFSET),
                      SizeWord);
    asMasm().append(access, asMasm().size() - 4);
    asMasm().ma_store(value.low, address, SizeWord);
  }
  asMasm().memoryBarrierAfter(access.sync());
}

static void EnterAtomic64Region(MacroAssembler& masm,
                                const wasm::MemoryAccessDesc& access,
                                Register addr, Register spinlock,
                                Register scratch) {
  masm.movePtr(wasm::SymbolicAddress::js_jit_gAtomic64Lock, spinlock);

  masm.append(access, masm.size());
  masm.as_lbu(
      zero, addr,
      7);  // Force memory trap on invalid access before we enter the spinlock.

  Label tryLock;

  masm.memoryBarrier(MembarFull);

  masm.bind(&tryLock);

  masm.as_ll(scratch, spinlock, 0);
  masm.ma_b(scratch, scratch, &tryLock, Assembler::NonZero, ShortJump);
  masm.ma_li(scratch, Imm32(1));
  masm.as_sc(scratch, spinlock, 0);
  masm.ma_b(scratch, scratch, &tryLock, Assembler::Zero, ShortJump);

  masm.memoryBarrier(MembarFull);
}

static void ExitAtomic64Region(MacroAssembler& masm, Register spinlock) {
  masm.memoryBarrier(MembarFull);
  masm.as_sw(zero, spinlock, 0);
  masm.memoryBarrier(MembarFull);
}

template <typename T>
static void AtomicLoad64(MacroAssembler& masm,
                         const wasm::MemoryAccessDesc& access, const T& mem,
                         Register64 temp, Register64 output) {
  MOZ_ASSERT(temp.low == InvalidReg && temp.high == InvalidReg);

  masm.computeEffectiveAddress(mem, SecondScratchReg);

  EnterAtomic64Region(masm, access, /* addr= */ SecondScratchReg,
                      /* spinlock= */ ScratchRegister,
                      /* scratch= */ output.low);

  masm.load64(Address(SecondScratchReg, 0), output);

  ExitAtomic64Region(masm, /* spinlock= */ ScratchRegister);
}

void MacroAssembler::wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                                      const Address& mem, Register64 temp,
                                      Register64 output) {
  AtomicLoad64(*this, access, mem, temp, output);
}

void MacroAssembler::wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                                      const BaseIndex& mem, Register64 temp,
                                      Register64 output) {
  AtomicLoad64(*this, access, mem, temp, output);
}

template <typename T>
void MacroAssemblerMIPSCompat::wasmAtomicStore64(
    const wasm::MemoryAccessDesc& access, const T& mem, Register temp,
    Register64 value) {
  computeEffectiveAddress(mem, SecondScratchReg);

  EnterAtomic64Region(asMasm(), access, /* addr= */ SecondScratchReg,
                      /* spinlock= */ ScratchRegister, /* scratch= */ temp);

  store64(value, Address(SecondScratchReg, 0));

  ExitAtomic64Region(asMasm(), /* spinlock= */ ScratchRegister);
}

template void MacroAssemblerMIPSCompat::wasmAtomicStore64(
    const wasm::MemoryAccessDesc& access, const Address& mem, Register temp,
    Register64 value);
template void MacroAssemblerMIPSCompat::wasmAtomicStore64(
    const wasm::MemoryAccessDesc& access, const BaseIndex& mem, Register temp,
    Register64 value);

template <typename T>
static void WasmCompareExchange64(MacroAssembler& masm,
                                  const wasm::MemoryAccessDesc& access,
                                  const T& mem, Register64 expect,
                                  Register64 replace, Register64 output) {
  MOZ_ASSERT(output != expect);
  MOZ_ASSERT(output != replace);

  Label exit;

  masm.computeEffectiveAddress(mem, SecondScratchReg);
  Address addr(SecondScratchReg, 0);

  EnterAtomic64Region(masm, access, /* addr= */ SecondScratchReg,
                      /* spinlock= */ ScratchRegister,
                      /* scratch= */ output.low);

  masm.load64(addr, output);

  masm.ma_b(output.low, expect.low, &exit, Assembler::NotEqual, ShortJump);
  masm.ma_b(output.high, expect.high, &exit, Assembler::NotEqual, ShortJump);
  masm.store64(replace, addr);
  masm.bind(&exit);
  ExitAtomic64Region(masm, /* spinlock= */ ScratchRegister);
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const Address& mem,
                                           Register64 expect,
                                           Register64 replace,
                                           Register64 output) {
  WasmCompareExchange64(*this, access, mem, expect, replace, output);
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const BaseIndex& mem,
                                           Register64 expect,
                                           Register64 replace,
                                           Register64 output) {
  WasmCompareExchange64(*this, access, mem, expect, replace, output);
}

template <typename T>
static void WasmAtomicExchange64(MacroAssembler& masm,
                                 const wasm::MemoryAccessDesc& access,
                                 const T& mem, Register64 src,
                                 Register64 output) {
  masm.computeEffectiveAddress(mem, SecondScratchReg);
  Address addr(SecondScratchReg, 0);

  EnterAtomic64Region(masm, access, /* addr= */ SecondScratchReg,
                      /* spinlock= */ ScratchRegister,
                      /* scratch= */ output.low);

  masm.load64(addr, output);
  masm.store64(src, addr);

  ExitAtomic64Region(masm, /* spinlock= */ ScratchRegister);
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

template <typename T>
static void AtomicFetchOp64(MacroAssembler& masm,
                            const wasm::MemoryAccessDesc& access, AtomicOp op,
                            Register64 value, const T& mem, Register64 temp,
                            Register64 output) {
  masm.computeEffectiveAddress(mem, SecondScratchReg);

  EnterAtomic64Region(masm, access, /* addr= */ SecondScratchReg,
                      /* spinlock= */ ScratchRegister,
                      /* scratch= */ output.low);

  masm.load64(Address(SecondScratchReg, 0), output);

  switch (op) {
    case AtomicFetchAddOp:
      masm.as_addu(temp.low, output.low, value.low);
      masm.as_sltu(temp.high, temp.low, output.low);
      masm.as_addu(temp.high, temp.high, output.high);
      masm.as_addu(temp.high, temp.high, value.high);
      break;
    case AtomicFetchSubOp:
      masm.as_sltu(temp.high, output.low, value.low);
      masm.as_subu(temp.high, output.high, temp.high);
      masm.as_subu(temp.low, output.low, value.low);
      masm.as_subu(temp.high, temp.high, value.high);
      break;
    case AtomicFetchAndOp:
      masm.as_and(temp.low, output.low, value.low);
      masm.as_and(temp.high, output.high, value.high);
      break;
    case AtomicFetchOrOp:
      masm.as_or(temp.low, output.low, value.low);
      masm.as_or(temp.high, output.high, value.high);
      break;
    case AtomicFetchXorOp:
      masm.as_xor(temp.low, output.low, value.low);
      masm.as_xor(temp.high, output.high, value.high);
      break;
    default:
      MOZ_CRASH();
  }

  masm.store64(temp, Address(SecondScratchReg, 0));

  ExitAtomic64Region(masm, /* spinlock= */ ScratchRegister);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const Address& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp64(*this, access, op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access,
                                         AtomicOp op, Register64 value,
                                         const BaseIndex& mem, Register64 temp,
                                         Register64 output) {
  AtomicFetchOp64(*this, access, op, value, mem, temp, output);
}

// ========================================================================
// Convert floating point.

static const double TO_DOUBLE_HIGH_SCALE = 0x100000000;

bool MacroAssembler::convertUInt64ToDoubleNeedsTemp() { return false; }

void MacroAssembler::convertUInt64ToDouble(Register64 src, FloatRegister dest,
                                           Register temp) {
  MOZ_ASSERT(temp == Register::Invalid());
  convertUInt32ToDouble(src.high, dest);
  loadConstantDouble(TO_DOUBLE_HIGH_SCALE, ScratchDoubleReg);
  mulDouble(ScratchDoubleReg, dest);
  convertUInt32ToDouble(src.low, ScratchDoubleReg);
  addDouble(ScratchDoubleReg, dest);
}

void MacroAssembler::convertInt64ToDouble(Register64 src, FloatRegister dest) {
  convertInt32ToDouble(src.high, dest);
  loadConstantDouble(TO_DOUBLE_HIGH_SCALE, ScratchDoubleReg);
  mulDouble(ScratchDoubleReg, dest);
  convertUInt32ToDouble(src.low, ScratchDoubleReg);
  addDouble(ScratchDoubleReg, dest);
}

void MacroAssembler::convertIntPtrToDouble(Register src, FloatRegister dest) {
  convertInt32ToDouble(src, dest);
}

//}}} check_macroassembler_style

// Copyright 2015, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "jit/arm64/vixl/MacroAssembler-vixl.h"

#include <ctype.h>

namespace vixl {

MacroAssembler::MacroAssembler()
    : js::jit::Assembler(),
      sp_(x28),
      tmp_list_(ip0, ip1),
      fptmp_list_(d31)
{
}


void MacroAssembler::FinalizeCode() {
  Assembler::FinalizeCode();
}


int MacroAssembler::MoveImmediateHelper(MacroAssembler* masm,
                                        const Register &rd,
                                        uint64_t imm) {
  bool emit_code = (masm != NULL);
  VIXL_ASSERT(is_uint32(imm) || is_int32(imm) || rd.Is64Bits());
  // The worst case for size is mov 64-bit immediate to sp:
  //  * up to 4 instructions to materialise the constant
  //  * 1 instruction to move to sp
  MacroEmissionCheckScope guard(masm);

  // Immediates on Aarch64 can be produced using an initial value, and zero to
  // three move keep operations.
  //
  // Initial values can be generated with:
  //  1. 64-bit move zero (movz).
  //  2. 32-bit move inverted (movn).
  //  3. 64-bit move inverted.
  //  4. 32-bit orr immediate.
  //  5. 64-bit orr immediate.
  // Move-keep may then be used to modify each of the 16-bit half words.
  //
  // The code below supports all five initial value generators, and
  // applying move-keep operations to move-zero and move-inverted initial
  // values.

  // Try to move the immediate in one instruction, and if that fails, switch to
  // using multiple instructions.
  if (OneInstrMoveImmediateHelper(masm, rd, imm)) {
    return 1;
  } else {
    int instruction_count = 0;
    unsigned reg_size = rd.size();

    // Generic immediate case. Imm will be represented by
    //   [imm3, imm2, imm1, imm0], where each imm is 16 bits.
    // A move-zero or move-inverted is generated for the first non-zero or
    // non-0xffff immX, and a move-keep for subsequent non-zero immX.

    uint64_t ignored_halfword = 0;
    bool invert_move = false;
    // If the number of 0xffff halfwords is greater than the number of 0x0000
    // halfwords, it's more efficient to use move-inverted.
    if (CountClearHalfWords(~imm, reg_size) >
        CountClearHalfWords(imm, reg_size)) {
      ignored_halfword = 0xffff;
      invert_move = true;
    }

    // Mov instructions can't move values into the stack pointer, so set up a
    // temporary register, if needed.
    UseScratchRegisterScope temps;
    Register temp;
    if (emit_code) {
      temps.Open(masm);
      temp = rd.IsSP() ? temps.AcquireSameSizeAs(rd) : rd;
    }

    // Iterate through the halfwords. Use movn/movz for the first non-ignored
    // halfword, and movk for subsequent halfwords.
    VIXL_ASSERT((reg_size % 16) == 0);
    bool first_mov_done = false;
    for (unsigned i = 0; i < (temp.size() / 16); i++) {
      uint64_t imm16 = (imm >> (16 * i)) & 0xffff;
      if (imm16 != ignored_halfword) {
        if (!first_mov_done) {
          if (invert_move) {
            if (emit_code) masm->movn(temp, ~imm16 & 0xffff, 16 * i);
            instruction_count++;
          } else {
            if (emit_code) masm->movz(temp, imm16, 16 * i);
            instruction_count++;
          }
          first_mov_done = true;
        } else {
          // Construct a wider constant.
          if (emit_code) masm->movk(temp, imm16, 16 * i);
          instruction_count++;
        }
      }
    }

    VIXL_ASSERT(first_mov_done);

    // Move the temporary if the original destination register was the stack
    // pointer.
    if (rd.IsSP()) {
      if (emit_code) masm->mov(rd, temp);
      instruction_count++;
    }
    return instruction_count;
  }
}


bool MacroAssembler::OneInstrMoveImmediateHelper(MacroAssembler* masm,
                                                 const Register& dst,
                                                 int64_t imm) {
  bool emit_code = masm != NULL;
  unsigned n, imm_s, imm_r;
  int reg_size = dst.size();

  if (IsImmMovz(imm, reg_size) && !dst.IsSP()) {
    // Immediate can be represented in a move zero instruction. Movz can't write
    // to the stack pointer.
    if (emit_code) {
      masm->movz(dst, imm);
    }
    return true;
  } else if (IsImmMovn(imm, reg_size) && !dst.IsSP()) {
    // Immediate can be represented in a move negative instruction. Movn can't
    // write to the stack pointer.
    if (emit_code) {
      masm->movn(dst, dst.Is64Bits() ? ~imm : (~imm & kWRegMask));
    }
    return true;
  } else if (IsImmLogical(imm, reg_size, &n, &imm_s, &imm_r)) {
    // Immediate can be represented in a logical orr instruction.
    VIXL_ASSERT(!dst.IsZero());
    if (emit_code) {
      masm->LogicalImmediate(
          dst, AppropriateZeroRegFor(dst), n, imm_s, imm_r, ORR);
    }
    return true;
  }
  return false;
}


void MacroAssembler::B(Label* label, BranchType type, Register reg, int bit) {
  VIXL_ASSERT((reg.Is(NoReg) || (type >= kBranchTypeFirstUsingReg)) &&
              ((bit == -1) || (type >= kBranchTypeFirstUsingBit)));
  if (kBranchTypeFirstCondition <= type && type <= kBranchTypeLastCondition) {
    B(static_cast<Condition>(type), label);
  } else {
    switch (type) {
      case always:        B(label);              break;
      case never:         break;
      case reg_zero:      Cbz(reg, label);       break;
      case reg_not_zero:  Cbnz(reg, label);      break;
      case reg_bit_clear: Tbz(reg, bit, label);  break;
      case reg_bit_set:   Tbnz(reg, bit, label); break;
      default:
        VIXL_UNREACHABLE();
    }
  }
}


void MacroAssembler::B(Label* label) {
  SingleEmissionCheckScope guard(this);
  b(label);
}


void MacroAssembler::B(Label* label, Condition cond) {
  VIXL_ASSERT((cond != al) && (cond != nv));
  EmissionCheckScope guard(this, 2 * kInstructionSize);

  if (label->bound() && LabelIsOutOfRange(label, CondBranchType)) {
    Label done;
    b(&done, InvertCondition(cond));
    b(label);
    bind(&done);
  } else {
    // TODO: Need to register a slot in a literal pool, so that we can
    // write a branch instruction there and use that to branch in case
    // the unbound label winds up being out of range.
    b(label, cond);
  }
}


void MacroAssembler::Cbnz(const Register& rt, Label* label) {
  VIXL_ASSERT(!rt.IsZero());
  EmissionCheckScope guard(this, 2 * kInstructionSize);

  if (label->bound() && LabelIsOutOfRange(label, CondBranchType)) {
    Label done;
    cbz(rt, &done);
    b(label);
    bind(&done);
  } else {
    // TODO: Need to register a slot in a literal pool, so that we can
    // write a branch instruction there and use that to branch in case
    // the unbound label winds up being out of range.
    cbnz(rt, label);
  }
}


void MacroAssembler::Cbz(const Register& rt, Label* label) {
  VIXL_ASSERT(!rt.IsZero());
  EmissionCheckScope guard(this, 2 * kInstructionSize);

  if (label->bound() && LabelIsOutOfRange(label, CondBranchType)) {
    Label done;
    cbnz(rt, &done);
    b(label);
    bind(&done);
  } else {
    // TODO: Nede to register a slot in a literal pool, so that we can
    // write a branch instruction there and use that to branch in case
    // the unbound label winds up being out of range.
    cbz(rt, label);
  }
}


void MacroAssembler::Tbnz(const Register& rt, unsigned bit_pos, Label* label) {
  VIXL_ASSERT(!rt.IsZero());
  EmissionCheckScope guard(this, 2 * kInstructionSize);

  if (label->bound() && LabelIsOutOfRange(label, TestBranchType)) {
    Label done;
    tbz(rt, bit_pos, &done);
    b(label);
    bind(&done);
  } else {
    // TODO: Nede to register a slot in a literal pool, so that we can
    // write a branch instruction there and use that to branch in case
    // the unbound label winds up being out of range.
    tbnz(rt, bit_pos, label);
  }
}


void MacroAssembler::Tbz(const Register& rt, unsigned bit_pos, Label* label) {
  VIXL_ASSERT(!rt.IsZero());
  EmissionCheckScope guard(this, 2 * kInstructionSize);

  if (label->bound() && LabelIsOutOfRange(label, TestBranchType)) {
    Label done;
    tbnz(rt, bit_pos, &done);
    b(label);
    bind(&done);
  } else {
    // TODO: Nede to register a slot in a literal pool, so that we can
    // write a branch instruction there and use that to branch in case
    // the unbound label winds up being out of range.
    tbz(rt, bit_pos, label);
  }
}


void MacroAssembler::And(const Register& rd,
                         const Register& rn,
                         const Operand& operand) {
  LogicalMacro(rd, rn, operand, AND);
}


void MacroAssembler::Ands(const Register& rd,
                          const Register& rn,
                          const Operand& operand) {
  LogicalMacro(rd, rn, operand, ANDS);
}


void MacroAssembler::Tst(const Register& rn,
                         const Operand& operand) {
  Ands(AppropriateZeroRegFor(rn), rn, operand);
}


void MacroAssembler::Bic(const Register& rd,
                         const Register& rn,
                         const Operand& operand) {
  LogicalMacro(rd, rn, operand, BIC);
}


void MacroAssembler::Bics(const Register& rd,
                          const Register& rn,
                          const Operand& operand) {
  LogicalMacro(rd, rn, operand, BICS);
}


void MacroAssembler::Orr(const Register& rd,
                         const Register& rn,
                         const Operand& operand) {
  LogicalMacro(rd, rn, operand, ORR);
}


void MacroAssembler::Orn(const Register& rd,
                         const Register& rn,
                         const Operand& operand) {
  LogicalMacro(rd, rn, operand, ORN);
}


void MacroAssembler::Eor(const Register& rd,
                         const Register& rn,
                         const Operand& operand) {
  LogicalMacro(rd, rn, operand, EOR);
}


void MacroAssembler::Eon(const Register& rd,
                         const Register& rn,
                         const Operand& operand) {
  LogicalMacro(rd, rn, operand, EON);
}


void MacroAssembler::LogicalMacro(const Register& rd,
                                  const Register& rn,
                                  const Operand& operand,
                                  LogicalOp op) {
  // The worst case for size is logical immediate to sp:
  //  * up to 4 instructions to materialise the constant
  //  * 1 instruction to do the operation
  //  * 1 instruction to move to sp
  MacroEmissionCheckScope guard(this);
  UseScratchRegisterScope temps(this);

  if (operand.IsImmediate()) {
    int64_t immediate = operand.immediate();
    unsigned reg_size = rd.size();

    // If the operation is NOT, invert the operation and immediate.
    if ((op & NOT) == NOT) {
      op = static_cast<LogicalOp>(op & ~NOT);
      immediate = ~immediate;
    }

    // Ignore the top 32 bits of an immediate if we're moving to a W register.
    if (rd.Is32Bits()) {
      // Check that the top 32 bits are consistent.
      VIXL_ASSERT(((immediate >> kWRegSize) == 0) ||
                  ((immediate >> kWRegSize) == -1));
      immediate &= kWRegMask;
    }

    VIXL_ASSERT(rd.Is64Bits() || is_uint32(immediate));

    // Special cases for all set or all clear immediates.
    if (immediate == 0) {
      switch (op) {
        case AND:
          Mov(rd, 0);
          return;
        case ORR:
          VIXL_FALLTHROUGH();
        case EOR:
          Mov(rd, rn);
          return;
        case ANDS:
          VIXL_FALLTHROUGH();
        case BICS:
          break;
        default:
          VIXL_UNREACHABLE();
      }
    } else if ((rd.Is64Bits() && (immediate == -1)) ||
               (rd.Is32Bits() && (immediate == 0xffffffff))) {
      switch (op) {
        case AND:
          Mov(rd, rn);
          return;
        case ORR:
          Mov(rd, immediate);
          return;
        case EOR:
          Mvn(rd, rn);
          return;
        case ANDS:
          VIXL_FALLTHROUGH();
        case BICS:
          break;
        default:
          VIXL_UNREACHABLE();
      }
    }

    unsigned n, imm_s, imm_r;
    if (IsImmLogical(immediate, reg_size, &n, &imm_s, &imm_r)) {
      // Immediate can be encoded in the instruction.
      LogicalImmediate(rd, rn, n, imm_s, imm_r, op);
    } else {
      // Immediate can't be encoded: synthesize using move immediate.
      Register temp = temps.AcquireSameSizeAs(rn);

      // If the left-hand input is the stack pointer, we can't pre-shift the
      // immediate, as the encoding won't allow the subsequent post shift.
      PreShiftImmMode mode = rn.IsSP() ? kNoShift : kAnyShift;
      Operand imm_operand = MoveImmediateForShiftedOp(temp, immediate, mode);

      // VIXL can acquire temp registers. Assert that the caller is aware.
      VIXL_ASSERT(!temp.Is(rd) && !temp.Is(rn));
      VIXL_ASSERT(!temp.Is(operand.maybeReg()));

      if (rd.Is(sp)) {
        // If rd is the stack pointer we cannot use it as the destination
        // register so we use the temp register as an intermediate again.
        Logical(temp, rn, imm_operand, op);
        Mov(sp, temp);
      } else {
        Logical(rd, rn, imm_operand, op);
      }
    }
  } else if (operand.IsExtendedRegister()) {
    VIXL_ASSERT(operand.reg().size() <= rd.size());
    // Add/sub extended supports shift <= 4. We want to support exactly the
    // same modes here.
    VIXL_ASSERT(operand.shift_amount() <= 4);
    VIXL_ASSERT(operand.reg().Is64Bits() ||
           ((operand.extend() != UXTX) && (operand.extend() != SXTX)));

    temps.Exclude(operand.reg());
    Register temp = temps.AcquireSameSizeAs(rn);

    // VIXL can acquire temp registers. Assert that the caller is aware.
    VIXL_ASSERT(!temp.Is(rd) && !temp.Is(rn));
    VIXL_ASSERT(!temp.Is(operand.maybeReg()));

    EmitExtendShift(temp, operand.reg(), operand.extend(),
                    operand.shift_amount());
    Logical(rd, rn, Operand(temp), op);
  } else {
    // The operand can be encoded in the instruction.
    VIXL_ASSERT(operand.IsShiftedRegister());
    Logical(rd, rn, operand, op);
  }
}


void MacroAssembler::Mov(const Register& rd,
                         const Operand& operand,
                         DiscardMoveMode discard_mode) {
  // The worst case for size is mov immediate with up to 4 instructions.
  MacroEmissionCheckScope guard(this);

  if (operand.IsImmediate()) {
    // Call the macro assembler for generic immediates.
    Mov(rd, operand.immediate());
  } else if (operand.IsShiftedRegister() && (operand.shift_amount() != 0)) {
    // Emit a shift instruction if moving a shifted register. This operation
    // could also be achieved using an orr instruction (like orn used by Mvn),
    // but using a shift instruction makes the disassembly clearer.
    EmitShift(rd, operand.reg(), operand.shift(), operand.shift_amount());
  } else if (operand.IsExtendedRegister()) {
    // Emit an extend instruction if moving an extended register. This handles
    // extend with post-shift operations, too.
    EmitExtendShift(rd, operand.reg(), operand.extend(),
                    operand.shift_amount());
  } else {
    // Otherwise, emit a register move only if the registers are distinct, or
    // if they are not X registers.
    //
    // Note that mov(w0, w0) is not a no-op because it clears the top word of
    // x0. A flag is provided (kDiscardForSameWReg) if a move between the same W
    // registers is not required to clear the top word of the X register. In
    // this case, the instruction is discarded.
    //
    // If the sp is an operand, add #0 is emitted, otherwise, orr #0.
    if (!rd.Is(operand.reg()) || (rd.Is32Bits() &&
                                  (discard_mode == kDontDiscardForSameWReg))) {
      mov(rd, operand.reg());
    }
  }
}


void MacroAssembler::Movi16bitHelper(const VRegister& vd, uint64_t imm) {
  VIXL_ASSERT(is_uint16(imm));
  int byte1 = (imm & 0xff);
  int byte2 = ((imm >> 8) & 0xff);
  if (byte1 == byte2) {
    movi(vd.Is64Bits() ? vd.V8B() : vd.V16B(), byte1);
  } else if (byte1 == 0) {
    movi(vd, byte2, LSL, 8);
  } else if (byte2 == 0) {
    movi(vd, byte1);
  } else if (byte1 == 0xff) {
    mvni(vd, ~byte2 & 0xff, LSL, 8);
  } else if (byte2 == 0xff) {
    mvni(vd, ~byte1 & 0xff);
  } else {
    UseScratchRegisterScope temps(this);
    Register temp = temps.AcquireW();
    movz(temp, imm);
    dup(vd, temp);
  }
}


void MacroAssembler::Movi32bitHelper(const VRegister& vd, uint64_t imm) {
  VIXL_ASSERT(is_uint32(imm));

  uint8_t bytes[sizeof(imm)];
  memcpy(bytes, &imm, sizeof(imm));

  // All bytes are either 0x00 or 0xff.
  {
    bool all0orff = true;
    for (int i = 0; i < 4; ++i) {
      if ((bytes[i] != 0) && (bytes[i] != 0xff)) {
        all0orff = false;
        break;
      }
    }

    if (all0orff == true) {
      movi(vd.Is64Bits() ? vd.V1D() : vd.V2D(), ((imm << 32) | imm));
      return;
    }
  }

  // Of the 4 bytes, only one byte is non-zero.
  for (int i = 0; i < 4; i++) {
    if ((imm & (0xff << (i * 8))) == imm) {
      movi(vd, bytes[i], LSL, i * 8);
      return;
    }
  }

  // Of the 4 bytes, only one byte is not 0xff.
  for (int i = 0; i < 4; i++) {
    uint32_t mask = ~(0xff << (i * 8));
    if ((imm & mask) == mask) {
      mvni(vd, ~bytes[i] & 0xff, LSL, i * 8);
      return;
    }
  }

  // Immediate is of the form 0x00MMFFFF.
  if ((imm & 0xff00ffff) == 0x0000ffff) {
    movi(vd, bytes[2], MSL, 16);
    return;
  }

  // Immediate is of the form 0x0000MMFF.
  if ((imm & 0xffff00ff) == 0x000000ff) {
    movi(vd, bytes[1], MSL, 8);
    return;
  }

  // Immediate is of the form 0xFFMM0000.
  if ((imm & 0xff00ffff) == 0xff000000) {
    mvni(vd, ~bytes[2] & 0xff, MSL, 16);
    return;
  }
  // Immediate is of the form 0xFFFFMM00.
  if ((imm & 0xffff00ff) == 0xffff0000) {
    mvni(vd, ~bytes[1] & 0xff, MSL, 8);
    return;
  }

  // Top and bottom 16-bits are equal.
  if (((imm >> 16) & 0xffff) == (imm & 0xffff)) {
    Movi16bitHelper(vd.Is64Bits() ? vd.V4H() : vd.V8H(), imm & 0xffff);
    return;
  }

  // Default case.
  {
    UseScratchRegisterScope temps(this);
    Register temp = temps.AcquireW();
    Mov(temp, imm);
    dup(vd, temp);
  }
}


void MacroAssembler::Movi64bitHelper(const VRegister& vd, uint64_t imm) {
  // All bytes are either 0x00 or 0xff.
  {
    bool all0orff = true;
    for (int i = 0; i < 8; ++i) {
      int byteval = (imm >> (i * 8)) & 0xff;
      if (byteval != 0 && byteval != 0xff) {
        all0orff = false;
        break;
      }
    }
    if (all0orff == true) {
      movi(vd, imm);
      return;
    }
  }

  // Top and bottom 32-bits are equal.
  if (((imm >> 32) & 0xffffffff) == (imm & 0xffffffff)) {
    Movi32bitHelper(vd.Is64Bits() ? vd.V2S() : vd.V4S(), imm & 0xffffffff);
    return;
  }

  // Default case.
  {
    UseScratchRegisterScope temps(this);
    Register temp = temps.AcquireX();
    Mov(temp, imm);
    if (vd.Is1D()) {
      mov(vd.D(), 0, temp);
    } else {
      dup(vd.V2D(), temp);
    }
  }
}


void MacroAssembler::Movi(const VRegister& vd,
                          uint64_t imm,
                          Shift shift,
                          int shift_amount) {
  MacroEmissionCheckScope guard(this);
  if (shift_amount != 0 || shift != LSL) {
    movi(vd, imm, shift, shift_amount);
  } else if (vd.Is8B() || vd.Is16B()) {
    // 8-bit immediate.
    VIXL_ASSERT(is_uint8(imm));
    movi(vd, imm);
  } else if (vd.Is4H() || vd.Is8H()) {
    // 16-bit immediate.
    Movi16bitHelper(vd, imm);
  } else if (vd.Is2S() || vd.Is4S()) {
    // 32-bit immediate.
    Movi32bitHelper(vd, imm);
  } else {
    // 64-bit immediate.
    Movi64bitHelper(vd, imm);
  }
}


void MacroAssembler::Movi(const VRegister& vd,
                          uint64_t hi,
                          uint64_t lo) {
  // TODO: Move 128-bit values in a more efficient way.
  VIXL_ASSERT(vd.Is128Bits());
  UseScratchRegisterScope temps(this);
  Movi(vd.V2D(), lo);
  Register temp = temps.AcquireX();
  Mov(temp, hi);
  Ins(vd.V2D(), 1, temp);
}


void MacroAssembler::Mvn(const Register& rd, const Operand& operand) {
  // The worst case for size is mvn immediate with up to 4 instructions.
  MacroEmissionCheckScope guard(this);

  if (operand.IsImmediate()) {
    // Call the macro assembler for generic immediates.
    Mvn(rd, operand.immediate());
  } else if (operand.IsExtendedRegister()) {
    UseScratchRegisterScope temps(this);
    temps.Exclude(operand.reg());

    // Emit two instructions for the extend case. This differs from Mov, as
    // the extend and invert can't be achieved in one instruction.
    Register temp = temps.AcquireSameSizeAs(rd);

    // VIXL can acquire temp registers. Assert that the caller is aware.
    VIXL_ASSERT(!temp.Is(rd) && !temp.Is(operand.maybeReg()));

    EmitExtendShift(temp, operand.reg(), operand.extend(),
                    operand.shift_amount());
    mvn(rd, Operand(temp));
  } else {
    // Otherwise, register and shifted register cases can be handled by the
    // assembler directly, using orn.
    mvn(rd, operand);
  }
}


void MacroAssembler::Mov(const Register& rd, uint64_t imm) {
  MoveImmediateHelper(this, rd, imm);
}


void MacroAssembler::Ccmp(const Register& rn,
                          const Operand& operand,
                          StatusFlags nzcv,
                          Condition cond) {
  if (operand.IsImmediate() && (operand.immediate() < 0)) {
    ConditionalCompareMacro(rn, -operand.immediate(), nzcv, cond, CCMN);
  } else {
    ConditionalCompareMacro(rn, operand, nzcv, cond, CCMP);
  }
}


void MacroAssembler::Ccmn(const Register& rn,
                          const Operand& operand,
                          StatusFlags nzcv,
                          Condition cond) {
  if (operand.IsImmediate() && (operand.immediate() < 0)) {
    ConditionalCompareMacro(rn, -operand.immediate(), nzcv, cond, CCMP);
  } else {
    ConditionalCompareMacro(rn, operand, nzcv, cond, CCMN);
  }
}


void MacroAssembler::ConditionalCompareMacro(const Register& rn,
                                             const Operand& operand,
                                             StatusFlags nzcv,
                                             Condition cond,
                                             ConditionalCompareOp op) {
  VIXL_ASSERT((cond != al) && (cond != nv));
  // The worst case for size is ccmp immediate:
  //  * up to 4 instructions to materialise the constant
  //  * 1 instruction for ccmp
  MacroEmissionCheckScope guard(this);

  if ((operand.IsShiftedRegister() && (operand.shift_amount() == 0)) ||
      (operand.IsImmediate() && IsImmConditionalCompare(operand.immediate()))) {
    // The immediate can be encoded in the instruction, or the operand is an
    // unshifted register: call the assembler.
    ConditionalCompare(rn, operand, nzcv, cond, op);
  } else {
    UseScratchRegisterScope temps(this);
    // The operand isn't directly supported by the instruction: perform the
    // operation on a temporary register.
    Register temp = temps.AcquireSameSizeAs(rn);
    VIXL_ASSERT(!temp.Is(rn) && !temp.Is(operand.maybeReg()));
    Mov(temp, operand);
    ConditionalCompare(rn, temp, nzcv, cond, op);
  }
}


void MacroAssembler::Csel(const Register& rd,
                          const Register& rn,
                          const Operand& operand,
                          Condition cond) {
  VIXL_ASSERT(!rd.IsZero());
  VIXL_ASSERT(!rn.IsZero());
  VIXL_ASSERT((cond != al) && (cond != nv));
  // The worst case for size is csel immediate:
  //  * up to 4 instructions to materialise the constant
  //  * 1 instruction for csel
  MacroEmissionCheckScope guard(this);

  if (operand.IsImmediate()) {
    // Immediate argument. Handle special cases of 0, 1 and -1 using zero
    // register.
    int64_t imm = operand.immediate();
    Register zr = AppropriateZeroRegFor(rn);
    if (imm == 0) {
      csel(rd, rn, zr, cond);
    } else if (imm == 1) {
      csinc(rd, rn, zr, cond);
    } else if (imm == -1) {
      csinv(rd, rn, zr, cond);
    } else {
      UseScratchRegisterScope temps(this);
      Register temp = temps.AcquireSameSizeAs(rn);
      VIXL_ASSERT(!temp.Is(rd) && !temp.Is(rn));
      VIXL_ASSERT(!temp.Is(operand.maybeReg()));
      Mov(temp, operand.immediate());
      csel(rd, rn, temp, cond);
    }
  } else if (operand.IsShiftedRegister() && (operand.shift_amount() == 0)) {
    // Unshifted register argument.
    csel(rd, rn, operand.reg(), cond);
  } else {
    // All other arguments.
    UseScratchRegisterScope temps(this);
    Register temp = temps.AcquireSameSizeAs(rn);
    VIXL_ASSERT(!temp.Is(rd) && !temp.Is(rn));
    VIXL_ASSERT(!temp.Is(operand.maybeReg()));
    Mov(temp, operand);
    csel(rd, rn, temp, cond);
  }
}


void MacroAssembler::Add(const Register& rd,
                         const Register& rn,
                         const Operand& operand,
                         FlagsUpdate S) {
  if (operand.IsImmediate() && (operand.immediate() < 0) &&
      IsImmAddSub(-operand.immediate())) {
    AddSubMacro(rd, rn, -operand.immediate(), S, SUB);
  } else {
    AddSubMacro(rd, rn, operand, S, ADD);
  }
}


void MacroAssembler::Adds(const Register& rd,
                          const Register& rn,
                          const Operand& operand) {
  Add(rd, rn, operand, SetFlags);
}


void MacroAssembler::Sub(const Register& rd,
                         const Register& rn,
                         const Operand& operand,
                         FlagsUpdate S) {
  if (operand.IsImmediate() && (operand.immediate() < 0) &&
      IsImmAddSub(-operand.immediate())) {
    AddSubMacro(rd, rn, -operand.immediate(), S, ADD);
  } else {
    AddSubMacro(rd, rn, operand, S, SUB);
  }
}


void MacroAssembler::Subs(const Register& rd,
                          const Register& rn,
                          const Operand& operand) {
  Sub(rd, rn, operand, SetFlags);
}


void MacroAssembler::Cmn(const Register& rn, const Operand& operand) {
  Adds(AppropriateZeroRegFor(rn), rn, operand);
}


void MacroAssembler::Cmp(const Register& rn, const Operand& operand) {
  Subs(AppropriateZeroRegFor(rn), rn, operand);
}


void MacroAssembler::Fcmp(const FPRegister& fn, double value,
                          FPTrapFlags trap) {
  // The worst case for size is:
  //  * 1 to materialise the constant, using literal pool if necessary
  //  * 1 instruction for fcmp{e}
  MacroEmissionCheckScope guard(this);
  if (value != 0.0) {
    UseScratchRegisterScope temps(this);
    FPRegister tmp = temps.AcquireSameSizeAs(fn);
    VIXL_ASSERT(!tmp.Is(fn));
    Fmov(tmp, value);
    FPCompareMacro(fn, tmp, trap);
  } else {
    FPCompareMacro(fn, value, trap);
  }
}


void MacroAssembler::Fcmpe(const FPRegister& fn, double value) {
  Fcmp(fn, value, EnableTrap);
}


void MacroAssembler::Fmov(VRegister vd, double imm) {
  // Floating point immediates are loaded through the literal pool.
  MacroEmissionCheckScope guard(this);

  if (vd.Is1S() || vd.Is2S() || vd.Is4S()) {
    Fmov(vd, static_cast<float>(imm));
    return;
  }

  VIXL_ASSERT(vd.Is1D() || vd.Is2D());
  if (IsImmFP64(imm)) {
    fmov(vd, imm);
  } else {
    uint64_t rawbits = double_to_rawbits(imm);
    if (vd.IsScalar()) {
      if (rawbits == 0) {
        fmov(vd, xzr);
      } else {
        Assembler::fImmPool64(vd, imm);
      }
    } else {
      // TODO: consider NEON support for load literal.
      Movi(vd, rawbits);
    }
  }
}


void MacroAssembler::Fmov(VRegister vd, float imm) {
  // Floating point immediates are loaded through the literal pool.
  MacroEmissionCheckScope guard(this);

  if (vd.Is1D() || vd.Is2D()) {
    Fmov(vd, static_cast<double>(imm));
    return;
  }

  VIXL_ASSERT(vd.Is1S() || vd.Is2S() || vd.Is4S());
  if (IsImmFP32(imm)) {
    fmov(vd, imm);
  } else {
    uint32_t rawbits = float_to_rawbits(imm);
    if (vd.IsScalar()) {
      if (rawbits == 0) {
        fmov(vd, wzr);
      } else {
        Assembler::fImmPool32(vd, imm);
      }
    } else {
      // TODO: consider NEON support for load literal.
      Movi(vd, rawbits);
    }
  }
}



void MacroAssembler::Neg(const Register& rd,
                         const Operand& operand) {
  if (operand.IsImmediate()) {
    Mov(rd, -operand.immediate());
  } else {
    Sub(rd, AppropriateZeroRegFor(rd), operand);
  }
}


void MacroAssembler::Negs(const Register& rd,
                          const Operand& operand) {
  Subs(rd, AppropriateZeroRegFor(rd), operand);
}


bool MacroAssembler::TryOneInstrMoveImmediate(const Register& dst,
                                              int64_t imm) {
  return OneInstrMoveImmediateHelper(this, dst, imm);
}


Operand MacroAssembler::MoveImmediateForShiftedOp(const Register& dst,
                                                  int64_t imm,
                                                  PreShiftImmMode mode) {
  int reg_size = dst.size();

  // Encode the immediate in a single move instruction, if possible.
  if (TryOneInstrMoveImmediate(dst, imm)) {
    // The move was successful; nothing to do here.
  } else {
    // Pre-shift the immediate to the least-significant bits of the register.
    int shift_low = CountTrailingZeros(imm, reg_size);
    if (mode == kLimitShiftForSP) {
      // When applied to the stack pointer, the subsequent arithmetic operation
      // can use the extend form to shift left by a maximum of four bits. Right
      // shifts are not allowed, so we filter them out later before the new
      // immediate is tested.
      shift_low = std::min(shift_low, 4);
    }

    int64_t imm_low = imm >> shift_low;

    // Pre-shift the immediate to the most-significant bits of the register,
    // inserting set bits in the least-significant bits.
    int shift_high = CountLeadingZeros(imm, reg_size);
    int64_t imm_high = (imm << shift_high) | ((INT64_C(1) << shift_high) - 1);

    if ((mode != kNoShift) && TryOneInstrMoveImmediate(dst, imm_low)) {
      // The new immediate has been moved into the destination's low bits:
      // return a new leftward-shifting operand.
      return Operand(dst, LSL, shift_low);
    } else if ((mode == kAnyShift) && TryOneInstrMoveImmediate(dst, imm_high)) {
      // The new immediate has been moved into the destination's high bits:
      // return a new rightward-shifting operand.
      return Operand(dst, LSR, shift_high);
    } else {
      Mov(dst, imm);
    }
  }
  return Operand(dst);
}


void MacroAssembler::ComputeAddress(const Register& dst,
                                    const MemOperand& mem_op) {
  // We cannot handle pre-indexing or post-indexing.
  VIXL_ASSERT(mem_op.addrmode() == Offset);
  Register base = mem_op.base();
  if (mem_op.IsImmediateOffset()) {
    Add(dst, base, mem_op.offset());
  } else {
    VIXL_ASSERT(mem_op.IsRegisterOffset());
    Register reg_offset = mem_op.regoffset();
    Shift shift = mem_op.shift();
    Extend extend = mem_op.extend();
    if (shift == NO_SHIFT) {
      VIXL_ASSERT(extend != NO_EXTEND);
      Add(dst, base, Operand(reg_offset, extend, mem_op.shift_amount()));
    } else {
      VIXL_ASSERT(extend == NO_EXTEND);
      Add(dst, base, Operand(reg_offset, shift, mem_op.shift_amount()));
    }
  }
}


void MacroAssembler::AddSubMacro(const Register& rd,
                                 const Register& rn,
                                 const Operand& operand,
                                 FlagsUpdate S,
                                 AddSubOp op) {
  // Worst case is add/sub immediate:
  //  * up to 4 instructions to materialise the constant
  //  * 1 instruction for add/sub
  MacroEmissionCheckScope guard(this);

  if (operand.IsZero() && rd.Is(rn) && rd.Is64Bits() && rn.Is64Bits() &&
      (S == LeaveFlags)) {
    // The instruction would be a nop. Avoid generating useless code.
    return;
  }

  if ((operand.IsImmediate() && !IsImmAddSub(operand.immediate())) ||
      (rn.IsZero() && !operand.IsShiftedRegister()) ||
      (operand.IsShiftedRegister() && (operand.shift() == ROR))) {
    UseScratchRegisterScope temps(this);
    Register temp = temps.AcquireSameSizeAs(rn);
    if (operand.IsImmediate()) {
      PreShiftImmMode mode = kAnyShift;

      // If the destination or source register is the stack pointer, we can
      // only pre-shift the immediate right by values supported in the add/sub
      // extend encoding.
      if (rd.IsSP()) {
        // If the destination is SP and flags will be set, we can't pre-shift
        // the immediate at all. 
        mode = (S == SetFlags) ? kNoShift : kLimitShiftForSP;
      } else if (rn.IsSP()) {
        mode = kLimitShiftForSP;
      } 

      Operand imm_operand =
          MoveImmediateForShiftedOp(temp, operand.immediate(), mode);
      AddSub(rd, rn, imm_operand, S, op); 
    } else {
      Mov(temp, operand);
      AddSub(rd, rn, temp, S, op);
    }
  } else {
    AddSub(rd, rn, operand, S, op);
  }
}


void MacroAssembler::Adc(const Register& rd,
                         const Register& rn,
                         const Operand& operand) {
  AddSubWithCarryMacro(rd, rn, operand, LeaveFlags, ADC);
}


void MacroAssembler::Adcs(const Register& rd,
                          const Register& rn,
                          const Operand& operand) {
  AddSubWithCarryMacro(rd, rn, operand, SetFlags, ADC);
}


void MacroAssembler::Sbc(const Register& rd,
                         const Register& rn,
                         const Operand& operand) {
  AddSubWithCarryMacro(rd, rn, operand, LeaveFlags, SBC);
}


void MacroAssembler::Sbcs(const Register& rd,
                          const Register& rn,
                          const Operand& operand) {
  AddSubWithCarryMacro(rd, rn, operand, SetFlags, SBC);
}


void MacroAssembler::Ngc(const Register& rd,
                         const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rd);
  Sbc(rd, zr, operand);
}


void MacroAssembler::Ngcs(const Register& rd,
                         const Operand& operand) {
  Register zr = AppropriateZeroRegFor(rd);
  Sbcs(rd, zr, operand);
}


void MacroAssembler::AddSubWithCarryMacro(const Register& rd,
                                          const Register& rn,
                                          const Operand& operand,
                                          FlagsUpdate S,
                                          AddSubWithCarryOp op) {
  VIXL_ASSERT(rd.size() == rn.size());
  // Worst case is addc/subc immediate:
  //  * up to 4 instructions to materialise the constant
  //  * 1 instruction for add/sub
  MacroEmissionCheckScope guard(this);
  UseScratchRegisterScope temps(this);

  if (operand.IsImmediate() ||
      (operand.IsShiftedRegister() && (operand.shift() == ROR))) {
    // Add/sub with carry (immediate or ROR shifted register.)
    Register temp = temps.AcquireSameSizeAs(rn);
    VIXL_ASSERT(!temp.Is(rd) && !temp.Is(rn) && !temp.Is(operand.maybeReg()));
    Mov(temp, operand);
    AddSubWithCarry(rd, rn, Operand(temp), S, op);
  } else if (operand.IsShiftedRegister() && (operand.shift_amount() != 0)) {
    // Add/sub with carry (shifted register).
    VIXL_ASSERT(operand.reg().size() == rd.size());
    VIXL_ASSERT(operand.shift() != ROR);
    VIXL_ASSERT(is_uintn(rd.size() == kXRegSize ? kXRegSizeLog2 : kWRegSizeLog2,
                    operand.shift_amount()));
    temps.Exclude(operand.reg());
    Register temp = temps.AcquireSameSizeAs(rn);
    VIXL_ASSERT(!temp.Is(rd) && !temp.Is(rn) && !temp.Is(operand.maybeReg()));
    EmitShift(temp, operand.reg(), operand.shift(), operand.shift_amount());
    AddSubWithCarry(rd, rn, Operand(temp), S, op);
  } else if (operand.IsExtendedRegister()) {
    // Add/sub with carry (extended register).
    VIXL_ASSERT(operand.reg().size() <= rd.size());
    // Add/sub extended supports a shift <= 4. We want to support exactly the
    // same modes.
    VIXL_ASSERT(operand.shift_amount() <= 4);
    VIXL_ASSERT(operand.reg().Is64Bits() ||
           ((operand.extend() != UXTX) && (operand.extend() != SXTX)));
    temps.Exclude(operand.reg());
    Register temp = temps.AcquireSameSizeAs(rn);
    VIXL_ASSERT(!temp.Is(rd) && !temp.Is(rn) && !temp.Is(operand.maybeReg()));
    EmitExtendShift(temp, operand.reg(), operand.extend(),
                    operand.shift_amount());
    AddSubWithCarry(rd, rn, Operand(temp), S, op);
  } else {
    // The addressing mode is directly supported by the instruction.
    AddSubWithCarry(rd, rn, operand, S, op);
  }
}


#define DEFINE_FUNCTION(FN, REGTYPE, REG, OP)                         \
void MacroAssembler::FN(const REGTYPE REG, const MemOperand& addr) {  \
  LoadStoreMacro(REG, addr, OP);                                      \
}
LS_MACRO_LIST(DEFINE_FUNCTION)
#undef DEFINE_FUNCTION


void MacroAssembler::LoadStoreMacro(const CPURegister& rt,
                                    const MemOperand& addr,
                                    LoadStoreOp op) {
  // Worst case is ldr/str pre/post index:
  //  * 1 instruction for ldr/str
  //  * up to 4 instructions to materialise the constant
  //  * 1 instruction to update the base
  MacroEmissionCheckScope guard(this);

  int64_t offset = addr.offset();
  unsigned access_size = CalcLSDataSize(op);

  // Check if an immediate offset fits in the immediate field of the
  // appropriate instruction. If not, emit two instructions to perform
  // the operation.
  if (addr.IsImmediateOffset() && !IsImmLSScaled(offset, access_size) &&
      !IsImmLSUnscaled(offset)) {
    // Immediate offset that can't be encoded using unsigned or unscaled
    // addressing modes.
    UseScratchRegisterScope temps(this);
    Register temp = temps.AcquireSameSizeAs(addr.base());
    VIXL_ASSERT(!temp.Is(rt));
    VIXL_ASSERT(!temp.Is(addr.base()) && !temp.Is(addr.regoffset()));
    Mov(temp, addr.offset());
    LoadStore(rt, MemOperand(addr.base(), temp), op);
  } else if (addr.IsPostIndex() && !IsImmLSUnscaled(offset)) {
    // Post-index beyond unscaled addressing range.
    LoadStore(rt, MemOperand(addr.base()), op);
    Add(addr.base(), addr.base(), Operand(offset));
  } else if (addr.IsPreIndex() && !IsImmLSUnscaled(offset)) {
    // Pre-index beyond unscaled addressing range.
    Add(addr.base(), addr.base(), Operand(offset));
    LoadStore(rt, MemOperand(addr.base()), op);
  } else {
    // Encodable in one load/store instruction.
    LoadStore(rt, addr, op);
  }
}


#define DEFINE_FUNCTION(FN, REGTYPE, REG, REG2, OP)  \
void MacroAssembler::FN(const REGTYPE REG,           \
                        const REGTYPE REG2,          \
                        const MemOperand& addr) {    \
  LoadStorePairMacro(REG, REG2, addr, OP);           \
}
LSPAIR_MACRO_LIST(DEFINE_FUNCTION)
#undef DEFINE_FUNCTION

void MacroAssembler::LoadStorePairMacro(const CPURegister& rt,
                                        const CPURegister& rt2,
                                        const MemOperand& addr,
                                        LoadStorePairOp op) {
  // TODO(all): Should we support register offset for load-store-pair?
  VIXL_ASSERT(!addr.IsRegisterOffset());
  // Worst case is ldp/stp immediate:
  //  * 1 instruction for ldp/stp
  //  * up to 4 instructions to materialise the constant
  //  * 1 instruction to update the base
  MacroEmissionCheckScope guard(this);

  int64_t offset = addr.offset();
  unsigned access_size = CalcLSPairDataSize(op);

  // Check if the offset fits in the immediate field of the appropriate
  // instruction. If not, emit two instructions to perform the operation.
  if (IsImmLSPair(offset, access_size)) {
    // Encodable in one load/store pair instruction.
    LoadStorePair(rt, rt2, addr, op);
  } else {
    Register base = addr.base();
    if (addr.IsImmediateOffset()) {
      UseScratchRegisterScope temps(this);
      Register temp = temps.AcquireSameSizeAs(base);
      Add(temp, base, offset);
      LoadStorePair(rt, rt2, MemOperand(temp), op);
    } else if (addr.IsPostIndex()) {
      LoadStorePair(rt, rt2, MemOperand(base), op);
      Add(base, base, offset);
    } else {
      VIXL_ASSERT(addr.IsPreIndex());
      Add(base, base, offset);
      LoadStorePair(rt, rt2, MemOperand(base), op);
    }
  }
}


void MacroAssembler::Prfm(PrefetchOperation op, const MemOperand& addr) {
  MacroEmissionCheckScope guard(this);

  // There are no pre- or post-index modes for prfm.
  VIXL_ASSERT(addr.IsImmediateOffset() || addr.IsRegisterOffset());

  // The access size is implicitly 8 bytes for all prefetch operations.
  unsigned size = kXRegSizeInBytesLog2;

  // Check if an immediate offset fits in the immediate field of the
  // appropriate instruction. If not, emit two instructions to perform
  // the operation.
  if (addr.IsImmediateOffset() && !IsImmLSScaled(addr.offset(), size) &&
      !IsImmLSUnscaled(addr.offset())) {
    // Immediate offset that can't be encoded using unsigned or unscaled
    // addressing modes.
    UseScratchRegisterScope temps(this);
    Register temp = temps.AcquireSameSizeAs(addr.base());
    Mov(temp, addr.offset());
    Prefetch(op, MemOperand(addr.base(), temp));
  } else {
    // Simple register-offsets are encodable in one instruction.
    Prefetch(op, addr);
  }
}


void MacroAssembler::PushStackPointer() {
  PrepareForPush(1, 8);

  // Pushing a stack pointer leads to implementation-defined
  // behavior, which may be surprising. In particular,
  //   str x28, [x28, #-8]!
  // pre-decrements the stack pointer, storing the decremented value.
  // Additionally, sp is read as xzr in this context, so it cannot be pushed.
  // So we must use a scratch register.
  UseScratchRegisterScope temps(this);
  Register scratch = temps.AcquireX();

  Mov(scratch, GetStackPointer64());
  str(scratch, MemOperand(GetStackPointer64(), -8, PreIndex));
}


void MacroAssembler::Push(const CPURegister& src0, const CPURegister& src1,
                          const CPURegister& src2, const CPURegister& src3) {
  VIXL_ASSERT(AreSameSizeAndType(src0, src1, src2, src3));
  VIXL_ASSERT(src0.IsValid());

  int count = 1 + src1.IsValid() + src2.IsValid() + src3.IsValid();
  int size = src0.SizeInBytes();

  if (src0.Is(GetStackPointer64())) {
    VIXL_ASSERT(count == 1);
    VIXL_ASSERT(size == 8);
    PushStackPointer();
    return;
  }

  PrepareForPush(count, size);
  PushHelper(count, size, src0, src1, src2, src3);
}


void MacroAssembler::Pop(const CPURegister& dst0, const CPURegister& dst1,
                         const CPURegister& dst2, const CPURegister& dst3) {
  // It is not valid to pop into the same register more than once in one
  // instruction, not even into the zero register.
  VIXL_ASSERT(!AreAliased(dst0, dst1, dst2, dst3));
  VIXL_ASSERT(AreSameSizeAndType(dst0, dst1, dst2, dst3));
  VIXL_ASSERT(dst0.IsValid());

  int count = 1 + dst1.IsValid() + dst2.IsValid() + dst3.IsValid();
  int size = dst0.SizeInBytes();

  PrepareForPop(count, size);
  PopHelper(count, size, dst0, dst1, dst2, dst3);
}


void MacroAssembler::PushCPURegList(CPURegList registers) {
  VIXL_ASSERT(!registers.Overlaps(*TmpList()));
  VIXL_ASSERT(!registers.Overlaps(*FPTmpList()));

  int reg_size = registers.RegisterSizeInBytes();
  PrepareForPush(registers.Count(), reg_size);

  // Bump the stack pointer and store two registers at the bottom.
  int size = registers.TotalSizeInBytes();
  const CPURegister& bottom_0 = registers.PopLowestIndex();
  const CPURegister& bottom_1 = registers.PopLowestIndex();
  if (bottom_0.IsValid() && bottom_1.IsValid()) {
    Stp(bottom_0, bottom_1, MemOperand(GetStackPointer64(), -size, PreIndex));
  } else if (bottom_0.IsValid()) {
    Str(bottom_0, MemOperand(GetStackPointer64(), -size, PreIndex));
  }

  int offset = 2 * reg_size;
  while (!registers.IsEmpty()) {
    const CPURegister& src0 = registers.PopLowestIndex();
    const CPURegister& src1 = registers.PopLowestIndex();
    if (src1.IsValid()) {
      Stp(src0, src1, MemOperand(GetStackPointer64(), offset));
    } else {
      Str(src0, MemOperand(GetStackPointer64(), offset));
    }
    offset += 2 * reg_size;
  }
}


void MacroAssembler::PopCPURegList(CPURegList registers) {
  VIXL_ASSERT(!registers.Overlaps(*TmpList()));
  VIXL_ASSERT(!registers.Overlaps(*FPTmpList()));

  int reg_size = registers.RegisterSizeInBytes();
  PrepareForPop(registers.Count(), reg_size);


  int size = registers.TotalSizeInBytes();
  const CPURegister& bottom_0 = registers.PopLowestIndex();
  const CPURegister& bottom_1 = registers.PopLowestIndex();

  int offset = 2 * reg_size;
  while (!registers.IsEmpty()) {
    const CPURegister& dst0 = registers.PopLowestIndex();
    const CPURegister& dst1 = registers.PopLowestIndex();
    if (dst1.IsValid()) {
      Ldp(dst0, dst1, MemOperand(GetStackPointer64(), offset));
    } else {
      Ldr(dst0, MemOperand(GetStackPointer64(), offset));
    }
    offset += 2 * reg_size;
  }

  // Load the two registers at the bottom and drop the stack pointer.
  if (bottom_0.IsValid() && bottom_1.IsValid()) {
    Ldp(bottom_0, bottom_1, MemOperand(GetStackPointer64(), size, PostIndex));
  } else if (bottom_0.IsValid()) {
    Ldr(bottom_0, MemOperand(GetStackPointer64(), size, PostIndex));
  }
}


void MacroAssembler::PushMultipleTimes(int count, Register src) {
  int size = src.SizeInBytes();

  PrepareForPush(count, size);
  // Push up to four registers at a time if possible because if the current
  // stack pointer is sp and the register size is 32, registers must be pushed
  // in blocks of four in order to maintain the 16-byte alignment for sp.
  while (count >= 4) {
    PushHelper(4, size, src, src, src, src);
    count -= 4;
  }
  if (count >= 2) {
    PushHelper(2, size, src, src, NoReg, NoReg);
    count -= 2;
  }
  if (count == 1) {
    PushHelper(1, size, src, NoReg, NoReg, NoReg);
    count -= 1;
  }
  VIXL_ASSERT(count == 0);
}


void MacroAssembler::PushHelper(int count, int size,
                                const CPURegister& src0,
                                const CPURegister& src1,
                                const CPURegister& src2,
                                const CPURegister& src3) {
  // Ensure that we don't unintentionally modify scratch or debug registers.
  // Worst case for size is 2 stp.
  InstructionAccurateScope scope(this, 2,
                                 InstructionAccurateScope::kMaximumSize);

  VIXL_ASSERT(AreSameSizeAndType(src0, src1, src2, src3));
  VIXL_ASSERT(size == src0.SizeInBytes());

  // Pushing the stack pointer has unexpected behavior. See PushStackPointer().
  VIXL_ASSERT(!src0.Is(GetStackPointer64()) && !src0.Is(sp));
  VIXL_ASSERT(!src1.Is(GetStackPointer64()) && !src1.Is(sp));
  VIXL_ASSERT(!src2.Is(GetStackPointer64()) && !src2.Is(sp));
  VIXL_ASSERT(!src3.Is(GetStackPointer64()) && !src3.Is(sp));

  // The JS engine should never push 4 bytes.
  VIXL_ASSERT(size >= 8);

  // When pushing multiple registers, the store order is chosen such that
  // Push(a, b) is equivalent to Push(a) followed by Push(b).
  switch (count) {
    case 1:
      VIXL_ASSERT(src1.IsNone() && src2.IsNone() && src3.IsNone());
      str(src0, MemOperand(GetStackPointer64(), -1 * size, PreIndex));
      break;
    case 2:
      VIXL_ASSERT(src2.IsNone() && src3.IsNone());
      stp(src1, src0, MemOperand(GetStackPointer64(), -2 * size, PreIndex));
      break;
    case 3:
      VIXL_ASSERT(src3.IsNone());
      stp(src2, src1, MemOperand(GetStackPointer64(), -3 * size, PreIndex));
      str(src0, MemOperand(GetStackPointer64(), 2 * size));
      break;
    case 4:
      // Skip over 4 * size, then fill in the gap. This allows four W registers
      // to be pushed using sp, whilst maintaining 16-byte alignment for sp at
      // all times.
      stp(src3, src2, MemOperand(GetStackPointer64(), -4 * size, PreIndex));
      stp(src1, src0, MemOperand(GetStackPointer64(), 2 * size));
      break;
    default:
      VIXL_UNREACHABLE();
  }
}


void MacroAssembler::PopHelper(int count, int size,
                               const CPURegister& dst0,
                               const CPURegister& dst1,
                               const CPURegister& dst2,
                               const CPURegister& dst3) {
  // Ensure that we don't unintentionally modify scratch or debug registers.
  // Worst case for size is 2 ldp.
  InstructionAccurateScope scope(this, 2,
                                 InstructionAccurateScope::kMaximumSize);

  VIXL_ASSERT(AreSameSizeAndType(dst0, dst1, dst2, dst3));
  VIXL_ASSERT(size == dst0.SizeInBytes());

  // When popping multiple registers, the load order is chosen such that
  // Pop(a, b) is equivalent to Pop(a) followed by Pop(b).
  switch (count) {
    case 1:
      VIXL_ASSERT(dst1.IsNone() && dst2.IsNone() && dst3.IsNone());
      ldr(dst0, MemOperand(GetStackPointer64(), 1 * size, PostIndex));
      break;
    case 2:
      VIXL_ASSERT(dst2.IsNone() && dst3.IsNone());
      ldp(dst0, dst1, MemOperand(GetStackPointer64(), 2 * size, PostIndex));
      break;
    case 3:
      VIXL_ASSERT(dst3.IsNone());
      ldr(dst2, MemOperand(GetStackPointer64(), 2 * size));
      ldp(dst0, dst1, MemOperand(GetStackPointer64(), 3 * size, PostIndex));
      break;
    case 4:
      // Load the higher addresses first, then load the lower addresses and skip
      // the whole block in the second instruction. This allows four W registers
      // to be popped using sp, whilst maintaining 16-byte alignment for sp at
      // all times.
      ldp(dst2, dst3, MemOperand(GetStackPointer64(), 2 * size));
      ldp(dst0, dst1, MemOperand(GetStackPointer64(), 4 * size, PostIndex));
      break;
    default:
      VIXL_UNREACHABLE();
  }
}


void MacroAssembler::PrepareForPush(int count, int size) {
  if (sp.Is(GetStackPointer64())) {
    // If the current stack pointer is sp, then it must be aligned to 16 bytes
    // on entry and the total size of the specified registers must also be a
    // multiple of 16 bytes.
    VIXL_ASSERT((count * size) % 16 == 0);
  } else {
    // Even if the current stack pointer is not the system stack pointer (sp),
    // the system stack pointer will still be modified in order to comply with
    // ABI rules about accessing memory below the system stack pointer.
    BumpSystemStackPointer(count * size);
  }
}


void MacroAssembler::PrepareForPop(int count, int size) {
  USE(count, size);
  if (sp.Is(GetStackPointer64())) {
    // If the current stack pointer is sp, then it must be aligned to 16 bytes
    // on entry and the total size of the specified registers must also be a
    // multiple of 16 bytes.
    VIXL_ASSERT((count * size) % 16 == 0);
  }
}

void MacroAssembler::Poke(const Register& src, const Operand& offset) {
  if (offset.IsImmediate()) {
    VIXL_ASSERT(offset.immediate() >= 0);
  }

  Str(src, MemOperand(GetStackPointer64(), offset));
}


void MacroAssembler::Peek(const Register& dst, const Operand& offset) {
  if (offset.IsImmediate()) {
    VIXL_ASSERT(offset.immediate() >= 0);
  }

  Ldr(dst, MemOperand(GetStackPointer64(), offset));
}


void MacroAssembler::Claim(const Operand& size) {

  if (size.IsZero()) {
    return;
  }

  if (size.IsImmediate()) {
    VIXL_ASSERT(size.immediate() > 0);
    if (sp.Is(GetStackPointer64())) {
      VIXL_ASSERT((size.immediate() % 16) == 0);
    }
  }

  Sub(GetStackPointer64(), GetStackPointer64(), size);

  // Make sure the real stack pointer reflects the claimed stack space.
  // We can't use stack memory below the stack pointer, it could be clobbered by
  // interupts and signal handlers.
  if (!sp.Is(GetStackPointer64())) {
    Mov(sp, GetStackPointer64());
  }
}


void MacroAssembler::Drop(const Operand& size) {

  if (size.IsZero()) {
    return;
  }

  if (size.IsImmediate()) {
    VIXL_ASSERT(size.immediate() > 0);
    if (sp.Is(GetStackPointer64())) {
      VIXL_ASSERT((size.immediate() % 16) == 0);
    }
  }

  Add(GetStackPointer64(), GetStackPointer64(), size);
}


void MacroAssembler::PushCalleeSavedRegisters() {
  // Ensure that the macro-assembler doesn't use any scratch registers.
  // 10 stp will be emitted.
  // TODO(all): Should we use GetCalleeSaved and SavedFP.
  InstructionAccurateScope scope(this, 10);

  // This method must not be called unless the current stack pointer is sp.
  VIXL_ASSERT(sp.Is(GetStackPointer64()));

  MemOperand tos(sp, -2 * static_cast<int>(kXRegSizeInBytes), PreIndex);

  stp(x29, x30, tos);
  stp(x27, x28, tos);
  stp(x25, x26, tos);
  stp(x23, x24, tos);
  stp(x21, x22, tos);
  stp(x19, x20, tos);

  stp(d14, d15, tos);
  stp(d12, d13, tos);
  stp(d10, d11, tos);
  stp(d8, d9, tos);
}


void MacroAssembler::PopCalleeSavedRegisters() {
  // Ensure that the macro-assembler doesn't use any scratch registers.
  // 10 ldp will be emitted.
  // TODO(all): Should we use GetCalleeSaved and SavedFP.
  InstructionAccurateScope scope(this, 10);

  // This method must not be called unless the current stack pointer is sp.
  VIXL_ASSERT(sp.Is(GetStackPointer64()));

  MemOperand tos(sp, 2 * kXRegSizeInBytes, PostIndex);

  ldp(d8, d9, tos);
  ldp(d10, d11, tos);
  ldp(d12, d13, tos);
  ldp(d14, d15, tos);

  ldp(x19, x20, tos);
  ldp(x21, x22, tos);
  ldp(x23, x24, tos);
  ldp(x25, x26, tos);
  ldp(x27, x28, tos);
  ldp(x29, x30, tos);
}

void MacroAssembler::LoadCPURegList(CPURegList registers,
                                    const MemOperand& src) {
  LoadStoreCPURegListHelper(kLoad, registers, src);
}

void MacroAssembler::StoreCPURegList(CPURegList registers,
                                     const MemOperand& dst) {
  LoadStoreCPURegListHelper(kStore, registers, dst);
}


void MacroAssembler::LoadStoreCPURegListHelper(LoadStoreCPURegListAction op,
                                               CPURegList registers,
                                               const MemOperand& mem) {
  // We do not handle pre-indexing or post-indexing.
  VIXL_ASSERT(!(mem.IsPreIndex() || mem.IsPostIndex()));
  VIXL_ASSERT(!registers.Overlaps(tmp_list_));
  VIXL_ASSERT(!registers.Overlaps(fptmp_list_));
  VIXL_ASSERT(!registers.IncludesAliasOf(sp));

  UseScratchRegisterScope temps(this);

  MemOperand loc = BaseMemOperandForLoadStoreCPURegList(registers,
                                                        mem,
                                                        &temps);

  while (registers.Count() >= 2) {
    const CPURegister& dst0 = registers.PopLowestIndex();
    const CPURegister& dst1 = registers.PopLowestIndex();
    if (op == kStore) {
      Stp(dst0, dst1, loc);
    } else {
      VIXL_ASSERT(op == kLoad);
      Ldp(dst0, dst1, loc);
    }
    loc.AddOffset(2 * registers.RegisterSizeInBytes());
  }
  if (!registers.IsEmpty()) {
    if (op == kStore) {
      Str(registers.PopLowestIndex(), loc);
    } else {
      VIXL_ASSERT(op == kLoad);
      Ldr(registers.PopLowestIndex(), loc);
    }
  }
}

MemOperand MacroAssembler::BaseMemOperandForLoadStoreCPURegList(
    const CPURegList& registers,
    const MemOperand& mem,
    UseScratchRegisterScope* scratch_scope) {
  // If necessary, pre-compute the base address for the accesses.
  if (mem.IsRegisterOffset()) {
    Register reg_base = scratch_scope->AcquireX();
    ComputeAddress(reg_base, mem);
    return MemOperand(reg_base);

  } else if (mem.IsImmediateOffset()) {
    int reg_size = registers.RegisterSizeInBytes();
    int total_size = registers.TotalSizeInBytes();
    int64_t min_offset = mem.offset();
    int64_t max_offset = mem.offset() + std::max(0, total_size - 2 * reg_size);
    if ((registers.Count() >= 2) &&
        (!Assembler::IsImmLSPair(min_offset, WhichPowerOf2(reg_size)) ||
         !Assembler::IsImmLSPair(max_offset, WhichPowerOf2(reg_size)))) {
      Register reg_base = scratch_scope->AcquireX();
      ComputeAddress(reg_base, mem);
      return MemOperand(reg_base);
    }
  }

  return mem;
}

void MacroAssembler::BumpSystemStackPointer(const Operand& space) {
  VIXL_ASSERT(!sp.Is(GetStackPointer64()));
  // TODO: Several callers rely on this not using scratch registers, so we use
  // the assembler directly here. However, this means that large immediate
  // values of 'space' cannot be handled.
  InstructionAccurateScope scope(this, 1);
  sub(sp, GetStackPointer64(), space);
}


void MacroAssembler::Trace(TraceParameters parameters, TraceCommand command) {

#ifdef JS_SIMULATOR_ARM64
  // The arguments to the trace pseudo instruction need to be contiguous in
  // memory, so make sure we don't try to emit a literal pool.
  InstructionAccurateScope scope(this, kTraceLength / kInstructionSize);

  Label start;
  bind(&start);

  // Refer to simulator-a64.h for a description of the marker and its
  // arguments.
  hlt(kTraceOpcode);

  // VIXL_ASSERT(SizeOfCodeGeneratedSince(&start) == kTraceParamsOffset);
  dc32(parameters);

  // VIXL_ASSERT(SizeOfCodeGeneratedSince(&start) == kTraceCommandOffset);
  dc32(command);
#else
  // Emit nothing on real hardware.
  USE(parameters, command);
#endif
}


void MacroAssembler::Log(TraceParameters parameters) {

#ifdef JS_SIMULATOR_ARM64
  // The arguments to the log pseudo instruction need to be contiguous in
  // memory, so make sure we don't try to emit a literal pool.
  InstructionAccurateScope scope(this, kLogLength / kInstructionSize);

  Label start;
  bind(&start);

  // Refer to simulator-a64.h for a description of the marker and its
  // arguments.
  hlt(kLogOpcode);

  // VIXL_ASSERT(SizeOfCodeGeneratedSince(&start) == kLogParamsOffset);
  dc32(parameters);
#else
  // Emit nothing on real hardware.
  USE(parameters);
#endif
}


void MacroAssembler::EnableInstrumentation() {
  VIXL_ASSERT(!isprint(InstrumentStateEnable));
  InstructionAccurateScope scope(this, 1);
  movn(xzr, InstrumentStateEnable);
}


void MacroAssembler::DisableInstrumentation() {
  VIXL_ASSERT(!isprint(InstrumentStateDisable));
  InstructionAccurateScope scope(this, 1);
  movn(xzr, InstrumentStateDisable);
}


void MacroAssembler::AnnotateInstrumentation(const char* marker_name) {
  VIXL_ASSERT(strlen(marker_name) == 2);

  // We allow only printable characters in the marker names. Unprintable
  // characters are reserved for controlling features of the instrumentation.
  VIXL_ASSERT(isprint(marker_name[0]) && isprint(marker_name[1]));

  InstructionAccurateScope scope(this, 1);
  movn(xzr, (marker_name[1] << 8) | marker_name[0]);
}


void UseScratchRegisterScope::Open(MacroAssembler* masm) {
  VIXL_ASSERT(!initialised_);
  available_ = masm->TmpList();
  availablefp_ = masm->FPTmpList();
  old_available_ = available_->list();
  old_availablefp_ = availablefp_->list();
  VIXL_ASSERT(available_->type() == CPURegister::kRegister);
  VIXL_ASSERT(availablefp_->type() == CPURegister::kVRegister);
#ifdef DEBUG
  initialised_ = true;
#endif
}


void UseScratchRegisterScope::Close() {
  if (available_) {
    available_->set_list(old_available_);
    available_ = NULL;
  }
  if (availablefp_) {
    availablefp_->set_list(old_availablefp_);
    availablefp_ = NULL;
  }
#ifdef DEBUG
  initialised_ = false;
#endif
}


UseScratchRegisterScope::UseScratchRegisterScope(MacroAssembler* masm) {
#ifdef DEBUG
  initialised_ = false;
#endif
  Open(masm);
}

// This allows deferred (and optional) initialisation of the scope.
UseScratchRegisterScope::UseScratchRegisterScope()
    : available_(NULL), availablefp_(NULL),
      old_available_(0), old_availablefp_(0) {
#ifdef DEBUG
  initialised_ = false;
#endif
}

UseScratchRegisterScope::~UseScratchRegisterScope() {
  Close();
}


bool UseScratchRegisterScope::IsAvailable(const CPURegister& reg) const {
  return available_->IncludesAliasOf(reg) || availablefp_->IncludesAliasOf(reg);
}


Register UseScratchRegisterScope::AcquireSameSizeAs(const Register& reg) {
  int code = AcquireNextAvailable(available_).code();
  return Register(code, reg.size());
}


FPRegister UseScratchRegisterScope::AcquireSameSizeAs(const FPRegister& reg) {
  int code = AcquireNextAvailable(availablefp_).code();
  return FPRegister(code, reg.size());
}


void UseScratchRegisterScope::Release(const CPURegister& reg) {
  VIXL_ASSERT(initialised_);
  if (reg.IsRegister()) {
    ReleaseByCode(available_, reg.code());
  } else if (reg.IsFPRegister()) {
    ReleaseByCode(availablefp_, reg.code());
  } else {
    VIXL_ASSERT(reg.IsNone());
  }
}


void UseScratchRegisterScope::Include(const CPURegList& list) {
  VIXL_ASSERT(initialised_);
  if (list.type() == CPURegister::kRegister) {
    // Make sure that neither sp nor xzr are included the list.
    IncludeByRegList(available_, list.list() & ~(xzr.Bit() | sp.Bit()));
  } else {
    VIXL_ASSERT(list.type() == CPURegister::kVRegister);
    IncludeByRegList(availablefp_, list.list());
  }
}


void UseScratchRegisterScope::Include(const Register& reg1,
                                      const Register& reg2,
                                      const Register& reg3,
                                      const Register& reg4) {
  VIXL_ASSERT(initialised_);
  RegList include = reg1.Bit() | reg2.Bit() | reg3.Bit() | reg4.Bit();
  // Make sure that neither sp nor xzr are included the list.
  include &= ~(xzr.Bit() | sp.Bit());

  IncludeByRegList(available_, include);
}


void UseScratchRegisterScope::Include(const FPRegister& reg1,
                                      const FPRegister& reg2,
                                      const FPRegister& reg3,
                                      const FPRegister& reg4) {
  RegList include = reg1.Bit() | reg2.Bit() | reg3.Bit() | reg4.Bit();
  IncludeByRegList(availablefp_, include);
}


void UseScratchRegisterScope::Exclude(const CPURegList& list) {
  if (list.type() == CPURegister::kRegister) {
    ExcludeByRegList(available_, list.list());
  } else {
    VIXL_ASSERT(list.type() == CPURegister::kVRegister);
    ExcludeByRegList(availablefp_, list.list());
  }
}


void UseScratchRegisterScope::Exclude(const Register& reg1,
                                      const Register& reg2,
                                      const Register& reg3,
                                      const Register& reg4) {
  RegList exclude = reg1.Bit() | reg2.Bit() | reg3.Bit() | reg4.Bit();
  ExcludeByRegList(available_, exclude);
}


void UseScratchRegisterScope::Exclude(const FPRegister& reg1,
                                      const FPRegister& reg2,
                                      const FPRegister& reg3,
                                      const FPRegister& reg4) {
  RegList excludefp = reg1.Bit() | reg2.Bit() | reg3.Bit() | reg4.Bit();
  ExcludeByRegList(availablefp_, excludefp);
}


void UseScratchRegisterScope::Exclude(const CPURegister& reg1,
                                      const CPURegister& reg2,
                                      const CPURegister& reg3,
                                      const CPURegister& reg4) {
  RegList exclude = 0;
  RegList excludefp = 0;

  const CPURegister regs[] = {reg1, reg2, reg3, reg4};

  for (unsigned i = 0; i < (sizeof(regs) / sizeof(regs[0])); i++) {
    if (regs[i].IsRegister()) {
      exclude |= regs[i].Bit();
    } else if (regs[i].IsFPRegister()) {
      excludefp |= regs[i].Bit();
    } else {
      VIXL_ASSERT(regs[i].IsNone());
    }
  }

  ExcludeByRegList(available_, exclude);
  ExcludeByRegList(availablefp_, excludefp);
}


void UseScratchRegisterScope::ExcludeAll() {
  ExcludeByRegList(available_, available_->list());
  ExcludeByRegList(availablefp_, availablefp_->list());
}


CPURegister UseScratchRegisterScope::AcquireNextAvailable(
    CPURegList* available) {
  VIXL_CHECK(!available->IsEmpty());
  CPURegister result = available->PopLowestIndex();
  VIXL_ASSERT(!AreAliased(result, xzr, sp));
  return result;
}


void UseScratchRegisterScope::ReleaseByCode(CPURegList* available, int code) {
  ReleaseByRegList(available, static_cast<RegList>(1) << code);
}


void UseScratchRegisterScope::ReleaseByRegList(CPURegList* available,
                                               RegList regs) {
  available->set_list(available->list() | regs);
}


void UseScratchRegisterScope::IncludeByRegList(CPURegList* available,
                                               RegList regs) {
  available->set_list(available->list() | regs);
}


void UseScratchRegisterScope::ExcludeByRegList(CPURegList* available,
                                               RegList exclude) {
  available->set_list(available->list() & ~exclude);
}

}  // namespace vixl

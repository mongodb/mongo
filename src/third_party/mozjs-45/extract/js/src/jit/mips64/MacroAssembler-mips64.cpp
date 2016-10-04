/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips64/MacroAssembler-mips64.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"
#include "jit/mips64/Simulator-mips64.h"
#include "jit/MoveEmitter.h"
#include "jit/SharedICRegisters.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace jit;

using mozilla::Abs;

static_assert(sizeof(intptr_t) == 8, "Not 32-bit clean.");

void
MacroAssemblerMIPS64Compat::convertBoolToInt32(Register src, Register dest)
{
    // Note that C++ bool is only 1 byte, so zero extend it to clear the
    // higher-order bits.
    ma_and(dest, src, Imm32(0xff));
}

void
MacroAssemblerMIPS64Compat::convertInt32ToDouble(Register src, FloatRegister dest)
{
    as_mtc1(src, dest);
    as_cvtdw(dest, dest);
}

void
MacroAssemblerMIPS64Compat::convertInt32ToDouble(const Address& src, FloatRegister dest)
{
    ma_ls(dest, src);
    as_cvtdw(dest, dest);
}

void
MacroAssemblerMIPS64Compat::convertInt32ToDouble(const BaseIndex& src, FloatRegister dest)
{
    computeScaledAddress(src, ScratchRegister);
    convertInt32ToDouble(Address(ScratchRegister, src.offset), dest);
}

void
MacroAssemblerMIPS64Compat::convertUInt32ToDouble(Register src, FloatRegister dest)
{
    // We use SecondScratchDoubleReg because MacroAssembler::loadFromTypedArray
    // calls with ScratchDoubleReg as dest.
    MOZ_ASSERT(dest != SecondScratchDoubleReg);

    // Subtract INT32_MIN to get a positive number
    ma_subu(ScratchRegister, src, Imm32(INT32_MIN));

    // Convert value
    as_mtc1(ScratchRegister, dest);
    as_cvtdw(dest, dest);

    // Add unsigned value of INT32_MIN
    ma_lid(SecondScratchDoubleReg, 2147483648.0);
    as_addd(dest, dest, SecondScratchDoubleReg);
}

void
MacroAssemblerMIPS64Compat::convertUInt64ToDouble(Register64 src, Register temp, FloatRegister dest)
{
    Label positive, done;
    ma_b(src.reg, src.reg, &positive, NotSigned, ShortJump);

    MOZ_ASSERT(src.reg != ScratchRegister);
    MOZ_ASSERT(src.reg != SecondScratchReg);

    ma_and(ScratchRegister, src.reg, Imm32(1));
    ma_dsrl(SecondScratchReg, src.reg, Imm32(1));
    ma_or(ScratchRegister, SecondScratchReg);
    as_dmtc1(ScratchRegister, dest);
    as_cvtdl(dest, dest);
    addDouble(dest, dest);
    ma_b(&done, ShortJump);

    bind(&positive);
    as_dmtc1(src.reg, dest);
    as_cvtdl(dest, dest);

    bind(&done);
}

void
MacroAssemblerMIPS64Compat::convertUInt32ToFloat32(Register src, FloatRegister dest)
{
    Label positive, done;
    ma_b(src, src, &positive, NotSigned, ShortJump);

    // We cannot do the same as convertUInt32ToDouble because float32 doesn't
    // have enough precision.
    convertUInt32ToDouble(src, dest);
    convertDoubleToFloat32(dest, dest);
    ma_b(&done, ShortJump);

    bind(&positive);
    convertInt32ToFloat32(src, dest);

    bind(&done);
}

void
MacroAssemblerMIPS64Compat::convertDoubleToFloat32(FloatRegister src, FloatRegister dest)
{
    as_cvtsd(dest, src);
}

// Convert the floating point value to an integer, if it did not fit, then it
// was clamped to INT32_MIN/INT32_MAX, and we can test it.
// NOTE: if the value really was supposed to be INT32_MAX / INT32_MIN then it
// will be wrong.
void
MacroAssemblerMIPS64Compat::branchTruncateDouble(FloatRegister src, Register dest,
                                                 Label* fail)
{
    Label test, success;
    as_truncwd(ScratchDoubleReg, src);
    as_mfc1(dest, ScratchDoubleReg);

    ma_b(dest, Imm32(INT32_MAX), fail, Assembler::Equal);
    ma_b(dest, Imm32(INT32_MIN), fail, Assembler::Equal);
}

// Checks whether a double is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void
MacroAssemblerMIPS64Compat::convertDoubleToInt32(FloatRegister src, Register dest,
                                                 Label* fail, bool negativeZeroCheck)
{
    // Convert double to int, then convert back and check if we have the
    // same number.
    as_cvtwd(ScratchDoubleReg, src);
    as_mfc1(dest, ScratchDoubleReg);
    as_cvtdw(ScratchDoubleReg, ScratchDoubleReg);
    ma_bc1d(src, ScratchDoubleReg, fail, Assembler::DoubleNotEqualOrUnordered);

    if (negativeZeroCheck) {
        Label notZero;
        ma_b(dest, Imm32(0), &notZero, Assembler::NotEqual, ShortJump);
        // Test and bail for -0.0, when integer result is 0
        // Move the top word of the double into the output reg, if it is
        // non-zero, then the original value was -0.0
        moveFromDoubleHi(src, dest);
        ma_b(dest, Imm32(INT32_MIN), fail, Assembler::Equal);
        bind(&notZero);
    }
}

// Checks whether a float32 is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void
MacroAssemblerMIPS64Compat::convertFloat32ToInt32(FloatRegister src, Register dest,
                                                  Label* fail, bool negativeZeroCheck)
{
    // Converting the floating point value to an integer and then converting it
    // back to a float32 would not work, as float to int32 conversions are
    // clamping (e.g. float(INT32_MAX + 1) would get converted into INT32_MAX
    // and then back to float(INT32_MAX + 1)).  If this ever happens, we just
    // bail out.
    as_cvtws(ScratchFloat32Reg, src);
    as_mfc1(dest, ScratchFloat32Reg);
    as_cvtsw(ScratchFloat32Reg, ScratchFloat32Reg);
    ma_bc1s(src, ScratchFloat32Reg, fail, Assembler::DoubleNotEqualOrUnordered);

    // Bail out in the clamped cases.
    ma_b(dest, Imm32(INT32_MAX), fail, Assembler::Equal);

    if (negativeZeroCheck) {
        Label notZero;
        ma_b(dest, Imm32(0), &notZero, Assembler::NotEqual, ShortJump);
        // Test and bail for -0.0, when integer result is 0
        // Move the top word of the double into the output reg,
        // if it is non-zero, then the original value was -0.0
        moveFromDoubleHi(src, dest);
        ma_b(dest, Imm32(INT32_MIN), fail, Assembler::Equal);
        bind(&notZero);
    }
}

void
MacroAssemblerMIPS64Compat::convertFloat32ToDouble(FloatRegister src, FloatRegister dest)
{
    as_cvtds(dest, src);
}

void
MacroAssemblerMIPS64Compat::branchTruncateFloat32(FloatRegister src, Register dest,
                                                  Label* fail)
{
    Label test, success;
    as_truncws(ScratchFloat32Reg, src);
    as_mfc1(dest, ScratchFloat32Reg);

    ma_b(dest, Imm32(INT32_MAX), fail, Assembler::Equal);
}

void
MacroAssemblerMIPS64Compat::convertInt32ToFloat32(Register src, FloatRegister dest)
{
    as_mtc1(src, dest);
    as_cvtsw(dest, dest);
}

void
MacroAssemblerMIPS64Compat::convertInt32ToFloat32(const Address& src, FloatRegister dest)
{
    ma_ls(dest, src);
    as_cvtsw(dest, dest);
}

void
MacroAssemblerMIPS64Compat::addDouble(FloatRegister src, FloatRegister dest)
{
    as_addd(dest, dest, src);
}

void
MacroAssemblerMIPS64Compat::subDouble(FloatRegister src, FloatRegister dest)
{
    as_subd(dest, dest, src);
}

void
MacroAssemblerMIPS64Compat::mulDouble(FloatRegister src, FloatRegister dest)
{
    as_muld(dest, dest, src);
}

void
MacroAssemblerMIPS64Compat::divDouble(FloatRegister src, FloatRegister dest)
{
    as_divd(dest, dest, src);
}

void
MacroAssemblerMIPS64Compat::negateDouble(FloatRegister reg)
{
    as_negd(reg, reg);
}

void
MacroAssemblerMIPS64Compat::inc64(AbsoluteAddress dest)
{
    ma_li(ScratchRegister, ImmWord(uintptr_t(dest.addr)));
    as_ld(SecondScratchReg, ScratchRegister, 0);
    as_daddiu(SecondScratchReg, SecondScratchReg, 1);
    as_sd(SecondScratchReg, ScratchRegister, 0);
}

void
MacroAssemblerMIPS64Compat::movq(Register rs, Register rd)
{
    ma_move(rd, rs);
}

void
MacroAssemblerMIPS64::ma_li(Register dest, CodeOffset* label)
{
    BufferOffset bo = m_buffer.nextOffset();
    ma_liPatchable(dest, ImmWord(/* placeholder */ 0));
    label->bind(bo.getOffset());
}

void
MacroAssemblerMIPS64::ma_li(Register dest, ImmWord imm)
{
    if ((int64_t)imm.value >= INT16_MIN  && (int64_t)imm.value <= INT16_MAX) {
        as_addiu(dest, zero, imm.value);
    } else if (imm.value <= UINT16_MAX) {
        as_ori(dest, zero, Imm16::Lower(Imm32(imm.value)).encode());
    } else if (0 == (imm.value & 0xffff) && 0 == (imm.value >> 32)) {
        as_lui(dest, Imm16::Upper(Imm32(imm.value)).encode());
    } else if (imm.value <= UINT32_MAX) {
        as_lui(dest, Imm16::Upper(Imm32(imm.value)).encode());
        as_ori(dest, dest, Imm16::Lower(Imm32(imm.value)).encode());
    } else {
        if (imm.value >> 48) {
            as_lui(dest, Imm16::Upper(Imm32(imm.value >> 32)).encode());
            if ((imm.value >> 32) & 0xffff)
              as_ori(dest, dest, Imm16::Lower(Imm32(imm.value >> 32)).encode());
        } else {
            as_ori(dest, zero, Imm16::Lower(Imm32(imm.value >> 32)).encode());
        }
        as_dsll(dest, dest, 16);
        if ((imm.value >> 16) & 0xffff)
          as_ori(dest, dest, Imm16::Upper(Imm32(imm.value)).encode());
        as_dsll(dest, dest, 16);
        if (imm.value & 0xffff)
          as_ori(dest, dest, Imm16::Lower(Imm32(imm.value)).encode());
    }
}

// This method generates lui, dsll and ori instruction block that can be modified
// by UpdateLoad64Value, either during compilation (eg. Assembler::bind), or
// during execution (eg. jit::PatchJump).
void
MacroAssemblerMIPS64::ma_liPatchable(Register dest, ImmPtr imm)
{
    return ma_liPatchable(dest, ImmWord(uintptr_t(imm.value)));
}

void
MacroAssemblerMIPS64::ma_liPatchable(Register dest, ImmWord imm, LiFlags flags)
{
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

// Shifts
void
MacroAssemblerMIPS64::ma_dsll(Register rd, Register rt, Imm32 shift)
{
    if (31 < shift.value)
      as_dsll32(rd, rt, shift.value);
    else
      as_dsll(rd, rt, shift.value);
}

void
MacroAssemblerMIPS64::ma_dsrl(Register rd, Register rt, Imm32 shift)
{
    if (31 < shift.value)
      as_dsrl32(rd, rt, shift.value);
    else
      as_dsrl(rd, rt, shift.value);
}

void
MacroAssemblerMIPS64::ma_dsra(Register rd, Register rt, Imm32 shift)
{
    if (31 < shift.value)
      as_dsra32(rd, rt, shift.value);
    else
      as_dsra(rd, rt, shift.value);
}

void
MacroAssemblerMIPS64::ma_dror(Register rd, Register rt, Imm32 shift)
{
    if (31 < shift.value)
      as_drotr32(rd, rt, shift.value);
    else
      as_drotr(rd, rt, shift.value);
}

void
MacroAssemblerMIPS64::ma_drol(Register rd, Register rt, Imm32 shift)
{
    uint32_t s =  64 - shift.value;

    if (31 < s)
      as_drotr32(rd, rt, s);
    else
      as_drotr(rd, rt, s);
}

void
MacroAssemblerMIPS64::ma_dsll(Register rd, Register rt, Register shift)
{
    as_dsllv(rd, rt, shift);
}

void
MacroAssemblerMIPS64::ma_dsrl(Register rd, Register rt, Register shift)
{
    as_dsrlv(rd, rt, shift);
}

void
MacroAssemblerMIPS64::ma_dsra(Register rd, Register rt, Register shift)
{
    as_dsrav(rd, rt, shift);
}

void
MacroAssemblerMIPS64::ma_dror(Register rd, Register rt, Register shift)
{
    as_drotrv(rd, rt, shift);
}

void
MacroAssemblerMIPS64::ma_drol(Register rd, Register rt, Register shift)
{
    ma_negu(ScratchRegister, shift);
    as_drotrv(rd, rt, ScratchRegister);
}

void
MacroAssemblerMIPS64::ma_dins(Register rt, Register rs, Imm32 pos, Imm32 size)
{
    if (pos.value >= 0 && pos.value < 32) {
        if (size.value >= 2)
          as_dinsm(rt, rs, pos.value, size.value);
        else
          as_dins(rt, rs, pos.value, size.value);
    } else {
        as_dinsu(rt, rs, pos.value, size.value);
    }
}

void
MacroAssemblerMIPS64::ma_dext(Register rt, Register rs, Imm32 pos, Imm32 size)
{
    if (pos.value >= 0 && pos.value < 32) {
        if (size.value > 32)
          as_dextm(rt, rs, pos.value, size.value);
        else
          as_dext(rt, rs, pos.value, size.value);
    } else {
        as_dextu(rt, rs, pos.value, size.value);
    }
}

// Arithmetic-based ops.

// Add.
void
MacroAssemblerMIPS64::ma_daddu(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInSignedRange(imm.value)) {
        as_daddiu(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_daddu(rd, rs, ScratchRegister);
    }
}

void
MacroAssemblerMIPS64::ma_daddu(Register rd, Register rs)
{
    as_daddu(rd, rd, rs);
}

void
MacroAssemblerMIPS64::ma_daddu(Register rd, Imm32 imm)
{
    ma_daddu(rd, rd, imm);
}

void
MacroAssemblerMIPS64::ma_addTestOverflow(Register rd, Register rs, Register rt, Label* overflow)
{
    as_addu(rd, rs, rt);
    as_daddu(ScratchRegister, rs, rt);
    ma_b(rd, ScratchRegister, overflow, Assembler::NotEqual);
}

void
MacroAssemblerMIPS64::ma_addTestOverflow(Register rd, Register rs, Imm32 imm, Label* overflow)
{
    // Check for signed range because of as_daddiu
    if (Imm16::IsInSignedRange(imm.value) && Imm16::IsInUnsignedRange(imm.value)) {
        as_addiu(rd, rs, imm.value);
        as_daddiu(ScratchRegister, rs, imm.value);
        ma_b(rd, ScratchRegister, overflow, Assembler::NotEqual);
    } else {
        ma_li(ScratchRegister, imm);
        ma_addTestOverflow(rd, rs, ScratchRegister, overflow);
    }
}

// Subtract.
void
MacroAssemblerMIPS64::ma_dsubu(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInSignedRange(-imm.value)) {
        as_daddiu(rd, rs, -imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_dsubu(rd, rs, ScratchRegister);
    }
}

void
MacroAssemblerMIPS64::ma_dsubu(Register rd, Imm32 imm)
{
    ma_dsubu(rd, rd, imm);
}

void
MacroAssemblerMIPS64::ma_subTestOverflow(Register rd, Register rs, Register rt, Label* overflow)
{
    as_subu(rd, rs, rt);
    as_dsubu(ScratchRegister, rs, rt);
    ma_b(rd, ScratchRegister, overflow, Assembler::NotEqual);
}

void
MacroAssemblerMIPS64::ma_dmult(Register rs, Imm32 imm)
{
    ma_li(ScratchRegister, imm);
    as_dmult(rs, ScratchRegister);
}

// Memory.

void
MacroAssemblerMIPS64::ma_load(Register dest, Address address,
                              LoadStoreSize size, LoadStoreExtension extension)
{
    int16_t encodedOffset;
    Register base;
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
        if (ZeroExtend == extension)
            as_lbu(dest, base, encodedOffset);
        else
            as_lb(dest, base, encodedOffset);
        break;
      case SizeHalfWord:
        if (ZeroExtend == extension)
            as_lhu(dest, base, encodedOffset);
        else
            as_lh(dest, base, encodedOffset);
        break;
      case SizeWord:
        if (ZeroExtend == extension)
            as_lwu(dest, base, encodedOffset);
        else
            as_lw(dest, base, encodedOffset);
        break;
      case SizeDouble:
        as_ld(dest, base, encodedOffset);
        break;
      default:
        MOZ_CRASH("Invalid argument for ma_load");
    }
}

void
MacroAssemblerMIPS64::ma_store(Register data, Address address, LoadStoreSize size,
                               LoadStoreExtension extension)
{
    int16_t encodedOffset;
    Register base;
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

void
MacroAssemblerMIPS64Compat::computeScaledAddress(const BaseIndex& address, Register dest)
{
    int32_t shift = Imm32::ShiftOf(address.scale).value;
    if (shift) {
        ma_dsll(ScratchRegister, address.index, Imm32(shift));
        as_daddu(dest, address.base, ScratchRegister);
    } else {
        as_daddu(dest, address.base, address.index);
    }
}

// Shortcut for when we know we're transferring 32 bits of data.
void
MacroAssemblerMIPS64::ma_pop(Register r)
{
    as_ld(r, StackPointer, 0);
    as_daddiu(StackPointer, StackPointer, sizeof(intptr_t));
}

void
MacroAssemblerMIPS64::ma_push(Register r)
{
    if (r == sp) {
        // Pushing sp requires one more instruction.
        ma_move(ScratchRegister, sp);
        r = ScratchRegister;
    }

    as_daddiu(StackPointer, StackPointer, (int32_t)-sizeof(intptr_t));
    as_sd(r, StackPointer, 0);
}

// Branches when done from within mips-specific code.
void
MacroAssemblerMIPS64::ma_b(Register lhs, ImmWord imm, Label* label, Condition c, JumpKind jumpKind)
{
    MOZ_ASSERT(c != Overflow);
    if (imm.value == 0) {
        if (c == Always || c == AboveOrEqual)
            ma_b(label, jumpKind);
        else if (c == Below)
            ; // This condition is always false. No branch required.
        else
            branchWithCode(getBranchCode(lhs, c), label, jumpKind);
    } else {
        MOZ_ASSERT(lhs != ScratchRegister);
        ma_li(ScratchRegister, imm);
        ma_b(lhs, ScratchRegister, label, c, jumpKind);
    }
}

void
MacroAssemblerMIPS64::ma_b(Register lhs, Address addr, Label* label, Condition c, JumpKind jumpKind)
{
    MOZ_ASSERT(lhs != ScratchRegister);
    ma_load(ScratchRegister, addr, SizeDouble);
    ma_b(lhs, ScratchRegister, label, c, jumpKind);
}

void
MacroAssemblerMIPS64::ma_b(Address addr, Imm32 imm, Label* label, Condition c, JumpKind jumpKind)
{
    ma_load(SecondScratchReg, addr, SizeDouble);
    ma_b(SecondScratchReg, imm, label, c, jumpKind);
}

void
MacroAssemblerMIPS64::ma_b(Address addr, ImmGCPtr imm, Label* label, Condition c, JumpKind jumpKind)
{
    ma_load(SecondScratchReg, addr, SizeDouble);
    ma_b(SecondScratchReg, imm, label, c, jumpKind);
}

void
MacroAssemblerMIPS64::ma_bal(Label* label, DelaySlotFill delaySlotFill)
{
    if (label->bound()) {
        // Generate the long jump for calls because return address has to be
        // the address after the reserved block.
        addLongJump(nextOffset());
        ma_liPatchable(ScratchRegister, ImmWord(label->offset()));
        as_jalr(ScratchRegister);
        if (delaySlotFill == FillDelaySlot)
            as_nop();
        return;
    }

    // Second word holds a pointer to the next branch in label's chain.
    uint32_t nextInChain = label->used() ? label->offset() : LabelBase::INVALID_OFFSET;

    // Make the whole branch continous in the buffer. The '6'
    // instructions are writing at below (contain delay slot).
    m_buffer.ensureSpace(6 * sizeof(uint32_t));

    BufferOffset bo = writeInst(getBranchCode(BranchIsCall).encode());
    writeInst(nextInChain);
    if (!oom())
        label->use(bo.getOffset());
    // Leave space for long jump.
    as_nop();
    as_nop();
    as_nop();
    if (delaySlotFill == FillDelaySlot)
        as_nop();
}

void
MacroAssemblerMIPS64::branchWithCode(InstImm code, Label* label, JumpKind jumpKind)
{
    MOZ_ASSERT(code.encode() != InstImm(op_regimm, zero, rt_bgezal, BOffImm16(0)).encode());
    InstImm inst_beq = InstImm(op_beq, zero, zero, BOffImm16(0));

    if (label->bound()) {
        int32_t offset = label->offset() - m_buffer.nextOffset().getOffset();

        if (BOffImm16::IsInRange(offset))
            jumpKind = ShortJump;

        if (jumpKind == ShortJump) {
            MOZ_ASSERT(BOffImm16::IsInRange(offset));
            code.setBOffImm16(BOffImm16(offset));
            writeInst(code.encode());
            as_nop();
            return;
        }

        if (code.encode() == inst_beq.encode()) {
            // Handle long jump
            addLongJump(nextOffset());
            ma_liPatchable(ScratchRegister, ImmWord(label->offset()));
            as_jr(ScratchRegister);
            as_nop();
            return;
        }

        // Handle long conditional branch, the target offset is based on self,
        // point to next instruction of nop at below.
        writeInst(invertBranch(code, BOffImm16(7 * sizeof(uint32_t))).encode());
        // No need for a "nop" here because we can clobber scratch.
        addLongJump(nextOffset());
        ma_liPatchable(ScratchRegister, ImmWord(label->offset()));
        as_jr(ScratchRegister);
        as_nop();
        return;
    }

    // Generate open jump and link it to a label.

    // Second word holds a pointer to the next branch in label's chain.
    uint32_t nextInChain = label->used() ? label->offset() : LabelBase::INVALID_OFFSET;

    if (jumpKind == ShortJump) {
        // Make the whole branch continous in the buffer.
        m_buffer.ensureSpace(2 * sizeof(uint32_t));

        // Indicate that this is short jump with offset 4.
        code.setBOffImm16(BOffImm16(4));
        BufferOffset bo = writeInst(code.encode());
        writeInst(nextInChain);
        if (!oom())
            label->use(bo.getOffset());
        return;
    }

    bool conditional = code.encode() != inst_beq.encode();

    // Make the whole branch continous in the buffer. The '7'
    // instructions are writing at below (contain conditional nop).
    m_buffer.ensureSpace(7 * sizeof(uint32_t));

    BufferOffset bo = writeInst(code.encode());
    writeInst(nextInChain);
    if (!oom())
        label->use(bo.getOffset());
    // Leave space for potential long jump.
    as_nop();
    as_nop();
    as_nop();
    as_nop();
    if (conditional)
        as_nop();
}

void
MacroAssemblerMIPS64::ma_cmp_set(Register rd, Register rs, ImmWord imm, Condition c)
{
    ma_li(ScratchRegister, imm);
    ma_cmp_set(rd, rs, ScratchRegister, c);
}

void
MacroAssemblerMIPS64::ma_cmp_set(Register rd, Register rs, ImmPtr imm, Condition c)
{
    ma_li(ScratchRegister, ImmWord(uintptr_t(imm.value)));
    ma_cmp_set(rd, rs, ScratchRegister, c);
}

// fp instructions
void
MacroAssemblerMIPS64::ma_lid(FloatRegister dest, double value)
{
    ImmWord imm(mozilla::BitwiseCast<uint64_t>(value));

    ma_li(ScratchRegister, imm);
    moveToDouble(ScratchRegister, dest);
}

void
MacroAssemblerMIPS64::ma_mv(FloatRegister src, ValueOperand dest)
{
    as_dmfc1(dest.valueReg(), src);
}

void
MacroAssemblerMIPS64::ma_mv(ValueOperand src, FloatRegister dest)
{
    as_dmtc1(src.valueReg(), dest);
}

void
MacroAssemblerMIPS64::ma_ls(FloatRegister ft, Address address)
{
    if (Imm16::IsInSignedRange(address.offset)) {
        as_ls(ft, address.base, address.offset);
    } else {
        MOZ_ASSERT(address.base != ScratchRegister);
        ma_li(ScratchRegister, Imm32(address.offset));
        as_daddu(ScratchRegister, address.base, ScratchRegister);
        as_ls(ft, ScratchRegister, 0);
    }
}

void
MacroAssemblerMIPS64::ma_ld(FloatRegister ft, Address address)
{
    if (Imm16::IsInSignedRange(address.offset)) {
        as_ld(ft, address.base, address.offset);
    } else {
        ma_li(ScratchRegister, Imm32(address.offset));
        as_daddu(ScratchRegister, address.base, ScratchRegister);
        as_ld(ft, ScratchRegister, 0);
    }
}

void
MacroAssemblerMIPS64::ma_sd(FloatRegister ft, Address address)
{
    if (Imm16::IsInSignedRange(address.offset)) {
        as_sd(ft, address.base, address.offset);
    } else {
        ma_li(ScratchRegister, Imm32(address.offset));
        as_daddu(ScratchRegister, address.base, ScratchRegister);
        as_sd(ft, ScratchRegister, 0);
    }
}

void
MacroAssemblerMIPS64::ma_ss(FloatRegister ft, Address address)
{
    if (Imm16::IsInSignedRange(address.offset)) {
        as_ss(ft, address.base, address.offset);
    } else {
        ma_li(ScratchRegister, Imm32(address.offset));
        as_daddu(ScratchRegister, address.base, ScratchRegister);
        as_ss(ft, ScratchRegister, 0);
    }
}

void
MacroAssemblerMIPS64::ma_pop(FloatRegister fs)
{
    ma_ld(fs, Address(StackPointer, 0));
    as_daddiu(StackPointer, StackPointer, sizeof(double));
}

void
MacroAssemblerMIPS64::ma_push(FloatRegister fs)
{
    as_daddiu(StackPointer, StackPointer, (int32_t)-sizeof(double));
    ma_sd(fs, Address(StackPointer, 0));
}

bool
MacroAssemblerMIPS64Compat::buildOOLFakeExitFrame(void* fakeReturnAddr)
{
    uint32_t descriptor = MakeFrameDescriptor(asMasm().framePushed(), JitFrame_IonJS);

    asMasm().Push(Imm32(descriptor)); // descriptor_
    asMasm().Push(ImmPtr(fakeReturnAddr));

    return true;
}

void
MacroAssemblerMIPS64Compat::add32(Register src, Register dest)
{
    as_addu(dest, dest, src);
}

void
MacroAssemblerMIPS64Compat::add32(Imm32 imm, Register dest)
{
    ma_addu(dest, dest, imm);
}

void

MacroAssemblerMIPS64Compat::add32(Imm32 imm, const Address& dest)
{
    load32(dest, SecondScratchReg);
    ma_addu(SecondScratchReg, imm);
    store32(SecondScratchReg, dest);
}

void
MacroAssemblerMIPS64Compat::addPtr(Register src, Register dest)
{
    ma_daddu(dest, src);
}

void
MacroAssemblerMIPS64Compat::addPtr(const Address& src, Register dest)
{
    loadPtr(src, ScratchRegister);
    ma_daddu(dest, ScratchRegister);
}

void
MacroAssemblerMIPS64Compat::subPtr(Register src, Register dest)
{
    as_dsubu(dest, dest, src);
}

void
MacroAssemblerMIPS64Compat::move32(Imm32 imm, Register dest)
{
    ma_li(dest, imm);
}

void
MacroAssemblerMIPS64Compat::move32(Register src, Register dest)
{
    ma_move(dest, src);
}

void
MacroAssemblerMIPS64Compat::movePtr(Register src, Register dest)
{
    ma_move(dest, src);
}
void
MacroAssemblerMIPS64Compat::movePtr(ImmWord imm, Register dest)
{
    ma_li(dest, imm);
}

void
MacroAssemblerMIPS64Compat::movePtr(ImmGCPtr imm, Register dest)
{
    ma_li(dest, imm);
}

void
MacroAssemblerMIPS64Compat::movePtr(ImmPtr imm, Register dest)
{
    movePtr(ImmWord(uintptr_t(imm.value)), dest);
}
void
MacroAssemblerMIPS64Compat::movePtr(wasm::SymbolicAddress imm, Register dest)
{
    append(AsmJSAbsoluteLink(CodeOffset(nextOffset().getOffset()), imm));
    ma_liPatchable(dest, ImmWord(-1));
}

void
MacroAssemblerMIPS64Compat::load8ZeroExtend(const Address& address, Register dest)
{
    ma_load(dest, address, SizeByte, ZeroExtend);
}

void
MacroAssemblerMIPS64Compat::load8ZeroExtend(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeByte, ZeroExtend);
}

void
MacroAssemblerMIPS64Compat::load8SignExtend(const Address& address, Register dest)
{
    ma_load(dest, address, SizeByte, SignExtend);
}

void
MacroAssemblerMIPS64Compat::load8SignExtend(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeByte, SignExtend);
}

void
MacroAssemblerMIPS64Compat::load16ZeroExtend(const Address& address, Register dest)
{
    ma_load(dest, address, SizeHalfWord, ZeroExtend);
}

void
MacroAssemblerMIPS64Compat::load16ZeroExtend(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeHalfWord, ZeroExtend);
}

void
MacroAssemblerMIPS64Compat::load16SignExtend(const Address& address, Register dest)
{
    ma_load(dest, address, SizeHalfWord, SignExtend);
}

void
MacroAssemblerMIPS64Compat::load16SignExtend(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeHalfWord, SignExtend);
}

void
MacroAssemblerMIPS64Compat::load32(const Address& address, Register dest)
{
    ma_load(dest, address, SizeWord);
}

void
MacroAssemblerMIPS64Compat::load32(const BaseIndex& address, Register dest)
{
    ma_load(dest, address, SizeWord);
}

void
MacroAssemblerMIPS64Compat::load32(AbsoluteAddress address, Register dest)
{
    movePtr(ImmPtr(address.addr), ScratchRegister);
    load32(Address(ScratchRegister, 0), dest);
}

void
MacroAssemblerMIPS64Compat::load32(wasm::SymbolicAddress address, Register dest)
{
    movePtr(address, ScratchRegister);
    load32(Address(ScratchRegister, 0), dest);
}

void
MacroAssemblerMIPS64Compat::loadPtr(const Address& address, Register dest)
{
    ma_load(dest, address, SizeDouble);
}

void
MacroAssemblerMIPS64Compat::loadPtr(const BaseIndex& src, Register dest)
{
    ma_load(dest, src, SizeDouble);
}

void
MacroAssemblerMIPS64Compat::loadPtr(AbsoluteAddress address, Register dest)
{
    movePtr(ImmPtr(address.addr), ScratchRegister);
    loadPtr(Address(ScratchRegister, 0), dest);
}

void
MacroAssemblerMIPS64Compat::loadPtr(wasm::SymbolicAddress address, Register dest)
{
    movePtr(address, ScratchRegister);
    loadPtr(Address(ScratchRegister, 0), dest);
}

void
MacroAssemblerMIPS64Compat::loadPrivate(const Address& address, Register dest)
{
    loadPtr(address, dest);
    ma_dsll(dest, dest, Imm32(1));
}

void
MacroAssemblerMIPS64Compat::loadDouble(const Address& address, FloatRegister dest)
{
    ma_ld(dest, address);
}

void
MacroAssemblerMIPS64Compat::loadDouble(const BaseIndex& src, FloatRegister dest)
{
    computeScaledAddress(src, SecondScratchReg);
    ma_ld(dest, Address(SecondScratchReg, src.offset));
}

void
MacroAssemblerMIPS64Compat::loadFloatAsDouble(const Address& address, FloatRegister dest)
{
    ma_ls(dest, address);
    as_cvtds(dest, dest);
}

void
MacroAssemblerMIPS64Compat::loadFloatAsDouble(const BaseIndex& src, FloatRegister dest)
{
    loadFloat32(src, dest);
    as_cvtds(dest, dest);
}

void
MacroAssemblerMIPS64Compat::loadFloat32(const Address& address, FloatRegister dest)
{
    ma_ls(dest, address);
}

void
MacroAssemblerMIPS64Compat::loadFloat32(const BaseIndex& src, FloatRegister dest)
{
    computeScaledAddress(src, SecondScratchReg);
    ma_ls(dest, Address(SecondScratchReg, src.offset));
}

void
MacroAssemblerMIPS64Compat::store8(Imm32 imm, const Address& address)
{
    ma_li(SecondScratchReg, imm);
    ma_store(SecondScratchReg, address, SizeByte);
}

void
MacroAssemblerMIPS64Compat::store8(Register src, const Address& address)
{
    ma_store(src, address, SizeByte);
}

void
MacroAssemblerMIPS64Compat::store8(Imm32 imm, const BaseIndex& dest)
{
    ma_store(imm, dest, SizeByte);
}

void
MacroAssemblerMIPS64Compat::store8(Register src, const BaseIndex& dest)
{
    ma_store(src, dest, SizeByte);
}

void
MacroAssemblerMIPS64Compat::store16(Imm32 imm, const Address& address)
{
    ma_li(SecondScratchReg, imm);
    ma_store(SecondScratchReg, address, SizeHalfWord);
}

void
MacroAssemblerMIPS64Compat::store16(Register src, const Address& address)
{
    ma_store(src, address, SizeHalfWord);
}

void
MacroAssemblerMIPS64Compat::store16(Imm32 imm, const BaseIndex& dest)
{
    ma_store(imm, dest, SizeHalfWord);
}

void
MacroAssemblerMIPS64Compat::store16(Register src, const BaseIndex& address)
{
    ma_store(src, address, SizeHalfWord);
}

void
MacroAssemblerMIPS64Compat::store32(Register src, AbsoluteAddress address)
{
    movePtr(ImmPtr(address.addr), ScratchRegister);
    store32(src, Address(ScratchRegister, 0));
}

void
MacroAssemblerMIPS64Compat::store32(Register src, const Address& address)
{
    ma_store(src, address, SizeWord);
}

void
MacroAssemblerMIPS64Compat::store32(Imm32 src, const Address& address)
{
    move32(src, SecondScratchReg);
    ma_store(SecondScratchReg, address, SizeWord);
}

void
MacroAssemblerMIPS64Compat::store32(Imm32 imm, const BaseIndex& dest)
{
    ma_store(imm, dest, SizeWord);
}

void
MacroAssemblerMIPS64Compat::store32(Register src, const BaseIndex& dest)
{
    ma_store(src, dest, SizeWord);
}

template <typename T>
void
MacroAssemblerMIPS64Compat::storePtr(ImmWord imm, T address)
{
    ma_li(SecondScratchReg, imm);
    ma_store(SecondScratchReg, address, SizeDouble);
}

template void MacroAssemblerMIPS64Compat::storePtr<Address>(ImmWord imm, Address address);
template void MacroAssemblerMIPS64Compat::storePtr<BaseIndex>(ImmWord imm, BaseIndex address);

template <typename T>
void
MacroAssemblerMIPS64Compat::storePtr(ImmPtr imm, T address)
{
    storePtr(ImmWord(uintptr_t(imm.value)), address);
}

template void MacroAssemblerMIPS64Compat::storePtr<Address>(ImmPtr imm, Address address);
template void MacroAssemblerMIPS64Compat::storePtr<BaseIndex>(ImmPtr imm, BaseIndex address);

template <typename T>
void
MacroAssemblerMIPS64Compat::storePtr(ImmGCPtr imm, T address)
{
    storePtr(ImmWord(uintptr_t(imm.value)), address);
}

template void MacroAssemblerMIPS64Compat::storePtr<Address>(ImmGCPtr imm, Address address);
template void MacroAssemblerMIPS64Compat::storePtr<BaseIndex>(ImmGCPtr imm, BaseIndex address);

void
MacroAssemblerMIPS64Compat::storePtr(Register src, const Address& address)
{
    ma_store(src, address, SizeDouble);
}

void
MacroAssemblerMIPS64Compat::storePtr(Register src, const BaseIndex& address)
{
    ma_store(src, address, SizeDouble);
}

void
MacroAssemblerMIPS64Compat::storePtr(Register src, AbsoluteAddress dest)
{
    movePtr(ImmPtr(dest.addr), ScratchRegister);
    storePtr(src, Address(ScratchRegister, 0));
}

void
MacroAssemblerMIPS64Compat::clampIntToUint8(Register reg)
{
    // look at (reg >> 8) if it is 0, then src shouldn't be clamped
    // if it is <0, then we want to clamp to 0,
    // otherwise, we wish to clamp to 255
    Label done;
    ma_move(ScratchRegister, reg);
    asMasm().rshiftPtrArithmetic(Imm32(8), ScratchRegister);
    ma_b(ScratchRegister, ScratchRegister, &done, Assembler::Zero, ShortJump);
    {
        Label negative;
        ma_b(ScratchRegister, ScratchRegister, &negative, Assembler::Signed, ShortJump);
        {
            ma_li(reg, Imm32(255));
            ma_b(&done, ShortJump);
        }
        bind(&negative);
        {
            ma_move(reg, zero);
        }
    }
    bind(&done);
}

// Note: this function clobbers the input register.
void
MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output)
{
    MOZ_ASSERT(input != ScratchDoubleReg);
    Label positive, done;

    // <= 0 or NaN --> 0
    zeroDouble(ScratchDoubleReg);
    branchDouble(DoubleGreaterThan, input, ScratchDoubleReg, &positive);
    {
        move32(Imm32(0), output);
        jump(&done);
    }

    bind(&positive);

    // Add 0.5 and truncate.
    loadConstantDouble(0.5, ScratchDoubleReg);
    addDouble(ScratchDoubleReg, input);

    Label outOfRange;

    branchTruncateDouble(input, output, &outOfRange);
    branch32(Assembler::Above, output, Imm32(255), &outOfRange);
    {
        // Check if we had a tie.
        convertInt32ToDouble(output, ScratchDoubleReg);
        branchDouble(DoubleNotEqual, input, ScratchDoubleReg, &done);

        // It was a tie. Mask out the ones bit to get an even value.
        // See also js_TypedArray_uint8_clamp_double.
        and32(Imm32(~1), output);
        jump(&done);
    }

    // > 255 --> 255
    bind(&outOfRange);
    {
        move32(Imm32(255), output);
    }

    bind(&done);
}

void
MacroAssemblerMIPS64Compat::subPtr(Imm32 imm, const Register dest)
{
    ma_dsubu(dest, dest, imm);
}

void
MacroAssemblerMIPS64Compat::subPtr(const Address& addr, const Register dest)
{
    loadPtr(addr, SecondScratchReg);
    subPtr(SecondScratchReg, dest);
}

void
MacroAssemblerMIPS64Compat::subPtr(Register src, const Address& dest)
{
    loadPtr(dest, SecondScratchReg);
    subPtr(src, SecondScratchReg);
    storePtr(SecondScratchReg, dest);
}

void
MacroAssemblerMIPS64Compat::addPtr(Imm32 imm, const Register dest)
{
    ma_daddu(dest, imm);
}

void
MacroAssemblerMIPS64Compat::addPtr(Imm32 imm, const Address& dest)
{
    loadPtr(dest, ScratchRegister);
    addPtr(imm, ScratchRegister);
    storePtr(ScratchRegister, dest);
}

void
MacroAssemblerMIPS64Compat::branchDouble(DoubleCondition cond, FloatRegister lhs,
                                         FloatRegister rhs, Label* label)
{
    ma_bc1d(lhs, rhs, label, cond);
}

void
MacroAssemblerMIPS64Compat::branchFloat(DoubleCondition cond, FloatRegister lhs,
                                        FloatRegister rhs, Label* label)
{
    ma_bc1s(lhs, rhs, label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestGCThing(Condition cond, const Address& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET), label,
         (cond == Equal) ? AboveOrEqual : Below);
}
void
MacroAssemblerMIPS64Compat::branchTestGCThing(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET), label,
         (cond == Equal) ? AboveOrEqual : Below);
}

void
MacroAssemblerMIPS64Compat::branchTestPrimitive(Condition cond, const ValueOperand& value,
                                                Label* label)
{
    splitTag(value, SecondScratchReg);
    branchTestPrimitive(cond, SecondScratchReg, label);
}
void
MacroAssemblerMIPS64Compat::branchTestPrimitive(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET), label,
         (cond == Equal) ? Below : AboveOrEqual);
}

void
MacroAssemblerMIPS64Compat::branchTestInt32(Condition cond, const ValueOperand& value, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    splitTag(value, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_INT32), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestInt32(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_INT32), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestInt32(Condition cond, const Address& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_INT32), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestInt32(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_INT32), label, cond);
}

void
MacroAssemblerMIPS64Compat:: branchTestBoolean(Condition cond, const ValueOperand& value,
                                               Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    splitTag(value, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_BOOLEAN), label, cond);
}

void
MacroAssemblerMIPS64Compat:: branchTestBoolean(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_BOOLEAN), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestBoolean(Condition cond, const Address& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_BOOLEAN), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestBoolean(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_BOOLEAN), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestDouble(Condition cond, const ValueOperand& value, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == NotEqual);
    splitTag(value, SecondScratchReg);
    branchTestDouble(cond, SecondScratchReg, label);
}

void
MacroAssemblerMIPS64Compat::branchTestDouble(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == NotEqual);
    Condition actual = (cond == Equal) ? BelowOrEqual : Above;
    ma_b(tag, ImmTag(JSVAL_TAG_MAX_DOUBLE), label, actual);
}

void
MacroAssemblerMIPS64Compat::branchTestDouble(Condition cond, const Address& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, SecondScratchReg);
    branchTestDouble(cond, SecondScratchReg, label);
}

void
MacroAssemblerMIPS64Compat::branchTestDouble(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    branchTestDouble(cond, SecondScratchReg, label);
}

void
MacroAssemblerMIPS64Compat::branchTestNull(Condition cond, const ValueOperand& value, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    splitTag(value, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_NULL), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestNull(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_NULL), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestNull(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_NULL), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestNull(Condition cond, const Address& address, Label* label) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_NULL), label, cond);
}

void
MacroAssemblerMIPS64Compat::testNullSet(Condition cond, const ValueOperand& value, Register dest)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    splitTag(value, SecondScratchReg);
    ma_cmp_set(dest, SecondScratchReg, ImmTag(JSVAL_TAG_NULL), cond);
}

void
MacroAssemblerMIPS64Compat::branchTestObject(Condition cond, const ValueOperand& value, Label* label)
{
    splitTag(value, SecondScratchReg);
    branchTestObject(cond, SecondScratchReg, label);
}

void
MacroAssemblerMIPS64Compat::branchTestObject(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_OBJECT), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestObject(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_OBJECT), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestObject(Condition cond, const Address& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_OBJECT), label, cond);
}

void
MacroAssemblerMIPS64Compat::testObjectSet(Condition cond, const ValueOperand& value, Register dest)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    splitTag(value, SecondScratchReg);
    ma_cmp_set(dest, SecondScratchReg, ImmTag(JSVAL_TAG_OBJECT), cond);
}

void
MacroAssemblerMIPS64Compat::branchTestString(Condition cond, const ValueOperand& value, Label* label)
{
    splitTag(value, SecondScratchReg);
    branchTestString(cond, SecondScratchReg, label);
}

void
MacroAssemblerMIPS64Compat::branchTestString(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_STRING), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestString(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_STRING), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestSymbol(Condition cond, const ValueOperand& value, Label* label)
{
    splitTag(value, SecondScratchReg);
    branchTestSymbol(cond, SecondScratchReg, label);
}

void
MacroAssemblerMIPS64Compat::branchTestSymbol(Condition cond, const Register& tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_SYMBOL), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestSymbol(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_SYMBOL), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestUndefined(Condition cond, const ValueOperand& value,
                                                Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    splitTag(value, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_UNDEFINED), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestUndefined(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_UNDEFINED), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestUndefined(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_UNDEFINED), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestUndefined(Condition cond, const Address& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_UNDEFINED), label, cond);
}

void
MacroAssemblerMIPS64Compat::testUndefinedSet(Condition cond, const ValueOperand& value, Register dest)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    splitTag(value, SecondScratchReg);
    ma_cmp_set(dest, SecondScratchReg, ImmTag(JSVAL_TAG_UNDEFINED), cond);
}

void
MacroAssemblerMIPS64Compat::branchTestNumber(Condition cond, const ValueOperand& value, Label* label)
{
    splitTag(value, SecondScratchReg);
    branchTestNumber(cond, SecondScratchReg, label);
}

void
MacroAssemblerMIPS64Compat::branchTestNumber(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET), label,
         cond == Equal ? BelowOrEqual : Above);
}

void
MacroAssemblerMIPS64Compat::branchTestMagic(Condition cond, const ValueOperand& value, Label* label)
{
    splitTag(value, SecondScratchReg);
    branchTestMagic(cond, SecondScratchReg, label);
}

void
MacroAssemblerMIPS64Compat::branchTestMagic(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_MAGIC), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestMagic(Condition cond, const Address& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_MAGIC), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestMagic(Condition cond, const BaseIndex& src, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(src, SecondScratchReg);
    ma_b(SecondScratchReg, ImmTag(JSVAL_TAG_MAGIC), label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestValue(Condition cond, const ValueOperand& value,
                                            const Value& v, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    moveValue(v, ScratchRegister);
    ma_b(value.valueReg(), ScratchRegister, label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestValue(Condition cond, const Address& valaddr,
                                            const ValueOperand& value, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    loadPtr(Address(valaddr.base, valaddr.offset), ScratchRegister);
    ma_b(value.valueReg(), ScratchRegister, label, cond);
}

// unboxing code
void
MacroAssemblerMIPS64Compat::unboxNonDouble(const ValueOperand& operand, Register dest)
{
    ma_dext(dest, operand.valueReg(), Imm32(0), Imm32(JSVAL_TAG_SHIFT));
}

void
MacroAssemblerMIPS64Compat::unboxNonDouble(const Address& src, Register dest)
{
    loadPtr(Address(src.base, src.offset), dest);
    ma_dext(dest, dest, Imm32(0), Imm32(JSVAL_TAG_SHIFT));
}

void
MacroAssemblerMIPS64Compat::unboxNonDouble(const BaseIndex& src, Register dest)
{
    computeScaledAddress(src, SecondScratchReg);
    loadPtr(Address(SecondScratchReg, src.offset), dest);
    ma_dext(dest, dest, Imm32(0), Imm32(JSVAL_TAG_SHIFT));
}

void
MacroAssemblerMIPS64Compat::unboxInt32(const ValueOperand& operand, Register dest)
{
    ma_dsll(dest, operand.valueReg(), Imm32(32));
    ma_dsra(dest, dest, Imm32(32));
}

void
MacroAssemblerMIPS64Compat::unboxInt32(const Operand& operand, Register dest)
{
    switch(operand.getTag()) {
    case Operand::REG:
        ma_dsll(dest, operand.toReg(), Imm32(32));
        ma_dsra(dest, dest, Imm32(32));
        break;
    case Operand::MEM:
        unboxInt32(operand.toAddress(), dest);
        break;
    case Operand::FREG:
    default:
        MOZ_CRASH("unexpected operand kind");
        break;
    }
}

void
MacroAssemblerMIPS64Compat::unboxInt32(const Address& src, Register dest)
{
    load32(Address(src.base, src.offset), dest);
}

void
MacroAssemblerMIPS64Compat::unboxInt32(const BaseIndex& src, Register dest)
{
    computeScaledAddress(src, SecondScratchReg);
    load32(Address(SecondScratchReg, src.offset), dest);
}

void
MacroAssemblerMIPS64Compat::unboxBoolean(const ValueOperand& operand, Register dest)
{
    ma_dext(dest, operand.valueReg(), Imm32(0), Imm32(32));
}

void
MacroAssemblerMIPS64Compat::unboxBoolean(const Operand& operand, Register dest)
{
    switch(operand.getTag()) {
    case Operand::REG:
        ma_dext(dest, operand.toReg(), Imm32(0), Imm32(32));
        break;
    case Operand::MEM:
        unboxBoolean(operand.toAddress(), dest);
        break;
    case Operand::FREG:
    default:
        MOZ_CRASH("unexpected operand kind");
        break;
    }
}

void
MacroAssemblerMIPS64Compat::unboxBoolean(const Address& src, Register dest)
{
    ma_load(dest, Address(src.base, src.offset), SizeWord, ZeroExtend);
}

void
MacroAssemblerMIPS64Compat::unboxBoolean(const BaseIndex& src, Register dest)
{
    computeScaledAddress(src, SecondScratchReg);
    ma_load(dest, Address(SecondScratchReg, src.offset), SizeWord, ZeroExtend);
}

void
MacroAssemblerMIPS64Compat::unboxDouble(const ValueOperand& operand, FloatRegister dest)
{
    as_dmtc1(operand.valueReg(), dest);
}

void
MacroAssemblerMIPS64Compat::unboxDouble(const Address& src, FloatRegister dest)
{
    ma_ld(dest, Address(src.base, src.offset));
}

void
MacroAssemblerMIPS64Compat::unboxString(const ValueOperand& operand, Register dest)
{
    unboxNonDouble(operand, dest);
}

void
MacroAssemblerMIPS64Compat::unboxString(const Operand& operand, Register dest)
{
    switch(operand.getTag()) {
    case Operand::REG:
        ma_dext(dest, operand.toReg(), Imm32(0), Imm32(JSVAL_TAG_SHIFT));
        break;
    case Operand::MEM:
        unboxNonDouble(operand.toAddress(), dest);
        break;
    case Operand::FREG:
    default:
        MOZ_CRASH("unexpected operand kind");
        break;
    }
}

void
MacroAssemblerMIPS64Compat::unboxString(const Address& src, Register dest)
{
    unboxNonDouble(src, dest);
}

void
MacroAssemblerMIPS64Compat::unboxSymbol(const Operand& operand, Register dest)
{
    switch(operand.getTag()) {
    case Operand::REG:
        ma_dext(dest, operand.toReg(), Imm32(0), Imm32(JSVAL_TAG_SHIFT));
        break;
    case Operand::MEM:
        unboxNonDouble(operand.toAddress(), dest);
        break;
    case Operand::FREG:
    default:
        MOZ_CRASH("unexpected operand kind");
        break;
    }
}

void
MacroAssemblerMIPS64Compat::unboxSymbol(const Address& src, Register dest)
{
    unboxNonDouble(src, dest);
}

void
MacroAssemblerMIPS64Compat::unboxObject(const ValueOperand& src, Register dest)
{
    unboxNonDouble(src, dest);
}

void
MacroAssemblerMIPS64Compat::unboxObject(const Operand& src, Register dest)
{
    switch(src.getTag()) {
    case Operand::REG:
        ma_dext(dest, src.toReg(), Imm32(0), Imm32(JSVAL_TAG_SHIFT));
        break;
    case Operand::MEM:
        unboxNonDouble(src.toAddress(), dest);
        break;
    case Operand::FREG:
    default:
        MOZ_CRASH("unexpected operand kind");
        break;
    }
}

void
MacroAssemblerMIPS64Compat::unboxObject(const Address& src, Register dest)
{
    unboxNonDouble(src, dest);
}

void
MacroAssemblerMIPS64Compat::unboxValue(const ValueOperand& src, AnyRegister dest)
{
    if (dest.isFloat()) {
        Label notInt32, end;
        branchTestInt32(Assembler::NotEqual, src, &notInt32);
        convertInt32ToDouble(src.valueReg(), dest.fpu());
        ma_b(&end, ShortJump);
        bind(&notInt32);
        unboxDouble(src, dest.fpu());
        bind(&end);
    } else {
        unboxNonDouble(src, dest.gpr());
    }
}

void
MacroAssemblerMIPS64Compat::unboxPrivate(const ValueOperand& src, Register dest)
{
    ma_dsrl(dest, src.valueReg(), Imm32(1));
}

void
MacroAssemblerMIPS64Compat::boxDouble(FloatRegister src, const ValueOperand& dest)
{
    as_dmfc1(dest.valueReg(), src);
}

void
MacroAssemblerMIPS64Compat::boxNonDouble(JSValueType type, Register src,
                                         const ValueOperand& dest)
{
    MOZ_ASSERT(src != dest.valueReg());
    boxValue(type, src, dest.valueReg());
}

void
MacroAssemblerMIPS64Compat::boolValueToDouble(const ValueOperand& operand, FloatRegister dest)
{
    convertBoolToInt32(operand.valueReg(), ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
}

void
MacroAssemblerMIPS64Compat::int32ValueToDouble(const ValueOperand& operand,
                                               FloatRegister dest)
{
    convertInt32ToDouble(operand.valueReg(), dest);
}

void
MacroAssemblerMIPS64Compat::boolValueToFloat32(const ValueOperand& operand,
                                               FloatRegister dest)
{

    convertBoolToInt32(operand.valueReg(), ScratchRegister);
    convertInt32ToFloat32(ScratchRegister, dest);
}

void
MacroAssemblerMIPS64Compat::int32ValueToFloat32(const ValueOperand& operand,
                                                FloatRegister dest)
{
    convertInt32ToFloat32(operand.valueReg(), dest);
}

void
MacroAssemblerMIPS64Compat::loadConstantFloat32(float f, FloatRegister dest)
{
    ma_lis(dest, f);
}

void
MacroAssemblerMIPS64Compat::loadInt32OrDouble(const Address& src, FloatRegister dest)
{
    Label notInt32, end;
    // If it's an int, convert it to double.
    loadPtr(Address(src.base, src.offset), ScratchRegister);
    ma_dsrl(SecondScratchReg, ScratchRegister, Imm32(JSVAL_TAG_SHIFT));
    branchTestInt32(Assembler::NotEqual, SecondScratchReg, &notInt32);
    loadPtr(Address(src.base, src.offset), SecondScratchReg);
    convertInt32ToDouble(SecondScratchReg, dest);
    ma_b(&end, ShortJump);

    // Not an int, just load as double.
    bind(&notInt32);
    ma_ld(dest, src);
    bind(&end);
}

void
MacroAssemblerMIPS64Compat::loadInt32OrDouble(const BaseIndex& addr, FloatRegister dest)
{
    Label notInt32, end;

    // If it's an int, convert it to double.
    computeScaledAddress(addr, SecondScratchReg);
    // Since we only have one scratch, we need to stomp over it with the tag.
    loadPtr(Address(SecondScratchReg, 0), ScratchRegister);
    ma_dsrl(SecondScratchReg, ScratchRegister, Imm32(JSVAL_TAG_SHIFT));
    branchTestInt32(Assembler::NotEqual, SecondScratchReg, &notInt32);

    computeScaledAddress(addr, SecondScratchReg);
    loadPtr(Address(SecondScratchReg, 0), SecondScratchReg);
    convertInt32ToDouble(SecondScratchReg, dest);
    ma_b(&end, ShortJump);

    // Not an int, just load as double.
    bind(&notInt32);
    // First, recompute the offset that had been stored in the scratch register
    // since the scratch register was overwritten loading in the type.
    computeScaledAddress(addr, SecondScratchReg);
    loadDouble(Address(SecondScratchReg, 0), dest);
    bind(&end);
}

void
MacroAssemblerMIPS64Compat::loadConstantDouble(double dp, FloatRegister dest)
{
    ma_lid(dest, dp);
}

void
MacroAssemblerMIPS64Compat::branchTestInt32Truthy(bool b, const ValueOperand& value, Label* label)
{
    ma_dext(ScratchRegister, value.valueReg(), Imm32(0), Imm32(32));
    ma_b(ScratchRegister, ScratchRegister, label, b ? NonZero : Zero);
}

void
MacroAssemblerMIPS64Compat::branchTestStringTruthy(bool b, const ValueOperand& value, Label* label)
{
    unboxString(value, SecondScratchReg);
    load32(Address(SecondScratchReg, JSString::offsetOfLength()), SecondScratchReg);
    ma_b(SecondScratchReg, Imm32(0), label, b ? NotEqual : Equal);
}

void
MacroAssemblerMIPS64Compat::branchTestDoubleTruthy(bool b, FloatRegister value, Label* label)
{
    ma_lid(ScratchDoubleReg, 0.0);
    DoubleCondition cond = b ? DoubleNotEqual : DoubleEqualOrUnordered;
    ma_bc1d(value, ScratchDoubleReg, label, cond);
}

void
MacroAssemblerMIPS64Compat::branchTestBooleanTruthy(bool b, const ValueOperand& operand,
                                                    Label* label)
{
    unboxBoolean(operand, SecondScratchReg);
    ma_b(SecondScratchReg, SecondScratchReg, label, b ? NonZero : Zero);
}

Register
MacroAssemblerMIPS64Compat::extractObject(const Address& address, Register scratch)
{
    loadPtr(Address(address.base, address.offset), scratch);
    ma_dext(scratch, scratch, Imm32(0), Imm32(JSVAL_TAG_SHIFT));
    return scratch;
}

Register
MacroAssemblerMIPS64Compat::extractTag(const Address& address, Register scratch)
{
    loadPtr(Address(address.base, address.offset), scratch);
    ma_dext(scratch, scratch, Imm32(JSVAL_TAG_SHIFT), Imm32(64 - JSVAL_TAG_SHIFT));
    return scratch;
}

Register
MacroAssemblerMIPS64Compat::extractTag(const BaseIndex& address, Register scratch)
{
    computeScaledAddress(address, scratch);
    return extractTag(Address(scratch, address.offset), scratch);
}

template <typename T>
void
MacroAssemblerMIPS64Compat::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const T& dest,
                                              MIRType slotType)
{
    if (valueType == MIRType_Double) {
        storeDouble(value.reg().typedReg().fpu(), dest);
        return;
    }

    // For known integers and booleans, we can just store the unboxed value if
    // the slot has the same type.
    if ((valueType == MIRType_Int32 || valueType == MIRType_Boolean) && slotType == valueType) {
        if (value.constant()) {
            Value val = value.value();
            if (valueType == MIRType_Int32)
                store32(Imm32(val.toInt32()), dest);
            else
                store32(Imm32(val.toBoolean() ? 1 : 0), dest);
        } else {
            store32(value.reg().typedReg().gpr(), dest);
        }
        return;
    }

    if (value.constant())
        storeValue(value.value(), dest);
    else
        storeValue(ValueTypeFromMIRType(valueType), value.reg().typedReg().gpr(), dest);
}

template void
MacroAssemblerMIPS64Compat::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const Address& dest,
                                              MIRType slotType);

template void
MacroAssemblerMIPS64Compat::storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const BaseIndex& dest,
                                              MIRType slotType);

void
MacroAssemblerMIPS64Compat::moveValue(const Value& val, Register dest)
{
    jsval_layout jv = JSVAL_TO_IMPL(val);
    writeDataRelocation(val);
    movWithPatch(ImmWord(jv.asBits), dest);
}

void
MacroAssemblerMIPS64Compat::moveValue(const Value& val, const ValueOperand& dest)
{
    moveValue(val, dest.valueReg());
}

/* There are 3 paths trough backedge jump. They are listed here in the order
 * in which instructions are executed.
 *  - The short jump is simple:
 *     b offset            # Jumps directly to target.
 *     lui at, addr1_hl    # In delay slot. Don't care about 'at' here.
 *
 *  - The long jump to loop header:
 *      b label1
 *      lui at, addr1_hl   # In delay slot. We use the value in 'at' later.
 *    label1:
 *      ori at, addr1_lh
 *      drotr32 at, at, 48
 *      ori at, addr1_ll
 *      jr at
 *      lui at, addr2_hl   # In delay slot. Don't care about 'at' here.
 *
 *  - The long jump to interrupt loop:
 *      b label2
 *      ...
 *      jr at
 *    label2:
 *      lui at, addr2_hl   # In delay slot. Don't care about 'at' here.
 *      ori at, addr2_lh
 *      drotr32 at, at, 48
 *      ori at, addr2_ll
 *      jr at
 *      nop                # In delay slot.
 *
 * The backedge is done this way to avoid patching lui+ori pair while it is
 * being executed. Look also at jit::PatchBackedge().
 */
CodeOffsetJump
MacroAssemblerMIPS64Compat::backedgeJump(RepatchLabel* label, Label* documentation)
{
    // Only one branch per label.
    MOZ_ASSERT(!label->used());
    uint32_t dest = label->bound() ? label->offset() : LabelBase::INVALID_OFFSET;
    BufferOffset bo = nextOffset();
    label->use(bo.getOffset());

    // Backedges are short jumps when bound, but can become long when patched.
    m_buffer.ensureSpace(16 * sizeof(uint32_t));
    if (label->bound()) {
        int32_t offset = label->offset() - bo.getOffset();
        MOZ_ASSERT(BOffImm16::IsInRange(offset));
        as_b(BOffImm16(offset));
    } else {
        // Jump to "label1" by default to jump to the loop header.
        as_b(BOffImm16(2 * sizeof(uint32_t)));
    }
    // No need for nop here. We can safely put next instruction in delay slot.
    ma_liPatchable(ScratchRegister, ImmWord(dest));
    MOZ_ASSERT(nextOffset().getOffset() - bo.getOffset() == 5 * sizeof(uint32_t));
    as_jr(ScratchRegister);
    // No need for nop here. We can safely put next instruction in delay slot.
    ma_liPatchable(ScratchRegister, ImmWord(dest));
    as_jr(ScratchRegister);
    as_nop();
    MOZ_ASSERT(nextOffset().getOffset() - bo.getOffset() == 12 * sizeof(uint32_t));
    return CodeOffsetJump(bo.getOffset());
}

CodeOffsetJump
MacroAssemblerMIPS64Compat::jumpWithPatch(RepatchLabel* label, Label* documentation)
{
    // Only one branch per label.
    MOZ_ASSERT(!label->used());
    uint32_t dest = label->bound() ? label->offset() : LabelBase::INVALID_OFFSET;

    BufferOffset bo = nextOffset();
    label->use(bo.getOffset());
    addLongJump(bo);
    ma_liPatchable(ScratchRegister, ImmWord(dest));
    as_jr(ScratchRegister);
    as_nop();
    return CodeOffsetJump(bo.getOffset());
}

/////////////////////////////////////////////////////////////////
// X86/X64-common/ARM/MIPS interface.
/////////////////////////////////////////////////////////////////
void
MacroAssemblerMIPS64Compat::storeValue(ValueOperand val, Operand dst)
{
    storeValue(val, Address(Register::FromCode(dst.base()), dst.disp()));
}

void
MacroAssemblerMIPS64Compat::storeValue(ValueOperand val, const BaseIndex& dest)
{
    computeScaledAddress(dest, SecondScratchReg);
    storeValue(val, Address(SecondScratchReg, dest.offset));
}

void
MacroAssemblerMIPS64Compat::storeValue(JSValueType type, Register reg, BaseIndex dest)
{
    computeScaledAddress(dest, ScratchRegister);

    int32_t offset = dest.offset;
    if (!Imm16::IsInSignedRange(offset)) {
        ma_li(SecondScratchReg, Imm32(offset));
        as_daddu(ScratchRegister, ScratchRegister, SecondScratchReg);
        offset = 0;
    }

    storeValue(type, reg, Address(ScratchRegister, offset));
}

void
MacroAssemblerMIPS64Compat::storeValue(ValueOperand val, const Address& dest)
{
    storePtr(val.valueReg(), Address(dest.base, dest.offset));
}

void
MacroAssemblerMIPS64Compat::storeValue(JSValueType type, Register reg, Address dest)
{
    MOZ_ASSERT(dest.base != SecondScratchReg);

    ma_li(SecondScratchReg, ImmTag(JSVAL_TYPE_TO_TAG(type)));
    ma_dsll(SecondScratchReg, SecondScratchReg, Imm32(JSVAL_TAG_SHIFT));
    ma_dins(SecondScratchReg, reg, Imm32(0), Imm32(JSVAL_TAG_SHIFT));
    storePtr(SecondScratchReg, Address(dest.base, dest.offset));
}

void
MacroAssemblerMIPS64Compat::storeValue(const Value& val, Address dest)
{
    jsval_layout jv = JSVAL_TO_IMPL(val);
    if (val.isMarkable()) {
        writeDataRelocation(val);
        movWithPatch(ImmWord(jv.asBits), SecondScratchReg);
    } else {
        ma_li(SecondScratchReg, ImmWord(jv.asBits));
    }
    storePtr(SecondScratchReg, Address(dest.base, dest.offset));
}

void
MacroAssemblerMIPS64Compat::storeValue(const Value& val, BaseIndex dest)
{
    computeScaledAddress(dest, ScratchRegister);

    int32_t offset = dest.offset;
    if (!Imm16::IsInSignedRange(offset)) {
        ma_li(SecondScratchReg, Imm32(offset));
        as_daddu(ScratchRegister, ScratchRegister, SecondScratchReg);
        offset = 0;
    }
    storeValue(val, Address(ScratchRegister, offset));
}

void
MacroAssemblerMIPS64Compat::loadValue(const BaseIndex& addr, ValueOperand val)
{
    computeScaledAddress(addr, SecondScratchReg);
    loadValue(Address(SecondScratchReg, addr.offset), val);
}

void
MacroAssemblerMIPS64Compat::loadValue(Address src, ValueOperand val)
{
    loadPtr(Address(src.base, src.offset), val.valueReg());
}

void
MacroAssemblerMIPS64Compat::tagValue(JSValueType type, Register payload, ValueOperand dest)
{
    MOZ_ASSERT(dest.valueReg() != ScratchRegister);
    if (payload != dest.valueReg())
      ma_move(dest.valueReg(), payload);
    ma_li(ScratchRegister, ImmTag(JSVAL_TYPE_TO_TAG(type)));
    ma_dins(dest.valueReg(), ScratchRegister, Imm32(JSVAL_TAG_SHIFT), Imm32(64 - JSVAL_TAG_SHIFT));
}

void
MacroAssemblerMIPS64Compat::pushValue(ValueOperand val)
{
    // Allocate stack slots for Value. One for each.
    subPtr(Imm32(sizeof(Value)), StackPointer);
    // Store Value
    storeValue(val, Address(StackPointer, 0));
}

void
MacroAssemblerMIPS64Compat::pushValue(const Address& addr)
{
    // Load value before allocate stack, addr.base may be is sp.
    loadPtr(Address(addr.base, addr.offset), ScratchRegister);
    ma_dsubu(StackPointer, StackPointer, Imm32(sizeof(Value)));
    storePtr(ScratchRegister, Address(StackPointer, 0));
}

void
MacroAssemblerMIPS64Compat::popValue(ValueOperand val)
{
    as_ld(val.valueReg(), StackPointer, 0);
    as_daddiu(StackPointer, StackPointer, sizeof(Value));
}

void
MacroAssemblerMIPS64Compat::breakpoint()
{
    as_break(0);
}

void
MacroAssemblerMIPS64Compat::ensureDouble(const ValueOperand& source, FloatRegister dest,
                                         Label* failure)
{
    Label isDouble, done;
    Register tag = splitTagForTest(source);
    branchTestDouble(Assembler::Equal, tag, &isDouble);
    branchTestInt32(Assembler::NotEqual, tag, failure);

    unboxInt32(source, ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
    jump(&done);

    bind(&isDouble);
    unboxDouble(source, dest);

    bind(&done);
}

void
MacroAssemblerMIPS64Compat::cmpPtrSet(Assembler::Condition cond, Address lhs, ImmPtr rhs,
                                      Register dest)
{
    loadPtr(lhs, ScratchRegister);
    movePtr(rhs, SecondScratchReg);
    cmpPtrSet(cond, ScratchRegister, SecondScratchReg, dest);
}

void
MacroAssemblerMIPS64Compat::cmpPtrSet(Assembler::Condition cond, Register lhs, Address rhs,
                                      Register dest)
{
    loadPtr(rhs, ScratchRegister);
    cmpPtrSet(cond, lhs, ScratchRegister, dest);
}

void
MacroAssemblerMIPS64Compat::cmp32Set(Assembler::Condition cond, Register lhs, Address rhs,
                                     Register dest)
{
    load32(rhs, ScratchRegister);
    cmp32Set(cond, lhs, ScratchRegister, dest);
}

void
MacroAssemblerMIPS64Compat::checkStackAlignment()
{
#ifdef DEBUG
    Label aligned;
    as_andi(ScratchRegister, sp, ABIStackAlignment - 1);
    ma_b(ScratchRegister, zero, &aligned, Equal, ShortJump);
    as_break(BREAK_STACK_UNALIGNED);
    bind(&aligned);
#endif
}

void
MacroAssembler::alignFrameForICArguments(AfterICSaveLive& aic)
{
    if (framePushed() % ABIStackAlignment != 0) {
        aic.alignmentPadding = ABIStackAlignment - (framePushed() % ABIStackAlignment);
        reserveStack(aic.alignmentPadding);
    } else {
        aic.alignmentPadding = 0;
    }
    MOZ_ASSERT(framePushed() % ABIStackAlignment == 0);
    checkStackAlignment();
}

void
MacroAssembler::restoreFrameAlignmentForICArguments(AfterICSaveLive& aic)
{
    if (aic.alignmentPadding != 0)
        freeStack(aic.alignmentPadding);
}

void
MacroAssemblerMIPS64Compat::handleFailureWithHandlerTail(void* handler)
{
    // Reserve space for exception information.
    int size = (sizeof(ResumeFromException) + ABIStackAlignment) & ~(ABIStackAlignment - 1);
    subPtr(Imm32(size), StackPointer);
    ma_move(a0, StackPointer); // Use a0 since it is a first function argument

    // Call the handler.
    asMasm().setupUnalignedABICall(a1);
    asMasm().passABIArg(a0);
    asMasm().callWithABI(handler);

    Label entryFrame;
    Label catch_;
    Label finally;
    Label return_;
    Label bailout;

    // Already clobbered a0, so use it...
    load32(Address(StackPointer, offsetof(ResumeFromException, kind)), a0);
    branch32(Assembler::Equal, a0, Imm32(ResumeFromException::RESUME_ENTRY_FRAME), &entryFrame);
    branch32(Assembler::Equal, a0, Imm32(ResumeFromException::RESUME_CATCH), &catch_);
    branch32(Assembler::Equal, a0, Imm32(ResumeFromException::RESUME_FINALLY), &finally);
    branch32(Assembler::Equal, a0, Imm32(ResumeFromException::RESUME_FORCED_RETURN), &return_);
    branch32(Assembler::Equal, a0, Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);

    breakpoint(); // Invalid kind.

    // No exception handler. Load the error value, load the new stack pointer
    // and return from the entry frame.
    bind(&entryFrame);
    moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);

    // We're going to be returning by the ion calling convention
    ma_pop(ra);
    as_jr(ra);
    as_nop();

    // If we found a catch handler, this must be a baseline frame. Restore
    // state and jump to the catch block.
    bind(&catch_);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, target)), a0);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);
    jump(a0);

    // If we found a finally block, this must be a baseline frame. Push
    // two values expected by JSOP_RETSUB: BooleanValue(true) and the
    // exception.
    bind(&finally);
    ValueOperand exception = ValueOperand(a1);
    loadValue(Address(sp, offsetof(ResumeFromException, exception)), exception);

    loadPtr(Address(sp, offsetof(ResumeFromException, target)), a0);
    loadPtr(Address(sp, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(sp, offsetof(ResumeFromException, stackPointer)), sp);

    pushValue(BooleanValue(true));
    pushValue(exception);
    jump(a0);

    // Only used in debug mode. Return BaselineFrame->returnValue() to the
    // caller.
    bind(&return_);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);
    loadValue(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue()),
              JSReturnOperand);
    ma_move(StackPointer, BaselineFrameReg);
    pop(BaselineFrameReg);

    // If profiling is enabled, then update the lastProfilingFrame to refer to caller
    // frame before returning.
    {
        Label skipProfilingInstrumentation;
        // Test if profiler enabled.
        AbsoluteAddress addressOfEnabled(GetJitContext()->runtime->spsProfiler().addressOfEnabled());
        branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skipProfilingInstrumentation);
        profilerExitFrame();
        bind(&skipProfilingInstrumentation);
    }

    ret();

    // If we are bailing out to baseline to handle an exception, jump to
    // the bailout tail stub.
    bind(&bailout);
    loadPtr(Address(sp, offsetof(ResumeFromException, bailoutInfo)), a2);
    ma_li(ReturnReg, Imm32(BAILOUT_RETURN_OK));
    loadPtr(Address(sp, offsetof(ResumeFromException, target)), a1);
    jump(a1);
}

template<typename T>
void
MacroAssemblerMIPS64Compat::compareExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem,
                                                           Register oldval, Register newval,
                                                           Register temp, Register valueTemp,
                                                           Register offsetTemp, Register maskTemp,
                                                           AnyRegister output)
{
    switch (arrayType) {
      case Scalar::Int8:
        compareExchange8SignExtend(mem, oldval, newval, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Uint8:
        compareExchange8ZeroExtend(mem, oldval, newval, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Int16:
        compareExchange16SignExtend(mem, oldval, newval, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Uint16:
        compareExchange16ZeroExtend(mem, oldval, newval, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Int32:
        compareExchange32(mem, oldval, newval, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Uint32:
        // At the moment, the code in MCallOptimize.cpp requires the output
        // type to be double for uint32 arrays.  See bug 1077305.
        MOZ_ASSERT(output.isFloat());
        compareExchange32(mem, oldval, newval, valueTemp, offsetTemp, maskTemp, temp);
        convertUInt32ToDouble(temp, output.fpu());
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
}

template void
MacroAssemblerMIPS64Compat::compareExchangeToTypedIntArray(Scalar::Type arrayType, const Address& mem,
                                                           Register oldval, Register newval, Register temp,
                                                           Register valueTemp, Register offsetTemp, Register maskTemp,
                                                           AnyRegister output);
template void
MacroAssemblerMIPS64Compat::compareExchangeToTypedIntArray(Scalar::Type arrayType, const BaseIndex& mem,
                                                           Register oldval, Register newval, Register temp,
                                                           Register valueTemp, Register offsetTemp, Register maskTemp,
                                                           AnyRegister output);

template<typename T>
void
MacroAssemblerMIPS64Compat::atomicExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem,
                                                          Register value, Register temp, Register valueTemp,
                                                          Register offsetTemp, Register maskTemp,
                                                          AnyRegister output)
{
    switch (arrayType) {
      case Scalar::Int8:
        atomicExchange8SignExtend(mem, value, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Uint8:
        atomicExchange8ZeroExtend(mem, value, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Int16:
        atomicExchange16SignExtend(mem, value, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Uint16:
        atomicExchange16ZeroExtend(mem, value, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Int32:
        atomicExchange32(mem, value, valueTemp, offsetTemp, maskTemp, output.gpr());
        break;
      case Scalar::Uint32:
        // At the moment, the code in MCallOptimize.cpp requires the output
        // type to be double for uint32 arrays.  See bug 1077305.
        MOZ_ASSERT(output.isFloat());
        atomicExchange32(mem, value, valueTemp, offsetTemp, maskTemp, temp);
        convertUInt32ToDouble(temp, output.fpu());
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
}

template void
MacroAssemblerMIPS64Compat::atomicExchangeToTypedIntArray(Scalar::Type arrayType, const Address& mem,
                                                          Register value, Register temp, Register valueTemp,
                                                          Register offsetTemp, Register maskTemp,
                                                          AnyRegister output);
template void
MacroAssemblerMIPS64Compat::atomicExchangeToTypedIntArray(Scalar::Type arrayType, const BaseIndex& mem,
                                                          Register value, Register temp, Register valueTemp,
                                                          Register offsetTemp, Register maskTemp,
                                                          AnyRegister output);

CodeOffset
MacroAssemblerMIPS64Compat::toggledJump(Label* label)
{
    CodeOffset ret(nextOffset().getOffset());
    ma_b(label);
    return ret;
}

CodeOffset
MacroAssemblerMIPS64Compat::toggledCall(JitCode* target, bool enabled)
{
    BufferOffset bo = nextOffset();
    CodeOffset offset(bo.getOffset());
    addPendingJump(bo, ImmPtr(target->raw()), Relocation::JITCODE);
    ma_liPatchable(ScratchRegister, ImmPtr(target->raw()));
    if (enabled) {
        as_jalr(ScratchRegister);
        as_nop();
    } else {
        as_nop();
        as_nop();
    }
    MOZ_ASSERT_IF(!oom(), nextOffset().getOffset() - offset.offset() == ToggledCallSize(nullptr));
    return offset;
}

void
MacroAssemblerMIPS64Compat::branchPtrInNurseryRange(Condition cond, Register ptr, Register temp,
                                                    Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(ptr != temp);
    MOZ_ASSERT(ptr != SecondScratchReg);

    const Nursery& nursery = GetJitContext()->runtime->gcNursery();
    movePtr(ImmWord(-ptrdiff_t(nursery.start())), SecondScratchReg);
    addPtr(ptr, SecondScratchReg);
    branchPtr(cond == Assembler::Equal ? Assembler::Below : Assembler::AboveOrEqual,
              SecondScratchReg, Imm32(nursery.nurserySize()), label);
}

void
MacroAssemblerMIPS64Compat::branchValueIsNurseryObject(Condition cond, ValueOperand value,
                                                       Register temp, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

    // 'Value' representing the start of the nursery tagged as a JSObject
    const Nursery& nursery = GetJitContext()->runtime->gcNursery();
    Value start = ObjectValue(*reinterpret_cast<JSObject *>(nursery.start()));

    movePtr(ImmWord(-ptrdiff_t(start.asRawBits())), SecondScratchReg);
    addPtr(value.valueReg(), SecondScratchReg);
    branchPtr(cond == Assembler::Equal ? Assembler::Below : Assembler::AboveOrEqual,
              SecondScratchReg, Imm32(nursery.nurserySize()), label);
}

void
MacroAssemblerMIPS64Compat::profilerEnterFrame(Register framePtr, Register scratch)
{
    AbsoluteAddress activation(GetJitContext()->runtime->addressOfProfilingActivation());
    loadPtr(activation, scratch);
    storePtr(framePtr, Address(scratch, JitActivation::offsetOfLastProfilingFrame()));
    storePtr(ImmPtr(nullptr), Address(scratch, JitActivation::offsetOfLastProfilingCallSite()));
}

void
MacroAssemblerMIPS64Compat::profilerExitFrame()
{
    branch(GetJitContext()->runtime->jitRuntime()->getProfilerExitFrameTail());
}

//{{{ check_macroassembler_style
// ===============================================================
// Stack manipulation functions.

void
MacroAssembler::PushRegsInMask(LiveRegisterSet set)
{
    int32_t diff = set.gprs().size() * sizeof(intptr_t) +
        set.fpus().getPushSizeInBytes();
    const int32_t reserved = diff;

    reserveStack(reserved);
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); iter++) {
        diff -= sizeof(intptr_t);
        storePtr(*iter, Address(StackPointer, diff));
    }
    for (FloatRegisterBackwardIterator iter(set.fpus().reduceSetForPush()); iter.more(); iter++) {
        diff -= sizeof(double);
        storeDouble(*iter, Address(StackPointer, diff));
    }
    MOZ_ASSERT(diff == 0);
}

void
MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set, LiveRegisterSet ignore)
{
    int32_t diff = set.gprs().size() * sizeof(intptr_t) +
        set.fpus().getPushSizeInBytes();
    const int32_t reserved = diff;

    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); iter++) {
        diff -= sizeof(intptr_t);
        if (!ignore.has(*iter))
          loadPtr(Address(StackPointer, diff), *iter);
    }
    for (FloatRegisterBackwardIterator iter(set.fpus().reduceSetForPush()); iter.more(); iter++) {
        diff -= sizeof(double);
        if (!ignore.has(*iter))
          loadDouble(Address(StackPointer, diff), *iter);
    }
    MOZ_ASSERT(diff == 0);
    freeStack(reserved);
}

void
MacroAssembler::reserveStack(uint32_t amount)
{
    if (amount)
        subPtr(Imm32(amount), StackPointer);
    adjustFrame(amount);
}

// ===============================================================
// ABI function calls.

void
MacroAssembler::setupUnalignedABICall(Register scratch)
{
    setupABICall();
    dynamicAlignment_ = true;

    ma_move(scratch, StackPointer);

    // Force sp to be aligned
    subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
    ma_and(StackPointer, StackPointer, Imm32(~(ABIStackAlignment - 1)));
    storePtr(scratch, Address(StackPointer, 0));
}

void
MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromAsmJS)
{
    MOZ_ASSERT(inCall_);
    uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

    // Reserve place for $ra.
    stackForCall += sizeof(intptr_t);

    if (dynamicAlignment_) {
        stackForCall += ComputeByteAlignment(stackForCall, ABIStackAlignment);
    } else {
        uint32_t alignmentAtPrologue = callFromAsmJS ? sizeof(AsmJSFrame) : 0;
        stackForCall += ComputeByteAlignment(stackForCall + framePushed() + alignmentAtPrologue,
                                             ABIStackAlignment);
    }

    *stackAdjust = stackForCall;
    reserveStack(stackForCall);

    // Save $ra because call is going to clobber it. Restore it in
    // callWithABIPost. NOTE: This is needed for calls from SharedIC.
    // Maybe we can do this differently.
    storePtr(ra, Address(StackPointer, stackForCall - sizeof(intptr_t)));

    // Position all arguments.
    {
        enoughMemory_ = enoughMemory_ && moveResolver_.resolve();
        if (!enoughMemory_)
            return;

        MoveEmitter emitter(*this);
        emitter.emit(moveResolver_);
        emitter.finish();
    }

    assertStackAlignment(ABIStackAlignment);
}

void
MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result)
{
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

void
MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result)
{
    // Load the callee in t9, no instruction between the lw and call
    // should clobber it. Note that we can't use fun.base because it may
    // be one of the IntArg registers clobbered before the call.
    ma_move(t9, fun);
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(t9);
    callWithABIPost(stackAdjust, result);
}

void
MacroAssembler::callWithABINoProfiler(const Address& fun, MoveOp::Type result)
{
    // Load the callee in t9, as above.
    loadPtr(Address(fun.base, fun.offset), t9);
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    call(t9);
    callWithABIPost(stackAdjust, result);
}

//}}} check_macroassembler_style

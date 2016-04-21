/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips-shared/Assembler-mips-shared.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jscompartment.h"
#include "jsutil.h"

#include "gc/Marking.h"
#include "jit/ExecutableAllocator.h"
#include "jit/JitCompartment.h"

using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

// Encode a standard register when it is being used as rd, the rs, and
// an extra register(rt). These should never be called with an InvalidReg.
uint32_t
js::jit::RS(Register r)
{
    MOZ_ASSERT((r.code() & ~RegMask) == 0);
    return r.code() << RSShift;
}

uint32_t
js::jit::RT(Register r)
{
    MOZ_ASSERT((r.code() & ~RegMask) == 0);
    return r.code() << RTShift;
}

uint32_t
js::jit::RD(Register r)
{
    MOZ_ASSERT((r.code() & ~RegMask) == 0);
    return r.code() << RDShift;
}

uint32_t
js::jit::SA(uint32_t value)
{
    MOZ_ASSERT(value < 32);
    return value << SAShift;
}

Register
js::jit::toRS(Instruction& i)
{
    return Register::FromCode((i.encode() & RSMask ) >> RSShift);
}

Register
js::jit::toRT(Instruction& i)
{
    return Register::FromCode((i.encode() & RTMask ) >> RTShift);
}

Register
js::jit::toRD(Instruction& i)
{
    return Register::FromCode((i.encode() & RDMask ) >> RDShift);
}

Register
js::jit::toR(Instruction& i)
{
    return Register::FromCode(i.encode() & RegMask);
}

void
InstImm::extractImm16(BOffImm16* dest)
{
    *dest = BOffImm16(*this);
}

void
AssemblerMIPSShared::finish()
{
    MOZ_ASSERT(!isFinished);
    isFinished = true;
}

bool
AssemblerMIPSShared::asmMergeWith(const AssemblerMIPSShared& other)
{
    if (!AssemblerShared::asmMergeWith(size(), other))
        return false;
    for (size_t i = 0; i < other.numLongJumps(); i++) {
        size_t off = other.longJumps_[i];
        addLongJump(BufferOffset(size() + off));
    }
    return m_buffer.appendBuffer(other.m_buffer);
}

uint32_t
AssemblerMIPSShared::actualIndex(uint32_t idx_) const
{
    return idx_;
}

uint8_t*
AssemblerMIPSShared::PatchableJumpAddress(JitCode* code, uint32_t pe_)
{
    return code->raw() + pe_;
}

void
AssemblerMIPSShared::copyJumpRelocationTable(uint8_t* dest)
{
    if (jumpRelocations_.length())
        memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
}

void
AssemblerMIPSShared::copyDataRelocationTable(uint8_t* dest)
{
    if (dataRelocations_.length())
        memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
}

void
AssemblerMIPSShared::copyPreBarrierTable(uint8_t* dest)
{
    if (preBarriers_.length())
        memcpy(dest, preBarriers_.buffer(), preBarriers_.length());
}

void
AssemblerMIPSShared::processCodeLabels(uint8_t* rawCode)
{
    for (size_t i = 0; i < codeLabels_.length(); i++) {
        CodeLabel label = codeLabels_[i];
        Bind(rawCode, label.patchAt(), rawCode + label.target()->offset());
    }
}

AssemblerMIPSShared::Condition
AssemblerMIPSShared::InvertCondition(Condition cond)
{
    switch (cond) {
      case Equal:
        return NotEqual;
      case NotEqual:
        return Equal;
      case Zero:
        return NonZero;
      case NonZero:
        return Zero;
      case LessThan:
        return GreaterThanOrEqual;
      case LessThanOrEqual:
        return GreaterThan;
      case GreaterThan:
        return LessThanOrEqual;
      case GreaterThanOrEqual:
        return LessThan;
      case Above:
        return BelowOrEqual;
      case AboveOrEqual:
        return Below;
      case Below:
        return AboveOrEqual;
      case BelowOrEqual:
        return Above;
      case Signed:
        return NotSigned;
      case NotSigned:
        return Signed;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

AssemblerMIPSShared::DoubleCondition
AssemblerMIPSShared::InvertCondition(DoubleCondition cond)
{
    switch (cond) {
      case DoubleOrdered:
        return DoubleUnordered;
      case DoubleEqual:
        return DoubleNotEqualOrUnordered;
      case DoubleNotEqual:
        return DoubleEqualOrUnordered;
      case DoubleGreaterThan:
        return DoubleLessThanOrEqualOrUnordered;
      case DoubleGreaterThanOrEqual:
        return DoubleLessThanOrUnordered;
      case DoubleLessThan:
        return DoubleGreaterThanOrEqualOrUnordered;
      case DoubleLessThanOrEqual:
        return DoubleGreaterThanOrUnordered;
      case DoubleUnordered:
        return DoubleOrdered;
      case DoubleEqualOrUnordered:
        return DoubleNotEqual;
      case DoubleNotEqualOrUnordered:
        return DoubleEqual;
      case DoubleGreaterThanOrUnordered:
        return DoubleLessThanOrEqual;
      case DoubleGreaterThanOrEqualOrUnordered:
        return DoubleLessThan;
      case DoubleLessThanOrUnordered:
        return DoubleGreaterThanOrEqual;
      case DoubleLessThanOrEqualOrUnordered:
        return DoubleGreaterThan;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

BOffImm16::BOffImm16(InstImm inst)
  : data(inst.encode() & Imm16Mask)
{
}

bool
AssemblerMIPSShared::oom() const
{
    return AssemblerShared::oom() ||
           m_buffer.oom() ||
           jumpRelocations_.oom() ||
           dataRelocations_.oom() ||
           preBarriers_.oom();
}

// Size of the instruction stream, in bytes.
size_t
AssemblerMIPSShared::size() const
{
    return m_buffer.size();
}

// Size of the relocation table, in bytes.
size_t
AssemblerMIPSShared::jumpRelocationTableBytes() const
{
    return jumpRelocations_.length();
}

size_t
AssemblerMIPSShared::dataRelocationTableBytes() const
{
    return dataRelocations_.length();
}

size_t
AssemblerMIPSShared::preBarrierTableBytes() const
{
    return preBarriers_.length();
}

// Size of the data table, in bytes.
size_t
AssemblerMIPSShared::bytesNeeded() const
{
    return size() +
           jumpRelocationTableBytes() +
           dataRelocationTableBytes() +
           preBarrierTableBytes();
}

// write a blob of binary into the instruction stream
BufferOffset
AssemblerMIPSShared::writeInst(uint32_t x, uint32_t* dest)
{
    if (dest == nullptr)
        return m_buffer.putInt(x);

    WriteInstStatic(x, dest);
    return BufferOffset();
}

void
AssemblerMIPSShared::WriteInstStatic(uint32_t x, uint32_t* dest)
{
    MOZ_ASSERT(dest != nullptr);
    *dest = x;
}

BufferOffset
AssemblerMIPSShared::haltingAlign(int alignment)
{
    // TODO: Implement a proper halting align.
    return nopAlign(alignment);
}

BufferOffset
AssemblerMIPSShared::nopAlign(int alignment)
{
    BufferOffset ret;
    MOZ_ASSERT(m_buffer.isAligned(4));
    if (alignment == 8) {
        if (!m_buffer.isAligned(alignment)) {
            BufferOffset tmp = as_nop();
            if (!ret.assigned())
                ret = tmp;
        }
    } else {
        MOZ_ASSERT((alignment & (alignment - 1)) == 0);
        while (size() & (alignment - 1)) {
            BufferOffset tmp = as_nop();
            if (!ret.assigned())
                ret = tmp;
        }
    }
    return ret;
}

BufferOffset
AssemblerMIPSShared::as_nop()
{
    return writeInst(op_special | ff_sll);
}

// Logical operations.
BufferOffset
AssemblerMIPSShared::as_and(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_and).encode());
}

BufferOffset
AssemblerMIPSShared::as_or(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_or).encode());
}

BufferOffset
AssemblerMIPSShared::as_xor(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_xor).encode());
}

BufferOffset
AssemblerMIPSShared::as_nor(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_nor).encode());
}

BufferOffset
AssemblerMIPSShared::as_andi(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_andi, rs, rd, Imm16(j)).encode());
}

BufferOffset
AssemblerMIPSShared::as_ori(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_ori, rs, rd, Imm16(j)).encode());
}

BufferOffset
AssemblerMIPSShared::as_xori(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_xori, rs, rd, Imm16(j)).encode());
}

// Branch and jump instructions
BufferOffset
AssemblerMIPSShared::as_bal(BOffImm16 off)
{
    BufferOffset bo = writeInst(InstImm(op_regimm, zero, rt_bgezal, off).encode());
    return bo;
}

BufferOffset
AssemblerMIPSShared::as_b(BOffImm16 off)
{
    BufferOffset bo = writeInst(InstImm(op_beq, zero, zero, off).encode());
    return bo;
}

InstImm
AssemblerMIPSShared::getBranchCode(JumpOrCall jumpOrCall)
{
    if (jumpOrCall == BranchIsCall)
        return InstImm(op_regimm, zero, rt_bgezal, BOffImm16(0));

    return InstImm(op_beq, zero, zero, BOffImm16(0));
}

InstImm
AssemblerMIPSShared::getBranchCode(Register s, Register t, Condition c)
{
    MOZ_ASSERT(c == AssemblerMIPSShared::Equal || c == AssemblerMIPSShared::NotEqual);
    return InstImm(c == AssemblerMIPSShared::Equal ? op_beq : op_bne, s, t, BOffImm16(0));
}

InstImm
AssemblerMIPSShared::getBranchCode(Register s, Condition c)
{
    switch (c) {
      case AssemblerMIPSShared::Equal:
      case AssemblerMIPSShared::Zero:
      case AssemblerMIPSShared::BelowOrEqual:
        return InstImm(op_beq, s, zero, BOffImm16(0));
      case AssemblerMIPSShared::NotEqual:
      case AssemblerMIPSShared::NonZero:
      case AssemblerMIPSShared::Above:
        return InstImm(op_bne, s, zero, BOffImm16(0));
      case AssemblerMIPSShared::GreaterThan:
        return InstImm(op_bgtz, s, zero, BOffImm16(0));
      case AssemblerMIPSShared::GreaterThanOrEqual:
      case AssemblerMIPSShared::NotSigned:
        return InstImm(op_regimm, s, rt_bgez, BOffImm16(0));
      case AssemblerMIPSShared::LessThan:
      case AssemblerMIPSShared::Signed:
        return InstImm(op_regimm, s, rt_bltz, BOffImm16(0));
      case AssemblerMIPSShared::LessThanOrEqual:
        return InstImm(op_blez, s, zero, BOffImm16(0));
      default:
        MOZ_CRASH("Condition not supported.");
    }
}

InstImm
AssemblerMIPSShared::getBranchCode(FloatTestKind testKind, FPConditionBit fcc)
{
    MOZ_ASSERT(!(fcc && FccMask));
    uint32_t rtField = ((testKind == TestForTrue ? 1 : 0) | (fcc << FccShift)) << RTShift;

    return InstImm(op_cop1, rs_bc1, rtField, BOffImm16(0));
}

BufferOffset
AssemblerMIPSShared::as_j(JOffImm26 off)
{
    BufferOffset bo = writeInst(InstJump(op_j, off).encode());
    return bo;
}
BufferOffset
AssemblerMIPSShared::as_jal(JOffImm26 off)
{
    BufferOffset bo = writeInst(InstJump(op_jal, off).encode());
    return bo;
}

BufferOffset
AssemblerMIPSShared::as_jr(Register rs)
{
    BufferOffset bo = writeInst(InstReg(op_special, rs, zero, zero, ff_jr).encode());
    return bo;
}
BufferOffset
AssemblerMIPSShared::as_jalr(Register rs)
{
    BufferOffset bo = writeInst(InstReg(op_special, rs, zero, ra, ff_jalr).encode());
    return bo;
}


// Arithmetic instructions
BufferOffset
AssemblerMIPSShared::as_addu(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_addu).encode());
}

BufferOffset
AssemblerMIPSShared::as_addiu(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(j));
    return writeInst(InstImm(op_addiu, rs, rd, Imm16(j)).encode());
}

BufferOffset
AssemblerMIPSShared::as_daddu(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_daddu).encode());
}

BufferOffset
AssemblerMIPSShared::as_daddiu(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(j));
    return writeInst(InstImm(op_daddiu, rs, rd, Imm16(j)).encode());
}

BufferOffset
AssemblerMIPSShared::as_subu(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_subu).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsubu(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_dsubu).encode());
}

BufferOffset
AssemblerMIPSShared::as_mult(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_mult).encode());
}

BufferOffset
AssemblerMIPSShared::as_multu(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_multu).encode());
}

BufferOffset
AssemblerMIPSShared::as_dmult(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_dmult).encode());
}

BufferOffset
AssemblerMIPSShared::as_dmultu(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_dmultu).encode());
}

BufferOffset
AssemblerMIPSShared::as_div(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_div).encode());
}

BufferOffset
AssemblerMIPSShared::as_divu(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_divu).encode());
}

BufferOffset
AssemblerMIPSShared::as_ddiv(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_ddiv).encode());
}

BufferOffset
AssemblerMIPSShared::as_ddivu(Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, ff_ddivu).encode());
}

BufferOffset
AssemblerMIPSShared::as_mul(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special2, rs, rt, rd, ff_mul).encode());
}

BufferOffset
AssemblerMIPSShared::as_lui(Register rd, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_lui, zero, rd, Imm16(j)).encode());
}

// Shift instructions
BufferOffset
AssemblerMIPSShared::as_sll(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_sll).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsll(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_dsll).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsll32(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(31 < sa && sa < 64);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa - 32, ff_dsll32).encode());
}

BufferOffset
AssemblerMIPSShared::as_sllv(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_sllv).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsllv(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_dsllv).encode());
}

BufferOffset
AssemblerMIPSShared::as_srl(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_srl).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsrl(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_dsrl).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsrl32(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(31 < sa && sa < 64);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa - 32, ff_dsrl32).encode());
}

BufferOffset
AssemblerMIPSShared::as_srlv(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_srlv).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsrlv(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_dsrlv).encode());
}

BufferOffset
AssemblerMIPSShared::as_sra(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_sra).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsra(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa, ff_dsra).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsra32(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(31 < sa && sa < 64);
    return writeInst(InstReg(op_special, rs_zero, rt, rd, sa - 32, ff_dsra32).encode());
}

BufferOffset
AssemblerMIPSShared::as_srav(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_srav).encode());
}

BufferOffset
AssemblerMIPSShared::as_dsrav(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_dsrav).encode());
}

BufferOffset
AssemblerMIPSShared::as_rotr(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_one, rt, rd, sa, ff_srl).encode());
}

BufferOffset
AssemblerMIPSShared::as_drotr(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(sa < 32);
    return writeInst(InstReg(op_special, rs_one, rt, rd, sa, ff_dsrl).encode());
}

BufferOffset
AssemblerMIPSShared::as_drotr32(Register rd, Register rt, uint16_t sa)
{
    MOZ_ASSERT(31 < sa && sa < 64);
    return writeInst(InstReg(op_special, rs_one, rt, rd, sa - 32, ff_dsrl32).encode());
}

BufferOffset
AssemblerMIPSShared::as_rotrv(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, 1, ff_srlv).encode());
}

BufferOffset
AssemblerMIPSShared::as_drotrv(Register rd, Register rt, Register rs)
{
    return writeInst(InstReg(op_special, rs, rt, rd, 1, ff_dsrlv).encode());
}

// Load and store instructions
BufferOffset
AssemblerMIPSShared::as_lb(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lb, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_lbu(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lbu, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_lh(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lh, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_lhu(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lhu, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_lw(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lw, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_lwu(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lwu, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_lwl(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lwl, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_lwr(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_lwr, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_ll(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_ll, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_ld(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_ld, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_ldl(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_ldl, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_ldr(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_ldr, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_sb(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sb, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_sh(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sh, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_sw(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sw, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_swl(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_swl, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_swr(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_swr, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_sc(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sc, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_sd(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sd, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_sdl(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sdl, rs, rd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_sdr(Register rd, Register rs, int16_t off)
{
    return writeInst(InstImm(op_sdr, rs, rd, Imm16(off)).encode());
}

// Move from HI/LO register.
BufferOffset
AssemblerMIPSShared::as_mfhi(Register rd)
{
    return writeInst(InstReg(op_special, rd, ff_mfhi).encode());
}

BufferOffset
AssemblerMIPSShared::as_mflo(Register rd)
{
    return writeInst(InstReg(op_special, rd, ff_mflo).encode());
}

// Set on less than.
BufferOffset
AssemblerMIPSShared::as_slt(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_slt).encode());
}

BufferOffset
AssemblerMIPSShared::as_sltu(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_sltu).encode());
}

BufferOffset
AssemblerMIPSShared::as_slti(Register rd, Register rs, int32_t j)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(j));
    return writeInst(InstImm(op_slti, rs, rd, Imm16(j)).encode());
}

BufferOffset
AssemblerMIPSShared::as_sltiu(Register rd, Register rs, uint32_t j)
{
    MOZ_ASSERT(Imm16::IsInUnsignedRange(j));
    return writeInst(InstImm(op_sltiu, rs, rd, Imm16(j)).encode());
}

// Conditional move.
BufferOffset
AssemblerMIPSShared::as_movz(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_movz).encode());
}

BufferOffset
AssemblerMIPSShared::as_movn(Register rd, Register rs, Register rt)
{
    return writeInst(InstReg(op_special, rs, rt, rd, ff_movn).encode());
}

BufferOffset
AssemblerMIPSShared::as_movt(Register rd, Register rs, uint16_t cc)
{
    Register rt;
    rt = Register::FromCode((cc & 0x7) << 2 | 1);
    return writeInst(InstReg(op_special, rs, rt, rd, ff_movci).encode());
}

BufferOffset
AssemblerMIPSShared::as_movf(Register rd, Register rs, uint16_t cc)
{
    Register rt;
    rt = Register::FromCode((cc & 0x7) << 2 | 0);
    return writeInst(InstReg(op_special, rs, rt, rd, ff_movci).encode());
}

// Bit twiddling.
BufferOffset
AssemblerMIPSShared::as_clz(Register rd, Register rs)
{
    return writeInst(InstReg(op_special2, rs, rd, rd, ff_clz).encode());
}

BufferOffset
AssemblerMIPSShared::as_dclz(Register rd, Register rs)
{
    return writeInst(InstReg(op_special2, rs, rd, rd, ff_dclz).encode());
}

BufferOffset
AssemblerMIPSShared::as_ins(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 && pos + size <= 32);
    Register rd;
    rd = Register::FromCode(pos + size - 1);
    return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_ins).encode());
}

BufferOffset
AssemblerMIPSShared::as_dins(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 && pos + size <= 32);
    Register rd;
    rd = Register::FromCode(pos + size - 1);
    return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_dins).encode());
}

BufferOffset
AssemblerMIPSShared::as_dinsm(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos < 32 && size >= 2 && size <= 64 && pos + size > 32 && pos + size <= 64);
    Register rd;
    rd = Register::FromCode(pos + size - 1 - 32);
    return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_dinsm).encode());
}

BufferOffset
AssemblerMIPSShared::as_dinsu(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos >= 32 && pos < 64 && size >= 1 && size <= 32 && pos + size > 32 && pos + size <= 64);
    Register rd;
    rd = Register::FromCode(pos + size - 1 - 32);
    return writeInst(InstReg(op_special3, rs, rt, rd, pos - 32, ff_dinsu).encode());
}

BufferOffset
AssemblerMIPSShared::as_ext(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 && pos + size <= 32);
    Register rd;
    rd = Register::FromCode(size - 1);
    return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_ext).encode());
}

// Sign extend
BufferOffset
AssemblerMIPSShared::as_seb(Register rd, Register rt)
{
    return writeInst(InstReg(op_special3, zero, rt, rd, 16, ff_bshfl).encode());
}

BufferOffset
AssemblerMIPSShared::as_seh(Register rd, Register rt)
{
    return writeInst(InstReg(op_special3, zero, rt, rd, 24, ff_bshfl).encode());
}

BufferOffset
AssemblerMIPSShared::as_dext(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos < 32 && size != 0 && size <= 32 && pos + size != 0 && pos + size <= 63);
    Register rd;
    rd = Register::FromCode(size - 1);
    return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_dext).encode());
}

BufferOffset
AssemblerMIPSShared::as_dextm(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos < 32 && size > 32 && size <= 64 && pos + size > 32 && pos + size <= 64);
    Register rd;
    rd = Register::FromCode(size - 1 - 32);
   return writeInst(InstReg(op_special3, rs, rt, rd, pos, ff_dextm).encode());
}

BufferOffset
AssemblerMIPSShared::as_dextu(Register rt, Register rs, uint16_t pos, uint16_t size)
{
    MOZ_ASSERT(pos >= 32 && pos < 64 && size != 0 && size <= 32 && pos + size > 32 && pos + size <= 64);
    Register rd;
    rd = Register::FromCode(size - 1);
    return writeInst(InstReg(op_special3, rs, rt, rd, pos - 32, ff_dextu).encode());
}

// FP instructions
BufferOffset
AssemblerMIPSShared::as_ld(FloatRegister fd, Register base, int32_t off)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(off));
    return writeInst(InstImm(op_ldc1, base, fd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_sd(FloatRegister fd, Register base, int32_t off)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(off));
    return writeInst(InstImm(op_sdc1, base, fd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_ls(FloatRegister fd, Register base, int32_t off)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(off));
    return writeInst(InstImm(op_lwc1, base, fd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_ss(FloatRegister fd, Register base, int32_t off)
{
    MOZ_ASSERT(Imm16::IsInSignedRange(off));
    return writeInst(InstImm(op_swc1, base, fd, Imm16(off)).encode());
}

BufferOffset
AssemblerMIPSShared::as_movs(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_mov_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_movd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_mov_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_mtc1(Register rt, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_mtc1, rt, fs).encode());
}

BufferOffset
AssemblerMIPSShared::as_mfc1(Register rt, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_mfc1, rt, fs).encode());
}

BufferOffset
AssemblerMIPSShared::as_mthc1(Register rt, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_mthc1, rt, fs).encode());
}

BufferOffset
AssemblerMIPSShared::as_mfhc1(Register rt, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_mfhc1, rt, fs).encode());
}

BufferOffset
AssemblerMIPSShared::as_dmtc1(Register rt, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_dmtc1, rt, fs).encode());
}

BufferOffset
AssemblerMIPSShared::as_dmfc1(Register rt, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_dmfc1, rt, fs).encode());
}

// FP convert instructions
BufferOffset
AssemblerMIPSShared::as_ceilws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_ceil_w_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_floorws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_floor_w_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_roundws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_round_w_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_truncws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_trunc_w_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_ceilwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_ceil_w_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_floorwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_floor_w_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_roundwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_round_w_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_truncwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_trunc_w_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cvtdl(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_l, zero, fs, fd, ff_cvt_d_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cvtds(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_cvt_d_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cvtdw(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_w, zero, fs, fd, ff_cvt_d_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cvtsd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_cvt_s_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cvtsw(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_w, zero, fs, fd, ff_cvt_s_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cvtwd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_cvt_w_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cvtws(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_cvt_w_fmt).encode());
}

// FP arithmetic instructions
BufferOffset
AssemblerMIPSShared::as_adds(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_add_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_addd(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_add_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_subs(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_sub_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_subd(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_sub_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_abss(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_abs_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_absd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_abs_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_negs(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_neg_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_negd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_neg_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_muls(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_mul_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_muld(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_mul_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_divs(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_s, ft, fs, fd, ff_div_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_divd(FloatRegister fd, FloatRegister fs, FloatRegister ft)
{
    return writeInst(InstReg(op_cop1, rs_d, ft, fs, fd, ff_div_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_sqrts(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_s, zero, fs, fd, ff_sqrt_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_sqrtd(FloatRegister fd, FloatRegister fs)
{
    return writeInst(InstReg(op_cop1, rs_d, zero, fs, fd, ff_sqrt_fmt).encode());
}

// FP compare instructions
BufferOffset
AssemblerMIPSShared::as_cf(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_f_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cun(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_un_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_ceq(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_eq_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cueq(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_ueq_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_colt(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_olt_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cult(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_ult_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cole(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_ole_fmt).encode());
}

BufferOffset
AssemblerMIPSShared::as_cule(FloatFormat fmt, FloatRegister fs, FloatRegister ft, FPConditionBit fcc)
{
    RSField rs = fmt == DoubleFloat ? rs_d : rs_s;
    return writeInst(InstReg(op_cop1, rs, ft, fs, fcc << FccShift, ff_c_ule_fmt).encode());
}


void
AssemblerMIPSShared::bind(Label* label, BufferOffset boff)
{
    // If our caller didn't give us an explicit target to bind to
    // then we want to bind to the location of the next instruction
    BufferOffset dest = boff.assigned() ? boff : nextOffset();
    if (label->used()) {
        int32_t next;

        // A used label holds a link to branch that uses it.
        BufferOffset b(label);
        do {
            // Even a 0 offset may be invalid if we're out of memory.
            if (oom())
                return;

            Instruction* inst = editSrc(b);

            // Second word holds a pointer to the next branch in label's chain.
            next = inst[1].encode();
            bind(reinterpret_cast<InstImm*>(inst), b.getOffset(), dest.getOffset());

            b = BufferOffset(next);
        } while (next != LabelBase::INVALID_OFFSET);
    }
    label->bind(dest.getOffset());
}

void
AssemblerMIPSShared::retarget(Label* label, Label* target)
{
    if (label->used() && !oom()) {
        if (target->bound()) {
            bind(label, BufferOffset(target));
        } else if (target->used()) {
            // The target is not bound but used. Prepend label's branch list
            // onto target's.
            int32_t next;
            BufferOffset labelBranchOffset(label);

            // Find the head of the use chain for label.
            do {
                Instruction* inst = editSrc(labelBranchOffset);

                // Second word holds a pointer to the next branch in chain.
                next = inst[1].encode();
                labelBranchOffset = BufferOffset(next);
            } while (next != LabelBase::INVALID_OFFSET);

            // Then patch the head of label's use chain to the tail of
            // target's use chain, prepending the entire use chain of target.
            Instruction* inst = editSrc(labelBranchOffset);
            int32_t prev = target->use(label->offset());
            inst[1].setData(prev);
        } else {
            // The target is unbound and unused.  We can just take the head of
            // the list hanging off of label, and dump that into target.
            DebugOnly<uint32_t> prev = target->use(label->offset());
            MOZ_ASSERT((int32_t)prev == Label::INVALID_OFFSET);
        }
    }
    label->reset();
}

void
AssemblerMIPSShared::retargetWithOffset(size_t baseOffset, const LabelBase* label, Label* target)
{
    if (!label->used())
        return;

    MOZ_ASSERT(!target->bound());
    int32_t next;
    BufferOffset labelBranchOffset(label->offset() + baseOffset);
    do {
        Instruction* inst = editSrc(labelBranchOffset);
        int32_t prev = target->use(labelBranchOffset.getOffset());

        MOZ_RELEASE_ASSERT(prev == Label::INVALID_OFFSET || unsigned(prev) < size());

        next = inst[1].encode();
        inst[1].setData(prev);

        labelBranchOffset = BufferOffset(next + baseOffset);
    } while (next != LabelBase::INVALID_OFFSET);
}

void dbg_break() {}
void
AssemblerMIPSShared::as_break(uint32_t code)
{
    MOZ_ASSERT(code <= MAX_BREAK_CODE);
    writeInst(op_special | code << FunctionBits | ff_break);
}

void
AssemblerMIPSShared::as_sync(uint32_t stype)
{
    MOZ_ASSERT(stype <= 31);
    writeInst(InstReg(op_special, zero, zero, zero, stype, ff_sync).encode());
}

// This just stomps over memory with 32 bits of raw data. Its purpose is to
// overwrite the call of JITed code with 32 bits worth of an offset. This will
// is only meant to function on code that has been invalidated, so it should
// be totally safe. Since that instruction will never be executed again, a
// ICache flush should not be necessary
void
AssemblerMIPSShared::PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm)
{
    // Raw is going to be the return address.
    uint32_t* raw = (uint32_t*)label.raw();
    // Overwrite the 4 bytes before the return address, which will
    // end up being the call instruction.
    *(raw - 1) = imm.value;
}

uint8_t*
AssemblerMIPSShared::NextInstruction(uint8_t* inst_, uint32_t* count)
{
    Instruction* inst = reinterpret_cast<Instruction*>(inst_);
    if (count != nullptr)
        *count += sizeof(Instruction);
    return reinterpret_cast<uint8_t*>(inst->next());
}

// Since there are no pools in MIPS implementation, this should be simple.
Instruction*
Instruction::next()
{
    return this + 1;
}

InstImm AssemblerMIPSShared::invertBranch(InstImm branch, BOffImm16 skipOffset)
{
    uint32_t rt = 0;
    Opcode op = (Opcode) (branch.extractOpcode() << OpcodeShift);
    switch(op) {
      case op_beq:
        branch.setBOffImm16(skipOffset);
        branch.setOpcode(op_bne);
        return branch;
      case op_bne:
        branch.setBOffImm16(skipOffset);
        branch.setOpcode(op_beq);
        return branch;
      case op_bgtz:
        branch.setBOffImm16(skipOffset);
        branch.setOpcode(op_blez);
        return branch;
      case op_blez:
        branch.setBOffImm16(skipOffset);
        branch.setOpcode(op_bgtz);
        return branch;
      case op_regimm:
        branch.setBOffImm16(skipOffset);
        rt = branch.extractRT();
        if (rt == (rt_bltz >> RTShift)) {
            branch.setRT(rt_bgez);
            return branch;
        }
        if (rt == (rt_bgez >> RTShift)) {
            branch.setRT(rt_bltz);
            return branch;
        }

        MOZ_CRASH("Error creating long branch.");

      case op_cop1:
        MOZ_ASSERT(branch.extractRS() == rs_bc1 >> RSShift);

        branch.setBOffImm16(skipOffset);
        rt = branch.extractRT();
        if (rt & 0x1)
            branch.setRT((RTField) ((rt & ~0x1) << RTShift));
        else
            branch.setRT((RTField) ((rt | 0x1) << RTShift));
        return branch;
      default:
        MOZ_CRASH("Error creating long branch.");
    }
}

void
AssemblerMIPSShared::ToggleToJmp(CodeLocationLabel inst_)
{
    InstImm * inst = (InstImm*)inst_.raw();

    MOZ_ASSERT(inst->extractOpcode() == ((uint32_t)op_andi >> OpcodeShift));
    // We converted beq to andi, so now we restore it.
    inst->setOpcode(op_beq);

    AutoFlushICache::flush(uintptr_t(inst), 4);
}

void
AssemblerMIPSShared::ToggleToCmp(CodeLocationLabel inst_)
{
    InstImm * inst = (InstImm*)inst_.raw();

    // toggledJump is allways used for short jumps.
    MOZ_ASSERT(inst->extractOpcode() == ((uint32_t)op_beq >> OpcodeShift));
    // Replace "beq $zero, $zero, offset" with "andi $zero, $zero, offset"
    inst->setOpcode(op_andi);

    AutoFlushICache::flush(uintptr_t(inst), 4);
}


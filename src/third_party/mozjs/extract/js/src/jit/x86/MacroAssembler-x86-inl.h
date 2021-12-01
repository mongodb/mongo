/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_MacroAssembler_x86_inl_h
#define jit_x86_MacroAssembler_x86_inl_h

#include "jit/x86/MacroAssembler-x86.h"

#include "jit/x86-shared/MacroAssembler-x86-shared-inl.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style

void
MacroAssembler::move64(Imm64 imm, Register64 dest)
{
    movl(Imm32(imm.value & 0xFFFFFFFFL), dest.low);
    movl(Imm32((imm.value >> 32) & 0xFFFFFFFFL), dest.high);
}

void
MacroAssembler::move64(Register64 src, Register64 dest)
{
    movl(src.low, dest.low);
    movl(src.high, dest.high);
}

void
MacroAssembler::moveDoubleToGPR64(FloatRegister src, Register64 dest)
{
    ScratchDoubleScope scratch(*this);

    if (Assembler::HasSSE41()) {
        vmovd(src, dest.low);
        vpextrd(1, src, dest.high);
    } else {
        vmovd(src, dest.low);
        moveDouble(src, scratch);
        vpsrldq(Imm32(4), scratch, scratch);
        vmovd(scratch, dest.high);
    }
}

void
MacroAssembler::moveGPR64ToDouble(Register64 src, FloatRegister dest)
{
    ScratchDoubleScope scratch(*this);

    if (Assembler::HasSSE41()) {
        vmovd(src.low, dest);
        vpinsrd(1, src.high, dest, dest);
    } else {
        vmovd(src.low, dest);
        vmovd(src.high, ScratchDoubleReg);
        vunpcklps(ScratchDoubleReg, dest, dest);
    }
}

void
MacroAssembler::move64To32(Register64 src, Register dest)
{
    if (src.low != dest)
        movl(src.low, dest);
}

void
MacroAssembler::move32To64ZeroExtend(Register src, Register64 dest)
{
    if (src != dest.low)
        movl(src, dest.low);
    movl(Imm32(0), dest.high);
}

void
MacroAssembler::move8To64SignExtend(Register src, Register64 dest)
{
    MOZ_ASSERT(dest.low == eax);
    MOZ_ASSERT(dest.high == edx);
    move8SignExtend(src, eax);
    masm.cdq();
}

void
MacroAssembler::move16To64SignExtend(Register src, Register64 dest)
{
    MOZ_ASSERT(dest.low == eax);
    MOZ_ASSERT(dest.high == edx);
    move16SignExtend(src, eax);
    masm.cdq();
}

void
MacroAssembler::move32To64SignExtend(Register src, Register64 dest)
{
    MOZ_ASSERT(dest.low == eax);
    MOZ_ASSERT(dest.high == edx);
    if (src != eax)
        movl(src, eax);
    masm.cdq();
}

// ===============================================================
// Logical functions

void
MacroAssembler::andPtr(Register src, Register dest)
{
    andl(src, dest);
}

void
MacroAssembler::andPtr(Imm32 imm, Register dest)
{
    andl(imm, dest);
}

void
MacroAssembler::and64(Imm64 imm, Register64 dest)
{
    if (imm.low().value != int32_t(0xFFFFFFFF))
        andl(imm.low(), dest.low);
    if (imm.hi().value != int32_t(0xFFFFFFFF))
        andl(imm.hi(), dest.high);
}

void
MacroAssembler::or64(Imm64 imm, Register64 dest)
{
    if (imm.low().value != 0)
        orl(imm.low(), dest.low);
    if (imm.hi().value != 0)
        orl(imm.hi(), dest.high);
}

void
MacroAssembler::xor64(Imm64 imm, Register64 dest)
{
    if (imm.low().value != 0)
        xorl(imm.low(), dest.low);
    if (imm.hi().value != 0)
        xorl(imm.hi(), dest.high);
}

void
MacroAssembler::orPtr(Register src, Register dest)
{
    orl(src, dest);
}

void
MacroAssembler::orPtr(Imm32 imm, Register dest)
{
    orl(imm, dest);
}

void
MacroAssembler::and64(Register64 src, Register64 dest)
{
    andl(src.low, dest.low);
    andl(src.high, dest.high);
}

void
MacroAssembler::or64(Register64 src, Register64 dest)
{
    orl(src.low, dest.low);
    orl(src.high, dest.high);
}

void
MacroAssembler::xor64(Register64 src, Register64 dest)
{
    xorl(src.low, dest.low);
    xorl(src.high, dest.high);
}

void
MacroAssembler::xorPtr(Register src, Register dest)
{
    xorl(src, dest);
}

void
MacroAssembler::xorPtr(Imm32 imm, Register dest)
{
    xorl(imm, dest);
}

// ===============================================================
// Arithmetic functions

void
MacroAssembler::addPtr(Register src, Register dest)
{
    addl(src, dest);
}

void
MacroAssembler::addPtr(Imm32 imm, Register dest)
{
    addl(imm, dest);
}

void
MacroAssembler::addPtr(ImmWord imm, Register dest)
{
    addl(Imm32(imm.value), dest);
}

void
MacroAssembler::addPtr(Imm32 imm, const Address& dest)
{
    addl(imm, Operand(dest));
}

void
MacroAssembler::addPtr(Imm32 imm, const AbsoluteAddress& dest)
{
    addl(imm, Operand(dest));
}

void
MacroAssembler::addPtr(const Address& src, Register dest)
{
    addl(Operand(src), dest);
}

void
MacroAssembler::add64(Register64 src, Register64 dest)
{
    addl(src.low, dest.low);
    adcl(src.high, dest.high);
}

void
MacroAssembler::add64(Imm32 imm, Register64 dest)
{
    addl(imm, dest.low);
    adcl(Imm32(0), dest.high);
}

void
MacroAssembler::add64(Imm64 imm, Register64 dest)
{
    if (imm.low().value == 0) {
        addl(imm.hi(), dest.high);
        return;
    }
    addl(imm.low(), dest.low);
    adcl(imm.hi(), dest.high);
}

void
MacroAssembler::addConstantDouble(double d, FloatRegister dest)
{
    Double* dbl = getDouble(d);
    if (!dbl)
        return;
    masm.vaddsd_mr(nullptr, dest.encoding(), dest.encoding());
    propagateOOM(dbl->uses.append(CodeOffset(masm.size())));
}

CodeOffset
MacroAssembler::sub32FromStackPtrWithPatch(Register dest)
{
    moveStackPtrTo(dest);
    addlWithPatch(Imm32(0), dest);
    return CodeOffset(currentOffset());
}

void
MacroAssembler::patchSub32FromStackPtr(CodeOffset offset, Imm32 imm)
{
    patchAddl(offset, -imm.value);
}

void
MacroAssembler::subPtr(Register src, Register dest)
{
    subl(src, dest);
}

void
MacroAssembler::subPtr(Register src, const Address& dest)
{
    subl(src, Operand(dest));
}

void
MacroAssembler::subPtr(Imm32 imm, Register dest)
{
    subl(imm, dest);
}

void
MacroAssembler::subPtr(const Address& addr, Register dest)
{
    subl(Operand(addr), dest);
}

void
MacroAssembler::sub64(Register64 src, Register64 dest)
{
    subl(src.low, dest.low);
    sbbl(src.high, dest.high);
}

void
MacroAssembler::sub64(Imm64 imm, Register64 dest)
{
    if (imm.low().value == 0) {
        subl(imm.hi(), dest.high);
        return;
    }
    subl(imm.low(), dest.low);
    sbbl(imm.hi(), dest.high);
}

// Note: this function clobbers eax and edx.
void
MacroAssembler::mul64(Imm64 imm, const Register64& dest)
{
    // LOW32  = LOW(LOW(dest) * LOW(imm));
    // HIGH32 = LOW(HIGH(dest) * LOW(imm)) [multiply imm into upper bits]
    //        + LOW(LOW(dest) * HIGH(imm)) [multiply dest into upper bits]
    //        + HIGH(LOW(dest) * LOW(imm)) [carry]

    MOZ_ASSERT(dest.low != eax && dest.low != edx);
    MOZ_ASSERT(dest.high != eax && dest.high != edx);

    // HIGH(dest) = LOW(HIGH(dest) * LOW(imm));
    movl(Imm32(imm.value & 0xFFFFFFFFL), edx);
    imull(edx, dest.high);

    // edx:eax = LOW(dest) * LOW(imm);
    movl(Imm32(imm.value & 0xFFFFFFFFL), edx);
    movl(dest.low, eax);
    mull(edx);

    // HIGH(dest) += edx;
    addl(edx, dest.high);

    // HIGH(dest) += LOW(LOW(dest) * HIGH(imm));
    if (((imm.value >> 32) & 0xFFFFFFFFL) == 5)
        leal(Operand(dest.low, dest.low, TimesFour), edx);
    else
        MOZ_CRASH("Unsupported imm");
    addl(edx, dest.high);

    // LOW(dest) = eax;
    movl(eax, dest.low);
}

void
MacroAssembler::mul64(Imm64 imm, const Register64& dest, const Register temp)
{
    // LOW32  = LOW(LOW(dest) * LOW(src));                                  (1)
    // HIGH32 = LOW(HIGH(dest) * LOW(src)) [multiply src into upper bits]   (2)
    //        + LOW(LOW(dest) * HIGH(src)) [multiply dest into upper bits]  (3)
    //        + HIGH(LOW(dest) * LOW(src)) [carry]                          (4)

    MOZ_ASSERT(dest == Register64(edx, eax));
    MOZ_ASSERT(temp != edx && temp != eax);

    movl(dest.low, temp);

    // Compute mul64
    imull(imm.low(), dest.high); // (2)
    imull(imm.hi(), temp); // (3)
    addl(dest.high, temp);
    movl(imm.low(), dest.high);
    mull(dest.high/*, dest.low*/); // (4) + (1) output in edx:eax (dest_hi:dest_lo)
    addl(temp, dest.high);
}

void
MacroAssembler::mul64(const Register64& src, const Register64& dest, const Register temp)
{
    // LOW32  = LOW(LOW(dest) * LOW(src));                                  (1)
    // HIGH32 = LOW(HIGH(dest) * LOW(src)) [multiply src into upper bits]   (2)
    //        + LOW(LOW(dest) * HIGH(src)) [multiply dest into upper bits]  (3)
    //        + HIGH(LOW(dest) * LOW(src)) [carry]                          (4)

    MOZ_ASSERT(dest == Register64(edx, eax));
    MOZ_ASSERT(src != Register64(edx, eax) && src != Register64(eax, edx));

    // Make sure the rhs.high isn't the dest.high register anymore.
    // This saves us from doing other register moves.
    movl(dest.low, temp);

    // Compute mul64
    imull(src.low, dest.high); // (2)
    imull(src.high, temp); // (3)
    addl(dest.high, temp);
    movl(src.low, dest.high);
    mull(dest.high/*, dest.low*/); // (4) + (1) output in edx:eax (dest_hi:dest_lo)
    addl(temp, dest.high);
}

void
MacroAssembler::mulBy3(Register src, Register dest)
{
    lea(Operand(src, src, TimesTwo), dest);
}

void
MacroAssembler::mulDoublePtr(ImmPtr imm, Register temp, FloatRegister dest)
{
    movl(imm, temp);
    vmulsd(Operand(temp, 0), dest, dest);
}

void
MacroAssembler::inc64(AbsoluteAddress dest)
{
    addl(Imm32(1), Operand(dest));
    Label noOverflow;
    j(NonZero, &noOverflow);
    addl(Imm32(1), Operand(dest.offset(4)));
    bind(&noOverflow);
}

void
MacroAssembler::neg64(Register64 reg)
{
    negl(reg.low);
    adcl(Imm32(0), reg.high);
    negl(reg.high);
}

// ===============================================================
// Shift functions

void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 32);
    shll(imm, dest);
}

void
MacroAssembler::lshift64(Imm32 imm, Register64 dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    if (imm.value < 32) {
        shldl(imm, dest.low, dest.high);
        shll(imm, dest.low);
        return;
    }

    mov(dest.low, dest.high);
    shll(Imm32(imm.value & 0x1f), dest.high);
    xorl(dest.low, dest.low);
}

void
MacroAssembler::lshift64(Register shift, Register64 srcDest)
{
    MOZ_ASSERT(shift == ecx);
    MOZ_ASSERT(srcDest.low != ecx && srcDest.high != ecx);

    Label done;

    shldl_cl(srcDest.low, srcDest.high);
    shll_cl(srcDest.low);

    testl(Imm32(0x20), ecx);
    j(Condition::Equal, &done);

    // 32 - 63 bit shift
    movl(srcDest.low, srcDest.high);
    xorl(srcDest.low, srcDest.low);

    bind(&done);
}

void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 32);
    shrl(imm, dest);
}

void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    if (imm.value < 32) {
        shrdl(imm, dest.high, dest.low);
        shrl(imm, dest.high);
        return;
    }

    movl(dest.high, dest.low);
    shrl(Imm32(imm.value & 0x1f), dest.low);
    xorl(dest.high, dest.high);
}

void
MacroAssembler::rshift64(Register shift, Register64 srcDest)
{
    MOZ_ASSERT(shift == ecx);
    MOZ_ASSERT(srcDest.low != ecx && srcDest.high != ecx);

    Label done;

    shrdl_cl(srcDest.high, srcDest.low);
    shrl_cl(srcDest.high);

    testl(Imm32(0x20), ecx);
    j(Condition::Equal, &done);

    // 32 - 63 bit shift
    movl(srcDest.high, srcDest.low);
    xorl(srcDest.high, srcDest.high);

    bind(&done);
}

void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 32);
    sarl(imm, dest);
}

void
MacroAssembler::rshift64Arithmetic(Imm32 imm, Register64 dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    if (imm.value < 32) {
        shrdl(imm, dest.high, dest.low);
        sarl(imm, dest.high);
        return;
    }

    movl(dest.high, dest.low);
    sarl(Imm32(imm.value & 0x1f), dest.low);
    sarl(Imm32(0x1f), dest.high);
}

void
MacroAssembler::rshift64Arithmetic(Register shift, Register64 srcDest)
{
    MOZ_ASSERT(shift == ecx);
    MOZ_ASSERT(srcDest.low != ecx && srcDest.high != ecx);

    Label done;

    shrdl_cl(srcDest.high, srcDest.low);
    sarl_cl(srcDest.high);

    testl(Imm32(0x20), ecx);
    j(Condition::Equal, &done);

    // 32 - 63 bit shift
    movl(srcDest.high, srcDest.low);
    sarl(Imm32(0x1f), srcDest.high);

    bind(&done);
}

// ===============================================================
// Rotation functions

void
MacroAssembler::rotateLeft64(Register count, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(src == dest, "defineReuseInput");
    MOZ_ASSERT(count == ecx, "defineFixed(ecx)");

    Label done;

    movl(dest.high, temp);
    shldl_cl(dest.low, dest.high);
    shldl_cl(temp, dest.low);

    testl(Imm32(0x20), count);
    j(Condition::Equal, &done);
    xchgl(dest.high, dest.low);

    bind(&done);
}

void
MacroAssembler::rotateRight64(Register count, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(src == dest, "defineReuseInput");
    MOZ_ASSERT(count == ecx, "defineFixed(ecx)");

    Label done;

    movl(dest.high, temp);
    shrdl_cl(dest.low, dest.high);
    shrdl_cl(temp, dest.low);

    testl(Imm32(0x20), count);
    j(Condition::Equal, &done);
    xchgl(dest.high, dest.low);

    bind(&done);
}

void
MacroAssembler::rotateLeft64(Imm32 count, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(src == dest, "defineReuseInput");

    int32_t amount = count.value & 0x3f;
    if ((amount & 0x1f) != 0) {
        movl(dest.high, temp);
        shldl(Imm32(amount & 0x1f), dest.low, dest.high);
        shldl(Imm32(amount & 0x1f), temp, dest.low);
    }

    if (!!(amount & 0x20))
        xchgl(dest.high, dest.low);
}

void
MacroAssembler::rotateRight64(Imm32 count, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(src == dest, "defineReuseInput");

    int32_t amount = count.value & 0x3f;
    if ((amount & 0x1f) != 0) {
        movl(dest.high, temp);
        shrdl(Imm32(amount & 0x1f), dest.low, dest.high);
        shrdl(Imm32(amount & 0x1f), temp, dest.low);
    }

    if (!!(amount & 0x20))
        xchgl(dest.high, dest.low);
}

// ===============================================================
// Bit counting functions

void
MacroAssembler::clz64(Register64 src, Register dest)
{
    Label nonzero, zero;

    bsrl(src.high, dest);
    j(Assembler::Zero, &zero);
    orl(Imm32(32), dest);
    jump(&nonzero);

    bind(&zero);
    bsrl(src.low, dest);
    j(Assembler::NonZero, &nonzero);
    movl(Imm32(0x7F), dest);

    bind(&nonzero);
    xorl(Imm32(0x3F), dest);
}

void
MacroAssembler::ctz64(Register64 src, Register dest)
{
    Label done, nonzero;

    bsfl(src.low, dest);
    j(Assembler::NonZero, &done);
    bsfl(src.high, dest);
    j(Assembler::NonZero, &nonzero);
    movl(Imm32(64), dest);
    jump(&done);

    bind(&nonzero);
    orl(Imm32(32), dest);

    bind(&done);
}

void
MacroAssembler::popcnt64(Register64 src, Register64 dest, Register tmp)
{
    // The tmp register is only needed if there is no native POPCNT.

    MOZ_ASSERT(src.low != tmp && src.high != tmp);
    MOZ_ASSERT(dest.low != tmp && dest.high != tmp);

    if (dest.low != src.high) {
        popcnt32(src.low, dest.low, tmp);
        popcnt32(src.high, dest.high, tmp);
    } else {
        MOZ_ASSERT(dest.high != src.high);
        popcnt32(src.low, dest.high, tmp);
        popcnt32(src.high, dest.low, tmp);
    }
    addl(dest.high, dest.low);
    xorl(dest.high, dest.high);
}

// ===============================================================
// Condition functions

template <typename T1, typename T2>
void
MacroAssembler::cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest)
{
    cmpPtr(lhs, rhs);
    emitSet(cond, dest);
}

// ===============================================================
// Branch functions

void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    cmp32(Operand(lhs), rhs);
    j(cond, label);
}

void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    cmp32(Operand(lhs), rhs);
    j(cond, label);
}

void
MacroAssembler::branch32(Condition cond, wasm::SymbolicAddress lhs, Imm32 rhs, Label* label)
{
    cmpl(rhs, lhs);
    j(cond, label);
}

void
MacroAssembler::branch64(Condition cond, Register64 lhs, Imm64 val, Label* success, Label* fail)
{
    bool fallthrough = false;
    Label fallthroughLabel;

    if (!fail) {
        fail = &fallthroughLabel;
        fallthrough = true;
    }

    switch(cond) {
      case Assembler::Equal:
        branch32(Assembler::NotEqual, lhs.low, val.low(), fail);
        branch32(Assembler::Equal, lhs.high, val.hi(), success);
        if (!fallthrough)
            jump(fail);
        break;
      case Assembler::NotEqual:
        branch32(Assembler::NotEqual, lhs.low, val.low(), success);
        branch32(Assembler::NotEqual, lhs.high, val.hi(), success);
        if (!fallthrough)
            jump(fail);
        break;
      case Assembler::LessThan:
      case Assembler::LessThanOrEqual:
      case Assembler::GreaterThan:
      case Assembler::GreaterThanOrEqual:
      case Assembler::Below:
      case Assembler::BelowOrEqual:
      case Assembler::Above:
      case Assembler::AboveOrEqual: {
        Assembler::Condition cond1 = Assembler::ConditionWithoutEqual(cond);
        Assembler::Condition cond2 =
            Assembler::ConditionWithoutEqual(Assembler::InvertCondition(cond));
        Assembler::Condition cond3 = Assembler::UnsignedCondition(cond);

        cmp32(lhs.high, val.hi());
        j(cond1, success);
        j(cond2, fail);
        cmp32(lhs.low, val.low());
        j(cond3, success);
        if (!fallthrough)
            jump(fail);
        break;
      }
      default:
        MOZ_CRASH("Condition code not supported");
        break;
    }

    if (fallthrough)
        bind(fail);
}

void
MacroAssembler::branch64(Condition cond, Register64 lhs, Register64 rhs, Label* success, Label* fail)
{
    bool fallthrough = false;
    Label fallthroughLabel;

    if (!fail) {
        fail = &fallthroughLabel;
        fallthrough = true;
    }

    switch(cond) {
      case Assembler::Equal:
        branch32(Assembler::NotEqual, lhs.low, rhs.low, fail);
        branch32(Assembler::Equal, lhs.high, rhs.high, success);
        if (!fallthrough)
            jump(fail);
        break;
      case Assembler::NotEqual:
        branch32(Assembler::NotEqual, lhs.low, rhs.low, success);
        branch32(Assembler::NotEqual, lhs.high, rhs.high, success);
        if (!fallthrough)
            jump(fail);
        break;
      case Assembler::LessThan:
      case Assembler::LessThanOrEqual:
      case Assembler::GreaterThan:
      case Assembler::GreaterThanOrEqual:
      case Assembler::Below:
      case Assembler::BelowOrEqual:
      case Assembler::Above:
      case Assembler::AboveOrEqual: {
        Assembler::Condition cond1 = Assembler::ConditionWithoutEqual(cond);
        Assembler::Condition cond2 =
            Assembler::ConditionWithoutEqual(Assembler::InvertCondition(cond));
        Assembler::Condition cond3 = Assembler::UnsignedCondition(cond);

        cmp32(lhs.high, rhs.high);
        j(cond1, success);
        j(cond2, fail);
        cmp32(lhs.low, rhs.low);
        j(cond3, success);
        if (!fallthrough)
            jump(fail);
        break;
      }
      default:
        MOZ_CRASH("Condition code not supported");
        break;
    }

    if (fallthrough)
        bind(fail);
}

void
MacroAssembler::branch64(Condition cond, const Address& lhs, Imm64 val, Label* label)
{
    MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
               "other condition codes not supported");

    Label done;

    if (cond == Assembler::Equal)
        branch32(Assembler::NotEqual, lhs, val.firstHalf(), &done);
    else
        branch32(Assembler::NotEqual, lhs, val.firstHalf(), label);
    branch32(cond, Address(lhs.base, lhs.offset + sizeof(uint32_t)), val.secondHalf(), label);

    bind(&done);
}

void
MacroAssembler::branch64(Condition cond, const Address& lhs, const Address& rhs, Register scratch,
                         Label* label)
{
    MOZ_ASSERT(cond == Assembler::NotEqual || cond == Assembler::Equal,
               "other condition codes not supported");
    MOZ_ASSERT(lhs.base != scratch);
    MOZ_ASSERT(rhs.base != scratch);

    Label done;

    load32(rhs, scratch);
    if (cond == Assembler::Equal)
        branch32(Assembler::NotEqual, lhs, scratch, &done);
    else
        branch32(Assembler::NotEqual, lhs, scratch, label);

    load32(Address(rhs.base, rhs.offset + sizeof(uint32_t)), scratch);
    branch32(cond, Address(lhs.base, lhs.offset + sizeof(uint32_t)), scratch, label);

    bind(&done);
}

void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    branchPtrImpl(cond, lhs, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, ImmWord rhs, Label* label)
{
    branchPtrImpl(cond, lhs, rhs, label);
}

void
MacroAssembler::branchPtr(Condition cond, wasm::SymbolicAddress lhs, Register rhs, Label* label)
{
    cmpl(rhs, lhs);
    j(cond, label);
}

void
MacroAssembler::branchPrivatePtr(Condition cond, const Address& lhs, Register rhs, Label* label)
{
    branchPtr(cond, lhs, rhs, label);
}

void
MacroAssembler::branchTruncateFloat32ToPtr(FloatRegister src, Register dest, Label* fail)
{
    branchTruncateFloat32ToInt32(src, dest, fail);
}

void
MacroAssembler::branchTruncateFloat32MaybeModUint32(FloatRegister src, Register dest, Label* fail)
{
    branchTruncateFloat32ToInt32(src, dest, fail);
}

void
MacroAssembler::branchTruncateFloat32ToInt32(FloatRegister src, Register dest, Label* fail)
{
    vcvttss2si(src, dest);

    // vcvttss2si returns 0x80000000 on failure. Test for it by
    // subtracting 1 and testing overflow (this permits the use of a
    // smaller immediate field).
    cmp32(dest, Imm32(1));
    j(Assembler::Overflow, fail);
}

void
MacroAssembler::branchTruncateDoubleToPtr(FloatRegister src, Register dest, Label* fail)
{
    branchTruncateDoubleToInt32(src, dest, fail);
}

void
MacroAssembler::branchTruncateDoubleMaybeModUint32(FloatRegister src, Register dest, Label* fail)
{
    // TODO: X64 supports supports integers up till 64bits. Here we only support 32bits,
    // before failing. Implementing this for x86 might give a x86 kraken win.
    branchTruncateDoubleToInt32(src, dest, fail);
}

void
MacroAssembler::branchTruncateDoubleToInt32(FloatRegister src, Register dest, Label* fail)
{
    vcvttsd2si(src, dest);

    // vcvttsd2si returns 0x80000000 on failure. Test for it by
    // subtracting 1 and testing overflow (this permits the use of a
    // smaller immediate field).
    cmp32(dest, Imm32(1));
    j(Assembler::Overflow, fail);
}

void
MacroAssembler::branchTest32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    test32(Operand(lhs), rhs);
    j(cond, label);
}

template <class L>
void
MacroAssembler::branchTest64(Condition cond, Register64 lhs, Register64 rhs, Register temp,
                             L label)
{
    if (cond == Assembler::Zero || cond == Assembler::NonZero) {
        MOZ_ASSERT(lhs.low == rhs.low);
        MOZ_ASSERT(lhs.high == rhs.high);
        movl(lhs.low, temp);
        orl(lhs.high, temp);
        branchTestPtr(cond, temp, temp, label);
    } else {
        MOZ_CRASH("Unsupported condition");
    }
}

void
MacroAssembler::branchTestBooleanTruthy(bool truthy, const ValueOperand& value, Label* label)
{
    test32(value.payloadReg(), value.payloadReg());
    j(truthy ? NonZero : Zero, label);
}

void
MacroAssembler::branchTestMagic(Condition cond, const Address& valaddr, JSWhyMagic why, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

    Label notMagic;
    if (cond == Assembler::Equal)
        branchTestMagic(Assembler::NotEqual, valaddr, &notMagic);
    else
        branchTestMagic(Assembler::NotEqual, valaddr, label);

    branch32(cond, ToPayload(valaddr), Imm32(why), label);
    bind(&notMagic);
}

void
MacroAssembler::branchToComputedAddress(const BaseIndex& addr)
{
    jmp(Operand(addr));
}

void
MacroAssembler::cmp32MovePtr(Condition cond, Register lhs, Imm32 rhs, Register src,
                             Register dest)
{
    cmp32(lhs, rhs);
    cmovCCl(cond, Operand(src), dest);
}

void
MacroAssembler::test32LoadPtr(Condition cond, const Address& addr, Imm32 mask, const Address& src,
                              Register dest)
{
    MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);
    test32(addr, mask);
    cmovCCl(cond, Operand(src), dest);
}

void
MacroAssembler::test32MovePtr(Condition cond, const Address& addr, Imm32 mask, Register src,
                              Register dest)
{
    MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);
    test32(addr, mask);
    cmovCCl(cond, Operand(src), dest);
}

void
MacroAssembler::spectreMovePtr(Condition cond, Register src, Register dest)
{
    cmovCCl(cond, Operand(src), dest);
}

void
MacroAssembler::spectreBoundsCheck32(Register index, const Operand& length, Register maybeScratch,
                                     Label* failure)
{
    Label failurePopValue;
    bool pushedValue = false;
    if (JitOptions.spectreIndexMasking) {
        if (maybeScratch == InvalidReg) {
            push(Imm32(0));
            pushedValue = true;
        } else {
            move32(Imm32(0), maybeScratch);
        }
    }

    cmp32(index, length);
    j(Assembler::AboveOrEqual, pushedValue ? &failurePopValue : failure);

    if (JitOptions.spectreIndexMasking) {
        if (maybeScratch == InvalidReg) {
            Label done;
            cmovCCl(Assembler::AboveOrEqual, Operand(StackPointer, 0), index);
            lea(Operand(StackPointer, sizeof(void*)), StackPointer);
            jump(&done);

            bind(&failurePopValue);
            lea(Operand(StackPointer, sizeof(void*)), StackPointer);
            jump(failure);

            bind(&done);
        } else {
            cmovCCl(Assembler::AboveOrEqual, maybeScratch, index);
        }
    }
}

void
MacroAssembler::spectreBoundsCheck32(Register index, Register length, Register maybeScratch,
                                     Label* failure)
{
    MOZ_ASSERT(length != maybeScratch);
    MOZ_ASSERT(index != maybeScratch);

    spectreBoundsCheck32(index, Operand(length), maybeScratch, failure);
}

void
MacroAssembler::spectreBoundsCheck32(Register index, const Address& length, Register maybeScratch,
                                     Label* failure)
{
    MOZ_ASSERT(index != length.base);
    MOZ_ASSERT(length.base != maybeScratch);
    MOZ_ASSERT(index != maybeScratch);

    spectreBoundsCheck32(index, Operand(length), maybeScratch, failure);
}

// ========================================================================
// Truncate floating point.

void
MacroAssembler::truncateFloat32ToUInt64(Address src, Address dest, Register temp,
                                        FloatRegister floatTemp)
{
    Label done;

    loadFloat32(src, floatTemp);

    truncateFloat32ToInt64(src, dest, temp);

    // For unsigned conversion the case of [INT64, UINT64] needs to get handle seperately.
    load32(HighWord(dest), temp);
    branch32(Assembler::Condition::NotSigned, temp, Imm32(0), &done);

    // Move the value inside INT64 range.
    storeFloat32(floatTemp, dest);
    loadConstantFloat32(double(int64_t(0x8000000000000000)), floatTemp);
    vaddss(Operand(dest), floatTemp, floatTemp);
    storeFloat32(floatTemp, dest);
    truncateFloat32ToInt64(dest, dest, temp);

    load32(HighWord(dest), temp);
    orl(Imm32(0x80000000), temp);
    store32(temp, HighWord(dest));

    bind(&done);
}

void
MacroAssembler::truncateDoubleToUInt64(Address src, Address dest, Register temp,
                                       FloatRegister floatTemp)
{
    Label done;

    loadDouble(src, floatTemp);

    truncateDoubleToInt64(src, dest, temp);

    // For unsigned conversion the case of [INT64, UINT64] needs to get handle seperately.
    load32(HighWord(dest), temp);
    branch32(Assembler::Condition::NotSigned, temp, Imm32(0), &done);

    // Move the value inside INT64 range.
    storeDouble(floatTemp, dest);
    loadConstantDouble(double(int64_t(0x8000000000000000)), floatTemp);
    vaddsd(Operand(dest), floatTemp, floatTemp);
    storeDouble(floatTemp, dest);
    truncateDoubleToInt64(dest, dest, temp);

    load32(HighWord(dest), temp);
    orl(Imm32(0x80000000), temp);
    store32(temp, HighWord(dest));

    bind(&done);
}

// ========================================================================
// wasm support

template <class L>
void
MacroAssembler::wasmBoundsCheck(Condition cond, Register index, Register boundsCheckLimit, L label)
{
    cmp32(index, boundsCheckLimit);
    j(cond, label);
    if (JitOptions.spectreIndexMasking)
        cmovCCl(cond, Operand(boundsCheckLimit), index);
}

template <class L>
void
MacroAssembler::wasmBoundsCheck(Condition cond, Register index, Address boundsCheckLimit, L label)
{
    cmp32(index, Operand(boundsCheckLimit));
    j(cond, label);
    if (JitOptions.spectreIndexMasking)
        cmovCCl(cond, Operand(boundsCheckLimit), index);
}

//}}} check_macroassembler_style
// ===============================================================

// Note: this function clobbers the source register.
void
MacroAssemblerX86::convertUInt32ToDouble(Register src, FloatRegister dest)
{
    // src is [0, 2^32-1]
    subl(Imm32(0x80000000), src);

    // Now src is [-2^31, 2^31-1] - int range, but not the same value.
    convertInt32ToDouble(src, dest);

    // dest is now a double with the int range.
    // correct the double value by adding 0x80000000.
    asMasm().addConstantDouble(2147483648.0, dest);
}

// Note: this function clobbers the source register.
void
MacroAssemblerX86::convertUInt32ToFloat32(Register src, FloatRegister dest)
{
    convertUInt32ToDouble(src, dest);
    convertDoubleToFloat32(dest, dest);
}

void
MacroAssemblerX86::unboxValue(const ValueOperand& src, AnyRegister dest, JSValueType)
{
    if (dest.isFloat()) {
        Label notInt32, end;
        asMasm().branchTestInt32(Assembler::NotEqual, src, &notInt32);
        convertInt32ToDouble(src.payloadReg(), dest.fpu());
        jump(&end);
        bind(&notInt32);
        unboxDouble(src, dest.fpu());
        bind(&end);
    } else {
        if (src.payloadReg() != dest.gpr())
            movl(src.payloadReg(), dest.gpr());
    }
}

template <typename T>
void
MacroAssemblerX86::loadInt32OrDouble(const T& src, FloatRegister dest)
{
    Label notInt32, end;
    asMasm().branchTestInt32(Assembler::NotEqual, src, &notInt32);
    convertInt32ToDouble(ToPayload(src), dest);
    jump(&end);
    bind(&notInt32);
    loadDouble(src, dest);
    bind(&end);
}

template <typename T>
void
MacroAssemblerX86::loadUnboxedValue(const T& src, MIRType type, AnyRegister dest)
{
    if (dest.isFloat())
        loadInt32OrDouble(src, dest.fpu());
    else
        movl(Operand(src), dest.gpr());
}

// If source is a double, load it into dest. If source is int32,
// convert it to double. Else, branch to failure.
void
MacroAssemblerX86::ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure)
{
    Label isDouble, done;
    asMasm().branchTestDouble(Assembler::Equal, source.typeReg(), &isDouble);
    asMasm().branchTestInt32(Assembler::NotEqual, source.typeReg(), failure);

    convertInt32ToDouble(source.payloadReg(), dest);
    jump(&done);

    bind(&isDouble);
    unboxDouble(source, dest);

    bind(&done);
}

} // namespace jit
} // namespace js

#endif /* jit_x86_MacroAssembler_x86_inl_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_MacroAssembler_mips_shared_h
#define jit_mips_shared_MacroAssembler_mips_shared_h

#if defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/Assembler-mips32.h"
#endif

namespace js {
namespace jit {

enum LoadStoreSize
{
    SizeByte = 8,
    SizeHalfWord = 16,
    SizeWord = 32,
    SizeDouble = 64
};

enum LoadStoreExtension
{
    ZeroExtend = 0,
    SignExtend = 1
};

enum JumpKind
{
    LongJump = 0,
    ShortJump = 1
};

enum DelaySlotFill
{
    DontFillDelaySlot = 0,
    FillDelaySlot = 1
};

static Register CallReg = t9;

class MacroAssemblerMIPSShared : public Assembler
{
  protected:
    // Perform a downcast. Should be removed by Bug 996602.
    MacroAssembler& asMasm();
    const MacroAssembler& asMasm() const;

    Condition ma_cmp(Register rd, Register lhs, Register rhs, Condition c);

    void compareFloatingPoint(FloatFormat fmt, FloatRegister lhs, FloatRegister rhs,
                              DoubleCondition c, FloatTestKind* testKind,
                              FPConditionBit fcc = FCC0);

  public:
    void ma_move(Register rd, Register rs);

    void ma_li(Register dest, ImmGCPtr ptr);

    void ma_li(Register dest, Imm32 imm);

    // Shift operations
    void ma_sll(Register rd, Register rt, Imm32 shift);
    void ma_srl(Register rd, Register rt, Imm32 shift);
    void ma_sra(Register rd, Register rt, Imm32 shift);
    void ma_ror(Register rd, Register rt, Imm32 shift);
    void ma_rol(Register rd, Register rt, Imm32 shift);

    void ma_sll(Register rd, Register rt, Register shift);
    void ma_srl(Register rd, Register rt, Register shift);
    void ma_sra(Register rd, Register rt, Register shift);
    void ma_ror(Register rd, Register rt, Register shift);
    void ma_rol(Register rd, Register rt, Register shift);

    // Negate
    void ma_negu(Register rd, Register rs);

    void ma_not(Register rd, Register rs);

    // and
    void ma_and(Register rd, Register rs);
    void ma_and(Register rd, Imm32 imm);
    void ma_and(Register rd, Register rs, Imm32 imm);

    // or
    void ma_or(Register rd, Register rs);
    void ma_or(Register rd, Imm32 imm);
    void ma_or(Register rd, Register rs, Imm32 imm);

    // xor
    void ma_xor(Register rd, Register rs);
    void ma_xor(Register rd, Imm32 imm);
    void ma_xor(Register rd, Register rs, Imm32 imm);

    // load
    void ma_load(Register dest, const BaseIndex& src, LoadStoreSize size = SizeWord,
                 LoadStoreExtension extension = SignExtend);

    // store
    void ma_store(Register data, const BaseIndex& dest, LoadStoreSize size = SizeWord,
                  LoadStoreExtension extension = SignExtend);
    void ma_store(Imm32 imm, const BaseIndex& dest, LoadStoreSize size = SizeWord,
                  LoadStoreExtension extension = SignExtend);

    // arithmetic based ops
    // add
    void ma_addu(Register rd, Register rs, Imm32 imm);
    void ma_addu(Register rd, Register rs);
    void ma_addu(Register rd, Imm32 imm);

    // subtract
    void ma_subu(Register rd, Register rs, Imm32 imm);
    void ma_subu(Register rd, Register rs);
    void ma_subu(Register rd, Imm32 imm);
    void ma_subTestOverflow(Register rd, Register rs, Imm32 imm, Label* overflow);

    // multiplies.  For now, there are only few that we care about.
    void ma_mult(Register rs, Imm32 imm);
    void ma_mul_branch_overflow(Register rd, Register rs, Register rt, Label* overflow);
    void ma_mul_branch_overflow(Register rd, Register rs, Imm32 imm, Label* overflow);

    // divisions
    void ma_div_branch_overflow(Register rd, Register rs, Register rt, Label* overflow);
    void ma_div_branch_overflow(Register rd, Register rs, Imm32 imm, Label* overflow);

    // fast mod, uses scratch registers, and thus needs to be in the assembler
    // implicitly assumes that we can overwrite dest at the beginning of the sequence
    void ma_mod_mask(Register src, Register dest, Register hold, Register remain,
                     int32_t shift, Label* negZero = nullptr);

    // branches when done from within mips-specific code
    void ma_b(Register lhs, Register rhs, Label* l, Condition c, JumpKind jumpKind = LongJump);
    void ma_b(Register lhs, Imm32 imm, Label* l, Condition c, JumpKind jumpKind = LongJump);
    void ma_b(Register lhs, ImmPtr imm, Label* l, Condition c, JumpKind jumpKind = LongJump);
    void ma_b(Register lhs, ImmGCPtr imm, Label* l, Condition c, JumpKind jumpKind = LongJump) {
        MOZ_ASSERT(lhs != ScratchRegister);
        ma_li(ScratchRegister, imm);
        ma_b(lhs, ScratchRegister, l, c, jumpKind);
    }

    void ma_b(Label* l, JumpKind jumpKind = LongJump);

    // fp instructions
    void ma_lis(FloatRegister dest, float value);
    void ma_liNegZero(FloatRegister dest);

    void ma_sd(FloatRegister fd, BaseIndex address);
    void ma_ss(FloatRegister fd, BaseIndex address);

    //FP branches
    void ma_bc1s(FloatRegister lhs, FloatRegister rhs, Label* label, DoubleCondition c,
                 JumpKind jumpKind = LongJump, FPConditionBit fcc = FCC0);
    void ma_bc1d(FloatRegister lhs, FloatRegister rhs, Label* label, DoubleCondition c,
                 JumpKind jumpKind = LongJump, FPConditionBit fcc = FCC0);

    void ma_call(ImmPtr dest);

    void ma_jump(ImmPtr dest);

    void ma_cmp_set(Register dst, Register lhs, Register rhs, Condition c);
    void ma_cmp_set(Register dst, Register lhs, Imm32 imm, Condition c);
    void ma_cmp_set_double(Register dst, FloatRegister lhs, FloatRegister rhs, DoubleCondition c);
    void ma_cmp_set_float32(Register dst, FloatRegister lhs, FloatRegister rhs, DoubleCondition c);

    void moveToDoubleLo(Register src, FloatRegister dest) {
        as_mtc1(src, dest);
    }
    void moveFromDoubleLo(FloatRegister src, Register dest) {
        as_mfc1(dest, src);
    }

    void moveToFloat32(Register src, FloatRegister dest) {
        as_mtc1(src, dest);
    }
    void moveFromFloat32(FloatRegister src, Register dest) {
        as_mfc1(dest, src);
    }

  private:
    void atomicEffectOpMIPSr2(int nbytes, AtomicOp op, const Register& value, const Register& addr,
                              Register flagTemp, Register valueTemp, Register offsetTemp, Register maskTemp);
    void atomicFetchOpMIPSr2(int nbytes, bool signExtend, AtomicOp op, const Register& value, const Register& addr,
                             Register flagTemp, Register valueTemp, Register offsetTemp, Register maskTemp,
                             Register output);
    void compareExchangeMIPSr2(int nbytes, bool signExtend, const Register& addr, Register oldval,
                               Register newval, Register flagTemp, Register valueTemp, Register offsetTemp,
                               Register maskTemp, Register output);

  protected:
    void atomicEffectOp(int nbytes, AtomicOp op, const Imm32& value, const Address& address,
                        Register flagTemp, Register valueTemp, Register offsetTemp, Register maskTemp);
    void atomicEffectOp(int nbytes, AtomicOp op, const Imm32& value, const BaseIndex& address,
                        Register flagTemp, Register valueTemp, Register offsetTemp, Register maskTemp);
    void atomicEffectOp(int nbytes, AtomicOp op, const Register& value, const Address& address,
                        Register flagTemp, Register valueTemp, Register offsetTemp, Register maskTemp);
    void atomicEffectOp(int nbytes, AtomicOp op, const Register& value, const BaseIndex& address,
                        Register flagTemp, Register valueTemp, Register offsetTemp, Register maskTemp);

    void atomicFetchOp(int nbytes, bool signExtend, AtomicOp op, const Imm32& value,
                       const Address& address, Register flagTemp, Register valueTemp,
                       Register offsetTemp, Register maskTemp, Register output);
    void atomicFetchOp(int nbytes, bool signExtend, AtomicOp op, const Imm32& value,
                       const BaseIndex& address, Register flagTemp, Register valueTemp,
                       Register offsetTemp, Register maskTemp, Register output);
    void atomicFetchOp(int nbytes, bool signExtend, AtomicOp op, const Register& value,
                       const Address& address, Register flagTemp, Register valueTemp,
                       Register offsetTemp, Register maskTemp, Register output);
    void atomicFetchOp(int nbytes, bool signExtend, AtomicOp op, const Register& value,
                       const BaseIndex& address, Register flagTemp, Register valueTemp,
                       Register offsetTemp, Register maskTemp, Register output);

    void compareExchange(int nbytes, bool signExtend, const Address& address, Register oldval,
                         Register newval, Register valueTemp, Register offsetTemp, Register maskTemp,
                         Register output);
    void compareExchange(int nbytes, bool signExtend, const BaseIndex& address, Register oldval,
                         Register newval, Register valueTemp, Register offsetTemp, Register maskTemp,
                         Register output);

    void atomicExchange(int nbytes, bool signExtend, const Address& address, Register value,
                        Register valueTemp, Register offsetTemp, Register maskTemp,
                        Register output);
    void atomicExchange(int nbytes, bool signExtend, const BaseIndex& address, Register value,
                        Register valueTemp, Register offsetTemp, Register maskTemp,
                        Register output);
};

} // namespace jit
} // namespace js

#endif /* jit_mips_shared_MacroAssembler_mips_shared_h */

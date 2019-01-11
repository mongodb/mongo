/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_MacroAssembler_mips_shared_h
#define jit_mips_shared_MacroAssembler_mips_shared_h

#if defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/Assembler-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/Assembler-mips64.h"
#endif

#include "jit/AtomicOp.h"

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
    Condition ma_cmp(Register rd, Register lhs, Imm32 imm, Condition c);

    void compareFloatingPoint(FloatFormat fmt, FloatRegister lhs, FloatRegister rhs,
                              DoubleCondition c, FloatTestKind* testKind,
                              FPConditionBit fcc = FCC0);

  public:
    void ma_move(Register rd, Register rs);

    void ma_li(Register dest, ImmGCPtr ptr);

    void ma_li(Register dest, Imm32 imm);
    void ma_liPatchable(Register dest, Imm32 imm);

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

    // Bit extract/insert
    void ma_ext(Register rt, Register rs, uint16_t pos, uint16_t size);
    void ma_ins(Register rt, Register rs, uint16_t pos, uint16_t size);

    // Sign extend
    void ma_seb(Register rd, Register rt);
    void ma_seh(Register rd, Register rt);

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

    void ma_ctz(Register rd, Register rs);

    // load
    void ma_load(Register dest, const BaseIndex& src, LoadStoreSize size = SizeWord,
                 LoadStoreExtension extension = SignExtend);
    void ma_load_unaligned(const wasm::MemoryAccessDesc& access, Register dest, const BaseIndex& src, Register temp,
                           LoadStoreSize size, LoadStoreExtension extension);

    // store
    void ma_store(Register data, const BaseIndex& dest, LoadStoreSize size = SizeWord,
                  LoadStoreExtension extension = SignExtend);
    void ma_store(Imm32 imm, const BaseIndex& dest, LoadStoreSize size = SizeWord,
                  LoadStoreExtension extension = SignExtend);
    void ma_store_unaligned(const wasm::MemoryAccessDesc& access, Register data, const BaseIndex& dest, Register temp,
                            LoadStoreSize size, LoadStoreExtension extension);

    // arithmetic based ops
    // add
    void ma_addu(Register rd, Register rs, Imm32 imm);
    void ma_addu(Register rd, Register rs);
    void ma_addu(Register rd, Imm32 imm);
    template <typename L>
    void ma_addTestCarry(Register rd, Register rs, Register rt, L overflow);
    template <typename L>
    void ma_addTestCarry(Register rd, Register rs, Imm32 imm, L overflow);

    // subtract
    void ma_subu(Register rd, Register rs, Imm32 imm);
    void ma_subu(Register rd, Register rs);
    void ma_subu(Register rd, Imm32 imm);
    void ma_subTestOverflow(Register rd, Register rs, Imm32 imm, Label* overflow);

    // multiplies.  For now, there are only few that we care about.
    void ma_mul(Register rd, Register rs, Imm32 imm);
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
    template <typename T>
    void ma_b(Register lhs, T rhs, wasm::OldTrapDesc target, Condition c,
              JumpKind jumpKind = LongJump);

    void ma_b(Label* l, JumpKind jumpKind = LongJump);
    void ma_b(wasm::OldTrapDesc target, JumpKind jumpKind = LongJump);

    // fp instructions
    void ma_lis(FloatRegister dest, float value);

    void ma_sd(FloatRegister src, BaseIndex address);
    void ma_ss(FloatRegister src, BaseIndex address);

    void ma_ld(FloatRegister dest, const BaseIndex& src);
    void ma_ls(FloatRegister dest, const BaseIndex& src);

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

    // Evaluate srcDest = minmax<isMax>{Float32,Double}(srcDest, other).
    // Handle NaN specially if handleNaN is true.
    void minMaxDouble(FloatRegister srcDest, FloatRegister other, bool handleNaN, bool isMax);
    void minMaxFloat32(FloatRegister srcDest, FloatRegister other, bool handleNaN, bool isMax);

    void loadDouble(const Address& addr, FloatRegister dest);
    void loadDouble(const BaseIndex& src, FloatRegister dest);

    // Load a float value into a register, then expand it to a double.
    void loadFloatAsDouble(const Address& addr, FloatRegister dest);
    void loadFloatAsDouble(const BaseIndex& src, FloatRegister dest);

    void loadFloat32(const Address& addr, FloatRegister dest);
    void loadFloat32(const BaseIndex& src, FloatRegister dest);

   void outOfLineWasmTruncateToInt32Check(FloatRegister input, Register output, MIRType fromType,
                                           TruncFlags flags, Label* rejoin,
                                           wasm::BytecodeOffset trapOffset);
    void outOfLineWasmTruncateToInt64Check(FloatRegister input, Register64 output, MIRType fromType,
                                           TruncFlags flags, Label* rejoin,
                                           wasm::BytecodeOffset trapOffset);

  protected:
    void wasmLoadImpl(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                      Register ptrScratch, AnyRegister output, Register tmp);
    void wasmStoreImpl(const wasm::MemoryAccessDesc& access, AnyRegister value, Register memoryBase,
                       Register ptr, Register ptrScratch, Register tmp);
};

} // namespace jit
} // namespace js

#endif /* jit_mips_shared_MacroAssembler_mips_shared_h */

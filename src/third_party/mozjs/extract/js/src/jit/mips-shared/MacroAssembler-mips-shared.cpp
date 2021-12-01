/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips-shared/MacroAssembler-mips-shared.h"

#include "mozilla/EndianUtils.h"

#include "jit/MacroAssembler.h"

using namespace js;
using namespace jit;

void
MacroAssemblerMIPSShared::ma_move(Register rd, Register rs)
{
    as_or(rd, rs, zero);
}

void
MacroAssemblerMIPSShared::ma_li(Register dest, ImmGCPtr ptr)
{
    writeDataRelocation(ptr);
    asMasm().ma_liPatchable(dest, ImmPtr(ptr.value));
}

void
MacroAssemblerMIPSShared::ma_li(Register dest, Imm32 imm)
{
    if (Imm16::IsInSignedRange(imm.value)) {
        as_addiu(dest, zero, imm.value);
    } else if (Imm16::IsInUnsignedRange(imm.value)) {
        as_ori(dest, zero, Imm16::Lower(imm).encode());
    } else if (Imm16::Lower(imm).encode() == 0) {
        as_lui(dest, Imm16::Upper(imm).encode());
    } else {
        as_lui(dest, Imm16::Upper(imm).encode());
        as_ori(dest, dest, Imm16::Lower(imm).encode());
    }
}

// This method generates lui and ori instruction pair that can be modified by
// UpdateLuiOriValue, either during compilation (eg. Assembler::bind), or
// during execution (eg. jit::PatchJump).
void
MacroAssemblerMIPSShared::ma_liPatchable(Register dest, Imm32 imm)
{
    m_buffer.ensureSpace(2 * sizeof(uint32_t));
    as_lui(dest, Imm16::Upper(imm).encode());
    as_ori(dest, dest, Imm16::Lower(imm).encode());
}

// Shifts
void
MacroAssemblerMIPSShared::ma_sll(Register rd, Register rt, Imm32 shift)
{
    as_sll(rd, rt, shift.value % 32);
}
void
MacroAssemblerMIPSShared::ma_srl(Register rd, Register rt, Imm32 shift)
{
    as_srl(rd, rt, shift.value % 32);
}

void
MacroAssemblerMIPSShared::ma_sra(Register rd, Register rt, Imm32 shift)
{
    as_sra(rd, rt, shift.value % 32);
}

void
MacroAssemblerMIPSShared::ma_ror(Register rd, Register rt, Imm32 shift)
{
    if (hasR2()) {
        as_rotr(rd, rt, shift.value % 32);
    } else {
        ScratchRegisterScope scratch(asMasm());
        as_srl(scratch, rt, shift.value % 32);
        as_sll(rd, rt, (32 - (shift.value % 32)) % 32);
        as_or(rd, rd, scratch);
    }
}

void
MacroAssemblerMIPSShared::ma_rol(Register rd, Register rt, Imm32 shift)
{
    if (hasR2()) {
        as_rotr(rd, rt, (32 - (shift.value % 32)) % 32);
    } else {
        ScratchRegisterScope scratch(asMasm());
        as_srl(scratch, rt, (32 - (shift.value % 32)) % 32);
        as_sll(rd, rt, shift.value % 32);
        as_or(rd, rd, scratch);
    }
}

void
MacroAssemblerMIPSShared::ma_sll(Register rd, Register rt, Register shift)
{
    as_sllv(rd, rt, shift);
}

void
MacroAssemblerMIPSShared::ma_srl(Register rd, Register rt, Register shift)
{
    as_srlv(rd, rt, shift);
}

void
MacroAssemblerMIPSShared::ma_sra(Register rd, Register rt, Register shift)
{
    as_srav(rd, rt, shift);
}

void
MacroAssemblerMIPSShared::ma_ror(Register rd, Register rt, Register shift)
{
    if (hasR2()) {
        as_rotrv(rd, rt, shift);
    } else {
        ScratchRegisterScope scratch(asMasm());
        ma_negu(scratch, shift);
        as_sllv(scratch, rt, scratch);
        as_srlv(rd, rt, shift);
        as_or(rd, rd, scratch);
    }
}

void
MacroAssemblerMIPSShared::ma_rol(Register rd, Register rt, Register shift)
{
    ScratchRegisterScope scratch(asMasm());
    ma_negu(scratch, shift);
    if (hasR2()) {
        as_rotrv(rd, rt, scratch);
    } else {
        as_srlv(rd, rt, scratch);
        as_sllv(scratch, rt, shift);
        as_or(rd, rd, scratch);
    }
}

void
MacroAssemblerMIPSShared::ma_negu(Register rd, Register rs)
{
    as_subu(rd, zero, rs);
}

void
MacroAssemblerMIPSShared::ma_not(Register rd, Register rs)
{
    as_nor(rd, rs, zero);
}

// Bit extract/insert
void
MacroAssemblerMIPSShared::ma_ext(Register rt, Register rs, uint16_t pos, uint16_t size) {
    MOZ_ASSERT(pos < 32);
    MOZ_ASSERT(pos + size < 33);

    if (hasR2()) {
        as_ext(rt, rs, pos, size);
    } else {
        int shift_left = 32 - (pos + size);
        as_sll(rt, rs, shift_left);
        int shift_right = 32 - size;
        if (shift_right > 0) {
            as_srl(rt, rt, shift_right);
        }
    }
}

void
MacroAssemblerMIPSShared::ma_ins(Register rt, Register rs, uint16_t pos, uint16_t size) {
    MOZ_ASSERT(pos < 32);
    MOZ_ASSERT(pos + size <= 32);
    MOZ_ASSERT(size != 0);

    if (hasR2()) {
        as_ins(rt, rs, pos, size);
    } else {
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());
        ma_subu(scratch, zero, Imm32(1));
        as_srl(scratch, scratch, 32 - size);
        as_and(scratch2, rs, scratch);
        as_sll(scratch2, scratch2, pos);
        as_sll(scratch, scratch, pos);
        as_nor(scratch, scratch, zero);
        as_and(scratch, rt, scratch);
        as_or(rt, scratch2, scratch);
    }
}

// Sign extend
void
MacroAssemblerMIPSShared::ma_seb(Register rd, Register rt)
{
    if (hasR2()) {
        as_seb(rd, rt);
    } else {
        as_sll(rd, rt, 24);
        as_sra(rd, rd, 24);
    }
}

void
MacroAssemblerMIPSShared::ma_seh(Register rd, Register rt)
{
    if (hasR2()) {
        as_seh(rd, rt);
    } else {
        as_sll(rd, rt, 16);
        as_sra(rd, rd, 16);
    }
}

// And.
void
MacroAssemblerMIPSShared::ma_and(Register rd, Register rs)
{
    as_and(rd, rd, rs);
}

void
MacroAssemblerMIPSShared::ma_and(Register rd, Imm32 imm)
{
    ma_and(rd, rd, imm);
}

void
MacroAssemblerMIPSShared::ma_and(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInUnsignedRange(imm.value)) {
        as_andi(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_and(rd, rs, ScratchRegister);
    }
}

// Or.
void
MacroAssemblerMIPSShared::ma_or(Register rd, Register rs)
{
    as_or(rd, rd, rs);
}

void
MacroAssemblerMIPSShared::ma_or(Register rd, Imm32 imm)
{
    ma_or(rd, rd, imm);
}

void
MacroAssemblerMIPSShared::ma_or(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInUnsignedRange(imm.value)) {
        as_ori(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_or(rd, rs, ScratchRegister);
    }
}

// xor
void
MacroAssemblerMIPSShared::ma_xor(Register rd, Register rs)
{
    as_xor(rd, rd, rs);
}

void
MacroAssemblerMIPSShared::ma_xor(Register rd, Imm32 imm)
{
    ma_xor(rd, rd, imm);
}

void
MacroAssemblerMIPSShared::ma_xor(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInUnsignedRange(imm.value)) {
        as_xori(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_xor(rd, rs, ScratchRegister);
    }
}

void
MacroAssemblerMIPSShared::ma_ctz(Register rd, Register rs)
{
    as_addiu(ScratchRegister, rs, -1);
    as_xor(rd, ScratchRegister, rs);
    as_and(rd, rd, ScratchRegister);
    as_clz(rd, rd);
    ma_li(ScratchRegister, Imm32(0x20));
    as_subu(rd, ScratchRegister, rd);
}

// Arithmetic-based ops.

// Add.
void
MacroAssemblerMIPSShared::ma_addu(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInSignedRange(imm.value)) {
        as_addiu(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_addu(rd, rs, ScratchRegister);
    }
}

void
MacroAssemblerMIPSShared::ma_addu(Register rd, Register rs)
{
    as_addu(rd, rd, rs);
}

void
MacroAssemblerMIPSShared::ma_addu(Register rd, Imm32 imm)
{
    ma_addu(rd, rd, imm);
}

template <typename L>
void
MacroAssemblerMIPSShared::ma_addTestCarry(Register rd, Register rs, Register rt, L overflow)
{
    MOZ_ASSERT_IF(rd == rs, rt != rd);
    as_addu(rd, rs, rt);
    as_sltu(SecondScratchReg, rd, rd == rs? rt : rs);
    ma_b(SecondScratchReg, SecondScratchReg, overflow, Assembler::NonZero);
}

template void
MacroAssemblerMIPSShared::ma_addTestCarry<Label*>(Register rd, Register rs,
                                                  Register rt, Label* overflow);
template void
MacroAssemblerMIPSShared::ma_addTestCarry<wasm::OldTrapDesc>(Register rd, Register rs, Register rt,
                                                             wasm::OldTrapDesc overflow);

template <typename L>
void
MacroAssemblerMIPSShared::ma_addTestCarry(Register rd, Register rs, Imm32 imm, L overflow)
{
    ma_li(ScratchRegister, imm);
    ma_addTestCarry(rd, rs, ScratchRegister, overflow);
}

template void
MacroAssemblerMIPSShared::ma_addTestCarry<Label*>(Register rd, Register rs,
                                                  Imm32 imm, Label* overflow);
template void
MacroAssemblerMIPSShared::ma_addTestCarry<wasm::OldTrapDesc>(Register rd, Register rs, Imm32 imm,
                                                             wasm::OldTrapDesc overflow);

// Subtract.
void
MacroAssemblerMIPSShared::ma_subu(Register rd, Register rs, Imm32 imm)
{
    if (Imm16::IsInSignedRange(-imm.value)) {
        as_addiu(rd, rs, -imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_subu(rd, rs, ScratchRegister);
    }
}

void
MacroAssemblerMIPSShared::ma_subu(Register rd, Imm32 imm)
{
    ma_subu(rd, rd, imm);
}

void
MacroAssemblerMIPSShared::ma_subu(Register rd, Register rs)
{
    as_subu(rd, rd, rs);
}

void
MacroAssemblerMIPSShared::ma_subTestOverflow(Register rd, Register rs, Imm32 imm, Label* overflow)
{
    if (imm.value != INT32_MIN) {
        asMasm().ma_addTestOverflow(rd, rs, Imm32(-imm.value), overflow);
    } else {
        ma_li(ScratchRegister, Imm32(imm.value));
        asMasm().ma_subTestOverflow(rd, rs, ScratchRegister, overflow);
    }
}

void
MacroAssemblerMIPSShared::ma_mul(Register rd, Register rs, Imm32 imm)
{
    ma_li(ScratchRegister, imm);
    as_mul(rd, rs, ScratchRegister);
}

void
MacroAssemblerMIPSShared::ma_mul_branch_overflow(Register rd, Register rs, Register rt, Label* overflow)
{
    as_mult(rs, rt);
    as_mflo(rd);
    as_sra(ScratchRegister, rd, 31);
    as_mfhi(SecondScratchReg);
    ma_b(ScratchRegister, SecondScratchReg, overflow, Assembler::NotEqual);
}

void
MacroAssemblerMIPSShared::ma_mul_branch_overflow(Register rd, Register rs, Imm32 imm, Label* overflow)
{
    ma_li(ScratchRegister, imm);
    ma_mul_branch_overflow(rd, rs, ScratchRegister, overflow);
}

void
MacroAssemblerMIPSShared::ma_div_branch_overflow(Register rd, Register rs, Register rt, Label* overflow)
{
    as_div(rs, rt);
    as_mfhi(ScratchRegister);
    ma_b(ScratchRegister, ScratchRegister, overflow, Assembler::NonZero);
    as_mflo(rd);
}

void
MacroAssemblerMIPSShared::ma_div_branch_overflow(Register rd, Register rs, Imm32 imm, Label* overflow)
{
    ma_li(ScratchRegister, imm);
    ma_div_branch_overflow(rd, rs, ScratchRegister, overflow);
}

void
MacroAssemblerMIPSShared::ma_mod_mask(Register src, Register dest, Register hold, Register remain,
                                      int32_t shift, Label* negZero)
{
    // MATH:
    // We wish to compute x % (1<<y) - 1 for a known constant, y.
    // First, let b = (1<<y) and C = (1<<y)-1, then think of the 32 bit
    // dividend as a number in base b, namely
    // c_0*1 + c_1*b + c_2*b^2 ... c_n*b^n
    // now, since both addition and multiplication commute with modulus,
    // x % C == (c_0 + c_1*b + ... + c_n*b^n) % C ==
    // (c_0 % C) + (c_1%C) * (b % C) + (c_2 % C) * (b^2 % C)...
    // now, since b == C + 1, b % C == 1, and b^n % C == 1
    // this means that the whole thing simplifies to:
    // c_0 + c_1 + c_2 ... c_n % C
    // each c_n can easily be computed by a shift/bitextract, and the modulus
    // can be maintained by simply subtracting by C whenever the number gets
    // over C.
    int32_t mask = (1 << shift) - 1;
    Label head, negative, sumSigned, done;

    // hold holds -1 if the value was negative, 1 otherwise.
    // remain holds the remaining bits that have not been processed
    // SecondScratchReg serves as a temporary location to store extracted bits
    // into as well as holding the trial subtraction as a temp value dest is
    // the accumulator (and holds the final result)

    // move the whole value into the remain.
    ma_move(remain, src);
    // Zero out the dest.
    ma_li(dest, Imm32(0));
    // Set the hold appropriately.
    ma_b(remain, remain, &negative, Signed, ShortJump);
    ma_li(hold, Imm32(1));
    ma_b(&head, ShortJump);

    bind(&negative);
    ma_li(hold, Imm32(-1));
    ma_negu(remain, remain);

    // Begin the main loop.
    bind(&head);

    // Extract the bottom bits into SecondScratchReg.
    ma_and(SecondScratchReg, remain, Imm32(mask));
    // Add those bits to the accumulator.
    as_addu(dest, dest, SecondScratchReg);
    // Do a trial subtraction
    ma_subu(SecondScratchReg, dest, Imm32(mask));
    // If (sum - C) > 0, store sum - C back into sum, thus performing a
    // modulus.
    ma_b(SecondScratchReg, SecondScratchReg, &sumSigned, Signed, ShortJump);
    ma_move(dest, SecondScratchReg);
    bind(&sumSigned);
    // Get rid of the bits that we extracted before.
    as_srl(remain, remain, shift);
    // If the shift produced zero, finish, otherwise, continue in the loop.
    ma_b(remain, remain, &head, NonZero, ShortJump);
    // Check the hold to see if we need to negate the result.
    ma_b(hold, hold, &done, NotSigned, ShortJump);

    // If the hold was non-zero, negate the result to be in line with
    // what JS wants
    if (negZero != nullptr) {
        // Jump out in case of negative zero.
        ma_b(hold, hold, negZero, Zero);
        ma_negu(dest, dest);
    } else {
        ma_negu(dest, dest);
    }

    bind(&done);
}

// Memory.

void
MacroAssemblerMIPSShared::ma_load(Register dest, const BaseIndex& src,
                                  LoadStoreSize size, LoadStoreExtension extension)
{
    if (isLoongson() && ZeroExtend != extension && Imm8::IsInSignedRange(src.offset)) {
        Register index = src.index;

        if (src.scale != TimesOne) {
            int32_t shift = Imm32::ShiftOf(src.scale).value;

            MOZ_ASSERT(SecondScratchReg != src.base);
            index = SecondScratchReg;
#ifdef JS_CODEGEN_MIPS64
            asMasm().ma_dsll(index, src.index, Imm32(shift));
#else
            asMasm().ma_sll(index, src.index, Imm32(shift));
#endif
        }

        switch (size) {
          case SizeByte:
            as_gslbx(dest, src.base, index, src.offset);
            break;
          case SizeHalfWord:
            as_gslhx(dest, src.base, index, src.offset);
            break;
          case SizeWord:
            as_gslwx(dest, src.base, index, src.offset);
            break;
          case SizeDouble:
            as_gsldx(dest, src.base, index, src.offset);
            break;
          default:
            MOZ_CRASH("Invalid argument for ma_load");
        }
        return;
    }

    asMasm().computeScaledAddress(src, SecondScratchReg);
    asMasm().ma_load(dest, Address(SecondScratchReg, src.offset), size, extension);
}

void
MacroAssemblerMIPSShared::ma_load_unaligned(const wasm::MemoryAccessDesc& access, Register dest, const BaseIndex& src, Register temp,
                                            LoadStoreSize size, LoadStoreExtension extension)
{
    MOZ_ASSERT(MOZ_LITTLE_ENDIAN, "Wasm-only; wasm is disabled on big-endian.");
    int16_t lowOffset, hiOffset;
    Register base;

    asMasm().computeScaledAddress(src, SecondScratchReg);

    if (Imm16::IsInSignedRange(src.offset) && Imm16::IsInSignedRange(src.offset + size / 8 - 1)) {
        base = SecondScratchReg;
        lowOffset = Imm16(src.offset).encode();
        hiOffset = Imm16(src.offset + size / 8 - 1).encode();
    } else {
        ma_li(ScratchRegister, Imm32(src.offset));
        asMasm().addPtr(SecondScratchReg, ScratchRegister);
        base = ScratchRegister;
        lowOffset = Imm16(0).encode();
        hiOffset = Imm16(size / 8 - 1).encode();
    }

    BufferOffset load;
    switch (size) {
      case SizeHalfWord:
        if (extension != ZeroExtend)
            load = as_lbu(temp, base, hiOffset);
        else
            load = as_lb(temp, base, hiOffset);
        as_lbu(dest, base, lowOffset);
        ma_ins(dest, temp, 8, 24);
        break;
      case SizeWord:
        load = as_lwl(dest, base, hiOffset);
        as_lwr(dest, base, lowOffset);
#ifdef JS_CODEGEN_MIPS64
        if (extension != ZeroExtend)
            as_dext(dest, dest, 0, 32);
#endif
        break;
#ifdef JS_CODEGEN_MIPS64
      case SizeDouble:
        load = as_ldl(dest, base, hiOffset);
        as_ldr(dest, base, lowOffset);
        break;
#endif
      default:
        MOZ_CRASH("Invalid argument for ma_load");
    }

    append(access, load.getOffset(), asMasm().framePushed());
}

void
MacroAssemblerMIPSShared::ma_store(Register data, const BaseIndex& dest,
                                   LoadStoreSize size, LoadStoreExtension extension)
{
    if (isLoongson() && Imm8::IsInSignedRange(dest.offset)) {
        Register index = dest.index;

        if (dest.scale != TimesOne) {
            int32_t shift = Imm32::ShiftOf(dest.scale).value;

            MOZ_ASSERT(SecondScratchReg != dest.base);
            index = SecondScratchReg;
#ifdef JS_CODEGEN_MIPS64
            asMasm().ma_dsll(index, dest.index, Imm32(shift));
#else
            asMasm().ma_sll(index, dest.index, Imm32(shift));
#endif
        }

        switch (size) {
          case SizeByte:
            as_gssbx(data, dest.base, index, dest.offset);
            break;
          case SizeHalfWord:
            as_gsshx(data, dest.base, index, dest.offset);
            break;
          case SizeWord:
            as_gsswx(data, dest.base, index, dest.offset);
            break;
          case SizeDouble:
            as_gssdx(data, dest.base, index, dest.offset);
            break;
          default:
            MOZ_CRASH("Invalid argument for ma_store");
        }
        return;
    }

    asMasm().computeScaledAddress(dest, SecondScratchReg);
    asMasm().ma_store(data, Address(SecondScratchReg, dest.offset), size, extension);
}

void
MacroAssemblerMIPSShared::ma_store(Imm32 imm, const BaseIndex& dest,
                                   LoadStoreSize size, LoadStoreExtension extension)
{
    if (isLoongson() && Imm8::IsInSignedRange(dest.offset)) {
        Register data = zero;
        Register index = dest.index;

        if (imm.value) {
            MOZ_ASSERT(ScratchRegister != dest.base);
            MOZ_ASSERT(ScratchRegister != dest.index);
            data = ScratchRegister;
            ma_li(data, imm);
        }

        if (dest.scale != TimesOne) {
            int32_t shift = Imm32::ShiftOf(dest.scale).value;

            MOZ_ASSERT(SecondScratchReg != dest.base);
            index = SecondScratchReg;
#ifdef JS_CODEGEN_MIPS64
            asMasm().ma_dsll(index, dest.index, Imm32(shift));
#else
            asMasm().ma_sll(index, dest.index, Imm32(shift));
#endif
        }

        switch (size) {
          case SizeByte:
            as_gssbx(data, dest.base, index, dest.offset);
            break;
          case SizeHalfWord:
            as_gsshx(data, dest.base, index, dest.offset);
            break;
          case SizeWord:
            as_gsswx(data, dest.base, index, dest.offset);
            break;
          case SizeDouble:
            as_gssdx(data, dest.base, index, dest.offset);
            break;
          default:
            MOZ_CRASH("Invalid argument for ma_store");
        }
        return;
    }

    // Make sure that SecondScratchReg contains absolute address so that
    // offset is 0.
    asMasm().computeEffectiveAddress(dest, SecondScratchReg);

    // Scrach register is free now, use it for loading imm value
    ma_li(ScratchRegister, imm);

    // with offset=0 ScratchRegister will not be used in ma_store()
    // so we can use it as a parameter here
    asMasm().ma_store(ScratchRegister, Address(SecondScratchReg, 0), size, extension);
}

void
MacroAssemblerMIPSShared::ma_store_unaligned(const wasm::MemoryAccessDesc& access, Register data,
                                             const BaseIndex& dest, Register temp,
                                             LoadStoreSize size, LoadStoreExtension extension)
{
    MOZ_ASSERT(MOZ_LITTLE_ENDIAN, "Wasm-only; wasm is disabled on big-endian.");
    int16_t lowOffset, hiOffset;
    Register base;

    asMasm().computeScaledAddress(dest, SecondScratchReg);

    if (Imm16::IsInSignedRange(dest.offset) && Imm16::IsInSignedRange(dest.offset + size / 8 - 1)) {
        base = SecondScratchReg;
        lowOffset = Imm16(dest.offset).encode();
        hiOffset = Imm16(dest.offset + size / 8 - 1).encode();
    } else {
        ma_li(ScratchRegister, Imm32(dest.offset));
        asMasm().addPtr(SecondScratchReg, ScratchRegister);
        base = ScratchRegister;
        lowOffset = Imm16(0).encode();
        hiOffset = Imm16(size / 8 - 1).encode();
    }

    BufferOffset store;
    switch (size) {
      case SizeHalfWord:
        ma_ext(temp, data, 8, 8);
        store = as_sb(temp, base, hiOffset);
        as_sb(data, base, lowOffset);
        break;
      case SizeWord:
        store = as_swl(data, base, hiOffset);
        as_swr(data, base, lowOffset);
        break;
#ifdef JS_CODEGEN_MIPS64
      case SizeDouble:
        store = as_sdl(data, base, hiOffset);
        as_sdr(data, base, lowOffset);
        break;
#endif
      default:
        MOZ_CRASH("Invalid argument for ma_store");
    }
    append(access, store.getOffset(), asMasm().framePushed());
}

// Branches when done from within mips-specific code.
void
MacroAssemblerMIPSShared::ma_b(Register lhs, Register rhs, Label* label, Condition c, JumpKind jumpKind)
{
    switch (c) {
      case Equal :
      case NotEqual:
        asMasm().branchWithCode(getBranchCode(lhs, rhs, c), label, jumpKind);
        break;
      case Always:
        ma_b(label, jumpKind);
        break;
      case Zero:
      case NonZero:
      case Signed:
      case NotSigned:
        MOZ_ASSERT(lhs == rhs);
        asMasm().branchWithCode(getBranchCode(lhs, c), label, jumpKind);
        break;
      default:
        Condition cond = ma_cmp(ScratchRegister, lhs, rhs, c);
        asMasm().branchWithCode(getBranchCode(ScratchRegister, cond), label, jumpKind);
        break;
    }
}

void
MacroAssemblerMIPSShared::ma_b(Register lhs, Imm32 imm, Label* label, Condition c, JumpKind jumpKind)
{
    MOZ_ASSERT(c != Overflow);
    if (imm.value == 0) {
        if (c == Always || c == AboveOrEqual)
            ma_b(label, jumpKind);
        else if (c == Below)
            ; // This condition is always false. No branch required.
        else
            asMasm().branchWithCode(getBranchCode(lhs, c), label, jumpKind);
    } else {
      switch (c) {
        case Equal:
        case NotEqual:
          MOZ_ASSERT(lhs != ScratchRegister);
          ma_li(ScratchRegister, imm);
          ma_b(lhs, ScratchRegister, label, c, jumpKind);
          break;
        default:
          Condition cond = ma_cmp(ScratchRegister, lhs, imm, c);
          asMasm().branchWithCode(getBranchCode(ScratchRegister, cond), label, jumpKind);
        }
    }
}

void
MacroAssemblerMIPSShared::ma_b(Register lhs, ImmPtr imm, Label* l, Condition c, JumpKind jumpKind)
{
    asMasm().ma_b(lhs, ImmWord(uintptr_t(imm.value)), l, c, jumpKind);
}

template <typename T>
void
MacroAssemblerMIPSShared::ma_b(Register lhs, T rhs, wasm::OldTrapDesc target, Condition c,
                               JumpKind jumpKind)
{
    Label label;
    ma_b(lhs, rhs, &label, c, jumpKind);
    bindLater(&label, target);
}

template void MacroAssemblerMIPSShared::ma_b<Register>(Register lhs, Register rhs,
                                                       wasm::OldTrapDesc target, Condition c,
                                                       JumpKind jumpKind);
template void MacroAssemblerMIPSShared::ma_b<Imm32>(Register lhs, Imm32 rhs,
                                                       wasm::OldTrapDesc target, Condition c,
                                                       JumpKind jumpKind);
template void MacroAssemblerMIPSShared::ma_b<ImmTag>(Register lhs, ImmTag rhs,
                                                       wasm::OldTrapDesc target, Condition c,
                                                       JumpKind jumpKind);

void
MacroAssemblerMIPSShared::ma_b(Label* label, JumpKind jumpKind)
{
    asMasm().branchWithCode(getBranchCode(BranchIsJump), label, jumpKind);
}

void
MacroAssemblerMIPSShared::ma_b(wasm::OldTrapDesc target, JumpKind jumpKind)
{
    Label label;
    asMasm().branchWithCode(getBranchCode(BranchIsJump), &label, jumpKind);
    bindLater(&label, target);
}

Assembler::Condition
MacroAssemblerMIPSShared::ma_cmp(Register scratch, Register lhs, Register rhs, Condition c)
{
    switch (c) {
      case Above:
        // bgtu s,t,label =>
        //   sltu at,t,s
        //   bne at,$zero,offs
        as_sltu(scratch, rhs, lhs);
        return NotEqual;
      case AboveOrEqual:
        // bgeu s,t,label =>
        //   sltu at,s,t
        //   beq at,$zero,offs
        as_sltu(scratch, lhs, rhs);
        return Equal;
      case Below:
        // bltu s,t,label =>
        //   sltu at,s,t
        //   bne at,$zero,offs
        as_sltu(scratch, lhs, rhs);
        return NotEqual;
      case BelowOrEqual:
        // bleu s,t,label =>
        //   sltu at,t,s
        //   beq at,$zero,offs
        as_sltu(scratch, rhs, lhs);
        return Equal;
      case GreaterThan:
        // bgt s,t,label =>
        //   slt at,t,s
        //   bne at,$zero,offs
        as_slt(scratch, rhs, lhs);
        return NotEqual;
      case GreaterThanOrEqual:
        // bge s,t,label =>
        //   slt at,s,t
        //   beq at,$zero,offs
        as_slt(scratch, lhs, rhs);
        return Equal;
      case LessThan:
        // blt s,t,label =>
        //   slt at,s,t
        //   bne at,$zero,offs
        as_slt(scratch, lhs, rhs);
        return NotEqual;
      case LessThanOrEqual:
        // ble s,t,label =>
        //   slt at,t,s
        //   beq at,$zero,offs
        as_slt(scratch, rhs, lhs);
        return Equal;
      default:
        MOZ_CRASH("Invalid condition.");
    }
    return Always;
}

Assembler::Condition
MacroAssemblerMIPSShared::ma_cmp(Register scratch, Register lhs, Imm32 imm, Condition c)
{
    switch (c) {
      case Above:
      case BelowOrEqual:
        if (Imm16::IsInSignedRange(imm.value + 1) && imm.value != -1) {
          // lhs <= rhs via lhs < rhs + 1 if rhs + 1 does not overflow
          as_sltiu(scratch, lhs, imm.value + 1);

          return (c == BelowOrEqual ? NotEqual : Equal);
        } else {
          ma_li(scratch, imm);
          as_sltu(scratch, scratch, lhs);
          return (c == BelowOrEqual ? Equal : NotEqual);
        }
      case AboveOrEqual:
      case Below:
        if (Imm16::IsInSignedRange(imm.value)) {
          as_sltiu(scratch, lhs, imm.value);
        } else {
          ma_li(scratch, imm);
          as_sltu(scratch, lhs, scratch);
        }
        return (c == AboveOrEqual ? Equal : NotEqual);
      case GreaterThan:
      case LessThanOrEqual:
        if (Imm16::IsInSignedRange(imm.value + 1)) {
          // lhs <= rhs via lhs < rhs + 1.
          as_slti(scratch, lhs, imm.value + 1);
          return (c == LessThanOrEqual ? NotEqual : Equal);
        } else {
          ma_li(scratch, imm);
          as_slt(scratch, scratch, lhs);
          return (c == LessThanOrEqual ? Equal : NotEqual);
        }
      case GreaterThanOrEqual:
      case LessThan:
        if (Imm16::IsInSignedRange(imm.value)) {
          as_slti(scratch, lhs, imm.value);
        } else {
          ma_li(scratch, imm);
          as_slt(scratch, lhs, scratch);
        }
        return (c == GreaterThanOrEqual ? Equal : NotEqual);
      default:
        MOZ_CRASH("Invalid condition.");
    }
    return Always;
}

void
MacroAssemblerMIPSShared::ma_cmp_set(Register rd, Register rs, Register rt, Condition c)
{
    switch (c) {
      case Equal :
        // seq d,s,t =>
        //   xor d,s,t
        //   sltiu d,d,1
        as_xor(rd, rs, rt);
        as_sltiu(rd, rd, 1);
        break;
      case NotEqual:
        // sne d,s,t =>
        //   xor d,s,t
        //   sltu d,$zero,d
        as_xor(rd, rs, rt);
        as_sltu(rd, zero, rd);
        break;
      case Above:
        // sgtu d,s,t =>
        //   sltu d,t,s
        as_sltu(rd, rt, rs);
        break;
      case AboveOrEqual:
        // sgeu d,s,t =>
        //   sltu d,s,t
        //   xori d,d,1
        as_sltu(rd, rs, rt);
        as_xori(rd, rd, 1);
        break;
      case Below:
        // sltu d,s,t
        as_sltu(rd, rs, rt);
        break;
      case BelowOrEqual:
        // sleu d,s,t =>
        //   sltu d,t,s
        //   xori d,d,1
        as_sltu(rd, rt, rs);
        as_xori(rd, rd, 1);
        break;
      case GreaterThan:
        // sgt d,s,t =>
        //   slt d,t,s
        as_slt(rd, rt, rs);
        break;
      case GreaterThanOrEqual:
        // sge d,s,t =>
        //   slt d,s,t
        //   xori d,d,1
        as_slt(rd, rs, rt);
        as_xori(rd, rd, 1);
        break;
      case LessThan:
        // slt d,s,t
        as_slt(rd, rs, rt);
        break;
      case LessThanOrEqual:
        // sle d,s,t =>
        //   slt d,t,s
        //   xori d,d,1
        as_slt(rd, rt, rs);
        as_xori(rd, rd, 1);
        break;
      case Zero:
        MOZ_ASSERT(rs == rt);
        // seq d,s,$zero =>
        //   sltiu d,s,1
        as_sltiu(rd, rs, 1);
        break;
      case NonZero:
        MOZ_ASSERT(rs == rt);
        // sne d,s,$zero =>
        //   sltu d,$zero,s
        as_sltu(rd, zero, rs);
        break;
      case Signed:
        MOZ_ASSERT(rs == rt);
        as_slt(rd, rs, zero);
        break;
      case NotSigned:
        MOZ_ASSERT(rs == rt);
        // sge d,s,$zero =>
        //   slt d,s,$zero
        //   xori d,d,1
        as_slt(rd, rs, zero);
        as_xori(rd, rd, 1);
        break;
      default:
        MOZ_CRASH("Invalid condition.");
    }
}

void
MacroAssemblerMIPSShared::compareFloatingPoint(FloatFormat fmt, FloatRegister lhs, FloatRegister rhs,
                                               DoubleCondition c, FloatTestKind* testKind,
                                               FPConditionBit fcc)
{
    switch (c) {
      case DoubleOrdered:
        as_cun(fmt, lhs, rhs, fcc);
        *testKind = TestForFalse;
        break;
      case DoubleEqual:
        as_ceq(fmt, lhs, rhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleNotEqual:
        as_cueq(fmt, lhs, rhs, fcc);
        *testKind = TestForFalse;
        break;
      case DoubleGreaterThan:
        as_colt(fmt, rhs, lhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleGreaterThanOrEqual:
        as_cole(fmt, rhs, lhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleLessThan:
        as_colt(fmt, lhs, rhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleLessThanOrEqual:
        as_cole(fmt, lhs, rhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleUnordered:
        as_cun(fmt, lhs, rhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleEqualOrUnordered:
        as_cueq(fmt, lhs, rhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleNotEqualOrUnordered:
        as_ceq(fmt, lhs, rhs, fcc);
        *testKind = TestForFalse;
        break;
      case DoubleGreaterThanOrUnordered:
        as_cult(fmt, rhs, lhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleGreaterThanOrEqualOrUnordered:
        as_cule(fmt, rhs, lhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleLessThanOrUnordered:
        as_cult(fmt, lhs, rhs, fcc);
        *testKind = TestForTrue;
        break;
      case DoubleLessThanOrEqualOrUnordered:
        as_cule(fmt, lhs, rhs, fcc);
        *testKind = TestForTrue;
        break;
      default:
        MOZ_CRASH("Invalid DoubleCondition.");
    }
}

void
MacroAssemblerMIPSShared::ma_cmp_set_double(Register dest, FloatRegister lhs, FloatRegister rhs,
                                            DoubleCondition c)
{
    FloatTestKind moveCondition;
    compareFloatingPoint(DoubleFloat, lhs, rhs, c, &moveCondition);

    ma_li(dest, Imm32(1));

    if (moveCondition == TestForTrue)
        as_movf(dest, zero);
    else
        as_movt(dest, zero);
}

void
MacroAssemblerMIPSShared::ma_cmp_set_float32(Register dest, FloatRegister lhs, FloatRegister rhs,
                                             DoubleCondition c)
{
    FloatTestKind moveCondition;
    compareFloatingPoint(SingleFloat, lhs, rhs, c, &moveCondition);

    ma_li(dest, Imm32(1));

    if (moveCondition == TestForTrue)
        as_movf(dest, zero);
    else
        as_movt(dest, zero);
}

void
MacroAssemblerMIPSShared::ma_cmp_set(Register rd, Register rs, Imm32 imm, Condition c)
{
    if (imm.value == 0) {
        switch (c) {
          case Equal :
          case BelowOrEqual:
            as_sltiu(rd, rs, 1);
            break;
          case NotEqual:
          case Above:
            as_sltu(rd, zero, rs);
            break;
          case AboveOrEqual:
          case Below:
            as_ori(rd, zero, c == AboveOrEqual ? 1: 0);
            break;
          case GreaterThan:
          case LessThanOrEqual:
            as_slt(rd, zero, rs);
            if (c == LessThanOrEqual)
                as_xori(rd, rd, 1);
            break;
          case LessThan:
          case GreaterThanOrEqual:
            as_slt(rd, rs, zero);
            if (c == GreaterThanOrEqual)
                as_xori(rd, rd, 1);
            break;
          case Zero:
            as_sltiu(rd, rs, 1);
            break;
          case NonZero:
            as_sltu(rd, zero, rs);
            break;
          case Signed:
            as_slt(rd, rs, zero);
            break;
          case NotSigned:
            as_slt(rd, rs, zero);
            as_xori(rd, rd, 1);
            break;
          default:
            MOZ_CRASH("Invalid condition.");
        }
        return;
    }

    switch (c) {
      case Equal:
      case NotEqual:
        MOZ_ASSERT(rs != ScratchRegister);
        ma_xor(rd, rs, imm);
        if (c == Equal)
            as_sltiu(rd, rd, 1);
        else
            as_sltu(rd, zero, rd);
        break;
      case Zero:
      case NonZero:
      case Signed:
      case NotSigned:
        MOZ_CRASH("Invalid condition.");
      default:
        Condition cond = ma_cmp(rd, rs, imm, c);
        MOZ_ASSERT(cond == Equal || cond == NotEqual);

        if(cond == Equal)
            as_xori(rd, rd, 1);
    }
}

// fp instructions
void
MacroAssemblerMIPSShared::ma_lis(FloatRegister dest, float value)
{
    Imm32 imm(mozilla::BitwiseCast<uint32_t>(value));

    if(imm.value != 0) {
        ma_li(ScratchRegister, imm);
        moveToFloat32(ScratchRegister, dest);
    } else {
        moveToFloat32(zero, dest);
    }
}

void
MacroAssemblerMIPSShared::ma_sd(FloatRegister ft, BaseIndex address)
{
    if (isLoongson() && Imm8::IsInSignedRange(address.offset)) {
        Register index = address.index;

        if (address.scale != TimesOne) {
            int32_t shift = Imm32::ShiftOf(address.scale).value;

            MOZ_ASSERT(SecondScratchReg != address.base);
            index = SecondScratchReg;
#ifdef JS_CODEGEN_MIPS64
            asMasm().ma_dsll(index, address.index, Imm32(shift));
#else
            asMasm().ma_sll(index, address.index, Imm32(shift));
#endif
        }

        as_gssdx(ft, address.base, index, address.offset);
        return;
    }

    asMasm().computeScaledAddress(address, SecondScratchReg);
    asMasm().ma_sd(ft, Address(SecondScratchReg, address.offset));
}

void
MacroAssemblerMIPSShared::ma_ss(FloatRegister ft, BaseIndex address)
{
    if (isLoongson() && Imm8::IsInSignedRange(address.offset)) {
        Register index = address.index;

        if (address.scale != TimesOne) {
            int32_t shift = Imm32::ShiftOf(address.scale).value;

            MOZ_ASSERT(SecondScratchReg != address.base);
            index = SecondScratchReg;
#ifdef JS_CODEGEN_MIPS64
            asMasm().ma_dsll(index, address.index, Imm32(shift));
#else
            asMasm().ma_sll(index, address.index, Imm32(shift));
#endif
        }

        as_gsssx(ft, address.base, index, address.offset);
        return;
    }

    asMasm().computeScaledAddress(address, SecondScratchReg);
    asMasm().ma_ss(ft, Address(SecondScratchReg, address.offset));
}

void
MacroAssemblerMIPSShared::ma_ld(FloatRegister ft, const BaseIndex& src)
{
    asMasm().computeScaledAddress(src, SecondScratchReg);
    asMasm().ma_ld(ft, Address(SecondScratchReg, src.offset));
}

void
MacroAssemblerMIPSShared::ma_ls(FloatRegister ft, const BaseIndex& src)
{
    asMasm().computeScaledAddress(src, SecondScratchReg);
    asMasm().ma_ls(ft, Address(SecondScratchReg, src.offset));
}

void
MacroAssemblerMIPSShared::ma_bc1s(FloatRegister lhs, FloatRegister rhs, Label* label,
                                  DoubleCondition c, JumpKind jumpKind, FPConditionBit fcc)
{
    FloatTestKind testKind;
    compareFloatingPoint(SingleFloat, lhs, rhs, c, &testKind, fcc);
    asMasm().branchWithCode(getBranchCode(testKind, fcc), label, jumpKind);
}

void
MacroAssemblerMIPSShared::ma_bc1d(FloatRegister lhs, FloatRegister rhs, Label* label,
                                  DoubleCondition c, JumpKind jumpKind, FPConditionBit fcc)
{
    FloatTestKind testKind;
    compareFloatingPoint(DoubleFloat, lhs, rhs, c, &testKind, fcc);
    asMasm().branchWithCode(getBranchCode(testKind, fcc), label, jumpKind);
}

void
MacroAssemblerMIPSShared::minMaxDouble(FloatRegister srcDest, FloatRegister second,
                                       bool handleNaN, bool isMax)
{
    FloatRegister first = srcDest;

    Assembler::DoubleCondition cond = isMax
                                      ? Assembler::DoubleLessThanOrEqual
                                      : Assembler::DoubleGreaterThanOrEqual;
    Label nan, equal, done;
    FloatTestKind moveCondition;

    // First or second is NaN, result is NaN.
    ma_bc1d(first, second, &nan, Assembler::DoubleUnordered, ShortJump);
    // Make sure we handle -0 and 0 right.
    ma_bc1d(first, second, &equal, Assembler::DoubleEqual, ShortJump);
    compareFloatingPoint(DoubleFloat, first, second, cond, &moveCondition);
    MOZ_ASSERT(TestForTrue == moveCondition);
    as_movt(DoubleFloat, first, second);
    ma_b(&done, ShortJump);

    // Check for zero.
    bind(&equal);
    asMasm().loadConstantDouble(0.0, ScratchDoubleReg);
    compareFloatingPoint(DoubleFloat, first, ScratchDoubleReg,
                         Assembler::DoubleEqual, &moveCondition);

    // So now both operands are either -0 or 0.
    if (isMax) {
        // -0 + -0 = -0 and -0 + 0 = 0.
        as_addd(ScratchDoubleReg, first, second);
    } else {
        as_negd(ScratchDoubleReg, first);
        as_subd(ScratchDoubleReg, ScratchDoubleReg, second);
        as_negd(ScratchDoubleReg, ScratchDoubleReg);
    }
    MOZ_ASSERT(TestForTrue == moveCondition);
    // First is 0 or -0, move max/min to it, else just return it.
    as_movt(DoubleFloat, first, ScratchDoubleReg);
    ma_b(&done, ShortJump);

    bind(&nan);
    asMasm().loadConstantDouble(JS::GenericNaN(), srcDest);

    bind(&done);
}

void
MacroAssemblerMIPSShared::minMaxFloat32(FloatRegister srcDest, FloatRegister second,
                                        bool handleNaN, bool isMax)
{
    FloatRegister first = srcDest;

    Assembler::DoubleCondition cond = isMax
                                      ? Assembler::DoubleLessThanOrEqual
                                      : Assembler::DoubleGreaterThanOrEqual;
    Label nan, equal, done;
    FloatTestKind moveCondition;

    // First or second is NaN, result is NaN.
    ma_bc1s(first, second, &nan, Assembler::DoubleUnordered, ShortJump);
    // Make sure we handle -0 and 0 right.
    ma_bc1s(first, second, &equal, Assembler::DoubleEqual, ShortJump);
    compareFloatingPoint(SingleFloat, first, second, cond, &moveCondition);
    MOZ_ASSERT(TestForTrue == moveCondition);
    as_movt(SingleFloat, first, second);
    ma_b(&done, ShortJump);

    // Check for zero.
    bind(&equal);
    asMasm().loadConstantFloat32(0.0f, ScratchFloat32Reg);
    compareFloatingPoint(SingleFloat, first, ScratchFloat32Reg,
                         Assembler::DoubleEqual, &moveCondition);

    // So now both operands are either -0 or 0.
    if (isMax) {
        // -0 + -0 = -0 and -0 + 0 = 0.
        as_adds(ScratchFloat32Reg, first, second);
    } else {
        as_negs(ScratchFloat32Reg, first);
        as_subs(ScratchFloat32Reg, ScratchFloat32Reg, second);
        as_negs(ScratchFloat32Reg, ScratchFloat32Reg);
    }
    MOZ_ASSERT(TestForTrue == moveCondition);
    // First is 0 or -0, move max/min to it, else just return it.
    as_movt(SingleFloat, first, ScratchFloat32Reg);
    ma_b(&done, ShortJump);

    bind(&nan);
    asMasm().loadConstantFloat32(JS::GenericNaN(), srcDest);

    bind(&done);
}

void
MacroAssemblerMIPSShared::loadDouble(const Address& address, FloatRegister dest)
{
    asMasm().ma_ld(dest, address);
}

void
MacroAssemblerMIPSShared::loadDouble(const BaseIndex& src, FloatRegister dest)
{
    asMasm().ma_ld(dest, src);
}

void
MacroAssemblerMIPSShared::loadFloatAsDouble(const Address& address, FloatRegister dest)
{
    asMasm().ma_ls(dest, address);
    as_cvtds(dest, dest);
}

void
MacroAssemblerMIPSShared::loadFloatAsDouble(const BaseIndex& src, FloatRegister dest)
{
    asMasm().loadFloat32(src, dest);
    as_cvtds(dest, dest);
}

void
MacroAssemblerMIPSShared::loadFloat32(const Address& address, FloatRegister dest)
{
    asMasm().ma_ls(dest, address);
}

void
MacroAssemblerMIPSShared::loadFloat32(const BaseIndex& src, FloatRegister dest)
{
    asMasm().ma_ls(dest, src);
}

void
MacroAssemblerMIPSShared::ma_call(ImmPtr dest)
{
    asMasm().ma_liPatchable(CallReg, dest);
    as_jalr(CallReg);
    as_nop();
}

void
MacroAssemblerMIPSShared::ma_jump(ImmPtr dest)
{
    asMasm().ma_liPatchable(ScratchRegister, dest);
    as_jr(ScratchRegister);
    as_nop();
}

MacroAssembler&
MacroAssemblerMIPSShared::asMasm()
{
    return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler&
MacroAssemblerMIPSShared::asMasm() const
{
    return *static_cast<const MacroAssembler*>(this);
}

//{{{ check_macroassembler_style
// ===============================================================
// MacroAssembler high-level usage.

void
MacroAssembler::flush()
{
}

// ===============================================================
// Stack manipulation functions.

void
MacroAssembler::Push(Register reg)
{
    ma_push(reg);
    adjustFrame(int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Push(const Imm32 imm)
{
    ma_li(ScratchRegister, imm);
    ma_push(ScratchRegister);
    adjustFrame(int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Push(const ImmWord imm)
{
    ma_li(ScratchRegister, imm);
    ma_push(ScratchRegister);
    adjustFrame(int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Push(const ImmPtr imm)
{
    Push(ImmWord(uintptr_t(imm.value)));
}

void
MacroAssembler::Push(const ImmGCPtr ptr)
{
    ma_li(ScratchRegister, ptr);
    ma_push(ScratchRegister);
    adjustFrame(int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Push(FloatRegister f)
{
    ma_push(f);
    adjustFrame(int32_t(f.pushSize()));
}

void
MacroAssembler::Pop(Register reg)
{
    ma_pop(reg);
    adjustFrame(-int32_t(sizeof(intptr_t)));
}

void
MacroAssembler::Pop(FloatRegister f)
{
    ma_pop(f);
    adjustFrame(-int32_t(f.pushSize()));
}

void
MacroAssembler::Pop(const ValueOperand& val)
{
    popValue(val);
    adjustFrame(-int32_t(sizeof(Value)));
}

void
MacroAssembler::PopStackPtr()
{
    loadPtr(Address(StackPointer, 0), StackPointer);
    adjustFrame(-int32_t(sizeof(intptr_t)));
}


// ===============================================================
// Simple call functions.

CodeOffset
MacroAssembler::call(Register reg)
{
    as_jalr(reg);
    as_nop();
    return CodeOffset(currentOffset());
}

CodeOffset
MacroAssembler::call(Label* label)
{
    ma_bal(label);
    return CodeOffset(currentOffset());
}

CodeOffset
MacroAssembler::callWithPatch()
{
    as_bal(BOffImm16(3 * sizeof(uint32_t)));
    addPtr(Imm32(5 * sizeof(uint32_t)), ra);
    // Allocate space which will be patched by patchCall().
    spew(".space 32bit initValue 0xffff ffff");
    writeInst(UINT32_MAX);
    as_lw(ScratchRegister, ra, -(int32_t)(5 * sizeof(uint32_t)));
    addPtr(ra, ScratchRegister);
    as_jr(ScratchRegister);
    as_nop();
    return CodeOffset(currentOffset());
}

void
MacroAssembler::patchCall(uint32_t callerOffset, uint32_t calleeOffset)
{
    BufferOffset call(callerOffset - 7 * sizeof(uint32_t));

    BOffImm16 offset = BufferOffset(calleeOffset).diffB<BOffImm16>(call);
    if (!offset.isInvalid()) {
        InstImm* bal = (InstImm*)editSrc(call);
        bal->setBOffImm16(offset);
    } else {
        uint32_t u32Offset = callerOffset - 5 * sizeof(uint32_t);
        uint32_t* u32 = reinterpret_cast<uint32_t*>(editSrc(BufferOffset(u32Offset)));
        *u32 = calleeOffset - callerOffset;
    }
}

CodeOffset
MacroAssembler::farJumpWithPatch()
{
    ma_move(SecondScratchReg, ra);
    as_bal(BOffImm16(3 * sizeof(uint32_t)));
    as_lw(ScratchRegister, ra, 0);
    // Allocate space which will be patched by patchFarJump().
    CodeOffset farJump(currentOffset());
    spew(".space 32bit initValue 0xffff ffff");
    writeInst(UINT32_MAX);
    addPtr(ra, ScratchRegister);
    as_jr(ScratchRegister);
    ma_move(ra, SecondScratchReg);
    return farJump;
}

void
MacroAssembler::patchFarJump(CodeOffset farJump, uint32_t targetOffset)
{
    uint32_t* u32 = reinterpret_cast<uint32_t*>(editSrc(BufferOffset(farJump.offset())));
    MOZ_ASSERT(*u32 == UINT32_MAX);
    *u32 = targetOffset - farJump.offset();
}

void
MacroAssembler::repatchFarJump(uint8_t* code, uint32_t farJumpOffset, uint32_t targetOffset)
{
    uint32_t* u32 = reinterpret_cast<uint32_t*>(code + farJumpOffset);
    *u32 = targetOffset - farJumpOffset;
}

CodeOffset
MacroAssembler::nopPatchableToNearJump()
{
    CodeOffset offset(currentOffset());
    as_nop();
    as_nop();
    return offset;
}

void
MacroAssembler::patchNopToNearJump(uint8_t* jump, uint8_t* target)
{
    new (jump) InstImm(op_beq, zero, zero, BOffImm16(target - jump));
}

void
MacroAssembler::patchNearJumpToNop(uint8_t* jump)
{
    new (jump) InstNOP();
}

void
MacroAssembler::call(wasm::SymbolicAddress target)
{
    movePtr(target, CallReg);
    call(CallReg);
}

void
MacroAssembler::call(const Address& addr)
{
    loadPtr(addr, CallReg);
    call(CallReg);
}

void
MacroAssembler::call(ImmWord target)
{
    call(ImmPtr((void*)target.value));
}

void
MacroAssembler::call(ImmPtr target)
{
    BufferOffset bo = m_buffer.nextOffset();
    addPendingJump(bo, target, Relocation::HARDCODED);
    ma_call(target);
}

void
MacroAssembler::call(JitCode* c)
{
    BufferOffset bo = m_buffer.nextOffset();
    addPendingJump(bo, ImmPtr(c->raw()), Relocation::JITCODE);
    ma_liPatchable(ScratchRegister, ImmPtr(c->raw()));
    callJitNoProfiler(ScratchRegister);
}

CodeOffset
MacroAssembler::nopPatchableToCall(const wasm::CallSiteDesc& desc)
{
    CodeOffset offset(currentOffset());
                // MIPS32   //MIPS64
    as_nop();   // lui      // lui
    as_nop();   // ori      // ori
    as_nop();   // jalr     // drotr32
    as_nop();               // ori
#ifdef JS_CODEGEN_MIPS64
    as_nop();               //jalr
    as_nop();
#endif
    append(desc, CodeOffset(currentOffset()));
    return offset;
}

void
MacroAssembler::patchNopToCall(uint8_t* call, uint8_t* target)
{
#ifdef JS_CODEGEN_MIPS64
    Instruction* inst = (Instruction*) call - 6 /* six nops */;
    Assembler::WriteLoad64Instructions(inst, ScratchRegister, (uint64_t) target);
    inst[4] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
#else
    Instruction* inst = (Instruction*) call - 4 /* four nops */;
    Assembler::WriteLuiOriInstructions(inst, &inst[1], ScratchRegister, (uint32_t) target);
    inst[2] = InstReg(op_special, ScratchRegister, zero, ra, ff_jalr);
#endif
}

void
MacroAssembler::patchCallToNop(uint8_t* call)
{
#ifdef JS_CODEGEN_MIPS64
    Instruction* inst = (Instruction*) call - 6 /* six nops */;
#else
    Instruction* inst = (Instruction*) call - 4 /* four nops */;
#endif

    inst[0].makeNop();
    inst[1].makeNop();
    inst[2].makeNop();
    inst[3].makeNop();
#ifdef JS_CODEGEN_MIPS64
    inst[4].makeNop();
    inst[5].makeNop();
#endif
}

void
MacroAssembler::pushReturnAddress()
{
    push(ra);
}

void
MacroAssembler::popReturnAddress()
{
    pop(ra);
}

// ===============================================================
// Jit Frames.

uint32_t
MacroAssembler::pushFakeReturnAddress(Register scratch)
{
    CodeLabel cl;

    ma_li(scratch, &cl);
    Push(scratch);
    bind(&cl);
    uint32_t retAddr = currentOffset();

    addCodeLabel(cl);
    return retAddr;
}

void
MacroAssembler::loadStoreBuffer(Register ptr, Register buffer)
{
    if (ptr != buffer)
        movePtr(ptr, buffer);
    orPtr(Imm32(gc::ChunkMask), buffer);
    loadPtr(Address(buffer, gc::ChunkStoreBufferOffsetFromLastByte), buffer);
}

void
MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr, Register temp,
                                        Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(ptr != temp);
    MOZ_ASSERT(ptr != SecondScratchReg);

    movePtr(ptr, SecondScratchReg);
    orPtr(Imm32(gc::ChunkMask), SecondScratchReg);
    branch32(cond, Address(SecondScratchReg, gc::ChunkLocationOffsetFromLastByte),
             Imm32(int32_t(gc::ChunkLocation::Nursery)), label);
}

void
MacroAssembler::comment(const char* msg)
{
    Assembler::comment(msg);
}

// ===============================================================
// WebAssembly

CodeOffset
MacroAssembler::wasmTrapInstruction()
{
    CodeOffset offset(currentOffset());
    as_teq(zero, zero, WASM_TRAP);
    return offset;
}

void
MacroAssembler::wasmTruncateDoubleToInt32(FloatRegister input, Register output, bool isSaturating,
                                          Label* oolEntry)
{
    as_truncwd(ScratchFloat32Reg, input);
    as_cfc1(ScratchRegister, Assembler::FCSR);
    moveFromFloat32(ScratchFloat32Reg, output);
    ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
    ma_b(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);
}


void
MacroAssembler::wasmTruncateFloat32ToInt32(FloatRegister input, Register output, bool isSaturating,
                                           Label* oolEntry)
{
    as_truncws(ScratchFloat32Reg, input);
    as_cfc1(ScratchRegister, Assembler::FCSR);
    moveFromFloat32(ScratchFloat32Reg, output);
    ma_ext(ScratchRegister, ScratchRegister, Assembler::CauseV, 1);
    ma_b(ScratchRegister, Imm32(0), oolEntry, Assembler::NotEqual);
}

void
MacroAssembler::oolWasmTruncateCheckF32ToI32(FloatRegister input, Register output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    outOfLineWasmTruncateToInt32Check(input, output, MIRType::Float32, flags, rejoin, off);
}

void
MacroAssembler::oolWasmTruncateCheckF64ToI32(FloatRegister input, Register output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    outOfLineWasmTruncateToInt32Check(input, output, MIRType::Double, flags, rejoin, off);
}

void
MacroAssembler::oolWasmTruncateCheckF32ToI64(FloatRegister input, Register64 output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    outOfLineWasmTruncateToInt64Check(input, output, MIRType::Float32, flags, rejoin, off);
}

void
MacroAssembler::oolWasmTruncateCheckF64ToI64(FloatRegister input, Register64 output,
                                             TruncFlags flags, wasm::BytecodeOffset off,
                                             Label* rejoin)
{
    outOfLineWasmTruncateToInt64Check(input, output, MIRType::Double, flags, rejoin, off);
}

void
MacroAssemblerMIPSShared::outOfLineWasmTruncateToInt32Check(FloatRegister input, Register output,
                                                            MIRType fromType, TruncFlags flags,
                                                            Label* rejoin,
                                                            wasm::BytecodeOffset trapOffset)
{
    bool isUnsigned = flags & TRUNC_UNSIGNED;
    bool isSaturating = flags & TRUNC_SATURATING;

    if(isSaturating) {

        if(fromType == MIRType::Double)
            asMasm().loadConstantDouble(0.0, ScratchDoubleReg);
        else
            asMasm().loadConstantFloat32(0.0f, ScratchFloat32Reg);

        if(isUnsigned) {

            ma_li(output, Imm32(UINT32_MAX));

            FloatTestKind moveCondition;
            compareFloatingPoint(fromType == MIRType::Double ? DoubleFloat : SingleFloat,
                                 input,
                                 fromType == MIRType::Double ? ScratchDoubleReg : ScratchFloat32Reg,
                                 Assembler::DoubleLessThanOrUnordered, &moveCondition);
            MOZ_ASSERT(moveCondition == TestForTrue);

            as_movt(output, zero);
        } else {

            // Positive overflow is already saturated to INT32_MAX, so we only have
            // to handle NaN and negative overflow here.

            FloatTestKind moveCondition;
            compareFloatingPoint(fromType == MIRType::Double ? DoubleFloat : SingleFloat,
                                 input,
                                 input,
                                 Assembler::DoubleUnordered, &moveCondition);
            MOZ_ASSERT(moveCondition == TestForTrue);

            as_movt(output, zero);

            compareFloatingPoint(fromType == MIRType::Double ? DoubleFloat : SingleFloat,
                                 input,
                                 fromType == MIRType::Double ? ScratchDoubleReg : ScratchFloat32Reg,
                                 Assembler::DoubleLessThan, &moveCondition);
            MOZ_ASSERT(moveCondition == TestForTrue);

            ma_li(ScratchRegister, Imm32(INT32_MIN));
            as_movt(output, ScratchRegister);
        }

        MOZ_ASSERT(rejoin->bound());
        asMasm().jump(rejoin);
        return;
    }

    Label inputIsNaN;

    if (fromType == MIRType::Double)
        asMasm().branchDouble(Assembler::DoubleUnordered, input, input, &inputIsNaN);
    else if (fromType == MIRType::Float32)
        asMasm().branchFloat(Assembler::DoubleUnordered, input, input, &inputIsNaN);

    asMasm().wasmTrap(wasm::Trap::IntegerOverflow, trapOffset);
    asMasm().bind(&inputIsNaN);
    asMasm().wasmTrap(wasm::Trap::InvalidConversionToInteger, trapOffset);
}

void
MacroAssemblerMIPSShared::outOfLineWasmTruncateToInt64Check(FloatRegister input, Register64 output_,
                                                            MIRType fromType, TruncFlags flags,
                                                            Label* rejoin,
                                                            wasm::BytecodeOffset trapOffset)
{
    bool isUnsigned = flags & TRUNC_UNSIGNED;
    bool isSaturating = flags & TRUNC_SATURATING;


    if(isSaturating) {
#if defined(JS_CODEGEN_MIPS32)
        // Saturating callouts don't use ool path.
        return;
#else
        Register output = output_.reg;

        if(fromType == MIRType::Double)
            asMasm().loadConstantDouble(0.0, ScratchDoubleReg);
        else
            asMasm().loadConstantFloat32(0.0f, ScratchFloat32Reg);

        if(isUnsigned) {

            asMasm().ma_li(output, ImmWord(UINT64_MAX));

            FloatTestKind moveCondition;
            compareFloatingPoint(fromType == MIRType::Double ? DoubleFloat : SingleFloat,
                                 input,
                                 fromType == MIRType::Double ? ScratchDoubleReg : ScratchFloat32Reg,
                                 Assembler::DoubleLessThanOrUnordered, &moveCondition);
            MOZ_ASSERT(moveCondition == TestForTrue);

            as_movt(output, zero);
        } else {

            // Positive overflow is already saturated to INT64_MAX, so we only have
            // to handle NaN and negative overflow here.

            FloatTestKind moveCondition;
            compareFloatingPoint(fromType == MIRType::Double ? DoubleFloat : SingleFloat,
                                 input,
                                 input,
                                 Assembler::DoubleUnordered, &moveCondition);
            MOZ_ASSERT(moveCondition == TestForTrue);

            as_movt(output, zero);

            compareFloatingPoint(fromType == MIRType::Double ? DoubleFloat : SingleFloat,
                                 input,
                                 fromType == MIRType::Double ? ScratchDoubleReg : ScratchFloat32Reg,
                                 Assembler::DoubleLessThan, &moveCondition);
            MOZ_ASSERT(moveCondition == TestForTrue);

            asMasm().ma_li(ScratchRegister, ImmWord(INT64_MIN));
            as_movt(output, ScratchRegister);
        }

        MOZ_ASSERT(rejoin->bound());
        asMasm().jump(rejoin);
        return;
#endif

    }

    Label inputIsNaN;

    if (fromType == MIRType::Double)
        asMasm().branchDouble(Assembler::DoubleUnordered, input, input, &inputIsNaN);
    else if (fromType == MIRType::Float32)
        asMasm().branchFloat(Assembler::DoubleUnordered, input, input, &inputIsNaN);

#if defined(JS_CODEGEN_MIPS32)

    // Only possible valid input that produces INT64_MIN result.
    double validInput = isUnsigned ? double(uint64_t(INT64_MIN)) : double(int64_t(INT64_MIN));

    if (fromType == MIRType::Double) {
        asMasm().loadConstantDouble(validInput, ScratchDoubleReg);
        asMasm().branchDouble(Assembler::DoubleEqual, input, ScratchDoubleReg, rejoin);
    } else {
        asMasm().loadConstantFloat32(float(validInput), ScratchFloat32Reg);
        asMasm().branchFloat(Assembler::DoubleEqual, input, ScratchDoubleReg, rejoin);
    }

#endif

    asMasm().wasmTrap(wasm::Trap::IntegerOverflow, trapOffset);
    asMasm().bind(&inputIsNaN);
    asMasm().wasmTrap(wasm::Trap::InvalidConversionToInteger, trapOffset);
}

void
MacroAssembler::wasmLoad(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                         Register ptrScratch, AnyRegister output)
{
    wasmLoadImpl(access, memoryBase, ptr, ptrScratch, output, InvalidReg);
}

void
MacroAssembler::wasmUnalignedLoad(const wasm::MemoryAccessDesc& access, Register memoryBase,
                                  Register ptr, Register ptrScratch, Register output, Register tmp)
{
    wasmLoadImpl(access, memoryBase, ptr, ptrScratch, AnyRegister(output), tmp);
}

void
MacroAssembler::wasmUnalignedLoadFP(const wasm::MemoryAccessDesc& access, Register memoryBase,
                                    Register ptr, Register ptrScratch, FloatRegister output,
                                    Register tmp1, Register tmp2, Register tmp3)
{
    MOZ_ASSERT(tmp2 == InvalidReg);
    MOZ_ASSERT(tmp3 == InvalidReg);
    wasmLoadImpl(access, memoryBase, ptr, ptrScratch, AnyRegister(output), tmp1);
}

void
MacroAssembler::wasmStore(const wasm::MemoryAccessDesc& access, AnyRegister value,
                          Register memoryBase, Register ptr, Register ptrScratch)
{
    wasmStoreImpl(access, value, memoryBase, ptr, ptrScratch, InvalidReg);
}

void
MacroAssembler::wasmUnalignedStore(const wasm::MemoryAccessDesc& access, Register value,
                                   Register memoryBase, Register ptr, Register ptrScratch,
                                   Register tmp)
{
    wasmStoreImpl(access, AnyRegister(value), memoryBase, ptr, ptrScratch, tmp);
}

void
MacroAssembler::wasmUnalignedStoreFP(const wasm::MemoryAccessDesc& access, FloatRegister floatValue,
                                     Register memoryBase, Register ptr, Register ptrScratch,
                                     Register tmp)
{
    wasmStoreImpl(access, AnyRegister(floatValue), memoryBase, ptr, ptrScratch, tmp);
}

void
MacroAssemblerMIPSShared::wasmLoadImpl(const wasm::MemoryAccessDesc& access, Register memoryBase,
                                       Register ptr, Register ptrScratch, AnyRegister output,
                                       Register tmp)
{
    uint32_t offset = access.offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);
    MOZ_ASSERT_IF(offset, ptrScratch != InvalidReg);

    // Maybe add the offset.
    if (offset) {
        asMasm().addPtr(Imm32(offset), ptrScratch);
        ptr = ptrScratch;
    }

    unsigned byteSize = access.byteSize();
    bool isSigned;
    bool isFloat = false;

    switch (access.type()) {
      case Scalar::Int8:    isSigned = true;  break;
      case Scalar::Uint8:   isSigned = false; break;
      case Scalar::Int16:   isSigned = true;  break;
      case Scalar::Uint16:  isSigned = false; break;
      case Scalar::Int32:   isSigned = true;  break;
      case Scalar::Uint32:  isSigned = false; break;
      case Scalar::Float64: isFloat  = true;  break;
      case Scalar::Float32: isFloat  = true;  break;
      default: MOZ_CRASH("unexpected array type");
    }

    BaseIndex address(memoryBase, ptr, TimesOne);
    if (IsUnaligned(access)) {
        MOZ_ASSERT(tmp != InvalidReg);
        if (isFloat) {
            if (byteSize == 4)
                asMasm().loadUnalignedFloat32(access, address, tmp, output.fpu());
            else
                asMasm().loadUnalignedDouble(access, address, tmp, output.fpu());
        } else {
            asMasm().ma_load_unaligned(access, output.gpr(), address, tmp,
                                       static_cast<LoadStoreSize>(8 * byteSize),
                                       isSigned ? SignExtend : ZeroExtend);
        }
        return;
    }

    asMasm().memoryBarrierBefore(access.sync());
    if (isFloat) {
        if (byteSize == 4)
            asMasm().ma_ls(output.fpu(), address);
         else
            asMasm().ma_ld(output.fpu(), address);
    } else {
        asMasm().ma_load(output.gpr(), address, static_cast<LoadStoreSize>(8 * byteSize),
                         isSigned ? SignExtend : ZeroExtend);
    }
    asMasm().append(access, asMasm().size() - 4, asMasm().framePushed());
    asMasm().memoryBarrierAfter(access.sync());
}

void
MacroAssemblerMIPSShared::wasmStoreImpl(const wasm::MemoryAccessDesc& access, AnyRegister value,
                                        Register memoryBase, Register ptr, Register ptrScratch,
                                        Register tmp)
{
    uint32_t offset = access.offset();
    MOZ_ASSERT(offset < wasm::OffsetGuardLimit);
    MOZ_ASSERT_IF(offset, ptrScratch != InvalidReg);

    // Maybe add the offset.
    if (offset) {
        asMasm().addPtr(Imm32(offset), ptrScratch);
        ptr = ptrScratch;
    }

    unsigned byteSize = access.byteSize();
    bool isSigned;
    bool isFloat = false;

    switch (access.type()) {
      case Scalar::Int8:    isSigned = true;  break;
      case Scalar::Uint8:   isSigned = false; break;
      case Scalar::Int16:   isSigned = true;  break;
      case Scalar::Uint16:  isSigned = false; break;
      case Scalar::Int32:   isSigned = true;  break;
      case Scalar::Uint32:  isSigned = false; break;
      case Scalar::Int64:   isSigned = true;  break;
      case Scalar::Float64: isFloat  = true;  break;
      case Scalar::Float32: isFloat  = true;  break;
      default: MOZ_CRASH("unexpected array type");
    }

    BaseIndex address(memoryBase, ptr, TimesOne);
    if (IsUnaligned(access)) {
        MOZ_ASSERT(tmp != InvalidReg);
        if (isFloat) {
            if (byteSize == 4)
                asMasm().storeUnalignedFloat32(access, value.fpu(), tmp, address);
            else
                asMasm().storeUnalignedDouble(access, value.fpu(), tmp, address);
        } else {
            asMasm().ma_store_unaligned(access, value.gpr(), address, tmp,
                                        static_cast<LoadStoreSize>(8 * byteSize),
                                        isSigned ? SignExtend : ZeroExtend);
        }
        return;
    }

    asMasm().memoryBarrierBefore(access.sync());
    if (isFloat) {
        if (byteSize == 4)
            asMasm().ma_ss(value.fpu(), address);
        else
            asMasm().ma_sd(value.fpu(), address);
    } else {
        asMasm().ma_store(value.gpr(), address,
                      static_cast<LoadStoreSize>(8 * byteSize),
                      isSigned ? SignExtend : ZeroExtend);
    }
    // Only the last emitted instruction is a memory access.
    asMasm().append(access, asMasm().size() - 4, asMasm().framePushed());
    asMasm().memoryBarrierAfter(access.sync());
}

// ========================================================================
// Primitive atomic operations.

template<typename T>
static void
CompareExchange(MacroAssembler& masm, Scalar::Type type, const Synchronization& sync, const T& mem,
                Register oldval, Register newval, Register valueTemp, Register offsetTemp,
                Register maskTemp, Register output)
{
    bool signExtend = Scalar::isSignedIntType(type);
    unsigned nbytes = Scalar::byteSize(type);

     switch (nbytes) {
        case 1:
        case 2:
            break;
        case 4:
            MOZ_ASSERT(valueTemp == InvalidReg);
            MOZ_ASSERT(offsetTemp == InvalidReg);
            MOZ_ASSERT(maskTemp == InvalidReg);
            break;
        default:
            MOZ_CRASH();
    }

    Label again, end;

    masm.computeEffectiveAddress(mem, SecondScratchReg);

    if (nbytes == 4) {

        masm.memoryBarrierBefore(sync);
        masm.bind(&again);

        masm.as_ll(output, SecondScratchReg, 0);
        masm.ma_b(output, oldval, &end, Assembler::NotEqual, ShortJump);
        masm.ma_move(ScratchRegister, newval);
        masm.as_sc(ScratchRegister, SecondScratchReg, 0);
        masm.ma_b(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

        masm.memoryBarrierAfter(sync);
        masm.bind(&end);

        return;
    }

    masm.as_andi(offsetTemp, SecondScratchReg, 3);
    masm.subPtr(offsetTemp, SecondScratchReg);
#if !MOZ_LITTLE_ENDIAN
    masm.as_xori(offsetTemp, offsetTemp, 3);
#endif
    masm.as_sll(offsetTemp, offsetTemp, 3);
    masm.ma_li(maskTemp, Imm32(UINT32_MAX >> ((4 - nbytes) * 8)));
    masm.as_sllv(maskTemp, maskTemp, offsetTemp);
    masm.as_nor(maskTemp, zero, maskTemp);

    masm.memoryBarrierBefore(sync);

    masm.bind(&again);

    masm.as_ll(ScratchRegister, SecondScratchReg, 0);

    masm.as_srlv(output, ScratchRegister, offsetTemp);

    switch (nbytes) {
        case 1:
            if (signExtend) {
                masm.ma_seb(valueTemp, oldval);
                masm.ma_seb(output, output);
            } else {
                masm.as_andi(valueTemp, oldval, 0xff);
                masm.as_andi(output, output, 0xff);
            }
            break;
        case 2:
            if (signExtend) {
                masm.ma_seh(valueTemp, oldval);
                masm.ma_seh(output, output);
            } else {
                masm.as_andi(valueTemp, oldval, 0xffff);
                masm.as_andi(output, output, 0xffff);
            }
            break;
    }

    masm.ma_b(output, valueTemp, &end, Assembler::NotEqual, ShortJump);

    masm.as_sllv(valueTemp, newval, offsetTemp);
    masm.as_and(ScratchRegister, ScratchRegister, maskTemp);
    masm.as_or(ScratchRegister, ScratchRegister, valueTemp);

    masm.as_sc(ScratchRegister, SecondScratchReg, 0);

    masm.ma_b(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

    masm.memoryBarrierAfter(sync);

    masm.bind(&end);

}


void
MacroAssembler::compareExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                                Register oldval, Register newval, Register valueTemp,
                                Register offsetTemp, Register maskTemp, Register output)
{
    CompareExchange(*this, type, sync, mem, oldval, newval, valueTemp, offsetTemp, maskTemp,
                    output);
}

void
MacroAssembler::compareExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                                Register oldval, Register newval, Register valueTemp,
                                Register offsetTemp, Register maskTemp, Register output)
{
    CompareExchange(*this, type, sync, mem, oldval, newval, valueTemp, offsetTemp, maskTemp,
                    output);
}


template<typename T>
static void
AtomicExchange(MacroAssembler& masm, Scalar::Type type, const Synchronization& sync, const T& mem,
               Register value, Register valueTemp, Register offsetTemp, Register maskTemp,
               Register output)
{
    bool signExtend = Scalar::isSignedIntType(type);
    unsigned nbytes = Scalar::byteSize(type);

     switch (nbytes) {
        case 1:
        case 2:
            break;
        case 4:
            MOZ_ASSERT(valueTemp == InvalidReg);
            MOZ_ASSERT(offsetTemp == InvalidReg);
            MOZ_ASSERT(maskTemp == InvalidReg);
            break;
        default:
            MOZ_CRASH();
    }

    Label again;

    masm.computeEffectiveAddress(mem, SecondScratchReg);

    if (nbytes == 4) {

        masm.memoryBarrierBefore(sync);
        masm.bind(&again);

        masm.as_ll(output, SecondScratchReg, 0);
        masm.ma_move(ScratchRegister, value);
        masm.as_sc(ScratchRegister, SecondScratchReg, 0);
        masm.ma_b(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

        masm.memoryBarrierAfter(sync);

        return;
    }

    masm.as_andi(offsetTemp, SecondScratchReg, 3);
    masm.subPtr(offsetTemp, SecondScratchReg);
#if !MOZ_LITTLE_ENDIAN
    masm.as_xori(offsetTemp, offsetTemp, 3);
#endif
    masm.as_sll(offsetTemp, offsetTemp, 3);
    masm.ma_li(maskTemp, Imm32(UINT32_MAX >> ((4 - nbytes) * 8)));
    masm.as_sllv(maskTemp, maskTemp, offsetTemp);
    masm.as_nor(maskTemp, zero, maskTemp);
    switch (nbytes) {
        case 1:
            masm.as_andi(valueTemp, value, 0xff);
            break;
        case 2:
            masm.as_andi(valueTemp, value, 0xffff);
            break;
    }
    masm.as_sllv(valueTemp, valueTemp, offsetTemp);

    masm.memoryBarrierBefore(sync);

    masm.bind(&again);

    masm.as_ll(output, SecondScratchReg, 0);
    masm.as_and(ScratchRegister, output, maskTemp);
    masm.as_or(ScratchRegister, ScratchRegister, valueTemp);

    masm.as_sc(ScratchRegister, SecondScratchReg, 0);

    masm.ma_b(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

    masm.as_srlv(output, output, offsetTemp);

    switch (nbytes) {
        case 1:
            if (signExtend) {
                masm.ma_seb(output, output);
            } else {
                masm.as_andi(output, output, 0xff);
            }
            break;
        case 2:
            if (signExtend) {
                masm.ma_seh(output, output);
            } else {
                masm.as_andi(output, output, 0xffff);
            }
            break;
    }

    masm.memoryBarrierAfter(sync);
}


void
MacroAssembler::atomicExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                               Register value, Register valueTemp, Register offsetTemp,
                               Register maskTemp, Register output)
{
    AtomicExchange(*this, type, sync, mem, value, valueTemp, offsetTemp, maskTemp, output);
}

void
MacroAssembler::atomicExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                               Register value, Register valueTemp, Register offsetTemp,
                               Register maskTemp, Register output)
{
    AtomicExchange(*this, type, sync, mem, value, valueTemp, offsetTemp, maskTemp, output);
}


template<typename T>
static void
AtomicFetchOp(MacroAssembler& masm, Scalar::Type type, const Synchronization& sync,
              AtomicOp op, const T& mem, Register value, Register valueTemp,
              Register offsetTemp, Register maskTemp, Register output)
{
    bool signExtend = Scalar::isSignedIntType(type);
    unsigned nbytes = Scalar::byteSize(type);

     switch (nbytes) {
        case 1:
        case 2:
            break;
        case 4:
            MOZ_ASSERT(valueTemp == InvalidReg);
            MOZ_ASSERT(offsetTemp == InvalidReg);
            MOZ_ASSERT(maskTemp == InvalidReg);
            break;
        default:
            MOZ_CRASH();
    }

    Label again;

    masm.computeEffectiveAddress(mem, SecondScratchReg);

    if (nbytes == 4) {

        masm.memoryBarrierBefore(sync);
        masm.bind(&again);

        masm.as_ll(output, SecondScratchReg, 0);

        switch (op) {
        case AtomicFetchAddOp:
            masm.as_addu(ScratchRegister, output, value);
            break;
        case AtomicFetchSubOp:
            masm.as_subu(ScratchRegister, output, value);
            break;
        case AtomicFetchAndOp:
            masm.as_and(ScratchRegister, output, value);
            break;
        case AtomicFetchOrOp:
            masm.as_or(ScratchRegister, output, value);
            break;
        case AtomicFetchXorOp:
            masm.as_xor(ScratchRegister, output, value);
            break;
        default:
            MOZ_CRASH();
        }

        masm.as_sc(ScratchRegister, SecondScratchReg, 0);
        masm.ma_b(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

        masm.memoryBarrierAfter(sync);

        return;
    }


    masm.as_andi(offsetTemp, SecondScratchReg, 3);
    masm.subPtr(offsetTemp, SecondScratchReg);
#if !MOZ_LITTLE_ENDIAN
    masm.as_xori(offsetTemp, offsetTemp, 3);
#endif
    masm.as_sll(offsetTemp, offsetTemp, 3);
    masm.ma_li(maskTemp, Imm32(UINT32_MAX >> ((4 - nbytes) * 8)));
    masm.as_sllv(maskTemp, maskTemp, offsetTemp);
    masm.as_nor(maskTemp, zero, maskTemp);

    masm.memoryBarrierBefore(sync);

    masm.bind(&again);

    masm.as_ll(ScratchRegister, SecondScratchReg, 0);
    masm.as_srlv(output, ScratchRegister, offsetTemp);

    switch (op) {
        case AtomicFetchAddOp:
            masm.as_addu(valueTemp, output, value);
            break;
        case AtomicFetchSubOp:
            masm.as_subu(valueTemp, output, value);
            break;
        case AtomicFetchAndOp:
            masm.as_and(valueTemp, output, value);
            break;
        case AtomicFetchOrOp:
            masm.as_or(valueTemp, output, value);
            break;
        case AtomicFetchXorOp:
            masm.as_xor(valueTemp, output, value);
            break;
        default:
            MOZ_CRASH();
    }

    switch (nbytes) {
        case 1:
            masm.as_andi(valueTemp, valueTemp, 0xff);
            break;
        case 2:
            masm.as_andi(valueTemp, valueTemp, 0xffff);
            break;
    }

    masm.as_sllv(valueTemp, valueTemp, offsetTemp);

    masm.as_and(ScratchRegister, ScratchRegister, maskTemp);
    masm.as_or(ScratchRegister, ScratchRegister, valueTemp);

    masm.as_sc(ScratchRegister, SecondScratchReg, 0);

    masm.ma_b(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

    switch (nbytes) {
        case 1:
            if (signExtend) {
                masm.ma_seb(output, output);
            } else {
                masm.as_andi(output, output, 0xff);
            }
            break;
        case 2:
            if (signExtend) {
                masm.ma_seh(output, output);
            } else {
                masm.as_andi(output, output, 0xffff);
            }
            break;
    }

    masm.memoryBarrierAfter(sync);
}

void
MacroAssembler::atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                              Register value, const Address& mem, Register valueTemp,
                              Register offsetTemp, Register maskTemp, Register output)
{
    AtomicFetchOp(*this, type, sync, op, mem, value, valueTemp, offsetTemp, maskTemp, output);
}

void
MacroAssembler::atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                              Register value, const BaseIndex& mem, Register valueTemp,
                              Register offsetTemp, Register maskTemp, Register output)
{
    AtomicFetchOp(*this, type, sync, op, mem, value, valueTemp, offsetTemp, maskTemp, output);
}

template<typename T>
static void
AtomicEffectOp(MacroAssembler& masm, Scalar::Type type, const Synchronization& sync, AtomicOp op,
        const T& mem, Register value, Register valueTemp, Register offsetTemp, Register maskTemp)
{
    unsigned nbytes = Scalar::byteSize(type);

     switch (nbytes) {
        case 1:
        case 2:
            break;
        case 4:
            MOZ_ASSERT(valueTemp == InvalidReg);
            MOZ_ASSERT(offsetTemp == InvalidReg);
            MOZ_ASSERT(maskTemp == InvalidReg);
            break;
        default:
            MOZ_CRASH();
    }

    Label again;

    masm.computeEffectiveAddress(mem, SecondScratchReg);

    if (nbytes == 4) {

        masm.memoryBarrierBefore(sync);
        masm.bind(&again);

        masm.as_ll(ScratchRegister, SecondScratchReg, 0);

        switch (op) {
        case AtomicFetchAddOp:
            masm.as_addu(ScratchRegister, ScratchRegister, value);
            break;
        case AtomicFetchSubOp:
            masm.as_subu(ScratchRegister, ScratchRegister, value);
            break;
        case AtomicFetchAndOp:
            masm.as_and(ScratchRegister, ScratchRegister, value);
            break;
        case AtomicFetchOrOp:
            masm.as_or(ScratchRegister, ScratchRegister, value);
            break;
        case AtomicFetchXorOp:
            masm.as_xor(ScratchRegister, ScratchRegister, value);
            break;
        default:
            MOZ_CRASH();
        }

        masm.as_sc(ScratchRegister, SecondScratchReg, 0);
        masm.ma_b(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

        masm.memoryBarrierAfter(sync);

        return;
    }

    masm.as_andi(offsetTemp, SecondScratchReg, 3);
    masm.subPtr(offsetTemp, SecondScratchReg);
#if !MOZ_LITTLE_ENDIAN
    masm.as_xori(offsetTemp, offsetTemp, 3);
#endif
    masm.as_sll(offsetTemp, offsetTemp, 3);
    masm.ma_li(maskTemp, Imm32(UINT32_MAX >> ((4 - nbytes) * 8)));
    masm.as_sllv(maskTemp, maskTemp, offsetTemp);
    masm.as_nor(maskTemp, zero, maskTemp);

    masm.memoryBarrierBefore(sync);

    masm.bind(&again);

    masm.as_ll(ScratchRegister, SecondScratchReg, 0);
    masm.as_srlv(valueTemp, ScratchRegister, offsetTemp);

    switch (op) {
        case AtomicFetchAddOp:
            masm.as_addu(valueTemp, valueTemp, value);
            break;
        case AtomicFetchSubOp:
            masm.as_subu(valueTemp, valueTemp, value);
            break;
        case AtomicFetchAndOp:
            masm.as_and(valueTemp, valueTemp, value);
            break;
        case AtomicFetchOrOp:
            masm.as_or(valueTemp, valueTemp, value);
            break;
        case AtomicFetchXorOp:
            masm.as_xor(valueTemp, valueTemp, value);
            break;
        default:
            MOZ_CRASH();
    }

    switch (nbytes) {
        case 1:
            masm.as_andi(valueTemp, valueTemp, 0xff);
            break;
        case 2:
            masm.as_andi(valueTemp, valueTemp, 0xffff);
            break;
    }

    masm.as_sllv(valueTemp, valueTemp, offsetTemp);

    masm.as_and(ScratchRegister, ScratchRegister, maskTemp);
    masm.as_or(ScratchRegister, ScratchRegister, valueTemp);

    masm.as_sc(ScratchRegister, SecondScratchReg, 0);

    masm.ma_b(ScratchRegister, ScratchRegister, &again, Assembler::Zero, ShortJump);

    masm.memoryBarrierAfter(sync);
}


void
MacroAssembler::atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                               Register value, const Address& mem, Register valueTemp,
                               Register offsetTemp, Register maskTemp)
{
    AtomicEffectOp(*this, type, sync, op, mem, value, valueTemp, offsetTemp, maskTemp);
}

void
MacroAssembler::atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                               Register value, const BaseIndex& mem, Register valueTemp,
                               Register offsetTemp, Register maskTemp)
{
    AtomicEffectOp(*this, type, sync, op, mem, value, valueTemp, offsetTemp, maskTemp);
}

// ========================================================================
// JS atomic operations.

template<typename T>
static void
CompareExchangeJS(MacroAssembler& masm, Scalar::Type arrayType, const Synchronization& sync,
                  const T& mem, Register oldval, Register newval, Register valueTemp,
                  Register offsetTemp, Register maskTemp, Register temp, AnyRegister output)
{
    if (arrayType == Scalar::Uint32) {
        masm.compareExchange(arrayType, sync, mem, oldval, newval, valueTemp, offsetTemp, maskTemp,
                             temp);
        masm.convertUInt32ToDouble(temp, output.fpu());
    } else {
        masm.compareExchange(arrayType, sync, mem, oldval, newval, valueTemp, maskTemp, temp,
                             output.gpr());
    }
}

void
MacroAssembler::compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                  const Address& mem, Register oldval, Register newval,
                                  Register valueTemp, Register offsetTemp, Register maskTemp,
                                  Register temp, AnyRegister output)
{
    CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, valueTemp, offsetTemp, maskTemp,
                      temp, output);
}

void
MacroAssembler::compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                  const BaseIndex& mem, Register oldval, Register newval,
                                  Register valueTemp, Register offsetTemp, Register maskTemp,
                                  Register temp, AnyRegister output)
{
    CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval,valueTemp, offsetTemp, maskTemp,
                      temp, output);
}

template<typename T>
static void
AtomicExchangeJS(MacroAssembler& masm, Scalar::Type arrayType, const Synchronization& sync,
                 const T& mem, Register value, Register valueTemp,
                 Register offsetTemp, Register maskTemp, Register temp, AnyRegister output)
{
    if (arrayType == Scalar::Uint32) {
        masm.atomicExchange(arrayType, sync, mem, value, valueTemp, offsetTemp, maskTemp, temp);
        masm.convertUInt32ToDouble(temp, output.fpu());
    } else {
        masm.atomicExchange(arrayType, sync, mem, value, valueTemp, offsetTemp, maskTemp,
                            output.gpr());
    }
}

void
MacroAssembler::atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                 const Address& mem, Register value, Register valueTemp,
                                 Register offsetTemp, Register maskTemp, Register temp,
                                 AnyRegister output)
{
    AtomicExchangeJS(*this, arrayType, sync, mem, value, valueTemp, offsetTemp, maskTemp, temp,
                     output);
}

void
MacroAssembler::atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                                 const BaseIndex& mem, Register value, Register valueTemp,
                                 Register offsetTemp, Register maskTemp, Register temp,
                                 AnyRegister output)
{
    AtomicExchangeJS(*this, arrayType, sync, mem, value, valueTemp, offsetTemp, maskTemp, temp, output);
}

template<typename T>
static void
AtomicFetchOpJS(MacroAssembler& masm, Scalar::Type arrayType, const Synchronization& sync,
                AtomicOp op, Register value, const T& mem, Register valueTemp,
                Register offsetTemp, Register maskTemp, Register temp,
                AnyRegister output)
{
    if (arrayType == Scalar::Uint32) {
        masm.atomicFetchOp(arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp, temp);
        masm.convertUInt32ToDouble(temp, output.fpu());
    } else {
        masm.atomicFetchOp(arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp,
                           output.gpr());
    }
}

void
MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                                Register value, const Address& mem, Register valueTemp,
                                Register offsetTemp, Register maskTemp, Register temp,
                                AnyRegister output)
{
    AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp, temp,
                    output);
}

void
MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                                Register value, const BaseIndex& mem, Register valueTemp,
                                Register offsetTemp, Register maskTemp, Register temp,
                                AnyRegister output)
{
    AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp, temp,
                    output);
}

void
MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                                 Register value, const BaseIndex& mem, Register valueTemp,
                                 Register offsetTemp, Register maskTemp)
{
    atomicEffectOp(arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp);
}

void
MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                                 Register value, const Address& mem, Register valueTemp,
                                 Register offsetTemp, Register maskTemp)
{
    atomicEffectOp(arrayType, sync, op, value, mem, valueTemp, offsetTemp, maskTemp);
}


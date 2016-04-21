/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_MacroAssembler_arm_h
#define jit_arm_MacroAssembler_arm_h

#include "mozilla/DebugOnly.h"

#include "jsopcode.h"

#include "jit/arm/Assembler-arm.h"
#include "jit/AtomicOp.h"
#include "jit/IonCaches.h"
#include "jit/JitFrames.h"
#include "jit/MoveResolver.h"

using mozilla::DebugOnly;

namespace js {
namespace jit {

static Register CallReg = ip;
static const int defaultShift = 3;
JS_STATIC_ASSERT(1 << defaultShift == sizeof(JS::Value));

// MacroAssemblerARM is inheriting form Assembler defined in
// Assembler-arm.{h,cpp}
class MacroAssemblerARM : public Assembler
{
  private:
    // Perform a downcast. Should be removed by Bug 996602.
    MacroAssembler& asMasm();
    const MacroAssembler& asMasm() const;

  protected:
    // On ARM, some instructions require a second scratch register. This
    // register defaults to lr, since it's non-allocatable (as it can be
    // clobbered by some instructions). Allow the baseline compiler to override
    // this though, since baseline IC stubs rely on lr holding the return
    // address.
    Register secondScratchReg_;

  public:
    // Higher level tag testing code.
    // TODO: Can probably remove the Operand versions.
    Operand ToPayload(Operand base) const {
        return Operand(Register::FromCode(base.base()), base.disp());
    }
    Address ToPayload(const Address& base) const {
        return base;
    }

  protected:
    Operand ToType(Operand base) const {
        return Operand(Register::FromCode(base.base()), base.disp() + sizeof(void*));
    }
    Address ToType(const Address& base) const {
        return ToType(Operand(base)).toAddress();
    }

    Address ToPayloadAfterStackPush(const Address& base) const {
        // If we are based on StackPointer, pass over the type tag just pushed.
        if (base.base == StackPointer)
            return Address(base.base, base.offset + sizeof(void *));
        return ToPayload(base);
    }

  public:
    MacroAssemblerARM()
      : secondScratchReg_(lr)
    { }

    void setSecondScratchReg(Register reg) {
        MOZ_ASSERT(reg != ScratchRegister);
        secondScratchReg_ = reg;
    }

    void convertBoolToInt32(Register source, Register dest);
    void convertInt32ToDouble(Register src, FloatRegister dest);
    void convertInt32ToDouble(const Address& src, FloatRegister dest);
    void convertInt32ToDouble(const BaseIndex& src, FloatRegister dest);
    void convertUInt32ToFloat32(Register src, FloatRegister dest);
    void convertUInt32ToDouble(Register src, FloatRegister dest);
    void convertDoubleToFloat32(FloatRegister src, FloatRegister dest,
                                Condition c = Always);
    void branchTruncateDouble(FloatRegister src, Register dest, Label* fail);
    void convertDoubleToInt32(FloatRegister src, Register dest, Label* fail,
                              bool negativeZeroCheck = true);
    void convertFloat32ToInt32(FloatRegister src, Register dest, Label* fail,
                               bool negativeZeroCheck = true);

    void convertFloat32ToDouble(FloatRegister src, FloatRegister dest);
    void branchTruncateFloat32(FloatRegister src, Register dest, Label* fail);
    void convertInt32ToFloat32(Register src, FloatRegister dest);
    void convertInt32ToFloat32(const Address& src, FloatRegister dest);

    void addDouble(FloatRegister src, FloatRegister dest);
    void subDouble(FloatRegister src, FloatRegister dest);
    void mulDouble(FloatRegister src, FloatRegister dest);
    void divDouble(FloatRegister src, FloatRegister dest);

    void negateDouble(FloatRegister reg);
    void inc64(AbsoluteAddress dest);

    // Somewhat direct wrappers for the low-level assembler funcitons
    // bitops. Attempt to encode a virtual alu instruction using two real
    // instructions.
  private:
    bool alu_dbl(Register src1, Imm32 imm, Register dest, ALUOp op,
                 SBit s, Condition c);

  public:
    void ma_alu(Register src1, Operand2 op2, Register dest, ALUOp op,
                SBit s = LeaveCC, Condition c = Always);
    void ma_alu(Register src1, Imm32 imm, Register dest,
                ALUOp op,
                SBit s =  LeaveCC, Condition c = Always);

    void ma_alu(Register src1, Operand op2, Register dest, ALUOp op,
                SBit s = LeaveCC, Condition c = Always);
    void ma_nop();

    void ma_movPatchable(Imm32 imm, Register dest, Assembler::Condition c,
                         RelocStyle rs);
    void ma_movPatchable(ImmPtr imm, Register dest, Assembler::Condition c,
                         RelocStyle rs);

    static void ma_mov_patch(Imm32 imm, Register dest, Assembler::Condition c,
                             RelocStyle rs, Instruction* i);
    static void ma_mov_patch(ImmPtr imm, Register dest, Assembler::Condition c,
                             RelocStyle rs, Instruction* i);

    // These should likely be wrapped up as a set of macros or something like
    // that. I cannot think of a good reason to explicitly have all of this
    // code.

    // ALU based ops
    // mov
    void ma_mov(Register src, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_mov(Imm32 imm, Register dest,
                SBit s = LeaveCC, Condition c = Always);
    void ma_mov(ImmWord imm, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_mov(ImmGCPtr ptr, Register dest);

    // Shifts (just a move with a shifting op2)
    void ma_lsl(Imm32 shift, Register src, Register dst);
    void ma_lsr(Imm32 shift, Register src, Register dst);
    void ma_asr(Imm32 shift, Register src, Register dst);
    void ma_ror(Imm32 shift, Register src, Register dst);
    void ma_rol(Imm32 shift, Register src, Register dst);
    // Shifts (just a move with a shifting op2)
    void ma_lsl(Register shift, Register src, Register dst);
    void ma_lsr(Register shift, Register src, Register dst);
    void ma_asr(Register shift, Register src, Register dst);
    void ma_ror(Register shift, Register src, Register dst);
    void ma_rol(Register shift, Register src, Register dst);

    // Move not (dest <- ~src)
    void ma_mvn(Imm32 imm, Register dest,
                SBit s = LeaveCC, Condition c = Always);


    void ma_mvn(Register src1, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    // Negate (dest <- -src) implemented as rsb dest, src, 0
    void ma_neg(Register src, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    // And
    void ma_and(Register src, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_and(Register src1, Register src2, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_and(Imm32 imm, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_and(Imm32 imm, Register src1, Register dest,
                SBit s = LeaveCC, Condition c = Always);



    // Bit clear (dest <- dest & ~imm) or (dest <- src1 & ~src2)
    void ma_bic(Imm32 imm, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    // Exclusive or
    void ma_eor(Register src, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_eor(Register src1, Register src2, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_eor(Imm32 imm, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_eor(Imm32 imm, Register src1, Register dest,
                SBit s = LeaveCC, Condition c = Always);


    // Or
    void ma_orr(Register src, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_orr(Register src1, Register src2, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_orr(Imm32 imm, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_orr(Imm32 imm, Register src1, Register dest,
                SBit s = LeaveCC, Condition c = Always);


    // Arithmetic based ops.
    // Add with carry:
    void ma_adc(Imm32 imm, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_adc(Register src, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_adc(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Add:
    void ma_add(Imm32 imm, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_add(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_add(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_add(Register src1, Operand op, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_add(Register src1, Imm32 op, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Subtract with carry:
    void ma_sbc(Imm32 imm, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sbc(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sbc(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Subtract:
    void ma_sub(Imm32 imm, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sub(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sub(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sub(Register src1, Operand op, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sub(Register src1, Imm32 op, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Reverse subtract:
    void ma_rsb(Imm32 imm, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_rsb(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_rsb(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_rsb(Register src1, Imm32 op2, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Reverse subtract with carry:
    void ma_rsc(Imm32 imm, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_rsc(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_rsc(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Compares/tests.
    // Compare negative (sets condition codes as src1 + src2 would):
    void ma_cmn(Register src1, Imm32 imm, Condition c = Always);
    void ma_cmn(Register src1, Register src2, Condition c = Always);
    void ma_cmn(Register src1, Operand op, Condition c = Always);

    // Compare (src - src2):
    void ma_cmp(Register src1, Imm32 imm, Condition c = Always);
    void ma_cmp(Register src1, ImmWord ptr, Condition c = Always);
    void ma_cmp(Register src1, ImmGCPtr ptr, Condition c = Always);
    void ma_cmp(Register src1, Operand op, Condition c = Always);
    void ma_cmp(Register src1, Register src2, Condition c = Always);


    // Test for equality, (src1 ^ src2):
    void ma_teq(Register src1, Imm32 imm, Condition c = Always);
    void ma_teq(Register src1, Register src2, Condition c = Always);
    void ma_teq(Register src1, Operand op, Condition c = Always);


    // Test (src1 & src2):
    void ma_tst(Register src1, Imm32 imm, Condition c = Always);
    void ma_tst(Register src1, Register src2, Condition c = Always);
    void ma_tst(Register src1, Operand op, Condition c = Always);

    // Multiplies. For now, there are only two that we care about.
    void ma_mul(Register src1, Register src2, Register dest);
    void ma_mul(Register src1, Imm32 imm, Register dest);
    Condition ma_check_mul(Register src1, Register src2, Register dest, Condition cond);
    Condition ma_check_mul(Register src1, Imm32 imm, Register dest, Condition cond);

    // Fast mod, uses scratch registers, and thus needs to be in the assembler
    // implicitly assumes that we can overwrite dest at the beginning of the
    // sequence.
    void ma_mod_mask(Register src, Register dest, Register hold, Register tmp,
                     int32_t shift);

    // Mod - depends on integer divide instructions being supported.
    void ma_smod(Register num, Register div, Register dest);
    void ma_umod(Register num, Register div, Register dest);

    // Division - depends on integer divide instructions being supported.
    void ma_sdiv(Register num, Register div, Register dest, Condition cond = Always);
    void ma_udiv(Register num, Register div, Register dest, Condition cond = Always);
    // Misc operations
    void ma_clz(Register src, Register dest, Condition cond = Always);
    // Memory:
    // Shortcut for when we know we're transferring 32 bits of data.
    void ma_dtr(LoadStore ls, Register rn, Imm32 offset, Register rt,
                Index mode = Offset, Condition cc = Always);

    void ma_dtr(LoadStore ls, Register rn, Register rm, Register rt,
                Index mode = Offset, Condition cc = Always);


    void ma_str(Register rt, DTRAddr addr, Index mode = Offset, Condition cc = Always);
    void ma_str(Register rt, const Address& addr, Index mode = Offset, Condition cc = Always);
    void ma_dtr(LoadStore ls, Register rt, const Address& addr, Index mode, Condition cc);

    void ma_ldr(DTRAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldr(const Address& addr, Register rt, Index mode = Offset, Condition cc = Always);

    void ma_ldrb(DTRAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldrh(EDtrAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldrsh(EDtrAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldrsb(EDtrAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldrd(EDtrAddr addr, Register rt, DebugOnly<Register> rt2, Index mode = Offset, Condition cc = Always);
    void ma_strb(Register rt, DTRAddr addr, Index mode = Offset, Condition cc = Always);
    void ma_strh(Register rt, EDtrAddr addr, Index mode = Offset, Condition cc = Always);
    void ma_strd(Register rt, DebugOnly<Register> rt2, EDtrAddr addr, Index mode = Offset, Condition cc = Always);
    // Specialty for moving N bits of data, where n == 8,16,32,64.
    BufferOffset ma_dataTransferN(LoadStore ls, int size, bool IsSigned,
                          Register rn, Register rm, Register rt,
                          Index mode = Offset, Condition cc = Always, unsigned scale = TimesOne);

    BufferOffset ma_dataTransferN(LoadStore ls, int size, bool IsSigned,
                          Register rn, Imm32 offset, Register rt,
                          Index mode = Offset, Condition cc = Always);
    void ma_pop(Register r);
    void ma_push(Register r);

    void ma_vpop(VFPRegister r);
    void ma_vpush(VFPRegister r);

    // Barriers.
    void ma_dmb(BarrierOption option=BarrierSY);
    void ma_dsb(BarrierOption option=BarrierSY);

    // Branches when done from within arm-specific code.
    BufferOffset ma_b(Label* dest, Condition c = Always);
    void ma_b(void* target, Condition c = Always);
    void ma_bx(Register dest, Condition c = Always);

    // This is almost NEVER necessary, we'll basically never be calling a label
    // except, possibly in the crazy bailout-table case.
    void ma_bl(Label* dest, Condition c = Always);

    void ma_blx(Register dest, Condition c = Always);

    // VFP/ALU:
    void ma_vadd(FloatRegister src1, FloatRegister src2, FloatRegister dst);
    void ma_vsub(FloatRegister src1, FloatRegister src2, FloatRegister dst);

    void ma_vmul(FloatRegister src1, FloatRegister src2, FloatRegister dst);
    void ma_vdiv(FloatRegister src1, FloatRegister src2, FloatRegister dst);

    void ma_vneg(FloatRegister src, FloatRegister dest, Condition cc = Always);
    void ma_vmov(FloatRegister src, FloatRegister dest, Condition cc = Always);
    void ma_vmov_f32(FloatRegister src, FloatRegister dest, Condition cc = Always);
    void ma_vabs(FloatRegister src, FloatRegister dest, Condition cc = Always);
    void ma_vabs_f32(FloatRegister src, FloatRegister dest, Condition cc = Always);

    void ma_vsqrt(FloatRegister src, FloatRegister dest, Condition cc = Always);
    void ma_vsqrt_f32(FloatRegister src, FloatRegister dest, Condition cc = Always);

    void ma_vimm(double value, FloatRegister dest, Condition cc = Always);
    void ma_vimm_f32(float value, FloatRegister dest, Condition cc = Always);

    void ma_vcmp(FloatRegister src1, FloatRegister src2, Condition cc = Always);
    void ma_vcmp_f32(FloatRegister src1, FloatRegister src2, Condition cc = Always);
    void ma_vcmpz(FloatRegister src1, Condition cc = Always);
    void ma_vcmpz_f32(FloatRegister src1, Condition cc = Always);

    void ma_vadd_f32(FloatRegister src1, FloatRegister src2, FloatRegister dst);
    void ma_vsub_f32(FloatRegister src1, FloatRegister src2, FloatRegister dst);

    void ma_vmul_f32(FloatRegister src1, FloatRegister src2, FloatRegister dst);
    void ma_vdiv_f32(FloatRegister src1, FloatRegister src2, FloatRegister dst);

    void ma_vneg_f32(FloatRegister src, FloatRegister dest, Condition cc = Always);

    // Source is F64, dest is I32:
    void ma_vcvt_F64_I32(FloatRegister src, FloatRegister dest, Condition cc = Always);
    void ma_vcvt_F64_U32(FloatRegister src, FloatRegister dest, Condition cc = Always);

    // Source is I32, dest is F64:
    void ma_vcvt_I32_F64(FloatRegister src, FloatRegister dest, Condition cc = Always);
    void ma_vcvt_U32_F64(FloatRegister src, FloatRegister dest, Condition cc = Always);

    // Source is F32, dest is I32:
    void ma_vcvt_F32_I32(FloatRegister src, FloatRegister dest, Condition cc = Always);
    void ma_vcvt_F32_U32(FloatRegister src, FloatRegister dest, Condition cc = Always);

    // Source is I32, dest is F32:
    void ma_vcvt_I32_F32(FloatRegister src, FloatRegister dest, Condition cc = Always);
    void ma_vcvt_U32_F32(FloatRegister src, FloatRegister dest, Condition cc = Always);


    // Transfer (do not coerce) a float into a gpr.
    void ma_vxfer(VFPRegister src, Register dest, Condition cc = Always);
    // Transfer (do not coerce) a double into a couple of gpr.
    void ma_vxfer(VFPRegister src, Register dest1, Register dest2, Condition cc = Always);

    // Transfer (do not coerce) a gpr into a float
    void ma_vxfer(Register src, FloatRegister dest, Condition cc = Always);
    // Transfer (do not coerce) a couple of gpr into a double
    void ma_vxfer(Register src1, Register src2, FloatRegister dest, Condition cc = Always);

    BufferOffset ma_vdtr(LoadStore ls, const Address& addr, VFPRegister dest, Condition cc = Always);


    BufferOffset ma_vldr(VFPAddr addr, VFPRegister dest, Condition cc = Always);
    BufferOffset ma_vldr(const Address& addr, VFPRegister dest, Condition cc = Always);
    BufferOffset ma_vldr(VFPRegister src, Register base, Register index, int32_t shift = defaultShift, Condition cc = Always);

    BufferOffset ma_vstr(VFPRegister src, VFPAddr addr, Condition cc = Always);
    BufferOffset ma_vstr(VFPRegister src, const Address& addr, Condition cc = Always);

    BufferOffset ma_vstr(VFPRegister src, Register base, Register index, int32_t shift,
                         int32_t offset, Condition cc = Always);

    void ma_call(ImmPtr dest);

    // Float registers can only be loaded/stored in continuous runs when using
    // vstm/vldm. This function breaks set into continuous runs and loads/stores
    // them at [rm]. rm will be modified and left in a state logically suitable
    // for the next load/store. Returns the offset from [dm] for the logical
    // next load/store.
    int32_t transferMultipleByRuns(FloatRegisterSet set, LoadStore ls,
                                   Register rm, DTMMode mode)
    {
        if (mode == IA) {
            return transferMultipleByRunsImpl
                <FloatRegisterForwardIterator>(set, ls, rm, mode, 1);
        }
        if (mode == DB) {
            return transferMultipleByRunsImpl
                <FloatRegisterBackwardIterator>(set, ls, rm, mode, -1);
        }
        MOZ_CRASH("Invalid data transfer addressing mode");
    }

private:
    // Implementation for transferMultipleByRuns so we can use different
    // iterators for forward/backward traversals. The sign argument should be 1
    // if we traverse forwards, -1 if we traverse backwards.
    template<typename RegisterIterator> int32_t
    transferMultipleByRunsImpl(FloatRegisterSet set, LoadStore ls,
                               Register rm, DTMMode mode, int32_t sign)
    {
        MOZ_ASSERT(sign == 1 || sign == -1);

        int32_t delta = sign * sizeof(float);
        int32_t offset = 0;
        // Build up a new set, which is the sum of all of the single and double
        // registers. This set can have up to 48 registers in it total
        // s0-s31 and d16-d31
        FloatRegisterSet mod = set.reduceSetForPush();

        RegisterIterator iter(mod);
        while (iter.more()) {
            startFloatTransferM(ls, rm, mode, WriteBack);
            int32_t reg = (*iter).code();
            do {
                offset += delta;
                if ((*iter).isDouble())
                    offset += delta;
                transferFloatReg(*iter);
            } while ((++iter).more() && int32_t((*iter).code()) == (reg += sign));
            finishFloatTransfer();
        }
        return offset;
    }
};

class MacroAssembler;

class MacroAssemblerARMCompat : public MacroAssemblerARM
{
  private:
    // Perform a downcast. Should be removed by Bug 996602.
    MacroAssembler& asMasm();
    const MacroAssembler& asMasm() const;

  public:
    MacroAssemblerARMCompat()
    { }

  public:

    // Jumps + other functions that should be called from non-arm specific
    // code. Basically, an x86 front end on top of the ARM code.
    void j(Condition code , Label* dest)
    {
        as_b(dest, code);
    }
    void j(Label* dest)
    {
        as_b(dest, Always);
    }

    void mov(Register src, Register dest) {
        ma_mov(src, dest);
    }
    void mov(ImmWord imm, Register dest) {
        ma_mov(Imm32(imm.value), dest);
    }
    void mov(ImmPtr imm, Register dest) {
        mov(ImmWord(uintptr_t(imm.value)), dest);
    }
    void mov(Register src, Address dest) {
        MOZ_CRASH("NYI-IC");
    }
    void mov(Address src, Register dest) {
        MOZ_CRASH("NYI-IC");
    }

    void branch(JitCode* c) {
        BufferOffset bo = m_buffer.nextOffset();
        addPendingJump(bo, ImmPtr(c->raw()), Relocation::JITCODE);
        RelocStyle rs;
        if (HasMOVWT())
            rs = L_MOVWT;
        else
            rs = L_LDR;

        ScratchRegisterScope scratch(asMasm());
        ma_movPatchable(ImmPtr(c->raw()), scratch, Always, rs);
        ma_bx(scratch);
    }
    void branch(const Register reg) {
        ma_bx(reg);
    }
    void nop() {
        ma_nop();
    }
    void shortJumpSizedNop() {
        ma_nop();
    }
    void ret() {
        ma_pop(pc);
    }
    void retn(Imm32 n) {
        // pc <- [sp]; sp += n
        ma_dtr(IsLoad, sp, n, pc, PostIndex);
    }
    void push(Imm32 imm) {
        ScratchRegisterScope scratch(asMasm());
        ma_mov(imm, scratch);
        ma_push(scratch);
    }
    void push(ImmWord imm) {
        push(Imm32(imm.value));
    }
    void push(ImmGCPtr imm) {
        ScratchRegisterScope scratch(asMasm());
        ma_mov(imm, scratch);
        ma_push(scratch);
    }
    void push(const Address& addr) {
        ScratchRegisterScope scratch(asMasm());
        ma_ldr(addr, scratch);
        ma_push(scratch);
    }
    void push(Register reg) {
        ma_push(reg);
    }
    void push(FloatRegister reg) {
        ma_vpush(VFPRegister(reg));
    }
    void pushWithPadding(Register reg, const Imm32 extraSpace) {
        Imm32 totSpace = Imm32(extraSpace.value + 4);
        ma_dtr(IsStore, sp, totSpace, reg, PreIndex);
    }
    void pushWithPadding(Imm32 imm, const Imm32 extraSpace) {
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        Imm32 totSpace = Imm32(extraSpace.value + 4);
        // ma_dtr may need the scratch register to adjust the stack, so use the
        // second scratch register.
        ma_mov(imm, scratch2);
        ma_dtr(IsStore, sp, totSpace, scratch2, PreIndex);
    }

    void pop(Register reg) {
        ma_pop(reg);
    }
    void pop(FloatRegister reg) {
        ma_vpop(VFPRegister(reg));
    }

    void popN(Register reg, Imm32 extraSpace) {
        Imm32 totSpace = Imm32(extraSpace.value + 4);
        ma_dtr(IsLoad, sp, totSpace, reg, PostIndex);
    }

    CodeOffset toggledJump(Label* label);

    // Emit a BLX or NOP instruction. ToggleCall can be used to patch this
    // instruction.
    CodeOffset toggledCall(JitCode* target, bool enabled);

    CodeOffset pushWithPatch(ImmWord imm) {
        ScratchRegisterScope scratch(asMasm());
        CodeOffset label = movWithPatch(imm, scratch);
        ma_push(scratch);
        return label;
    }

    CodeOffset movWithPatch(ImmWord imm, Register dest) {
        CodeOffset label = CodeOffset(currentOffset());
        ma_movPatchable(Imm32(imm.value), dest, Always, HasMOVWT() ? L_MOVWT : L_LDR);
        return label;
    }
    CodeOffset movWithPatch(ImmPtr imm, Register dest) {
        return movWithPatch(ImmWord(uintptr_t(imm.value)), dest);
    }

    void jump(Label* label) {
        as_b(label);
    }
    void jump(JitCode* code) {
        branch(code);
    }
    void jump(Register reg) {
        ma_bx(reg);
    }
    void jump(const Address& addr) {
        ScratchRegisterScope scratch(asMasm());
        ma_ldr(addr, scratch);
        ma_bx(scratch);
    }

    void neg32(Register reg) {
        ma_neg(reg, reg, SetCC);
    }
    void negl(Register reg) {
        ma_neg(reg, reg, SetCC);
    }
    void test32(Register lhs, Register rhs) {
        ma_tst(lhs, rhs);
    }
    void test32(Register lhs, Imm32 imm) {
        ma_tst(lhs, imm);
    }
    void test32(const Address& addr, Imm32 imm) {
        ScratchRegisterScope scratch(asMasm());
        ma_ldr(addr, scratch);
        ma_tst(scratch, imm);
    }
    void testPtr(Register lhs, Register rhs) {
        test32(lhs, rhs);
    }

    // Returns the register containing the type tag.
    Register splitTagForTest(const ValueOperand& value) {
        return value.typeReg();
    }

    // Higher level tag testing code.
    Condition testInt32(Condition cond, const ValueOperand& value);
    Condition testBoolean(Condition cond, const ValueOperand& value);
    Condition testDouble(Condition cond, const ValueOperand& value);
    Condition testNull(Condition cond, const ValueOperand& value);
    Condition testUndefined(Condition cond, const ValueOperand& value);
    Condition testString(Condition cond, const ValueOperand& value);
    Condition testSymbol(Condition cond, const ValueOperand& value);
    Condition testObject(Condition cond, const ValueOperand& value);
    Condition testNumber(Condition cond, const ValueOperand& value);
    Condition testMagic(Condition cond, const ValueOperand& value);

    Condition testPrimitive(Condition cond, const ValueOperand& value);

    // Register-based tests.
    Condition testInt32(Condition cond, Register tag);
    Condition testBoolean(Condition cond, Register tag);
    Condition testNull(Condition cond, Register tag);
    Condition testUndefined(Condition cond, Register tag);
    Condition testString(Condition cond, Register tag);
    Condition testSymbol(Condition cond, Register tag);
    Condition testObject(Condition cond, Register tag);
    Condition testDouble(Condition cond, Register tag);
    Condition testNumber(Condition cond, Register tag);
    Condition testMagic(Condition cond, Register tag);
    Condition testPrimitive(Condition cond, Register tag);

    Condition testGCThing(Condition cond, const Address& address);
    Condition testMagic(Condition cond, const Address& address);
    Condition testInt32(Condition cond, const Address& address);
    Condition testDouble(Condition cond, const Address& address);
    Condition testBoolean(Condition cond, const Address& address);
    Condition testNull(Condition cond, const Address& address);
    Condition testUndefined(Condition cond, const Address& address);
    Condition testString(Condition cond, const Address& address);
    Condition testSymbol(Condition cond, const Address& address);
    Condition testObject(Condition cond, const Address& address);
    Condition testNumber(Condition cond, const Address& address);

    Condition testUndefined(Condition cond, const BaseIndex& src);
    Condition testNull(Condition cond, const BaseIndex& src);
    Condition testBoolean(Condition cond, const BaseIndex& src);
    Condition testString(Condition cond, const BaseIndex& src);
    Condition testSymbol(Condition cond, const BaseIndex& src);
    Condition testInt32(Condition cond, const BaseIndex& src);
    Condition testObject(Condition cond, const BaseIndex& src);
    Condition testDouble(Condition cond, const BaseIndex& src);
    Condition testMagic(Condition cond, const BaseIndex& src);
    Condition testGCThing(Condition cond, const BaseIndex& src);

    template <typename T>
    void branchTestGCThing(Condition cond, const T& t, Label* label) {
        Condition c = testGCThing(cond, t);
        ma_b(label, c);
    }
    template <typename T>
    void branchTestPrimitive(Condition cond, const T& t, Label* label) {
        Condition c = testPrimitive(cond, t);
        ma_b(label, c);
    }

    void branchTestValue(Condition cond, const ValueOperand& value, const Value& v, Label* label);
    void branchTestValue(Condition cond, const Address& valaddr, const ValueOperand& value,
                         Label* label);

    // Unboxing code.
    void unboxNonDouble(const ValueOperand& operand, Register dest);
    void unboxNonDouble(const Address& src, Register dest);
    void unboxNonDouble(const BaseIndex& src, Register dest);
    void unboxInt32(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxInt32(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxBoolean(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxBoolean(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxString(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxString(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxSymbol(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxSymbol(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxObject(const ValueOperand& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxObject(const Address& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxObject(const BaseIndex& src, Register dest) { unboxNonDouble(src, dest); }
    void unboxDouble(const ValueOperand& src, FloatRegister dest);
    void unboxDouble(const Address& src, FloatRegister dest);
    void unboxValue(const ValueOperand& src, AnyRegister dest);
    void unboxPrivate(const ValueOperand& src, Register dest);

    void notBoolean(const ValueOperand& val) {
        ma_eor(Imm32(1), val.payloadReg());
    }

    // Boxing code.
    void boxDouble(FloatRegister src, const ValueOperand& dest);
    void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest);

    // Extended unboxing API. If the payload is already in a register, returns
    // that register. Otherwise, provides a move to the given scratch register,
    // and returns that.
    Register extractObject(const Address& address, Register scratch);
    Register extractObject(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractInt32(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractBoolean(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractTag(const Address& address, Register scratch);
    Register extractTag(const BaseIndex& address, Register scratch);
    Register extractTag(const ValueOperand& value, Register scratch) {
        return value.typeReg();
    }

    void boolValueToDouble(const ValueOperand& operand, FloatRegister dest);
    void int32ValueToDouble(const ValueOperand& operand, FloatRegister dest);
    void loadInt32OrDouble(const Address& src, FloatRegister dest);
    void loadInt32OrDouble(Register base, Register index,
                           FloatRegister dest, int32_t shift = defaultShift);
    void loadConstantDouble(double dp, FloatRegister dest);
    // Treat the value as a boolean, and set condition codes accordingly.
    Condition testInt32Truthy(bool truthy, const ValueOperand& operand);
    Condition testBooleanTruthy(bool truthy, const ValueOperand& operand);
    Condition testDoubleTruthy(bool truthy, FloatRegister reg);
    Condition testStringTruthy(bool truthy, const ValueOperand& value);

    void boolValueToFloat32(const ValueOperand& operand, FloatRegister dest);
    void int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest);
    void loadConstantFloat32(float f, FloatRegister dest);

    template<typename T>
    void branchTestInt32(Condition cond, const T & t, Label* label) {
        Condition c = testInt32(cond, t);
        ma_b(label, c);
    }
    template<typename T>
    void branchTestBoolean(Condition cond, const T & t, Label* label) {
        Condition c = testBoolean(cond, t);
        ma_b(label, c);
    }
    void branch32(Condition cond, Register lhs, Register rhs, Label* label) {
        ma_cmp(lhs, rhs);
        ma_b(label, cond);
    }
    void branch32(Condition cond, Register lhs, Imm32 imm, Label* label) {
        ma_cmp(lhs, imm);
        ma_b(label, cond);
    }
    void branch32(Condition cond, const Operand& lhs, Register rhs, Label* label) {
        if (lhs.getTag() == Operand::OP2) {
            branch32(cond, lhs.toReg(), rhs, label);
        } else {
            ScratchRegisterScope scratch(asMasm());
            ma_ldr(lhs.toAddress(), scratch);
            branch32(cond, scratch, rhs, label);
        }
    }
    void branch32(Condition cond, const Operand& lhs, Imm32 rhs, Label* label) {
        if (lhs.getTag() == Operand::OP2) {
            branch32(cond, lhs.toReg(), rhs, label);
        } else {
            // branch32 will use ScratchRegister.
            AutoRegisterScope scratch(asMasm(), secondScratchReg_);
            ma_ldr(lhs.toAddress(), scratch);
            branch32(cond, scratch, rhs, label);
        }
    }
    void branch32(Condition cond, const Address& lhs, Register rhs, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        load32(lhs, scratch);
        branch32(cond, scratch, rhs, label);
    }
    void branch32(Condition cond, const Address& lhs, Imm32 rhs, Label* label) {
        // branch32 will use ScratchRegister.
        AutoRegisterScope scratch(asMasm(), secondScratchReg_);
        load32(lhs, scratch);
        branch32(cond, scratch, rhs, label);
    }
    void branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs, Label* label) {
        // branch32 will use ScratchRegister.
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        load32(lhs, scratch2);
        branch32(cond, scratch2, rhs, label);
    }
    void branchPtr(Condition cond, const Address& lhs, Register rhs, Label* label) {
        branch32(cond, lhs, rhs, label);
    }

    void branchPrivatePtr(Condition cond, const Address& lhs, ImmPtr ptr, Label* label) {
        branchPtr(cond, lhs, ptr, label);
    }

    void branchPrivatePtr(Condition cond, const Address& lhs, Register ptr, Label* label) {
        branchPtr(cond, lhs, ptr, label);
    }

    void branchPrivatePtr(Condition cond, Register lhs, ImmWord ptr, Label* label) {
        branchPtr(cond, lhs, ptr, label);
    }

    template<typename T>
    void branchTestDouble(Condition cond, const T & t, Label* label) {
        Condition c = testDouble(cond, t);
        ma_b(label, c);
    }
    template<typename T>
    void branchTestNull(Condition cond, const T & t, Label* label) {
        Condition c = testNull(cond, t);
        ma_b(label, c);
    }
    template<typename T>
    void branchTestObject(Condition cond, const T & t, Label* label) {
        Condition c = testObject(cond, t);
        ma_b(label, c);
    }
    template<typename T>
    void branchTestString(Condition cond, const T & t, Label* label) {
        Condition c = testString(cond, t);
        ma_b(label, c);
    }
    template<typename T>
    void branchTestSymbol(Condition cond, const T & t, Label* label) {
        Condition c = testSymbol(cond, t);
        ma_b(label, c);
    }
    template<typename T>
    void branchTestUndefined(Condition cond, const T & t, Label* label) {
        Condition c = testUndefined(cond, t);
        ma_b(label, c);
    }
    template <typename T>
    void branchTestNumber(Condition cond, const T& t, Label* label) {
        cond = testNumber(cond, t);
        ma_b(label, cond);
    }
    template <typename T>
    void branchTestMagic(Condition cond, const T& t, Label* label) {
        cond = testMagic(cond, t);
        ma_b(label, cond);
    }
    void branchTestMagicValue(Condition cond, const ValueOperand& val, JSWhyMagic why,
                              Label* label) {
        MOZ_ASSERT(cond == Equal || cond == NotEqual);
        branchTestValue(cond, val, MagicValue(why), label);
    }
    void branchTestInt32Truthy(bool truthy, const ValueOperand& operand, Label* label) {
        Condition c = testInt32Truthy(truthy, operand);
        ma_b(label, c);
    }
    void branchTestBooleanTruthy(bool truthy, const ValueOperand& operand, Label* label) {
        Condition c = testBooleanTruthy(truthy, operand);
        ma_b(label, c);
    }
    void branchTestDoubleTruthy(bool truthy, FloatRegister reg, Label* label) {
        Condition c = testDoubleTruthy(truthy, reg);
        ma_b(label, c);
    }
    void branchTestStringTruthy(bool truthy, const ValueOperand& value, Label* label) {
        Condition c = testStringTruthy(truthy, value);
        ma_b(label, c);
    }
    void branchTest32(Condition cond, Register lhs, Register rhs, Label* label) {
        MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
        // x86 likes test foo, foo rather than cmp foo, #0.
        // Convert the former into the latter.
        if (lhs == rhs && (cond == Zero || cond == NonZero))
            ma_cmp(lhs, Imm32(0));
        else
            ma_tst(lhs, rhs);
        ma_b(label, cond);
    }
    void branchTest32(Condition cond, Register lhs, Imm32 imm, Label* label) {
        MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
        ma_tst(lhs, imm);
        ma_b(label, cond);
    }
    void branchTest32(Condition cond, const Address& address, Imm32 imm, Label* label) {
        // branchTest32 will use ScratchRegister.
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        load32(address, scratch2);
        branchTest32(cond, scratch2, imm, label);
    }
    void branchTest32(Condition cond, AbsoluteAddress address, Imm32 imm, Label* label) {
        // branchTest32 will use ScratchRegister.
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        load32(address, scratch2);
        branchTest32(cond, scratch2, imm, label);
    }
    void branchTestPtr(Condition cond, Register lhs, Register rhs, Label* label) {
        branchTest32(cond, lhs, rhs, label);
    }
    void branchTestPtr(Condition cond, Register lhs, const Imm32 rhs, Label* label) {
        branchTest32(cond, lhs, rhs, label);
    }
    void branchTestPtr(Condition cond, const Address& lhs, Imm32 imm, Label* label) {
        branchTest32(cond, lhs, imm, label);
    }
    void branchPtr(Condition cond, Register lhs, Register rhs, Label* label) {
        branch32(cond, lhs, rhs, label);
    }
    void branchPtr(Condition cond, Register lhs, ImmGCPtr ptr, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        movePtr(ptr, scratch);
        branchPtr(cond, lhs, scratch, label);
    }
    void branchPtr(Condition cond, Register lhs, ImmWord imm, Label* label) {
        branch32(cond, lhs, Imm32(imm.value), label);
    }
    void branchPtr(Condition cond, Register lhs, ImmPtr imm, Label* label) {
        branchPtr(cond, lhs, ImmWord(uintptr_t(imm.value)), label);
    }
    void branchPtr(Condition cond, Register lhs, wasm::SymbolicAddress imm, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        movePtr(imm, scratch);
        branchPtr(cond, lhs, scratch, label);
    }
    void branchPtr(Condition cond, Register lhs, Imm32 imm, Label* label) {
        branch32(cond, lhs, imm, label);
    }
    void decBranchPtr(Condition cond, Register lhs, Imm32 imm, Label* label) {
        subPtr(imm, lhs);
        branch32(cond, lhs, Imm32(0), label);
    }
    void branchTest64(Condition cond, Register64 lhs, Register64 rhs, Register temp, Label* label);
    void moveValue(const Value& val, Register type, Register data);

    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Condition cond = Always,
                                 Label* documentation = nullptr);
    CodeOffsetJump backedgeJump(RepatchLabel* label, Label* documentation) {
        return jumpWithPatch(label, Always, documentation);
    }
    template <typename T>
    CodeOffsetJump branchPtrWithPatch(Condition cond, Register reg, T ptr, RepatchLabel* label) {
        ma_cmp(reg, ptr);
        return jumpWithPatch(label, cond);
    }
    template <typename T>
    CodeOffsetJump branchPtrWithPatch(Condition cond, Address addr, T ptr, RepatchLabel* label) {
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        ma_ldr(addr, scratch2);
        ma_cmp(scratch2, ptr);
        return jumpWithPatch(label, cond);
    }
    void branchPtr(Condition cond, Address addr, ImmGCPtr ptr, Label* label) {
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        ma_ldr(addr, scratch2);
        ma_cmp(scratch2, ptr);
        ma_b(label, cond);
    }
    void branchPtr(Condition cond, Address addr, ImmWord ptr, Label* label) {
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        ma_ldr(addr, scratch2);
        ma_cmp(scratch2, ptr);
        ma_b(label, cond);
    }
    void branchPtr(Condition cond, Address addr, ImmPtr ptr, Label* label) {
        branchPtr(cond, addr, ImmWord(uintptr_t(ptr.value)), label);
    }
    void branchPtr(Condition cond, AbsoluteAddress addr, Register ptr, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        loadPtr(addr, scratch);
        ma_cmp(scratch, ptr);
        ma_b(label, cond);
    }
    void branchPtr(Condition cond, AbsoluteAddress addr, ImmWord ptr, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        loadPtr(addr, scratch);
        ma_cmp(scratch, ptr);
        ma_b(label, cond);
    }
    void branchPtr(Condition cond, wasm::SymbolicAddress addr, Register ptr, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        loadPtr(addr, scratch);
        ma_cmp(scratch, ptr);
        ma_b(label, cond);
    }
    void branch32(Condition cond, AbsoluteAddress lhs, Imm32 rhs, Label* label) {
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        loadPtr(lhs, scratch2); // ma_cmp will use the scratch register.
        ma_cmp(scratch2, rhs);
        ma_b(label, cond);
    }
    void branch32(Condition cond, AbsoluteAddress lhs, Register rhs, Label* label) {
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        loadPtr(lhs, scratch2); // ma_cmp will use the scratch register.
        ma_cmp(scratch2, rhs);
        ma_b(label, cond);
    }
    void branch32(Condition cond, wasm::SymbolicAddress addr, Imm32 imm, Label* label) {
        ScratchRegisterScope scratch(asMasm());
        loadPtr(addr, scratch);
        ma_cmp(scratch, imm);
        ma_b(label, cond);
    }

    void loadUnboxedValue(Address address, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(address, dest.fpu());
        else
            ma_ldr(address, dest.gpr());
    }

    void loadUnboxedValue(BaseIndex address, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(address.base, address.index, dest.fpu(), address.scale);
        else
            load32(address, dest.gpr());
    }

    template <typename T>
    void storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const T& dest,
                           MIRType slotType);

    template <typename T>
    void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes) {
        switch (nbytes) {
          case 4:
            storePtr(value.payloadReg(), address);
            return;
          case 1:
            store8(value.payloadReg(), address);
            return;
          default: MOZ_CRASH("Bad payload width");
        }
    }

    void moveValue(const Value& val, const ValueOperand& dest);

    void moveValue(const ValueOperand& src, const ValueOperand& dest) {
        Register s0 = src.typeReg(), d0 = dest.typeReg(),
                 s1 = src.payloadReg(), d1 = dest.payloadReg();

        // Either one or both of the source registers could be the same as a
        // destination register.
        if (s1 == d0) {
            if (s0 == d1) {
                // If both are, this is just a swap of two registers.
                ScratchRegisterScope scratch(asMasm());
                MOZ_ASSERT(d1 != scratch);
                MOZ_ASSERT(d0 != scratch);
                ma_mov(d1, scratch);
                ma_mov(d0, d1);
                ma_mov(scratch, d0);
                return;
            }
            // If only one is, copy that source first.
            mozilla::Swap(s0, s1);
            mozilla::Swap(d0, d1);
        }

        if (s0 != d0)
            ma_mov(s0, d0);
        if (s1 != d1)
            ma_mov(s1, d1);
    }

    void storeValue(ValueOperand val, const Address& dst);
    void storeValue(ValueOperand val, const BaseIndex& dest);
    void storeValue(JSValueType type, Register reg, BaseIndex dest) {
        ScratchRegisterScope scratch(asMasm());
        ma_alu(dest.base, lsl(dest.index, dest.scale), scratch, OpAdd);
        storeValue(type, reg, Address(scratch, dest.offset));
    }
    void storeValue(JSValueType type, Register reg, Address dest) {
        ma_str(reg, dest);
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        ma_mov(ImmTag(JSVAL_TYPE_TO_TAG(type)), scratch2);
        ma_str(scratch2, Address(dest.base, dest.offset + 4));
    }
    void storeValue(const Value& val, const Address& dest) {
        AutoRegisterScope scratch2(asMasm(), secondScratchReg_);
        jsval_layout jv = JSVAL_TO_IMPL(val);
        ma_mov(Imm32(jv.s.tag), scratch2);
        ma_str(scratch2, ToType(dest));
        if (val.isMarkable())
            ma_mov(ImmGCPtr(reinterpret_cast<gc::Cell*>(val.toGCThing())), scratch2);
        else
            ma_mov(Imm32(jv.s.payload.i32), scratch2);
        ma_str(scratch2, ToPayload(dest));
    }
    void storeValue(const Value& val, BaseIndex dest) {
        ScratchRegisterScope scratch(asMasm());
        ma_alu(dest.base, lsl(dest.index, dest.scale), scratch, OpAdd);
        storeValue(val, Address(scratch, dest.offset));
    }

    void loadValue(Address src, ValueOperand val);
    void loadValue(Operand dest, ValueOperand val) {
        loadValue(dest.toAddress(), val);
    }
    void loadValue(const BaseIndex& addr, ValueOperand val);
    void tagValue(JSValueType type, Register payload, ValueOperand dest);

    void pushValue(ValueOperand val);
    void popValue(ValueOperand val);
    void pushValue(const Value& val) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        push(Imm32(jv.s.tag));
        if (val.isMarkable())
            push(ImmGCPtr(reinterpret_cast<gc::Cell*>(val.toGCThing())));
        else
            push(Imm32(jv.s.payload.i32));
    }
    void pushValue(JSValueType type, Register reg) {
        push(ImmTag(JSVAL_TYPE_TO_TAG(type)));
        ma_push(reg);
    }
    void pushValue(const Address& addr);

    void storePayload(const Value& val, const Address& dest);
    void storePayload(Register src, const Address& dest);
    void storePayload(const Value& val, const BaseIndex& dest);
    void storePayload(Register src, const BaseIndex& dest);
    void storeTypeTag(ImmTag tag, const Address& dest);
    void storeTypeTag(ImmTag tag, const BaseIndex& dest);

    void handleFailureWithHandlerTail(void* handler);

    /////////////////////////////////////////////////////////////////
    // Common interface.
    /////////////////////////////////////////////////////////////////
  public:
    void add32(Register src, Register dest);
    void add32(Imm32 imm, Register dest);
    void add32(Imm32 imm, const Address& dest);
    template <typename T>
    void branchAdd32(Condition cond, T src, Register dest, Label* label) {
        add32(src, dest);
        j(cond, label);
    }
    template <typename T>
    void branchSub32(Condition cond, T src, Register dest, Label* label) {
        ma_sub(src, dest, SetCC);
        j(cond, label);
    }

    void addPtr(Register src, Register dest);
    void addPtr(const Address& src, Register dest);
    void add64(Imm32 imm, Register64 dest) {
        ma_add(imm, dest.low, SetCC);
        ma_adc(Imm32(0), dest.high, LeaveCC);
    }
    void not32(Register reg);

    void move32(Imm32 imm, Register dest);
    void move32(Register src, Register dest);

    void movePtr(Register src, Register dest);
    void movePtr(ImmWord imm, Register dest);
    void movePtr(ImmPtr imm, Register dest);
    void movePtr(wasm::SymbolicAddress imm, Register dest);
    void movePtr(ImmGCPtr imm, Register dest);
    void move64(Register64 src, Register64 dest) {
        move32(src.low, dest.low);
        move32(src.high, dest.high);
    }

    void load8SignExtend(const Address& address, Register dest);
    void load8SignExtend(const BaseIndex& src, Register dest);

    void load8ZeroExtend(const Address& address, Register dest);
    void load8ZeroExtend(const BaseIndex& src, Register dest);

    void load16SignExtend(const Address& address, Register dest);
    void load16SignExtend(const BaseIndex& src, Register dest);

    void load16ZeroExtend(const Address& address, Register dest);
    void load16ZeroExtend(const BaseIndex& src, Register dest);

    void load32(const Address& address, Register dest);
    void load32(const BaseIndex& address, Register dest);
    void load32(AbsoluteAddress address, Register dest);
    void load64(const Address& address, Register64 dest) {
        load32(address, dest.low);
        load32(Address(address.base, address.offset + 4), dest.high);
    }

    void loadPtr(const Address& address, Register dest);
    void loadPtr(const BaseIndex& src, Register dest);
    void loadPtr(AbsoluteAddress address, Register dest);
    void loadPtr(wasm::SymbolicAddress address, Register dest);

    void loadPrivate(const Address& address, Register dest);

    void loadInt32x1(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x1(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x2(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x2(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x3(const Address& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadInt32x3(const BaseIndex& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeInt32x1(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x1(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x2(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x2(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x3(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x3(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void loadAlignedInt32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeAlignedInt32x4(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void loadUnalignedInt32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadUnalignedInt32x4(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeUnalignedInt32x4(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void storeUnalignedInt32x4(FloatRegister src, BaseIndex addr) { MOZ_CRASH("NYI"); }

    void loadFloat32x3(const Address& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadFloat32x3(const BaseIndex& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeFloat32x3(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeFloat32x3(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void loadAlignedFloat32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeAlignedFloat32x4(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void loadUnalignedFloat32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadUnalignedFloat32x4(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeUnalignedFloat32x4(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void storeUnalignedFloat32x4(FloatRegister src, BaseIndex addr) { MOZ_CRASH("NYI"); }

    void loadDouble(const Address& addr, FloatRegister dest);
    void loadDouble(const BaseIndex& src, FloatRegister dest);

    // Load a float value into a register, then expand it to a double.
    void loadFloatAsDouble(const Address& addr, FloatRegister dest);
    void loadFloatAsDouble(const BaseIndex& src, FloatRegister dest);

    void loadFloat32(const Address& addr, FloatRegister dest);
    void loadFloat32(const BaseIndex& src, FloatRegister dest);

    void store8(Register src, const Address& address);
    void store8(Imm32 imm, const Address& address);
    void store8(Register src, const BaseIndex& address);
    void store8(Imm32 imm, const BaseIndex& address);

    void store16(Register src, const Address& address);
    void store16(Imm32 imm, const Address& address);
    void store16(Register src, const BaseIndex& address);
    void store16(Imm32 imm, const BaseIndex& address);

    void store32(Register src, AbsoluteAddress address);
    void store32(Register src, const Address& address);
    void store32(Register src, const BaseIndex& address);
    void store32(Imm32 src, const Address& address);
    void store32(Imm32 src, const BaseIndex& address);

    void store32_NoSecondScratch(Imm32 src, const Address& address);

    void store64(Register64 src, Address address) {
        store32(src.low, address);
        store32(src.high, Address(address.base, address.offset + 4));
    }

    template <typename T> void storePtr(ImmWord imm, T address);
    template <typename T> void storePtr(ImmPtr imm, T address);
    template <typename T> void storePtr(ImmGCPtr imm, T address);
    void storePtr(Register src, const Address& address);
    void storePtr(Register src, const BaseIndex& address);
    void storePtr(Register src, AbsoluteAddress dest);
    void storeDouble(FloatRegister src, Address addr) {
        ma_vstr(src, addr);
    }
    void storeDouble(FloatRegister src, BaseIndex addr) {
        uint32_t scale = Imm32::ShiftOf(addr.scale).value;
        ma_vstr(src, addr.base, addr.index, scale, addr.offset);
    }
    void moveDouble(FloatRegister src, FloatRegister dest) {
        ma_vmov(src, dest);
    }

    void storeFloat32(FloatRegister src, const Address& addr) {
        ma_vstr(VFPRegister(src).singleOverlay(), addr);
    }
    void storeFloat32(FloatRegister src, const BaseIndex& addr) {
        uint32_t scale = Imm32::ShiftOf(addr.scale).value;
        ma_vstr(VFPRegister(src).singleOverlay(), addr.base, addr.index, scale, addr.offset);
    }

  private:
    template<typename T>
    Register computePointer(const T& src, Register r);

    template<typename T>
    void compareExchangeARMv6(int nbytes, bool signExtend, const T& mem, Register oldval,
                              Register newval, Register output);

    template<typename T>
    void compareExchangeARMv7(int nbytes, bool signExtend, const T& mem, Register oldval,
                              Register newval, Register output);

    template<typename T>
    void compareExchange(int nbytes, bool signExtend, const T& address, Register oldval,
                         Register newval, Register output);

    template<typename T>
    void atomicExchangeARMv6(int nbytes, bool signExtend, const T& mem, Register value,
                             Register output);

    template<typename T>
    void atomicExchangeARMv7(int nbytes, bool signExtend, const T& mem, Register value,
                             Register output);

    template<typename T>
    void atomicExchange(int nbytes, bool signExtend, const T& address, Register value,
                        Register output);

    template<typename T>
    void atomicFetchOpARMv6(int nbytes, bool signExtend, AtomicOp op, const Register& value,
                            const T& mem, Register flagTemp, Register output);

    template<typename T>
    void atomicFetchOpARMv7(int nbytes, bool signExtend, AtomicOp op, const Register& value,
                            const T& mem, Register flagTemp, Register output);

    template<typename T>
    void atomicFetchOp(int nbytes, bool signExtend, AtomicOp op, const Imm32& value,
                       const T& address, Register flagTemp, Register output);

    template<typename T>
    void atomicFetchOp(int nbytes, bool signExtend, AtomicOp op, const Register& value,
                       const T& address, Register flagTemp, Register output);

    template<typename T>
    void atomicEffectOpARMv6(int nbytes, AtomicOp op, const Register& value, const T& address,
                             Register flagTemp);

    template<typename T>
    void atomicEffectOpARMv7(int nbytes, AtomicOp op, const Register& value, const T& address,
                             Register flagTemp);

    template<typename T>
    void atomicEffectOp(int nbytes, AtomicOp op, const Imm32& value, const T& address,
                             Register flagTemp);

    template<typename T>
    void atomicEffectOp(int nbytes, AtomicOp op, const Register& value, const T& address,
                             Register flagTemp);

  public:
    // T in {Address,BaseIndex}
    // S in {Imm32,Register}

    template<typename T>
    void compareExchange8SignExtend(const T& mem, Register oldval, Register newval, Register output)
    {
        compareExchange(1, true, mem, oldval, newval, output);
    }
    template<typename T>
    void compareExchange8ZeroExtend(const T& mem, Register oldval, Register newval, Register output)
    {
        compareExchange(1, false, mem, oldval, newval, output);
    }
    template<typename T>
    void compareExchange16SignExtend(const T& mem, Register oldval, Register newval, Register output)
    {
        compareExchange(2, true, mem, oldval, newval, output);
    }
    template<typename T>
    void compareExchange16ZeroExtend(const T& mem, Register oldval, Register newval, Register output)
    {
        compareExchange(2, false, mem, oldval, newval, output);
    }
    template<typename T>
    void compareExchange32(const T& mem, Register oldval, Register newval, Register output)  {
        compareExchange(4, false, mem, oldval, newval, output);
    }

    template<typename T>
    void atomicExchange8SignExtend(const T& mem, Register value, Register output)
    {
        atomicExchange(1, true, mem, value, output);
    }
    template<typename T>
    void atomicExchange8ZeroExtend(const T& mem, Register value, Register output)
    {
        atomicExchange(1, false, mem, value, output);
    }
    template<typename T>
    void atomicExchange16SignExtend(const T& mem, Register value, Register output)
    {
        atomicExchange(2, true, mem, value, output);
    }
    template<typename T>
    void atomicExchange16ZeroExtend(const T& mem, Register value, Register output)
    {
        atomicExchange(2, false, mem, value, output);
    }
    template<typename T>
    void atomicExchange32(const T& mem, Register value, Register output) {
        atomicExchange(4, false, mem, value, output);
    }

    template<typename T, typename S>
    void atomicFetchAdd8SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, true, AtomicFetchAddOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchAdd8ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, false, AtomicFetchAddOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchAdd16SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, true, AtomicFetchAddOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchAdd16ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, false, AtomicFetchAddOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchAdd32(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(4, false, AtomicFetchAddOp, value, mem, temp, output);
    }
    template <typename T, typename S>
    void atomicAdd8(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(1, AtomicFetchAddOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicAdd16(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(2, AtomicFetchAddOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicAdd32(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(4, AtomicFetchAddOp, value, mem, flagTemp);
    }

    template<typename T, typename S>
    void atomicFetchSub8SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, true, AtomicFetchSubOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchSub8ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, false, AtomicFetchSubOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchSub16SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, true, AtomicFetchSubOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchSub16ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, false, AtomicFetchSubOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchSub32(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(4, false, AtomicFetchSubOp, value, mem, temp, output);
    }
    template <typename T, typename S>
    void atomicSub8(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(1, AtomicFetchSubOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicSub16(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(2, AtomicFetchSubOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicSub32(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(4, AtomicFetchSubOp, value, mem, flagTemp);
    }

    template<typename T, typename S>
    void atomicFetchAnd8SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, true, AtomicFetchAndOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchAnd8ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, false, AtomicFetchAndOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchAnd16SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, true, AtomicFetchAndOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchAnd16ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, false, AtomicFetchAndOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchAnd32(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(4, false, AtomicFetchAndOp, value, mem, temp, output);
    }
    template <typename T, typename S>
    void atomicAnd8(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(1, AtomicFetchAndOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicAnd16(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(2, AtomicFetchAndOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicAnd32(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(4, AtomicFetchAndOp, value, mem, flagTemp);
    }

    template<typename T, typename S>
    void atomicFetchOr8SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, true, AtomicFetchOrOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchOr8ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, false, AtomicFetchOrOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchOr16SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, true, AtomicFetchOrOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchOr16ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, false, AtomicFetchOrOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchOr32(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(4, false, AtomicFetchOrOp, value, mem, temp, output);
    }
    template <typename T, typename S>
    void atomicOr8(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(1, AtomicFetchOrOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicOr16(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(2, AtomicFetchOrOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicOr32(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(4, AtomicFetchOrOp, value, mem, flagTemp);
    }

    template<typename T, typename S>
    void atomicFetchXor8SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, true, AtomicFetchXorOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchXor8ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(1, false, AtomicFetchXorOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchXor16SignExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, true, AtomicFetchXorOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchXor16ZeroExtend(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(2, false, AtomicFetchXorOp, value, mem, temp, output);
    }
    template<typename T, typename S>
    void atomicFetchXor32(const S& value, const T& mem, Register temp, Register output) {
        atomicFetchOp(4, false, AtomicFetchXorOp, value, mem, temp, output);
    }
    template <typename T, typename S>
    void atomicXor8(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(1, AtomicFetchXorOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicXor16(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(2, AtomicFetchXorOp, value, mem, flagTemp);
    }
    template <typename T, typename S>
    void atomicXor32(const S& value, const T& mem, Register flagTemp) {
        atomicEffectOp(4, AtomicFetchXorOp, value, mem, flagTemp);
    }

    template<typename T>
    void compareExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem, Register oldval, Register newval,
                                        Register temp, AnyRegister output);

    template<typename T>
    void atomicExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem, Register value,
                                       Register temp, AnyRegister output);

    void clampIntToUint8(Register reg) {
        // Look at (reg >> 8) if it is 0, then reg shouldn't be clamped if it is
        // <0, then we want to clamp to 0, otherwise, we wish to clamp to 255
        ScratchRegisterScope scratch(asMasm());
        as_mov(scratch, asr(reg, 8), SetCC);
        ma_mov(Imm32(0xff), reg, LeaveCC, NotEqual);
        ma_mov(Imm32(0), reg, LeaveCC, Signed);
    }

    void incrementInt32Value(const Address& addr) {
        add32(Imm32(1), ToPayload(addr));
    }

    void cmp32(Register lhs, Imm32 rhs);
    void cmp32(Register lhs, Register rhs);
    void cmp32(const Operand& lhs, Imm32 rhs);
    void cmp32(const Operand& lhs, Register rhs);

    void cmpPtr(Register lhs, Register rhs);
    void cmpPtr(Register lhs, ImmWord rhs);
    void cmpPtr(Register lhs, ImmPtr rhs);
    void cmpPtr(Register lhs, ImmGCPtr rhs);
    void cmpPtr(Register lhs, Imm32 rhs);
    void cmpPtr(const Address& lhs, Register rhs);
    void cmpPtr(const Address& lhs, ImmWord rhs);
    void cmpPtr(const Address& lhs, ImmPtr rhs);
    void cmpPtr(const Address& lhs, ImmGCPtr rhs);
    void cmpPtr(const Address& lhs, Imm32 rhs);

    void subPtr(Imm32 imm, const Register dest);
    void subPtr(const Address& addr, const Register dest);
    void subPtr(Register src, Register dest);
    void subPtr(Register src, const Address& dest);
    void addPtr(Imm32 imm, const Register dest);
    void addPtr(Imm32 imm, const Address& dest);
    void addPtr(ImmWord imm, const Register dest) {
        addPtr(Imm32(imm.value), dest);
    }
    void addPtr(ImmPtr imm, const Register dest) {
        addPtr(ImmWord(uintptr_t(imm.value)), dest);
    }
    void mulBy3(const Register& src, const Register& dest) {
        as_add(dest, src, lsl(src, 1));
    }
    void mul64(Imm64 imm, const Register64& dest) {
        // LOW32  = LOW(LOW(dest) * LOW(imm));
        // HIGH32 = LOW(HIGH(dest) * LOW(imm)) [multiply imm into upper bits]
        //        + LOW(LOW(dest) * HIGH(imm)) [multiply dest into upper bits]
        //        + HIGH(LOW(dest) * LOW(imm)) [carry]

        // HIGH(dest) = LOW(HIGH(dest) * LOW(imm));
        ma_mov(Imm32(imm.value & 0xFFFFFFFFL), ScratchRegister);
        as_mul(dest.high, dest.high, ScratchRegister);

        // high:low = LOW(dest) * LOW(imm);
        as_umull(secondScratchReg_, ScratchRegister, dest.low, ScratchRegister);

        // HIGH(dest) += high;
        as_add(dest.high, dest.high, O2Reg(secondScratchReg_));

        // HIGH(dest) += LOW(LOW(dest) * HIGH(imm));
        if (((imm.value >> 32) & 0xFFFFFFFFL) == 5)
            as_add(secondScratchReg_, dest.low, lsl(dest.low, 2));
        else
            MOZ_CRASH("Not supported imm");
        as_add(dest.high, dest.high, O2Reg(secondScratchReg_));

        // LOW(dest) = low;
        ma_mov(ScratchRegister, dest.low);
    }

    void convertUInt64ToDouble(Register64 src, Register temp, FloatRegister dest);
    void mulDoublePtr(ImmPtr imm, Register temp, FloatRegister dest) {
        movePtr(imm, ScratchRegister);
        loadDouble(Address(ScratchRegister, 0), ScratchDoubleReg);
        mulDouble(ScratchDoubleReg, dest);
    }

    void setStackArg(Register reg, uint32_t arg);

    void breakpoint();
    // Conditional breakpoint.
    void breakpoint(Condition cc);

    // Trigger the simulator's interactive read-eval-print loop.
    // The message will be printed at the stopping point.
    // (On non-simulator builds, does nothing.)
    void simulatorStop(const char* msg);

    void compareDouble(FloatRegister lhs, FloatRegister rhs);
    void branchDouble(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                      Label* label);

    void compareFloat(FloatRegister lhs, FloatRegister rhs);
    void branchFloat(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                     Label* label);

    void checkStackAlignment();

    // If source is a double, load it into dest. If source is int32, convert it
    // to double. Else, branch to failure.
    void ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure);

    void
    emitSet(Assembler::Condition cond, Register dest)
    {
        ma_mov(Imm32(0), dest);
        ma_mov(Imm32(1), dest, LeaveCC, cond);
    }

    template <typename T1, typename T2>
    void cmpPtrSet(Assembler::Condition cond, T1 lhs, T2 rhs, Register dest)
    {
        cmpPtr(lhs, rhs);
        emitSet(cond, dest);
    }
    template <typename T1, typename T2>
    void cmp32Set(Assembler::Condition cond, T1 lhs, T2 rhs, Register dest)
    {
        cmp32(lhs, rhs);
        emitSet(cond, dest);
    }

    void testNullSet(Condition cond, const ValueOperand& value, Register dest) {
        cond = testNull(cond, value);
        emitSet(cond, dest);
    }

    void testObjectSet(Condition cond, const ValueOperand& value, Register dest) {
        cond = testObject(cond, value);
        emitSet(cond, dest);
    }

    void testUndefinedSet(Condition cond, const ValueOperand& value, Register dest) {
        cond = testUndefined(cond, value);
        emitSet(cond, dest);
    }

  protected:
    bool buildOOLFakeExitFrame(void* fakeReturnAddr);

  public:
    CodeOffset labelForPatch() {
        return CodeOffset(nextOffset().getOffset());
    }

    void computeEffectiveAddress(const Address& address, Register dest) {
        ma_add(address.base, Imm32(address.offset), dest, LeaveCC);
    }
    void computeEffectiveAddress(const BaseIndex& address, Register dest) {
        ma_alu(address.base, lsl(address.index, address.scale), dest, OpAdd, LeaveCC);
        if (address.offset)
            ma_add(dest, Imm32(address.offset), dest, LeaveCC);
    }
    void floor(FloatRegister input, Register output, Label* handleNotAnInt);
    void floorf(FloatRegister input, Register output, Label* handleNotAnInt);
    void ceil(FloatRegister input, Register output, Label* handleNotAnInt);
    void ceilf(FloatRegister input, Register output, Label* handleNotAnInt);
    void round(FloatRegister input, Register output, Label* handleNotAnInt, FloatRegister tmp);
    void roundf(FloatRegister input, Register output, Label* handleNotAnInt, FloatRegister tmp);

    void clampCheck(Register r, Label* handleNotAnInt) {
        // Check explicitly for r == INT_MIN || r == INT_MAX
        // This is the instruction sequence that gcc generated for this
        // operation.
        ScratchRegisterScope scratch(asMasm());
        ma_sub(r, Imm32(0x80000001), scratch);
        ma_cmn(scratch, Imm32(3));
        ma_b(handleNotAnInt, Above);
    }

    void memIntToValue(Address Source, Address Dest) {
        load32(Source, lr);
        storeValue(JSVAL_TYPE_INT32, lr, Dest);
    }

    void lea(Operand addr, Register dest) {
        ma_add(addr.baseReg(), Imm32(addr.disp()), dest);
    }

    void abiret() {
        as_bx(lr);
    }

    void ma_storeImm(Imm32 c, const Address& dest) {
        ma_mov(c, lr);
        ma_str(lr, dest);
    }
    BufferOffset ma_BoundsCheck(Register bounded) {
        return as_cmp(bounded, Imm8(0));
    }

    void moveFloat32(FloatRegister src, FloatRegister dest) {
        as_vmov(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay());
    }

    void branchPtrInNurseryRange(Condition cond, Register ptr, Register temp, Label* label);
    void branchValueIsNurseryObject(Condition cond, ValueOperand value, Register temp, Label* label);

    void loadAsmJSActivation(Register dest) {
        loadPtr(Address(GlobalReg, wasm::ActivationGlobalDataOffset - AsmJSGlobalRegBias), dest);
    }
    void loadAsmJSHeapRegisterFromGlobalData() {
        loadPtr(Address(GlobalReg, wasm::HeapGlobalDataOffset - AsmJSGlobalRegBias), HeapReg);
    }
    // Instrumentation for entering and leaving the profiler.
    void profilerEnterFrame(Register framePtr, Register scratch);
    void profilerExitFrame();
};

typedef MacroAssemblerARMCompat MacroAssemblerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_arm_MacroAssembler_arm_h */

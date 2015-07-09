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
JS_STATIC_ASSERT(1 << defaultShift == sizeof(jsval));

// MacroAssemblerARM is inheriting form Assembler defined in
// Assembler-arm.{h,cpp}
class MacroAssemblerARM : public Assembler
{
  protected:
    // On ARM, some instructions require a second scratch register. This
    // register defaults to lr, since it's non-allocatable (as it can be
    // clobbered by some instructions). Allow the baseline compiler to override
    // this though, since baseline IC stubs rely on lr holding the return
    // address.
    Register secondScratchReg_;

  public:
    // Higher level tag testing code.
    Operand ToPayload(Operand base) {
        return Operand(Register::FromCode(base.base()), base.disp());
    }
    Address ToPayload(Address base) {
        return ToPayload(Operand(base)).toAddress();
    }

  protected:
    Operand ToType(Operand base) {
        return Operand(Register::FromCode(base.base()), base.disp() + sizeof(void*));
    }
    Address ToType(Address base) {
        return ToType(Operand(base)).toAddress();
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
                 SetCond_ sc, Condition c);

  public:
    void ma_alu(Register src1, Operand2 op2, Register dest, ALUOp op,
                SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_alu(Register src1, Imm32 imm, Register dest,
                ALUOp op,
                SetCond_ sc =  NoSetCond, Condition c = Always);

    void ma_alu(Register src1, Operand op2, Register dest, ALUOp op,
                SetCond_ sc = NoSetCond, Condition c = Always);
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
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_mov(Imm32 imm, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_mov(ImmWord imm, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

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
                SetCond_ sc = NoSetCond, Condition c = Always);


    void ma_mvn(Register src1, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    // Negate (dest <- -src) implemented as rsb dest, src, 0
    void ma_neg(Register src, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    // And
    void ma_and(Register src, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_and(Register src1, Register src2, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_and(Imm32 imm, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_and(Imm32 imm, Register src1, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);



    // Bit clear (dest <- dest & ~imm) or (dest <- src1 & ~src2)
    void ma_bic(Imm32 imm, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    // Exclusive or
    void ma_eor(Register src, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_eor(Register src1, Register src2, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_eor(Imm32 imm, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_eor(Imm32 imm, Register src1, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);


    // Or
    void ma_orr(Register src, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_orr(Register src1, Register src2, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_orr(Imm32 imm, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);

    void ma_orr(Imm32 imm, Register src1, Register dest,
                SetCond_ sc = NoSetCond, Condition c = Always);


    // Arithmetic based ops.
    // Add with carry:
    void ma_adc(Imm32 imm, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_adc(Register src, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_adc(Register src1, Register src2, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);

    // Add:
    void ma_add(Imm32 imm, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_add(Register src1, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_add(Register src1, Register src2, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_add(Register src1, Operand op, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_add(Register src1, Imm32 op, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);

    // Subtract with carry:
    void ma_sbc(Imm32 imm, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_sbc(Register src1, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_sbc(Register src1, Register src2, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);

    // Subtract:
    void ma_sub(Imm32 imm, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_sub(Register src1, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_sub(Register src1, Register src2, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_sub(Register src1, Operand op, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_sub(Register src1, Imm32 op, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);

    // Reverse subtract:
    void ma_rsb(Imm32 imm, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_rsb(Register src1, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_rsb(Register src1, Register src2, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_rsb(Register src1, Imm32 op2, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);

    // Reverse subtract with carry:
    void ma_rsc(Imm32 imm, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_rsc(Register src1, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);
    void ma_rsc(Register src1, Register src2, Register dest, SetCond_ sc = NoSetCond, Condition c = Always);

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
    void ma_str(Register rt, const Operand& addr, Index mode = Offset, Condition cc = Always);
    void ma_dtr(LoadStore ls, Register rt, const Operand& addr, Index mode, Condition cc);

    void ma_ldr(DTRAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldr(const Operand& addr, Register rt, Index mode = Offset, Condition cc = Always);

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
    void ma_bx(Register dest, Condition c = Always);

    void ma_b(void* target, Relocation::Kind reloc, Condition c = Always);

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


    void ma_vxfer(VFPRegister src, Register dest, Condition cc = Always);
    void ma_vxfer(VFPRegister src, Register dest1, Register dest2, Condition cc = Always);

    void ma_vxfer(Register src1, Register src2, FloatRegister dest, Condition cc = Always);

    BufferOffset ma_vdtr(LoadStore ls, const Operand& addr, VFPRegister dest, Condition cc = Always);


    BufferOffset ma_vldr(VFPAddr addr, VFPRegister dest, Condition cc = Always);
    BufferOffset ma_vldr(const Operand& addr, VFPRegister dest, Condition cc = Always);
    BufferOffset ma_vldr(VFPRegister src, Register base, Register index, int32_t shift = defaultShift, Condition cc = Always);

    BufferOffset ma_vstr(VFPRegister src, VFPAddr addr, Condition cc = Always);
    BufferOffset ma_vstr(VFPRegister src, const Operand& addr, Condition cc = Always);

    BufferOffset ma_vstr(VFPRegister src, Register base, Register index, int32_t shift,
                         int32_t offset, Condition cc = Always);
    // Calls an Ion function, assumes that the stack is untouched (8 byte
    // aligned).
    void ma_callJit(const Register reg);
    // Calls an Ion function, assuming that sp has already been decremented.
    void ma_callJitNoPush(const Register reg);
    // Calls an ion function, assuming that the stack is currently not 8 byte
    // aligned.
    void ma_callJitHalfPush(const Register reg);
    // Calls an ion function, assuming that the stack is currently not 8 byte
    // aligned.
    void ma_callJitHalfPush(Label* label);

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
            } while ((++iter).more() && (*iter).code() == (reg += sign));
            finishFloatTransfer();
        }
        return offset;
    }
};

class MacroAssemblerARMCompat : public MacroAssemblerARM
{
    bool inCall_;
    // Number of bytes the stack is adjusted inside a call to C. Calls to C may
    // not be nested.
    uint32_t args_;
    // The actual number of arguments that were passed, used to assert that the
    // initial number of arguments declared was correct.
    uint32_t passedArgs_;
    uint32_t passedArgTypes_;

    // ARM treats arguments as a vector in registers/memory, that looks like:
    // { r0, r1, r2, r3, [sp], [sp,+4], [sp,+8] ... }
    // usedIntSlots_ keeps track of how many of these have been used. It bears a
    // passing resemblance to passedArgs_, but a single argument can effectively
    // use between one and three slots depending on its size and alignment
    // requirements.
    uint32_t usedIntSlots_;
#if defined(JS_CODEGEN_ARM_HARDFP) || defined(JS_ARM_SIMULATOR)
    uint32_t usedFloatSlots_;
    bool usedFloat32_;
    uint32_t padding_;
#endif
    bool dynamicAlignment_;

    // Used to work around the move resolver's lack of support for moving into
    // register pairs, which the softfp ABI needs.
    mozilla::Array<MoveOperand, 4> floatArgsInGPR;
    mozilla::Array<bool, 4> floatArgsInGPRValid;

    // Compute space needed for the function call and set the properties of the
    // callee. It returns the space which has to be allocated for calling the
    // function.
    //
    // arg            Number of arguments of the function.
    void setupABICall(uint32_t arg);

  protected:
    MoveResolver moveResolver_;

    // Extra bytes currently pushed onto the frame beyond frameDepth_. This is
    // needed to compute offsets to stack slots while temporary space has been
    // reserved for unexpected spills or C++ function calls. It is maintained by
    // functions which track stack alignment, which for clear distinction use
    // StudlyCaps (for example, Push, Pop).
    uint32_t framePushed_;
    void adjustFrame(int value) {
        setFramePushed(framePushed_ + value);
    }
  public:
    MacroAssemblerARMCompat()
      : inCall_(false),
        framePushed_(0)
    { }

  public:
    using MacroAssemblerARM::call;

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

    void call(const Register reg) {
        as_blx(reg);
    }
    void call(Label* label) {
        // For now, assume that it'll be nearby?
        as_bl(label, Always);
    }
    void call(ImmWord imm) {
        call(ImmPtr((void*)imm.value));
    }
    void call(ImmPtr imm) {
        BufferOffset bo = m_buffer.nextOffset();
        addPendingJump(bo, imm, Relocation::HARDCODED);
        ma_call(imm);
    }
    void call(AsmJSImmPtr imm) {
        movePtr(imm, CallReg);
        call(CallReg);
    }
    void call(JitCode* c) {
        BufferOffset bo = m_buffer.nextOffset();
        addPendingJump(bo, ImmPtr(c->raw()), Relocation::JITCODE);
        RelocStyle rs;
        if (HasMOVWT())
            rs = L_MOVWT;
        else
            rs = L_LDR;

        ma_movPatchable(ImmPtr(c->raw()), ScratchRegister, Always, rs);
        ma_callJitHalfPush(ScratchRegister);
    }
    void call(const CallSiteDesc& desc, const Register reg) {
        call(reg);
        append(desc, currentOffset(), framePushed_);
    }
    void call(const CallSiteDesc& desc, Label* label) {
        call(label);
        append(desc, currentOffset(), framePushed_);
    }
    void callAndPushReturnAddress(Label* label) {
        AutoForbidPools afp(this, 2);
        ma_push(pc);
        call(label);
    }

    void branch(JitCode* c) {
        BufferOffset bo = m_buffer.nextOffset();
        addPendingJump(bo, ImmPtr(c->raw()), Relocation::JITCODE);
        RelocStyle rs;
        if (HasMOVWT())
            rs = L_MOVWT;
        else
            rs = L_LDR;

        ma_movPatchable(ImmPtr(c->raw()), ScratchRegister, Always, rs);
        ma_bx(ScratchRegister);
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
        ma_mov(imm, ScratchRegister);
        ma_push(ScratchRegister);
    }
    void push(ImmWord imm) {
        push(Imm32(imm.value));
    }
    void push(ImmGCPtr imm) {
        ma_mov(imm, ScratchRegister);
        ma_push(ScratchRegister);
    }
    void push(ImmMaybeNurseryPtr imm) {
        push(noteMaybeNurseryPtr(imm));
    }
    void push(const Address& address) {
        ma_ldr(Operand(address.base, address.offset), ScratchRegister);
        ma_push(ScratchRegister);
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
        Imm32 totSpace = Imm32(extraSpace.value + 4);
        // ma_dtr may need the scratch register to adjust the stack, so use the
        // second scratch register.
        ma_mov(imm, secondScratchReg_);
        ma_dtr(IsStore, sp, totSpace, secondScratchReg_, PreIndex);
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

    CodeOffsetLabel toggledJump(Label* label);

    // Emit a BLX or NOP instruction. ToggleCall can be used to patch this
    // instruction.
    CodeOffsetLabel toggledCall(JitCode* target, bool enabled);

    CodeOffsetLabel pushWithPatch(ImmWord imm) {
        CodeOffsetLabel label = movWithPatch(imm, ScratchRegister);
        ma_push(ScratchRegister);
        return label;
    }

    CodeOffsetLabel movWithPatch(ImmWord imm, Register dest) {
        CodeOffsetLabel label = CodeOffsetLabel(currentOffset());
        ma_movPatchable(Imm32(imm.value), dest, Always, HasMOVWT() ? L_MOVWT : L_LDR);
        return label;
    }
    CodeOffsetLabel movWithPatch(ImmPtr imm, Register dest) {
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
    void jump(const Address& address) {
        ma_ldr(Operand(address.base, address.offset), ScratchRegister);
        ma_bx(ScratchRegister);
    }

    void neg32(Register reg) {
        ma_neg(reg, reg, SetCond);
    }
    void negl(Register reg) {
        ma_neg(reg, reg, SetCond);
    }
    void test32(Register lhs, Register rhs) {
        ma_tst(lhs, rhs);
    }
    void test32(Register lhs, Imm32 imm) {
        ma_tst(lhs, imm);
    }
    void test32(const Address& address, Imm32 imm) {
        ma_ldr(Operand(address.base, address.offset), ScratchRegister);
        ma_tst(ScratchRegister, imm);
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
    void loadInt32OrDouble(const Operand& src, FloatRegister dest);
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
            ma_ldr(lhs, ScratchRegister);
            branch32(cond, ScratchRegister, rhs, label);
        }
    }
    void branch32(Condition cond, const Operand& lhs, Imm32 rhs, Label* label) {
        if (lhs.getTag() == Operand::OP2) {
            branch32(cond, lhs.toReg(), rhs, label);
        } else {
            // branch32 will use ScratchRegister.
            ma_ldr(lhs, secondScratchReg_);
            branch32(cond, secondScratchReg_, rhs, label);
        }
    }
    void branch32(Condition cond, const Address& lhs, Register rhs, Label* label) {
        load32(lhs, ScratchRegister);
        branch32(cond, ScratchRegister, rhs, label);
    }
    void branch32(Condition cond, const Address& lhs, Imm32 rhs, Label* label) {
        // branch32 will use ScratchRegister.
        load32(lhs, secondScratchReg_);
        branch32(cond, secondScratchReg_, rhs, label);
    }
    void branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs, Label* label) {
        // branch32 will use ScratchRegister.
        load32(lhs, secondScratchReg_);
        branch32(cond, secondScratchReg_, rhs, label);
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
        load32(address, secondScratchReg_);
        branchTest32(cond, secondScratchReg_, imm, label);
    }
    void branchTest32(Condition cond, AbsoluteAddress address, Imm32 imm, Label* label) {
        // branchTest32 will use ScratchRegister.
        load32(address, secondScratchReg_);
        branchTest32(cond, secondScratchReg_, imm, label);
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
        movePtr(ptr, ScratchRegister);
        branchPtr(cond, lhs, ScratchRegister, label);
    }
    void branchPtr(Condition cond, Register lhs, ImmWord imm, Label* label) {
        branch32(cond, lhs, Imm32(imm.value), label);
    }
    void branchPtr(Condition cond, Register lhs, ImmPtr imm, Label* label) {
        branchPtr(cond, lhs, ImmWord(uintptr_t(imm.value)), label);
    }
    void branchPtr(Condition cond, Register lhs, AsmJSImmPtr imm, Label* label) {
        movePtr(imm, ScratchRegister);
        branchPtr(cond, lhs, ScratchRegister, label);
    }
    void branchPtr(Condition cond, Register lhs, Imm32 imm, Label* label) {
        branch32(cond, lhs, imm, label);
    }
    void decBranchPtr(Condition cond, Register lhs, Imm32 imm, Label* label) {
        subPtr(imm, lhs);
        branch32(cond, lhs, Imm32(0), label);
    }
    void moveValue(const Value& val, Register type, Register data);

    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Condition cond = Always);
    CodeOffsetJump backedgeJump(RepatchLabel* label) {
        return jumpWithPatch(label);
    }
    template <typename T>
    CodeOffsetJump branchPtrWithPatch(Condition cond, Register reg, T ptr, RepatchLabel* label) {
        ma_cmp(reg, ptr);
        return jumpWithPatch(label, cond);
    }
    template <typename T>
    CodeOffsetJump branchPtrWithPatch(Condition cond, Address addr, T ptr, RepatchLabel* label) {
        ma_ldr(addr, secondScratchReg_);
        ma_cmp(secondScratchReg_, ptr);
        return jumpWithPatch(label, cond);
    }
    void branchPtr(Condition cond, Address addr, ImmGCPtr ptr, Label* label) {
        ma_ldr(addr, secondScratchReg_);
        ma_cmp(secondScratchReg_, ptr);
        ma_b(label, cond);
    }
    void branchPtr(Condition cond, Address addr, ImmMaybeNurseryPtr ptr, Label* label) {
        branchPtr(cond, addr, noteMaybeNurseryPtr(ptr), label);
    }
    void branchPtr(Condition cond, Address addr, ImmWord ptr, Label* label) {
        ma_ldr(addr, secondScratchReg_);
        ma_cmp(secondScratchReg_, ptr);
        ma_b(label, cond);
    }
    void branchPtr(Condition cond, Address addr, ImmPtr ptr, Label* label) {
        branchPtr(cond, addr, ImmWord(uintptr_t(ptr.value)), label);
    }
    void branchPtr(Condition cond, AbsoluteAddress addr, Register ptr, Label* label) {
        loadPtr(addr, ScratchRegister);
        ma_cmp(ScratchRegister, ptr);
        ma_b(label, cond);
    }
    void branchPtr(Condition cond, AbsoluteAddress addr, ImmWord ptr, Label* label) {
        loadPtr(addr, ScratchRegister);
        ma_cmp(ScratchRegister, ptr);
        ma_b(label, cond);
    }
    void branchPtr(Condition cond, AsmJSAbsoluteAddress addr, Register ptr, Label* label) {
        loadPtr(addr, ScratchRegister);
        ma_cmp(ScratchRegister, ptr);
        ma_b(label, cond);
    }
    void branch32(Condition cond, AbsoluteAddress lhs, Imm32 rhs, Label* label) {
        loadPtr(lhs, secondScratchReg_); // ma_cmp will use the scratch register.
        ma_cmp(secondScratchReg_, rhs);
        ma_b(label, cond);
    }
    void branch32(Condition cond, AbsoluteAddress lhs, Register rhs, Label* label) {
        loadPtr(lhs, secondScratchReg_); // ma_cmp will use the scratch register.
        ma_cmp(secondScratchReg_, rhs);
        ma_b(label, cond);
    }
    void branch32(Condition cond, AsmJSAbsoluteAddress addr, Imm32 imm, Label* label) {
        loadPtr(addr, ScratchRegister);
        ma_cmp(ScratchRegister, imm);
        ma_b(label, cond);
    }

    void loadUnboxedValue(Address address, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(Operand(address), dest.fpu());
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
                MOZ_ASSERT(d1 != ScratchRegister);
                MOZ_ASSERT(d0 != ScratchRegister);
                ma_mov(d1, ScratchRegister);
                ma_mov(d0, d1);
                ma_mov(ScratchRegister, d0);
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

    void storeValue(ValueOperand val, Operand dst);
    void storeValue(ValueOperand val, const BaseIndex& dest);
    void storeValue(JSValueType type, Register reg, BaseIndex dest) {
        ma_alu(dest.base, lsl(dest.index, dest.scale), ScratchRegister, OpAdd);
        storeValue(type, reg, Address(ScratchRegister, dest.offset));
    }
    void storeValue(ValueOperand val, const Address& dest) {
        storeValue(val, Operand(dest));
    }
    void storeValue(JSValueType type, Register reg, Address dest) {
        ma_str(reg, dest);
        ma_mov(ImmTag(JSVAL_TYPE_TO_TAG(type)), secondScratchReg_);
        ma_str(secondScratchReg_, Address(dest.base, dest.offset + 4));
    }
    void storeValue(const Value& val, Address dest) {
        jsval_layout jv = JSVAL_TO_IMPL(val);
        ma_mov(Imm32(jv.s.tag), secondScratchReg_);
        ma_str(secondScratchReg_, Address(dest.base, dest.offset + 4));
        if (val.isMarkable())
            ma_mov(ImmGCPtr(reinterpret_cast<gc::Cell*>(val.toGCThing())), secondScratchReg_);
        else
            ma_mov(Imm32(jv.s.payload.i32), secondScratchReg_);
        ma_str(secondScratchReg_, dest);
    }
    void storeValue(const Value& val, BaseIndex dest) {
        ma_alu(dest.base, lsl(dest.index, dest.scale), ScratchRegister, OpAdd);
        storeValue(val, Address(ScratchRegister, dest.offset));
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
            push(ImmMaybeNurseryPtr(reinterpret_cast<gc::Cell*>(val.toGCThing())));
        else
            push(Imm32(jv.s.payload.i32));
    }
    void pushValue(JSValueType type, Register reg) {
        push(ImmTag(JSVAL_TYPE_TO_TAG(type)));
        ma_push(reg);
    }
    void pushValue(const Address& addr);
    void Push(const ValueOperand& val) {
        pushValue(val);
        framePushed_ += sizeof(Value);
    }
    void Pop(const ValueOperand& val) {
        popValue(val);
        framePushed_ -= sizeof(Value);
    }
    void storePayload(const Value& val, Operand dest);
    void storePayload(Register src, Operand dest);
    void storePayload(const Value& val, const BaseIndex& dest);
    void storePayload(Register src, const BaseIndex& dest);
    void storeTypeTag(ImmTag tag, Operand dest);
    void storeTypeTag(ImmTag tag, const BaseIndex& dest);

    void makeFrameDescriptor(Register frameSizeReg, FrameType type) {
        ma_lsl(Imm32(FRAMESIZE_SHIFT), frameSizeReg, frameSizeReg);
        ma_orr(Imm32(type), frameSizeReg);
    }

    void handleFailureWithHandlerTail(void* handler);

    /////////////////////////////////////////////////////////////////
    // Common interface.
    /////////////////////////////////////////////////////////////////
  public:
    // The following functions are exposed for use in platform-shared code.
    void Push(Register reg) {
        ma_push(reg);
        adjustFrame(sizeof(intptr_t));
    }
    void Push(const Imm32 imm) {
        push(imm);
        adjustFrame(sizeof(intptr_t));
    }
    void Push(const ImmWord imm) {
        push(imm);
        adjustFrame(sizeof(intptr_t));
    }
    void Push(const ImmPtr imm) {
        Push(ImmWord(uintptr_t(imm.value)));
    }
    void Push(const ImmGCPtr ptr) {
        push(ptr);
        adjustFrame(sizeof(intptr_t));
    }
    void Push(FloatRegister t) {
        VFPRegister r = VFPRegister(t);
        ma_vpush(VFPRegister(t));
        adjustFrame(r.size());
    }

    CodeOffsetLabel PushWithPatch(ImmWord word) {
        framePushed_ += sizeof(word.value);
        return pushWithPatch(word);
    }
    CodeOffsetLabel PushWithPatch(ImmPtr imm) {
        return PushWithPatch(ImmWord(uintptr_t(imm.value)));
    }

    void PushWithPadding(Register reg, const Imm32 extraSpace) {
        pushWithPadding(reg, extraSpace);
        adjustFrame(sizeof(intptr_t) + extraSpace.value);
    }
    void PushWithPadding(const Imm32 imm, const Imm32 extraSpace) {
        pushWithPadding(imm, extraSpace);
        adjustFrame(sizeof(intptr_t) + extraSpace.value);
    }

    void Pop(Register reg) {
        ma_pop(reg);
        adjustFrame(-sizeof(intptr_t));
    }
    void implicitPop(uint32_t args) {
        MOZ_ASSERT(args % sizeof(intptr_t) == 0);
        adjustFrame(-args);
    }
    uint32_t framePushed() const {
        return framePushed_;
    }
    void setFramePushed(uint32_t framePushed) {
        framePushed_ = framePushed;
    }

    // Builds an exit frame on the stack, with a return address to an internal
    // non-function. Returns offset to be passed to markSafepointAt().
    void buildFakeExitFrame(Register scratch, uint32_t* offset);

    void callWithExitFrame(Label* target);
    void callWithExitFrame(JitCode* target);
    void callWithExitFrame(JitCode* target, Register dynStack);

    // Makes a call using the only two methods that it is sane for
    // independent code to make a call.
    void callJit(Register callee);
    void callJitFromAsmJS(Register callee) { as_blx(callee); }

    void reserveStack(uint32_t amount);
    void freeStack(uint32_t amount);
    void freeStack(Register amount);

    void add32(Register src, Register dest);
    void add32(Imm32 imm, Register dest);
    void add32(Imm32 imm, const Address& dest);
    void sub32(Imm32 imm, Register dest);
    void sub32(Register src, Register dest);
    template <typename T>
    void branchAdd32(Condition cond, T src, Register dest, Label* label) {
        add32(src, dest);
        j(cond, label);
    }
    template <typename T>
    void branchSub32(Condition cond, T src, Register dest, Label* label) {
        sub32(src, dest);
        j(cond, label);
    }
    void xor32(Imm32 imm, Register dest);

    void and32(Register src, Register dest);
    void and32(Imm32 imm, Register dest);
    void and32(Imm32 imm, const Address& dest);
    void and32(const Address& src, Register dest);
    void or32(Imm32 imm, Register dest);
    void or32(Imm32 imm, const Address& dest);
    void xorPtr(Imm32 imm, Register dest);
    void xorPtr(Register src, Register dest);
    void orPtr(Imm32 imm, Register dest);
    void orPtr(Register src, Register dest);
    void andPtr(Imm32 imm, Register dest);
    void andPtr(Register src, Register dest);
    void addPtr(Register src, Register dest);
    void addPtr(const Address& src, Register dest);
    void not32(Register reg);

    void move32(Imm32 imm, Register dest);
    void move32(Register src, Register dest);

    void movePtr(Register src, Register dest);
    void movePtr(ImmWord imm, Register dest);
    void movePtr(ImmPtr imm, Register dest);
    void movePtr(AsmJSImmPtr imm, Register dest);
    void movePtr(ImmGCPtr imm, Register dest);
    void movePtr(ImmMaybeNurseryPtr imm, Register dest);

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

    void loadPtr(const Address& address, Register dest);
    void loadPtr(const BaseIndex& src, Register dest);
    void loadPtr(AbsoluteAddress address, Register dest);
    void loadPtr(AsmJSAbsoluteAddress address, Register dest);

    void loadPrivate(const Address& address, Register dest);

    void loadAlignedInt32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeAlignedInt32x4(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void loadUnalignedInt32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeUnalignedInt32x4(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }

    void loadAlignedFloat32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeAlignedFloat32x4(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void loadUnalignedFloat32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeUnalignedFloat32x4(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }

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

    template <typename T> void storePtr(ImmWord imm, T address);
    template <typename T> void storePtr(ImmPtr imm, T address);
    template <typename T> void storePtr(ImmGCPtr imm, T address);
    void storePtr(Register src, const Address& address);
    void storePtr(Register src, const BaseIndex& address);
    void storePtr(Register src, AbsoluteAddress dest);
    void storeDouble(FloatRegister src, Address addr) {
        ma_vstr(src, Operand(addr));
    }
    void storeDouble(FloatRegister src, BaseIndex addr) {
        uint32_t scale = Imm32::ShiftOf(addr.scale).value;
        ma_vstr(src, addr.base, addr.index, scale, addr.offset);
    }
    void moveDouble(FloatRegister src, FloatRegister dest) {
        ma_vmov(src, dest);
    }

    void storeFloat32(FloatRegister src, Address addr) {
        ma_vstr(VFPRegister(src).singleOverlay(), Operand(addr));
    }
    void storeFloat32(FloatRegister src, BaseIndex addr) {
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
    void atomicFetchOpARMv6(int nbytes, bool signExtend, AtomicOp op, const Register& value,
                            const T& mem, Register temp, Register output);

    template<typename T>
    void atomicFetchOpARMv7(int nbytes, bool signExtend, AtomicOp op, const Register& value,
                            const T& mem, Register output);

    template<typename T>
    void atomicFetchOp(int nbytes, bool signExtend, AtomicOp op, const Imm32& value,
                       const T& address, Register temp, Register output);

    template<typename T>
    void atomicFetchOp(int nbytes, bool signExtend, AtomicOp op, const Register& value,
                       const T& address, Register temp, Register output);

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

    void clampIntToUint8(Register reg) {
        // Look at (reg >> 8) if it is 0, then reg shouldn't be clamped if it is
        // <0, then we want to clamp to 0, otherwise, we wish to clamp to 255
        as_mov(ScratchRegister, asr(reg, 8), SetCond);
        ma_mov(Imm32(0xff), reg, NoSetCond, NotEqual);
        ma_mov(Imm32(0), reg, NoSetCond, Signed);
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

    void rshiftPtr(Imm32 imm, Register dest) {
        ma_lsr(imm, dest, dest);
    }
    void rshiftPtrArithmetic(Imm32 imm, Register dest) {
        ma_asr(imm, dest, dest);
    }
    void lshiftPtr(Imm32 imm, Register dest) {
        ma_lsl(imm, dest, dest);
    }

    // If source is a double, load it into dest. If source is int32, convert it
    // to double. Else, branch to failure.
    void ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure);

    void
    emitSet(Assembler::Condition cond, Register dest)
    {
        ma_mov(Imm32(0), dest);
        ma_mov(Imm32(1), dest, NoSetCond, cond);
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

    // Setup a call to C/C++ code, given the number of general arguments it
    // takes. Note that this only supports cdecl.
    //
    // In order for alignment to work correctly, the MacroAssembler must have a
    // consistent view of the stack displacement. It is okay to call "push"
    // manually, however, if the stack alignment were to change, the macro
    // assembler should be notified before starting a call.
    void setupAlignedABICall(uint32_t args);

    // Sets up an ABI call for when the alignment is not known. This may need a
    // scratch register.
    void setupUnalignedABICall(uint32_t args, Register scratch);

    // Arguments must be assigned in a left-to-right order. This process may
    // temporarily use more stack, in which case esp-relative addresses will be
    // automatically adjusted. It is extremely important that esp-relative
    // addresses are computed *after* setupABICall(). Furthermore, no operations
    // should be emitted while setting arguments.
    void passABIArg(const MoveOperand& from, MoveOp::Type type);
    void passABIArg(Register reg);
    void passABIArg(FloatRegister reg, MoveOp::Type type);
    void passABIArg(const ValueOperand& regs);

  private:
    void passHardFpABIArg(const MoveOperand& from, MoveOp::Type type);
    void passSoftFpABIArg(const MoveOperand& from, MoveOp::Type type);

  protected:
    bool buildOOLFakeExitFrame(void* fakeReturnAddr);

  private:
    void callWithABIPre(uint32_t* stackAdjust, bool callFromAsmJS = false);
    void callWithABIPost(uint32_t stackAdjust, MoveOp::Type result);

  public:
    // Emits a call to a C/C++ function, resolving all argument moves.
    void callWithABI(void* fun, MoveOp::Type result = MoveOp::GENERAL);
    void callWithABI(AsmJSImmPtr imm, MoveOp::Type result = MoveOp::GENERAL);
    void callWithABI(const Address& fun, MoveOp::Type result = MoveOp::GENERAL);
    void callWithABI(Register fun, MoveOp::Type result = MoveOp::GENERAL);

    CodeOffsetLabel labelForPatch() {
        return CodeOffsetLabel(nextOffset().getOffset());
    }

    void computeEffectiveAddress(const Address& address, Register dest) {
        ma_add(address.base, Imm32(address.offset), dest, NoSetCond);
    }
    void computeEffectiveAddress(const BaseIndex& address, Register dest) {
        ma_alu(address.base, lsl(address.index, address.scale), dest, OpAdd, NoSetCond);
        if (address.offset)
            ma_add(dest, Imm32(address.offset), dest, NoSetCond);
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
        ma_sub(r, Imm32(0x80000001), ScratchRegister);
        ma_cmn(ScratchRegister, Imm32(3));
        ma_b(handleNotAnInt, Above);
    }

    void memIntToValue(Address Source, Address Dest) {
        load32(Source, lr);
        storeValue(JSVAL_TYPE_INT32, lr, Dest);
    }
    void memMove32(Address Source, Address Dest) {
        loadPtr(Source, lr);
        storePtr(lr, Dest);
    }
    void memMove64(Address Source, Address Dest) {
        loadPtr(Source, lr);
        storePtr(lr, Dest);
        loadPtr(Address(Source.base, Source.offset+4), lr);
        storePtr(lr, Address(Dest.base, Dest.offset+4));
    }

    void lea(Operand addr, Register dest) {
        ma_add(addr.baseReg(), Imm32(addr.disp()), dest);
    }

    void stackCheck(ImmWord limitAddr, Label* label) {
        int* foo = 0;
        *foo = 5;
        movePtr(limitAddr, ScratchRegister);
        ma_ldr(Address(ScratchRegister, 0), ScratchRegister);
        ma_cmp(ScratchRegister, StackPointer);
        ma_b(label, Assembler::AboveOrEqual);
    }
    void abiret() {
        as_bx(lr);
    }

    void ma_storeImm(Imm32 c, const Operand& dest) {
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
        loadPtr(Address(GlobalReg, AsmJSActivationGlobalDataOffset - AsmJSGlobalRegBias), dest);
    }
    void loadAsmJSHeapRegisterFromGlobalData() {
        loadPtr(Address(GlobalReg, AsmJSHeapGlobalDataOffset - AsmJSGlobalRegBias), HeapReg);
    }
    void pushReturnAddress() {
        push(lr);
    }

    // Instrumentation for entering and leaving the profiler.
    void profilerEnterFrame(Register framePtr, Register scratch);
    void profilerExitFrame();
};

typedef MacroAssemblerARMCompat MacroAssemblerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_arm_MacroAssembler_arm_h */

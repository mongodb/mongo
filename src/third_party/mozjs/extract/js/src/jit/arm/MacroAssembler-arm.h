/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_MacroAssembler_arm_h
#define jit_arm_MacroAssembler_arm_h

#include "mozilla/DebugOnly.h"

#include "jit/arm/Assembler-arm.h"
#include "jit/JitFrames.h"
#include "jit/MoveResolver.h"
#include "vm/BytecodeUtil.h"

using mozilla::DebugOnly;

namespace js {
namespace jit {

static Register CallReg = ip;
static const int defaultShift = 3;
JS_STATIC_ASSERT(1 << defaultShift == sizeof(JS::Value));

// See documentation for ScratchTagScope and ScratchTagScopeRelease in
// MacroAssembler-x64.h.

class ScratchTagScope
{
    const ValueOperand& v_;
  public:
    ScratchTagScope(MacroAssembler&, const ValueOperand& v) : v_(v) {}
    operator Register() { return v_.typeReg(); }
    void release() {}
    void reacquire() {}
};

class ScratchTagScopeRelease
{
  public:
    explicit ScratchTagScopeRelease(ScratchTagScope*) {}
};

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
    Register getSecondScratchReg() const {
        return secondScratchReg_;
    }

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
    void convertDoubleToInt32(FloatRegister src, Register dest, Label* fail,
                              bool negativeZeroCheck = true);
    void convertFloat32ToInt32(FloatRegister src, Register dest, Label* fail,
                               bool negativeZeroCheck = true);

    void convertFloat32ToDouble(FloatRegister src, FloatRegister dest);
    void convertInt32ToFloat32(Register src, FloatRegister dest);
    void convertInt32ToFloat32(const Address& src, FloatRegister dest);

    void wasmTruncateToInt32(FloatRegister input, Register output, MIRType fromType,
                             bool isUnsigned, bool isSaturating, Label* oolEntry);
    void outOfLineWasmTruncateToIntCheck(FloatRegister input, MIRType fromType,
                                         MIRType toType, TruncFlags flags,
                                         Label* rejoin, wasm::BytecodeOffset trapOffset);

    // Somewhat direct wrappers for the low-level assembler funcitons
    // bitops. Attempt to encode a virtual alu instruction using two real
    // instructions.
  private:
    bool alu_dbl(Register src1, Imm32 imm, Register dest, ALUOp op,
                 SBit s, Condition c);

  public:
    void ma_alu(Register src1, Imm32 imm, Register dest, AutoRegisterScope& scratch,
                ALUOp op, SBit s = LeaveCC, Condition c = Always);
    void ma_alu(Register src1, Operand2 op2, Register dest, ALUOp op,
                SBit s = LeaveCC, Condition c = Always);
    void ma_alu(Register src1, Operand op2, Register dest, ALUOp op,
                SBit s = LeaveCC, Condition c = Always);
    void ma_nop();

    BufferOffset ma_movPatchable(Imm32 imm, Register dest, Assembler::Condition c);
    BufferOffset ma_movPatchable(ImmPtr imm, Register dest, Assembler::Condition c);

    // To be used with Iter := InstructionIterator or BufferInstructionIterator.
    template<class Iter>
    static void ma_mov_patch(Imm32 imm, Register dest, Assembler::Condition c,
                             RelocStyle rs, Iter iter);

    // ALU based ops
    // mov
    void ma_mov(Register src, Register dest, SBit s = LeaveCC, Condition c = Always);

    void ma_mov(Imm32 imm, Register dest, Condition c = Always);
    void ma_mov(ImmWord imm, Register dest, Condition c = Always);

    void ma_mov(ImmGCPtr ptr, Register dest);

    // Shifts (just a move with a shifting op2)
    void ma_lsl(Imm32 shift, Register src, Register dst);
    void ma_lsr(Imm32 shift, Register src, Register dst);
    void ma_asr(Imm32 shift, Register src, Register dst);
    void ma_ror(Imm32 shift, Register src, Register dst);
    void ma_rol(Imm32 shift, Register src, Register dst);

    void ma_lsl(Register shift, Register src, Register dst);
    void ma_lsr(Register shift, Register src, Register dst);
    void ma_asr(Register shift, Register src, Register dst);
    void ma_ror(Register shift, Register src, Register dst);
    void ma_rol(Register shift, Register src, Register dst, AutoRegisterScope& scratch);

    // Move not (dest <- ~src)
    void ma_mvn(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Negate (dest <- -src) implemented as rsb dest, src, 0
    void ma_neg(Register src, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    // And
    void ma_and(Register src, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_and(Register src1, Register src2, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_and(Imm32 imm, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);

    void ma_and(Imm32 imm, Register src1, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);

    // Bit clear (dest <- dest & ~imm) or (dest <- src1 & ~src2)
    void ma_bic(Imm32 imm, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);

    // Exclusive or
    void ma_eor(Register src, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_eor(Register src1, Register src2, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_eor(Imm32 imm, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);

    void ma_eor(Imm32 imm, Register src1, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);

    // Or
    void ma_orr(Register src, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_orr(Register src1, Register src2, Register dest,
                SBit s = LeaveCC, Condition c = Always);

    void ma_orr(Imm32 imm, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);

    void ma_orr(Imm32 imm, Register src1, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);


    // Arithmetic based ops.
    // Add with carry:
    void ma_adc(Imm32 imm, Register dest, AutoRegisterScope& scratch, SBit s = LeaveCC, Condition c = Always);
    void ma_adc(Register src, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_adc(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Add:
    void ma_add(Imm32 imm, Register dest, AutoRegisterScope& scratch, SBit s = LeaveCC, Condition c = Always);
    void ma_add(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_add(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_add(Register src1, Operand op, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_add(Register src1, Imm32 op, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);

    // Subtract with carry:
    void ma_sbc(Imm32 imm, Register dest, AutoRegisterScope& scratch, SBit s = LeaveCC, Condition c = Always);
    void ma_sbc(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sbc(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Subtract:
    void ma_sub(Imm32 imm, Register dest, AutoRegisterScope& scratch, SBit s = LeaveCC, Condition c = Always);
    void ma_sub(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sub(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sub(Register src1, Operand op, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_sub(Register src1, Imm32 op, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);

    // Reverse subtract:
    void ma_rsb(Imm32 imm, Register dest, AutoRegisterScope& scratch, SBit s = LeaveCC, Condition c = Always);
    void ma_rsb(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_rsb(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_rsb(Register src1, Imm32 op2, Register dest, AutoRegisterScope& scratch,
                SBit s = LeaveCC, Condition c = Always);

    // Reverse subtract with carry:
    void ma_rsc(Imm32 imm, Register dest, AutoRegisterScope& scratch, SBit s = LeaveCC, Condition c = Always);
    void ma_rsc(Register src1, Register dest, SBit s = LeaveCC, Condition c = Always);
    void ma_rsc(Register src1, Register src2, Register dest, SBit s = LeaveCC, Condition c = Always);

    // Compares/tests.
    // Compare negative (sets condition codes as src1 + src2 would):
    void ma_cmn(Register src1, Imm32 imm, AutoRegisterScope& scratch, Condition c = Always);
    void ma_cmn(Register src1, Register src2, Condition c = Always);
    void ma_cmn(Register src1, Operand op, Condition c = Always);

    // Compare (src - src2):
    void ma_cmp(Register src1, Imm32 imm, AutoRegisterScope& scratch, Condition c = Always);
    void ma_cmp(Register src1, ImmTag tag, Condition c = Always);
    void ma_cmp(Register src1, ImmWord ptr, AutoRegisterScope& scratch, Condition c = Always);
    void ma_cmp(Register src1, ImmGCPtr ptr, AutoRegisterScope& scratch, Condition c = Always);
    void ma_cmp(Register src1, Operand op, AutoRegisterScope& scratch, AutoRegisterScope& scratch2,
                Condition c = Always);
    void ma_cmp(Register src1, Register src2, Condition c = Always);

    // Test for equality, (src1 ^ src2):
    void ma_teq(Register src1, Imm32 imm, AutoRegisterScope& scratch, Condition c = Always);
    void ma_teq(Register src1, Register src2, Condition c = Always);
    void ma_teq(Register src1, Operand op, Condition c = Always);

    // Test (src1 & src2):
    void ma_tst(Register src1, Imm32 imm, AutoRegisterScope& scratch, Condition c = Always);
    void ma_tst(Register src1, Register src2, Condition c = Always);
    void ma_tst(Register src1, Operand op, Condition c = Always);

    // Multiplies. For now, there are only two that we care about.
    void ma_mul(Register src1, Register src2, Register dest);
    void ma_mul(Register src1, Imm32 imm, Register dest, AutoRegisterScope& scratch);
    Condition ma_check_mul(Register src1, Register src2, Register dest,
                           AutoRegisterScope& scratch, Condition cond);
    Condition ma_check_mul(Register src1, Imm32 imm, Register dest,
                           AutoRegisterScope& scratch, Condition cond);

    void ma_umull(Register src1, Imm32 imm, Register destHigh, Register destLow, AutoRegisterScope& scratch);
    void ma_umull(Register src1, Register src2, Register destHigh, Register destLow);

    // Fast mod, uses scratch registers, and thus needs to be in the assembler
    // implicitly assumes that we can overwrite dest at the beginning of the
    // sequence.
    void ma_mod_mask(Register src, Register dest, Register hold, Register tmp,
                     AutoRegisterScope& scratch, AutoRegisterScope& scratch2, int32_t shift);

    // Mod - depends on integer divide instructions being supported.
    void ma_smod(Register num, Register div, Register dest, AutoRegisterScope& scratch);
    void ma_umod(Register num, Register div, Register dest, AutoRegisterScope& scratch);

    // Division - depends on integer divide instructions being supported.
    void ma_sdiv(Register num, Register div, Register dest, Condition cond = Always);
    void ma_udiv(Register num, Register div, Register dest, Condition cond = Always);
    // Misc operations
    void ma_clz(Register src, Register dest, Condition cond = Always);
    void ma_ctz(Register src, Register dest, AutoRegisterScope& scratch);
    // Memory:
    // Shortcut for when we know we're transferring 32 bits of data.
    void ma_dtr(LoadStore ls, Register rn, Imm32 offset, Register rt, AutoRegisterScope& scratch,
                Index mode = Offset, Condition cc = Always);
    void ma_dtr(LoadStore ls, Register rt, const Address& addr, AutoRegisterScope& scratch,
                Index mode, Condition cc);

    void ma_str(Register rt, DTRAddr addr, Index mode = Offset, Condition cc = Always);
    void ma_str(Register rt, const Address& addr, AutoRegisterScope& scratch,
                Index mode = Offset, Condition cc = Always);

    void ma_ldr(DTRAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldr(const Address& addr, Register rt, AutoRegisterScope& scratch,
                Index mode = Offset, Condition cc = Always);

    void ma_ldrb(DTRAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldrh(EDtrAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldrsh(EDtrAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldrsb(EDtrAddr addr, Register rt, Index mode = Offset, Condition cc = Always);
    void ma_ldrd(EDtrAddr addr, Register rt, DebugOnly<Register> rt2, Index mode = Offset,
                 Condition cc = Always);
    void ma_strb(Register rt, DTRAddr addr, Index mode = Offset, Condition cc = Always);
    void ma_strh(Register rt, EDtrAddr addr, Index mode = Offset, Condition cc = Always);
    void ma_strd(Register rt, DebugOnly<Register> rt2, EDtrAddr addr, Index mode = Offset,
                 Condition cc = Always);

    // Specialty for moving N bits of data, where n == 8,16,32,64.
    BufferOffset ma_dataTransferN(LoadStore ls, int size, bool IsSigned,
                                  Register rn, Register rm, Register rt, AutoRegisterScope& scratch,
                                  Index mode = Offset, Condition cc = Always,
                                  Scale scale = TimesOne);

    BufferOffset ma_dataTransferN(LoadStore ls, int size, bool IsSigned,
                                  Register rn, Register rm, Register rt,
                                  Index mode = Offset, Condition cc = Always);

    BufferOffset ma_dataTransferN(LoadStore ls, int size, bool IsSigned,
                                  Register rn, Imm32 offset, Register rt, AutoRegisterScope& scratch,
                                  Index mode = Offset, Condition cc = Always);

    void ma_pop(Register r);
    void ma_popn_pc(Imm32 n, AutoRegisterScope& scratch, AutoRegisterScope& scratch2);
    void ma_push(Register r);
    void ma_push_sp(Register r, AutoRegisterScope& scratch);

    void ma_vpop(VFPRegister r);
    void ma_vpush(VFPRegister r);

    // Barriers.
    void ma_dmb(BarrierOption option=BarrierSY);
    void ma_dsb(BarrierOption option=BarrierSY);

    // Branches when done from within arm-specific code.
    BufferOffset ma_b(Label* dest, Condition c = Always);
    BufferOffset ma_b(wasm::OldTrapDesc target, Condition c = Always);
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

    BufferOffset ma_vdtr(LoadStore ls, const Address& addr, VFPRegister dest, AutoRegisterScope& scratch,
                         Condition cc = Always);

    BufferOffset ma_vldr(VFPAddr addr, VFPRegister dest, Condition cc = Always);
    BufferOffset ma_vldr(const Address& addr, VFPRegister dest, AutoRegisterScope& scratch, Condition cc = Always);
    BufferOffset ma_vldr(VFPRegister src, Register base, Register index, AutoRegisterScope& scratch,
                         int32_t shift = defaultShift, Condition cc = Always);

    BufferOffset ma_vstr(VFPRegister src, VFPAddr addr, Condition cc = Always);
    BufferOffset ma_vstr(VFPRegister src, const Address& addr, AutoRegisterScope& scratch, Condition cc = Always);
    BufferOffset ma_vstr(VFPRegister src, Register base, Register index, AutoRegisterScope& scratch,
                         AutoRegisterScope& scratch2, int32_t shift, int32_t offset, Condition cc = Always);
    BufferOffset ma_vstr(VFPRegister src, Register base, Register index, AutoRegisterScope& scratch,
                         int32_t shift, Condition cc = Always);

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

    // `outAny` is valid if and only if `out64` == Register64::Invalid().
    void wasmLoadImpl(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                      Register ptrScratch, AnyRegister outAny, Register64 out64);

    // `valAny` is valid if and only if `val64` == Register64::Invalid().
    void wasmStoreImpl(const wasm::MemoryAccessDesc& access, AnyRegister valAny, Register64 val64,
                       Register memoryBase, Register ptr, Register ptrScratch);

  protected:
    // `outAny` is valid if and only if `out64` == Register64::Invalid().
    void wasmUnalignedLoadImpl(const wasm::MemoryAccessDesc& access, Register memoryBase,
                               Register ptr, Register ptrScratch, AnyRegister outAny,
                               Register64 out64, Register tmp1, Register tmp2, Register tmp3);

    // The value to be stored is in `floatValue` (if not invalid), `val64` (if not invalid),
    // or in `valOrTmp` (if `floatValue` and `val64` are both invalid).  Note `valOrTmp` must
    // always be valid.
    void wasmUnalignedStoreImpl(const wasm::MemoryAccessDesc& access, FloatRegister floatValue,
                                Register64 val64, Register memoryBase, Register ptr,
                                Register ptrScratch, Register valOrTmp);

  private:
    // Loads `byteSize` bytes, byte by byte, by reading from ptr[offset],
    // applying the indicated signedness (defined by isSigned).
    // - all three registers must be different.
    // - tmp and dest will get clobbered, ptr will remain intact.
    // - byteSize can be up to 4 bytes and no more (GPR are 32 bits on ARM).
    void emitUnalignedLoad(bool isSigned, unsigned byteSize, Register ptr, Register tmp,
                           Register dest, unsigned offset = 0);

    // Ditto, for a store. Note stores don't care about signedness.
    // - the two registers must be different.
    // - val will get clobbered, ptr will remain intact.
    // - byteSize can be up to 4 bytes and no more (GPR are 32 bits on ARM).
    void emitUnalignedStore(unsigned byteSize, Register ptr, Register val, unsigned offset = 0);

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

    void branch(JitCode* c) {
        BufferOffset bo = m_buffer.nextOffset();
        addPendingJump(bo, ImmPtr(c->raw()), Relocation::JITCODE);
        ScratchRegisterScope scratch(asMasm());
        ma_movPatchable(ImmPtr(c->raw()), scratch, Always);
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
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());
        ma_popn_pc(n, scratch, scratch2);
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
        SecondScratchRegisterScope scratch2(asMasm());
        ma_ldr(addr, scratch, scratch2);
        ma_push(scratch);
    }
    void push(Register reg) {
        if (reg == sp) {
            ScratchRegisterScope scratch(asMasm());
            ma_push_sp(reg, scratch);
        } else {
            ma_push(reg);
        }
    }
    void push(FloatRegister reg) {
        ma_vpush(VFPRegister(reg));
    }
    void pushWithPadding(Register reg, const Imm32 extraSpace) {
        ScratchRegisterScope scratch(asMasm());
        Imm32 totSpace = Imm32(extraSpace.value + 4);
        ma_dtr(IsStore, sp, totSpace, reg, scratch, PreIndex);
    }
    void pushWithPadding(Imm32 imm, const Imm32 extraSpace) {
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());
        Imm32 totSpace = Imm32(extraSpace.value + 4);
        ma_mov(imm, scratch);
        ma_dtr(IsStore, sp, totSpace, scratch, scratch2, PreIndex);
    }

    void pop(Register reg) {
        ma_pop(reg);
    }
    void pop(FloatRegister reg) {
        ma_vpop(VFPRegister(reg));
    }

    void popN(Register reg, Imm32 extraSpace) {
        ScratchRegisterScope scratch(asMasm());
        Imm32 totSpace = Imm32(extraSpace.value + 4);
        ma_dtr(IsLoad, sp, totSpace, reg, scratch, PostIndex);
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
        ma_movPatchable(Imm32(imm.value), dest, Always);
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
    void jump(TrampolinePtr code) {
        ScratchRegisterScope scratch(asMasm());
        movePtr(ImmPtr(code.value), scratch);
        ma_bx(scratch);
    }
    void jump(Register reg) {
        ma_bx(reg);
    }
    void jump(const Address& addr) {
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());
        ma_ldr(addr, scratch, scratch2);
        ma_bx(scratch);
    }
    void jump(wasm::OldTrapDesc target) {
        as_b(target);
    }

    void negl(Register reg) {
        ma_neg(reg, reg, SetCC);
    }
    void test32(Register lhs, Register rhs) {
        ma_tst(lhs, rhs);
    }
    void test32(Register lhs, Imm32 imm) {
        ScratchRegisterScope scratch(asMasm());
        ma_tst(lhs, imm, scratch);
    }
    void test32(const Address& addr, Imm32 imm) {
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());
        ma_ldr(addr, scratch, scratch2);
        ma_tst(scratch, imm, scratch2);
    }
    void testPtr(Register lhs, Register rhs) {
        test32(lhs, rhs);
    }

    void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag) {
        MOZ_ASSERT(value.typeReg() == tag);
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

    // Unboxing code.
    void unboxNonDouble(const ValueOperand& operand, Register dest, JSValueType type);
    void unboxNonDouble(const Address& src, Register dest, JSValueType type);
    void unboxNonDouble(const BaseIndex& src, Register dest, JSValueType type);
    void unboxInt32(const ValueOperand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_INT32);
    }
    void unboxInt32(const Address& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_INT32);
    }
    void unboxBoolean(const ValueOperand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_BOOLEAN);
    }
    void unboxBoolean(const Address& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_BOOLEAN);
    }
    void unboxString(const ValueOperand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
    }
    void unboxString(const Address& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
    }
    void unboxSymbol(const ValueOperand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
    }
    void unboxSymbol(const Address& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
    }
    void unboxObject(const ValueOperand& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    }
    void unboxObject(const Address& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    }
    void unboxObject(const BaseIndex& src, Register dest) {
        unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    }
    void unboxDouble(const ValueOperand& src, FloatRegister dest);
    void unboxDouble(const Address& src, FloatRegister dest);
    void unboxValue(const ValueOperand& src, AnyRegister dest, JSValueType type);
    void unboxPrivate(const ValueOperand& src, Register dest);

    // See comment in MacroAssembler-x64.h.
    void unboxGCThingForPreBarrierTrampoline(const Address& src, Register dest) {
        load32(ToPayload(src), dest);
    }

    void notBoolean(const ValueOperand& val) {
        as_eor(val.payloadReg(), val.payloadReg(), Imm8(1));
    }

    // Boxing code.
    void boxDouble(FloatRegister src, const ValueOperand& dest, FloatRegister);
    void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest);

    // Extended unboxing API. If the payload is already in a register, returns
    // that register. Otherwise, provides a move to the given scratch register,
    // and returns that.
    Register extractObject(const Address& address, Register scratch);
    Register extractObject(const ValueOperand& value, Register scratch) {
        unboxNonDouble(value, value.payloadReg(), JSVAL_TYPE_OBJECT);
        return value.payloadReg();
    }
    Register extractString(const ValueOperand& value, Register scratch) {
        unboxNonDouble(value, value.payloadReg(), JSVAL_TYPE_STRING);
        return value.payloadReg();
    }
    Register extractSymbol(const ValueOperand& value, Register scratch) {
        unboxNonDouble(value, value.payloadReg(), JSVAL_TYPE_SYMBOL);
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

    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Condition cond = Always,
                                 Label* documentation = nullptr);
    CodeOffsetJump backedgeJump(RepatchLabel* label, Label* documentation) {
        return jumpWithPatch(label, Always, documentation);
    }

    void loadUnboxedValue(Address address, MIRType type, AnyRegister dest) {
        if (dest.isFloat()) {
            loadInt32OrDouble(address, dest.fpu());
        } else {
            ScratchRegisterScope scratch(asMasm());
            ma_ldr(address, dest.gpr(), scratch);
        }
    }

    void loadUnboxedValue(BaseIndex address, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(address.base, address.index, dest.fpu(), address.scale);
        else
            load32(address, dest.gpr());
    }

    template <typename T>
    void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes, JSValueType) {
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

    void storeValue(ValueOperand val, const Address& dst);
    void storeValue(ValueOperand val, const BaseIndex& dest);
    void storeValue(JSValueType type, Register reg, BaseIndex dest) {
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());

        int32_t payloadoffset = dest.offset + NUNBOX32_PAYLOAD_OFFSET;
        int32_t typeoffset = dest.offset + NUNBOX32_TYPE_OFFSET;

        ma_alu(dest.base, lsl(dest.index, dest.scale), scratch, OpAdd);

        // Store the payload.
        if (payloadoffset < 4096 && payloadoffset > -4096)
            ma_str(reg, DTRAddr(scratch, DtrOffImm(payloadoffset)));
        else
            ma_str(reg, Address(scratch, payloadoffset), scratch2);

        // Store the type.
        if (typeoffset < 4096 && typeoffset > -4096) {
            // Encodable as DTRAddr, so only two instructions needed.
            ma_mov(ImmTag(JSVAL_TYPE_TO_TAG(type)), scratch2);
            ma_str(scratch2, DTRAddr(scratch, DtrOffImm(typeoffset)));
        } else {
            // Since there are only two scratch registers, the offset must be
            // applied early using a third instruction to be safe.
            ma_add(Imm32(typeoffset), scratch, scratch2);
            ma_mov(ImmTag(JSVAL_TYPE_TO_TAG(type)), scratch2);
            ma_str(scratch2, DTRAddr(scratch, DtrOffImm(0)));
        }
    }
    void storeValue(JSValueType type, Register reg, Address dest) {
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());

        ma_str(reg, dest, scratch2);
        ma_mov(ImmTag(JSVAL_TYPE_TO_TAG(type)), scratch);
        ma_str(scratch, Address(dest.base, dest.offset + NUNBOX32_TYPE_OFFSET), scratch2);
    }
    void storeValue(const Value& val, const Address& dest) {
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());

        ma_mov(Imm32(val.toNunboxTag()), scratch);
        ma_str(scratch, ToType(dest), scratch2);
        if (val.isGCThing())
            ma_mov(ImmGCPtr(val.toGCThing()), scratch);
        else
            ma_mov(Imm32(val.toNunboxPayload()), scratch);
        ma_str(scratch, ToPayload(dest), scratch2);
    }
    void storeValue(const Value& val, BaseIndex dest) {
        ScratchRegisterScope scratch(asMasm());
        SecondScratchRegisterScope scratch2(asMasm());

        int32_t typeoffset = dest.offset + NUNBOX32_TYPE_OFFSET;
        int32_t payloadoffset = dest.offset + NUNBOX32_PAYLOAD_OFFSET;

        ma_alu(dest.base, lsl(dest.index, dest.scale), scratch, OpAdd);

        // Store the type.
        if (typeoffset < 4096 && typeoffset > -4096) {
            ma_mov(Imm32(val.toNunboxTag()), scratch2);
            ma_str(scratch2, DTRAddr(scratch, DtrOffImm(typeoffset)));
        } else {
            ma_add(Imm32(typeoffset), scratch, scratch2);
            ma_mov(Imm32(val.toNunboxTag()), scratch2);
            ma_str(scratch2, DTRAddr(scratch, DtrOffImm(0)));
            // Restore scratch for the payload store.
            ma_alu(dest.base, lsl(dest.index, dest.scale), scratch, OpAdd);
        }

        // Store the payload, marking if necessary.
        if (payloadoffset < 4096 && payloadoffset > -4096) {
            if (val.isGCThing())
                ma_mov(ImmGCPtr(val.toGCThing()), scratch2);
            else
                ma_mov(Imm32(val.toNunboxPayload()), scratch2);
            ma_str(scratch2, DTRAddr(scratch, DtrOffImm(payloadoffset)));
        } else {
            ma_add(Imm32(payloadoffset), scratch, scratch2);
            if (val.isGCThing())
                ma_mov(ImmGCPtr(val.toGCThing()), scratch2);
            else
                ma_mov(Imm32(val.toNunboxPayload()), scratch2);
            ma_str(scratch2, DTRAddr(scratch, DtrOffImm(0)));
        }
    }
    void storeValue(const Address& src, const Address& dest, Register temp) {
        load32(ToType(src), temp);
        store32(temp, ToType(dest));

        load32(ToPayload(src), temp);
        store32(temp, ToPayload(dest));
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
        push(Imm32(val.toNunboxTag()));
        if (val.isGCThing())
            push(ImmGCPtr(val.toGCThing()));
        else
            push(Imm32(val.toNunboxPayload()));
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

    void handleFailureWithHandlerTail(void* handler, Label* profilerExitTail);

    /////////////////////////////////////////////////////////////////
    // Common interface.
    /////////////////////////////////////////////////////////////////
  public:
    void not32(Register reg);

    void move32(Imm32 imm, Register dest);
    void move32(Register src, Register dest);

    void movePtr(Register src, Register dest);
    void movePtr(ImmWord imm, Register dest);
    void movePtr(ImmPtr imm, Register dest);
    void movePtr(wasm::SymbolicAddress imm, Register dest);
    void movePtr(ImmGCPtr imm, Register dest);

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
        load32(LowWord(address), dest.low);
        load32(HighWord(address), dest.high);
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
    void loadInt32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeInt32x1(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x1(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x2(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x2(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x3(FloatRegister src, const Address& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x3(FloatRegister src, const BaseIndex& dest) { MOZ_CRASH("NYI"); }
    void storeInt32x4(FloatRegister src, const Address& addr) { MOZ_CRASH("NYI"); }
    void loadAlignedSimd128Int(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeAlignedSimd128Int(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void loadUnalignedSimd128Int(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadUnalignedSimd128Int(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeUnalignedSimd128Int(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void storeUnalignedSimd128Int(FloatRegister src, BaseIndex addr) { MOZ_CRASH("NYI"); }

    void loadFloat32x3(const Address& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadFloat32x3(const BaseIndex& src, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadFloat32x4(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeFloat32x4(FloatRegister src, const Address& addr) { MOZ_CRASH("NYI"); }

    void loadAlignedSimd128Float(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeAlignedSimd128Float(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void loadUnalignedSimd128Float(const Address& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void loadUnalignedSimd128Float(const BaseIndex& addr, FloatRegister dest) { MOZ_CRASH("NYI"); }
    void storeUnalignedSimd128Float(FloatRegister src, Address addr) { MOZ_CRASH("NYI"); }
    void storeUnalignedSimd128Float(FloatRegister src, BaseIndex addr) { MOZ_CRASH("NYI"); }

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

    void store64(Register64 src, Address address) {
        store32(src.low, LowWord(address));
        store32(src.high, HighWord(address));
    }

    void store64(Imm64 imm, Address address) {
        store32(imm.low(), LowWord(address));
        store32(imm.hi(), HighWord(address));
    }

    void storePtr(ImmWord imm, const Address& address);
    void storePtr(ImmWord imm, const BaseIndex& address);
    void storePtr(ImmPtr imm, const Address& address);
    void storePtr(ImmPtr imm, const BaseIndex& address);
    void storePtr(ImmGCPtr imm, const Address& address);
    void storePtr(ImmGCPtr imm, const BaseIndex& address);
    void storePtr(Register src, const Address& address);
    void storePtr(Register src, const BaseIndex& address);
    void storePtr(Register src, AbsoluteAddress dest);

    void moveDouble(FloatRegister src, FloatRegister dest, Condition cc = Always) {
        ma_vmov(src, dest, cc);
    }

    inline void incrementInt32Value(const Address& addr);

    void cmp32(Register lhs, Imm32 rhs);
    void cmp32(Register lhs, Register rhs);
    void cmp32(const Address& lhs, Imm32 rhs) {
        MOZ_CRASH("NYI");
    }
    void cmp32(const Address& lhs, Register rhs) {
        MOZ_CRASH("NYI");
    }

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

    void setStackArg(Register reg, uint32_t arg);

    void breakpoint();
    // Conditional breakpoint.
    void breakpoint(Condition cc);

    // Trigger the simulator's interactive read-eval-print loop.
    // The message will be printed at the stopping point.
    // (On non-simulator builds, does nothing.)
    void simulatorStop(const char* msg);

    // Evaluate srcDest = minmax<isMax>{Float32,Double}(srcDest, other).
    // Checks for NaN if canBeNaN is true.
    void minMaxDouble(FloatRegister srcDest, FloatRegister other, bool canBeNaN, bool isMax);
    void minMaxFloat32(FloatRegister srcDest, FloatRegister other, bool canBeNaN, bool isMax);

    void compareDouble(FloatRegister lhs, FloatRegister rhs);

    void compareFloat(FloatRegister lhs, FloatRegister rhs);

    void checkStackAlignment();

    // If source is a double, load it into dest. If source is int32, convert it
    // to double. Else, branch to failure.
    void ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure);

    void
    emitSet(Assembler::Condition cond, Register dest)
    {
        ma_mov(Imm32(0), dest);
        ma_mov(Imm32(1), dest, cond);
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
        ScratchRegisterScope scratch(asMasm());
        ma_add(address.base, Imm32(address.offset), dest, scratch, LeaveCC);
    }
    void computeEffectiveAddress(const BaseIndex& address, Register dest) {
        ScratchRegisterScope scratch(asMasm());
        ma_alu(address.base, lsl(address.index, address.scale), dest, OpAdd, LeaveCC);
        if (address.offset)
            ma_add(dest, Imm32(address.offset), dest, scratch, LeaveCC);
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
        SecondScratchRegisterScope scratch2(asMasm());
        ma_sub(r, Imm32(0x80000001), scratch, scratch2);
        as_cmn(scratch, Imm8(3));
        ma_b(handleNotAnInt, Above);
    }

    void lea(Operand addr, Register dest) {
        ScratchRegisterScope scratch(asMasm());
        ma_add(addr.baseReg(), Imm32(addr.disp()), dest, scratch);
    }

    void abiret() {
        as_bx(lr);
    }

    void moveFloat32(FloatRegister src, FloatRegister dest, Condition cc = Always) {
        as_vmov(VFPRegister(dest).singleOverlay(), VFPRegister(src).singleOverlay(), cc);
    }

    void loadWasmGlobalPtr(uint32_t globalDataOffset, Register dest) {
        loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, globalArea) + globalDataOffset), dest);
    }
    void loadWasmPinnedRegsFromTls() {
        ScratchRegisterScope scratch(asMasm());
        ma_ldr(Address(WasmTlsReg, offsetof(wasm::TlsData, memoryBase)), HeapReg, scratch);
    }

    // Instrumentation for entering and leaving the profiler.
    void profilerEnterFrame(Register framePtr, Register scratch);
    void profilerExitFrame();
};

typedef MacroAssemblerARMCompat MacroAssemblerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_arm_MacroAssembler_arm_h */

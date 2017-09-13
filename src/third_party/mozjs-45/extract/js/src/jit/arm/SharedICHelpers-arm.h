/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_SharedICHelpers_arm_h
#define jit_arm_SharedICHelpers_arm_h

#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICRegisters.h"

namespace js {
namespace jit {

// Distance from sp to the top Value inside an IC stub (no return address on the stack on ARM).
static const size_t ICStackValueOffset = 0;

inline void
EmitRestoreTailCallReg(MacroAssembler& masm)
{
    // No-op on ARM because link register is always holding the return address.
}

inline void
EmitRepushTailCallReg(MacroAssembler& masm)
{
    // No-op on ARM because link register is always holding the return address.
}

inline void
EmitCallIC(CodeOffset* patchOffset, MacroAssembler& masm)
{
    // Move ICEntry offset into ICStubReg
    CodeOffset offset = masm.movWithPatch(ImmWord(-1), ICStubReg);
    *patchOffset = offset;

    // Load stub pointer into ICStubReg
    masm.loadPtr(Address(ICStubReg, ICEntry::offsetOfFirstStub()), ICStubReg);

    // Load stubcode pointer from BaselineStubEntry.
    // R2 won't be active when we call ICs, so we can use r0.
    MOZ_ASSERT(R2 == ValueOperand(r1, r0));
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

    // Call the stubcode via a direct branch-and-link.
    masm.ma_blx(r0);
}

inline void
EmitEnterTypeMonitorIC(MacroAssembler& masm,
                       size_t monitorStubOffset = ICMonitoredStub::offsetOfFirstMonitorStub())
{
    // This is expected to be called from within an IC, when ICStubReg is
    // properly initialized to point to the stub.
    masm.loadPtr(Address(ICStubReg, (uint32_t) monitorStubOffset), ICStubReg);

    // Load stubcode pointer from BaselineStubEntry.
    // R2 won't be active when we call ICs, so we can use r0.
    MOZ_ASSERT(R2 == ValueOperand(r1, r0));
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

    // Jump to the stubcode.
    masm.branch(r0);
}

inline void
EmitReturnFromIC(MacroAssembler& masm)
{
    masm.ma_mov(lr, pc);
}

inline void
EmitChangeICReturnAddress(MacroAssembler& masm, Register reg)
{
    masm.ma_mov(reg, lr);
}

inline void
EmitBaselineTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t argSize)
{
    // We assume during this that R0 and R1 have been pushed, and that R2 is
    // unused.
    MOZ_ASSERT(R2 == ValueOperand(r1, r0));

    // Compute frame size.
    masm.movePtr(BaselineFrameReg, r0);
    masm.ma_add(Imm32(BaselineFrame::FramePointerOffset), r0);
    masm.ma_sub(BaselineStackReg, r0);

    // Store frame size without VMFunction arguments for GC marking.
    masm.ma_sub(r0, Imm32(argSize), r1);
    masm.store32(r1, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Push frame descriptor and perform the tail call.
    // ICTailCallReg (lr) already contains the return address (as we keep
    // it there through the stub calls), but the VMWrapper code being called
    // expects the return address to also be pushed on the stack.
    MOZ_ASSERT(ICTailCallReg == lr);
    masm.makeFrameDescriptor(r0, JitFrame_BaselineJS);
    masm.push(r0);
    masm.push(lr);
    masm.branch(target);
}

inline void
EmitIonTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t stackSize)
{
    // We assume during this that R0 and R1 have been pushed, and that R2 is
    // unused.
    MOZ_ASSERT(R2 == ValueOperand(r1, r0));

    masm.loadPtr(Address(sp, stackSize), r0);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), r0);
    masm.add32(Imm32(stackSize + JitStubFrameLayout::Size() - sizeof(intptr_t)), r0);

    // Push frame descriptor and perform the tail call.
    // ICTailCallReg (lr) already contains the return address (as we keep
    // it there through the stub calls), but the VMWrapper code being called
    // expects the return address to also be pushed on the stack.
    MOZ_ASSERT(ICTailCallReg == lr);
    masm.makeFrameDescriptor(r0, JitFrame_IonJS);
    masm.push(r0);
    masm.push(lr);
    masm.branch(target);
}

inline void
EmitBaselineCreateStubFrameDescriptor(MacroAssembler& masm, Register reg)
{
    // Compute stub frame size. We have to add two pointers: the stub reg and
    // previous frame pointer pushed by EmitEnterStubFrame.
    masm.mov(BaselineFrameReg, reg);
    masm.ma_add(Imm32(sizeof(void*) * 2), reg);
    masm.ma_sub(BaselineStackReg, reg);

    masm.makeFrameDescriptor(reg, JitFrame_BaselineStub);
}

inline void
EmitBaselineCallVM(JitCode* target, MacroAssembler& masm)
{
    EmitBaselineCreateStubFrameDescriptor(masm, r0);
    masm.push(r0);
    masm.call(target);
}

inline void
EmitIonCallVM(JitCode* target, size_t stackSlots, MacroAssembler& masm)
{
    uint32_t descriptor = MakeFrameDescriptor(masm.framePushed(), JitFrame_IonStub);
    masm.Push(Imm32(descriptor));
    masm.callJit(target);

    // Remove rest of the frame left on the stack. We remove the return address
    // which is implicitly popped when returning.
    size_t framePop = sizeof(ExitFrameLayout) - sizeof(void*);

    // Pop arguments from framePushed.
    masm.implicitPop(stackSlots * sizeof(void*) + framePop);
}

// Size of vales pushed by EmitEnterStubFrame.
static const uint32_t STUB_FRAME_SIZE = 4 * sizeof(void*);
static const uint32_t STUB_FRAME_SAVED_STUB_OFFSET = sizeof(void*);

inline void
EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch)
{
    MOZ_ASSERT(scratch != ICTailCallReg);

    // Compute frame size.
    masm.mov(BaselineFrameReg, scratch);
    masm.ma_add(Imm32(BaselineFrame::FramePointerOffset), scratch);
    masm.ma_sub(BaselineStackReg, scratch);

    masm.store32(scratch, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Note: when making changes here, don't forget to update STUB_FRAME_SIZE if
    // needed.

    // Push frame descriptor and return address.
    masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS);
    masm.Push(scratch);
    masm.Push(ICTailCallReg);

    // Save old frame pointer, stack pointer and stub reg.
    masm.Push(ICStubReg);
    masm.Push(BaselineFrameReg);
    masm.mov(BaselineStackReg, BaselineFrameReg);

    // We pushed 4 words, so the stack is still aligned to 8 bytes.
    masm.checkStackAlignment();
}

inline void
EmitIonEnterStubFrame(MacroAssembler& masm, Register scratch)
{
    MOZ_ASSERT(ICTailCallReg == lr);
    masm.Push(ICTailCallReg);
    masm.Push(ICStubReg);
}

inline void
EmitBaselineLeaveStubFrame(MacroAssembler& masm, bool calledIntoIon = false)
{
    ScratchRegisterScope scratch(masm);

    // Ion frames do not save and restore the frame pointer. If we called into
    // Ion, we have to restore the stack pointer from the frame descriptor. If
    // we performed a VM call, the descriptor has been popped already so in that
    // case we use the frame pointer.
    if (calledIntoIon) {
        masm.Pop(scratch);
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch);
        masm.add32(scratch, BaselineStackReg);
    } else {
        masm.mov(BaselineFrameReg, BaselineStackReg);
    }

    masm.Pop(BaselineFrameReg);
    masm.Pop(ICStubReg);

    // Load the return address.
    masm.Pop(ICTailCallReg);

    // Discard the frame descriptor.
    masm.Pop(scratch);
}

inline void
EmitIonLeaveStubFrame(MacroAssembler& masm)
{
    masm.Pop(ICStubReg);
    masm.Pop(ICTailCallReg);
}

inline void
EmitStowICValues(MacroAssembler& masm, int values)
{
    MOZ_ASSERT(values >= 0 && values <= 2);
    switch(values) {
      case 1:
        // Stow R0.
        masm.Push(R0);
        break;
      case 2:
        // Stow R0 and R1.
        masm.Push(R0);
        masm.Push(R1);
        break;
    }
}

inline void
EmitUnstowICValues(MacroAssembler& masm, int values, bool discard = false)
{
    MOZ_ASSERT(values >= 0 && values <= 2);
    switch(values) {
      case 1:
        // Unstow R0.
        if (discard)
            masm.addPtr(Imm32(sizeof(Value)), BaselineStackReg);
        else
            masm.popValue(R0);
        break;
      case 2:
        // Unstow R0 and R1.
        if (discard) {
            masm.addPtr(Imm32(sizeof(Value) * 2), BaselineStackReg);
        } else {
            masm.popValue(R1);
            masm.popValue(R0);
        }
        break;
    }
    masm.adjustFrame(-values * sizeof(Value));
}

inline void
EmitCallTypeUpdateIC(MacroAssembler& masm, JitCode* code, uint32_t objectOffset)
{
    MOZ_ASSERT(R2 == ValueOperand(r1, r0));

    // R0 contains the value that needs to be typechecked. The object we're
    // updating is a boxed Value on the stack, at offset objectOffset from esp,
    // excluding the return address.

    // Save the current ICStubReg to stack, as well as the TailCallReg,
    // since on ARM, the LR is live.
    masm.push(ICStubReg);
    masm.push(ICTailCallReg);

    // This is expected to be called from within an IC, when ICStubReg is
    // properly initialized to point to the stub.
    masm.loadPtr(Address(ICStubReg, ICUpdatedStub::offsetOfFirstUpdateStub()),
                 ICStubReg);

    // TODO: Change r0 uses below to use masm's configurable scratch register instead.

    // Load stubcode pointer from ICStubReg into ICTailCallReg.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

    // Call the stubcode.
    masm.ma_blx(r0);

    // Restore the old stub reg and tailcall reg.
    masm.pop(ICTailCallReg);
    masm.pop(ICStubReg);

    // The update IC will store 0 or 1 in R1.scratchReg() reflecting if the
    // value in R0 type-checked properly or not.
    Label success;
    masm.cmp32(R1.scratchReg(), Imm32(1));
    masm.j(Assembler::Equal, &success);

    // If the IC failed, then call the update fallback function.
    EmitBaselineEnterStubFrame(masm, R1.scratchReg());

    masm.loadValue(Address(BaselineStackReg, STUB_FRAME_SIZE + objectOffset), R1);

    masm.Push(R0);
    masm.Push(R1);
    masm.Push(ICStubReg);

    // Load previous frame pointer, push BaselineFrame*.
    masm.loadPtr(Address(BaselineFrameReg, 0), R0.scratchReg());
    masm.pushBaselineFramePtr(R0.scratchReg(), R0.scratchReg());

    EmitBaselineCallVM(code, masm);
    EmitBaselineLeaveStubFrame(masm);

    // Success at end.
    masm.bind(&success);
}

template <typename AddrType>
inline void
EmitPreBarrier(MacroAssembler& masm, const AddrType& addr, MIRType type)
{
    // On ARM, lr is clobbered by patchableCallPreBarrier. Save it first.
    masm.push(lr);
    masm.patchableCallPreBarrier(addr, type);
    masm.pop(lr);
}

inline void
EmitStubGuardFailure(MacroAssembler& masm)
{
    MOZ_ASSERT(R2 == ValueOperand(r1, r0));

    // NOTE: This routine assumes that the stub guard code left the stack in the
    // same state it was in when it was entered.

    // BaselineStubEntry points to the current stub.

    // Load next stub into ICStubReg.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfNext()), ICStubReg);

    // Load stubcode pointer from BaselineStubEntry into scratch register.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

    // Return address is already loaded, just jump to the next stubcode.
    MOZ_ASSERT(ICTailCallReg == lr);
    masm.branch(r0);
}


} // namespace jit
} // namespace js

#endif /* jit_arm_SharedICHelpers_arm_h */

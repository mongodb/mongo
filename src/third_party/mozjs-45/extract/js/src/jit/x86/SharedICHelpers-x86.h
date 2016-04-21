/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_SharedICHelpers_x86_h
#define jit_x86_SharedICHelpers_x86_h

#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICRegisters.h"

namespace js {
namespace jit {

// Distance from stack top to the top Value inside an IC stub (this is the return address).
static const size_t ICStackValueOffset = sizeof(void*);

inline void
EmitRestoreTailCallReg(MacroAssembler& masm)
{
    masm.Pop(ICTailCallReg);
}

inline void
EmitRepushTailCallReg(MacroAssembler& masm)
{
    masm.Push(ICTailCallReg);
}

inline void
EmitCallIC(CodeOffset* patchOffset, MacroAssembler& masm)
{
    // Move ICEntry offset into ICStubReg
    CodeOffset offset = masm.movWithPatch(ImmWord(-1), ICStubReg);
    *patchOffset = offset;

    // Load stub pointer into ICStubReg
    masm.loadPtr(Address(ICStubReg, (int32_t) ICEntry::offsetOfFirstStub()),
                 ICStubReg);

    // Load stubcode pointer from BaselineStubEntry into ICTailCallReg
    // ICTailCallReg will always be unused in the contexts where ICs are called.
    masm.call(Address(ICStubReg, ICStub::offsetOfStubCode()));
}

inline void
EmitEnterTypeMonitorIC(MacroAssembler& masm,
                       size_t monitorStubOffset = ICMonitoredStub::offsetOfFirstMonitorStub())
{
    // This is expected to be called from within an IC, when ICStubReg
    // is properly initialized to point to the stub.
    masm.loadPtr(Address(ICStubReg, (int32_t) monitorStubOffset), ICStubReg);

    // Jump to the stubcode.
    masm.jmp(Operand(ICStubReg, (int32_t) ICStub::offsetOfStubCode()));
}

inline void
EmitReturnFromIC(MacroAssembler& masm)
{
    masm.ret();
}

inline void
EmitChangeICReturnAddress(MacroAssembler& masm, Register reg)
{
    masm.storePtr(reg, Address(StackPointer, 0));
}

inline void
EmitBaselineTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t argSize)
{
    // We assume during this that R0 and R1 have been pushed.

    // Compute frame size.
    masm.movl(BaselineFrameReg, eax);
    masm.addl(Imm32(BaselineFrame::FramePointerOffset), eax);
    masm.subl(BaselineStackReg, eax);

    // Store frame size without VMFunction arguments for GC marking.
    masm.movl(eax, ebx);
    masm.subl(Imm32(argSize), ebx);
    masm.store32(ebx, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Push frame descriptor and perform the tail call.
    masm.makeFrameDescriptor(eax, JitFrame_BaselineJS);
    masm.push(eax);
    masm.push(ICTailCallReg);
    masm.jmp(target);
}

inline void
EmitIonTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t stackSize)
{
    // For tail calls, find the already pushed JitFrame_IonJS signifying the
    // end of the Ion frame. Retrieve the length of the frame and repush
    // JitFrame_IonJS with the extra stacksize, rendering the original
    // JitFrame_IonJS obsolete.

    masm.loadPtr(Address(esp, stackSize), eax);
    masm.shrl(Imm32(FRAMESIZE_SHIFT), eax);
    masm.addl(Imm32(stackSize + JitStubFrameLayout::Size() - sizeof(intptr_t)), eax);

    // Push frame descriptor and perform the tail call.
    masm.makeFrameDescriptor(eax, JitFrame_IonJS);
    masm.push(eax);
    masm.push(ICTailCallReg);
    masm.jmp(target);
}

inline void
EmitBaselineCreateStubFrameDescriptor(MacroAssembler& masm, Register reg)
{
    // Compute stub frame size. We have to add two pointers: the stub reg and previous
    // frame pointer pushed by EmitEnterStubFrame.
    masm.movl(BaselineFrameReg, reg);
    masm.addl(Imm32(sizeof(void*) * 2), reg);
    masm.subl(BaselineStackReg, reg);

    masm.makeFrameDescriptor(reg, JitFrame_BaselineStub);
}

inline void
EmitBaselineCallVM(JitCode* target, MacroAssembler& masm)
{
    EmitBaselineCreateStubFrameDescriptor(masm, eax);
    masm.push(eax);
    masm.call(target);
}

inline void
EmitIonCallVM(JitCode* target, size_t stackSlots, MacroAssembler& masm)
{
    // Stubs often use the return address. Which is actually accounted by the
    // caller of the stub. Though in the stubcode we fake that is part of the
    // stub. In order to make it possible to pop it. As a result we have to
    // fix it here, by subtracting it. Else it would be counted twice.
    uint32_t framePushed = masm.framePushed() - sizeof(void*);

    uint32_t descriptor = MakeFrameDescriptor(framePushed, JitFrame_IonStub);
    masm.Push(Imm32(descriptor));
    masm.call(target);

    // Remove rest of the frame left on the stack. We remove the return address
    // which is implicitly poped when returning.
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

    EmitRestoreTailCallReg(masm);

    // Compute frame size.
    masm.movl(BaselineFrameReg, scratch);
    masm.addl(Imm32(BaselineFrame::FramePointerOffset), scratch);
    masm.subl(BaselineStackReg, scratch);

    masm.store32(scratch, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Note: when making changes here,  don't forget to update STUB_FRAME_SIZE
    // if needed.

    // Push frame descriptor and return address.
    masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS);
    masm.Push(scratch);
    masm.Push(ICTailCallReg);

    // Save old frame pointer, stack pointer and stub reg.
    masm.Push(ICStubReg);
    masm.Push(BaselineFrameReg);
    masm.mov(BaselineStackReg, BaselineFrameReg);
}

inline void
EmitIonEnterStubFrame(MacroAssembler& masm, Register scratch)
{
    MOZ_ASSERT(scratch != ICTailCallReg);

    masm.loadPtr(Address(masm.getStackPointer(), 0), ICTailCallReg);
    masm.Push(ICTailCallReg);
    masm.Push(ICStubReg);
}

inline void
EmitBaselineLeaveStubFrame(MacroAssembler& masm, bool calledIntoIon = false)
{
    // Ion frames do not save and restore the frame pointer. If we called
    // into Ion, we have to restore the stack pointer from the frame descriptor.
    // If we performed a VM call, the descriptor has been popped already so
    // in that case we use the frame pointer.
    if (calledIntoIon) {
        Register scratch = ICTailCallReg;
        masm.Pop(scratch);
        masm.shrl(Imm32(FRAMESIZE_SHIFT), scratch);
        masm.addl(scratch, BaselineStackReg);
    } else {
        masm.mov(BaselineFrameReg, BaselineStackReg);
    }

    masm.Pop(BaselineFrameReg);
    masm.Pop(ICStubReg);

    // Pop return address.
    masm.Pop(ICTailCallReg);

    // Overwrite frame descriptor with return address, so that the stack matches
    // the state before entering the stub frame.
    masm.storePtr(ICTailCallReg, Address(BaselineStackReg, 0));
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
        // Stow R0
        masm.pop(ICTailCallReg);
        masm.Push(R0);
        masm.push(ICTailCallReg);
        break;
      case 2:
        // Stow R0 and R1
        masm.pop(ICTailCallReg);
        masm.Push(R0);
        masm.Push(R1);
        masm.push(ICTailCallReg);
        break;
    }
}

inline void
EmitUnstowICValues(MacroAssembler& masm, int values, bool discard = false)
{
    MOZ_ASSERT(values >= 0 && values <= 2);
    switch(values) {
      case 1:
        // Unstow R0
        masm.pop(ICTailCallReg);
        if (discard)
            masm.addPtr(Imm32(sizeof(Value)), BaselineStackReg);
        else
            masm.popValue(R0);
        masm.push(ICTailCallReg);
        break;
      case 2:
        // Unstow R0 and R1
        masm.pop(ICTailCallReg);
        if (discard) {
            masm.addPtr(Imm32(sizeof(Value) * 2), BaselineStackReg);
        } else {
            masm.popValue(R1);
            masm.popValue(R0);
        }
        masm.push(ICTailCallReg);
        break;
    }
    masm.adjustFrame(-values * sizeof(Value));
}

inline void
EmitCallTypeUpdateIC(MacroAssembler& masm, JitCode* code, uint32_t objectOffset)
{
    // R0 contains the value that needs to be typechecked.
    // The object we're updating is a boxed Value on the stack, at offset
    // objectOffset from stack top, excluding the return address.

    // Save the current ICStubReg to stack
    masm.push(ICStubReg);

    // This is expected to be called from within an IC, when ICStubReg
    // is properly initialized to point to the stub.
    masm.loadPtr(Address(ICStubReg, (int32_t) ICUpdatedStub::offsetOfFirstUpdateStub()),
                 ICStubReg);

    // Call the stubcode.
    masm.call(Address(ICStubReg, ICStub::offsetOfStubCode()));

    // Restore the old stub reg.
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
    masm.patchableCallPreBarrier(addr, type);
}

inline void
EmitStubGuardFailure(MacroAssembler& masm)
{
    // NOTE: This routine assumes that the stub guard code left the stack in the
    // same state it was in when it was entered.

    // BaselineStubEntry points to the current stub.

    // Load next stub into ICStubReg
    masm.loadPtr(Address(ICStubReg, (int32_t) ICStub::offsetOfNext()), ICStubReg);

    // Return address is already loaded, just jump to the next stubcode.
    masm.jmp(Operand(ICStubReg, (int32_t) ICStub::offsetOfStubCode()));
}


} // namespace jit
} // namespace js

#endif /* jit_x86_SharedICHelpers_x86_h */

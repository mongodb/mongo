/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_SharedICHelpers_mips32_h
#define jit_mips32_SharedICHelpers_mips32_h

#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICRegisters.h"

namespace js {
namespace jit {

// Distance from sp to the top Value inside an IC stub (no return address on
// the stack on MIPS).
static const size_t ICStackValueOffset = 0;

inline void
EmitRestoreTailCallReg(MacroAssembler& masm)
{
    // No-op on MIPS because ra register is always holding the return address.
}

inline void
EmitRepushTailCallReg(MacroAssembler& masm)
{
    // No-op on MIPS because ra register is always holding the return address.
}

inline void
EmitCallIC(CodeOffset* patchOffset, MacroAssembler& masm)
{
    // Move ICEntry offset into ICStubReg.
    CodeOffset offset = masm.movWithPatch(ImmWord(-1), ICStubReg);
    *patchOffset = offset;

    // Load stub pointer into ICStubReg.
    masm.loadPtr(Address(ICStubReg, ICEntry::offsetOfFirstStub()), ICStubReg);

    // Load stubcode pointer from BaselineStubEntry.
    // R2 won't be active when we call ICs, so we can use it as scratch.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), R2.scratchReg());

    // Call the stubcode via a direct jump-and-link
    masm.call(R2.scratchReg());
}

inline void
EmitEnterTypeMonitorIC(MacroAssembler& masm,
                       size_t monitorStubOffset = ICMonitoredStub::offsetOfFirstMonitorStub())
{
    // This is expected to be called from within an IC, when ICStubReg
    // is properly initialized to point to the stub.
    masm.loadPtr(Address(ICStubReg, (uint32_t) monitorStubOffset), ICStubReg);

    // Load stubcode pointer from BaselineStubEntry.
    // R2 won't be active when we call ICs, so we can use it.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), R2.scratchReg());

    // Jump to the stubcode.
    masm.branch(R2.scratchReg());
}

inline void
EmitReturnFromIC(MacroAssembler& masm)
{
    masm.branch(ra);
}

inline void
EmitChangeICReturnAddress(MacroAssembler& masm, Register reg)
{
    masm.movePtr(reg, ra);
}

inline void
EmitBaselineTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t argSize)
{
    // We assume during this that R0 and R1 have been pushed, and that R2 is
    // unused.
    MOZ_ASSERT(R2 == ValueOperand(t7, t6));

    // Compute frame size.
    masm.movePtr(BaselineFrameReg, t6);
    masm.addPtr(Imm32(BaselineFrame::FramePointerOffset), t6);
    masm.subPtr(BaselineStackReg, t6);

    // Store frame size without VMFunction arguments for GC marking.
    masm.ma_subu(t7, t6, Imm32(argSize));
    masm.storePtr(t7, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Push frame descriptor and perform the tail call.
    // ICTailCallReg (ra) already contains the return address (as we
    // keep it there through the stub calls), but the VMWrapper code being
    // called expects the return address to also be pushed on the stack.
    MOZ_ASSERT(ICTailCallReg == ra);
    masm.makeFrameDescriptor(t6, JitFrame_BaselineJS);
    masm.subPtr(Imm32(sizeof(CommonFrameLayout)), StackPointer);
    masm.storePtr(t6, Address(StackPointer, CommonFrameLayout::offsetOfDescriptor()));
    masm.storePtr(ra, Address(StackPointer, CommonFrameLayout::offsetOfReturnAddress()));

    masm.branch(target);
}

inline void
EmitIonTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t stackSize)
{
    MOZ_CRASH("Not implemented yet.");
}

inline void
EmitBaselineCreateStubFrameDescriptor(MacroAssembler& masm, Register reg)
{
    // Compute stub frame size. We have to add two pointers: the stub reg and
    // previous frame pointer pushed by EmitEnterStubFrame.
    masm.movePtr(BaselineFrameReg, reg);
    masm.addPtr(Imm32(sizeof(intptr_t) * 2), reg);
    masm.subPtr(BaselineStackReg, reg);

    masm.makeFrameDescriptor(reg, JitFrame_BaselineStub);
}

inline void
EmitBaselineCallVM(JitCode* target, MacroAssembler& masm)
{
    EmitBaselineCreateStubFrameDescriptor(masm, t6);
    masm.push(t6);
    masm.call(target);
}

inline void
EmitIonCallVM(JitCode* target, size_t stackSlots, MacroAssembler& masm)
{
    MOZ_CRASH("Not implemented yet.");
}

struct BaselineStubFrame {
    uintptr_t savedFrame;
    uintptr_t savedStub;
    uintptr_t returnAddress;
    uintptr_t descriptor;
};

static const uint32_t STUB_FRAME_SIZE = sizeof(BaselineStubFrame);
static const uint32_t STUB_FRAME_SAVED_STUB_OFFSET = offsetof(BaselineStubFrame, savedStub);

inline void
EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch)
{
    MOZ_ASSERT(scratch != ICTailCallReg);

    // Compute frame size.
    masm.movePtr(BaselineFrameReg, scratch);
    masm.addPtr(Imm32(BaselineFrame::FramePointerOffset), scratch);
    masm.subPtr(BaselineStackReg, scratch);

    masm.storePtr(scratch, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Note: when making changes here, don't forget to update
    // BaselineStubFrame if needed.

    // Push frame descriptor and return address.
    masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS);
    masm.subPtr(Imm32(STUB_FRAME_SIZE), StackPointer);
    masm.storePtr(scratch, Address(StackPointer, offsetof(BaselineStubFrame, descriptor)));
    masm.storePtr(ICTailCallReg, Address(StackPointer,
                                               offsetof(BaselineStubFrame, returnAddress)));

    // Save old frame pointer, stack pointer and stub reg.
    masm.storePtr(ICStubReg, Address(StackPointer,
                                           offsetof(BaselineStubFrame, savedStub)));
    masm.storePtr(BaselineFrameReg, Address(StackPointer,
                                            offsetof(BaselineStubFrame, savedFrame)));
    masm.movePtr(BaselineStackReg, BaselineFrameReg);

    // We pushed 4 words, so the stack is still aligned to 8 bytes.
    masm.checkStackAlignment();
}

inline void
EmitIonEnterStubFrame(MacroAssembler& masm, Register scratch)
{
    MOZ_CRASH("Not implemented yet.");
}

inline void
EmitBaselineLeaveStubFrame(MacroAssembler& masm, bool calledIntoIon = false)
{
    // Ion frames do not save and restore the frame pointer. If we called
    // into Ion, we have to restore the stack pointer from the frame descriptor.
    // If we performed a VM call, the descriptor has been popped already so
    // in that case we use the frame pointer.
    if (calledIntoIon) {
        masm.pop(ScratchRegister);
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), ScratchRegister);
        masm.addPtr(ScratchRegister, BaselineStackReg);
    } else {
        masm.movePtr(BaselineFrameReg, BaselineStackReg);
    }

    masm.loadPtr(Address(StackPointer, offsetof(BaselineStubFrame, savedFrame)),
                 BaselineFrameReg);
    masm.loadPtr(Address(StackPointer, offsetof(BaselineStubFrame, savedStub)),
                 ICStubReg);

    // Load the return address.
    masm.loadPtr(Address(StackPointer, offsetof(BaselineStubFrame, returnAddress)),
                 ICTailCallReg);

    // Discard the frame descriptor.
    masm.loadPtr(Address(StackPointer, offsetof(BaselineStubFrame, descriptor)), ScratchRegister);
    masm.addPtr(Imm32(STUB_FRAME_SIZE), StackPointer);
}

inline void
EmitIonLeaveStubFrame(MacroAssembler& masm)
{
    MOZ_CRASH("Not implemented yet.");
}

inline void
EmitStowICValues(MacroAssembler& masm, int values)
{
    MOZ_ASSERT(values >= 0 && values <= 2);
    switch(values) {
      case 1:
        // Stow R0
        masm.Push(R0);
        break;
      case 2:
        // Stow R0 and R1
        masm.Push(R0);
        masm.Push(R1);
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
    // R0 contains the value that needs to be typechecked.
    // The object we're updating is a boxed Value on the stack, at offset
    // objectOffset from $sp, excluding the return address.

    // Save the current ICStubReg to stack, as well as the TailCallReg,
    // since on mips, the $ra is live.
    masm.subPtr(Imm32(2 * sizeof(intptr_t)), StackPointer);
    masm.storePtr(ICStubReg, Address(StackPointer, sizeof(intptr_t)));
    masm.storePtr(ICTailCallReg, Address(StackPointer, 0));

    // This is expected to be called from within an IC, when ICStubReg
    // is properly initialized to point to the stub.
    masm.loadPtr(Address(ICStubReg, ICUpdatedStub::offsetOfFirstUpdateStub()),
                 ICStubReg);

    // Load stubcode pointer from ICStubReg into ICTailCallReg.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), R2.scratchReg());

    // Call the stubcode.
    masm.call(R2.scratchReg());

    // Restore the old stub reg and tailcall reg.
    masm.loadPtr(Address(StackPointer, 0), ICTailCallReg);
    masm.loadPtr(Address(StackPointer, sizeof(intptr_t)), ICStubReg);
    masm.addPtr(Imm32(2 * sizeof(intptr_t)), StackPointer);

    // The update IC will store 0 or 1 in R1.scratchReg() reflecting if the
    // value in R0 type-checked properly or not.
    Label success;
    masm.ma_b(R1.scratchReg(), Imm32(1), &success, Assembler::Equal, ShortJump);

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
    // On MIPS, $ra is clobbered by patchableCallPreBarrier. Save it first.
    masm.push(ra);
    masm.patchableCallPreBarrier(addr, type);
    masm.pop(ra);
}

inline void
EmitStubGuardFailure(MacroAssembler& masm)
{
    // NOTE: This routine assumes that the stub guard code left the stack in
    // the same state it was in when it was entered.

    // BaselineStubEntry points to the current stub.

    // Load next stub into ICStubReg
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfNext()), ICStubReg);

    // Load stubcode pointer from BaselineStubEntry into scratch register.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), R2.scratchReg());

    // Return address is already loaded, just jump to the next stubcode.
    MOZ_ASSERT(ICTailCallReg == ra);
    masm.branch(R2.scratchReg());
}


} // namespace jit
} // namespace js

#endif /* jit_mips32_SharedICHelpers_mips32_h */

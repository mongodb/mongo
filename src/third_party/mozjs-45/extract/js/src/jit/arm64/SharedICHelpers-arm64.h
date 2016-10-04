/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_SharedICHelpers_arm64_h
#define jit_arm64_SharedICHelpers_arm64_h

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
    MOZ_ASSERT(R2 == ValueOperand(r0));
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

    // Call the stubcode via a direct branch-and-link.
    masm.Blr(x0);
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
    MOZ_ASSERT(R2 == ValueOperand(r0));
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

    // Jump to the stubcode.
    masm.Br(x0);
}

inline void
EmitReturnFromIC(MacroAssembler& masm)
{
    masm.abiret(); // Defaults to lr.
}

inline void
EmitChangeICReturnAddress(MacroAssembler& masm, Register reg)
{
    masm.movePtr(reg, lr);
}

inline void
EmitBaselineTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t argSize)
{
    // We assume that R0 has been pushed, and R2 is unused.
    MOZ_ASSERT(R2 == ValueOperand(r0));

    // Compute frame size into w0. Used below in makeFrameDescriptor().
    masm.Sub(x0, BaselineFrameReg64, masm.GetStackPointer64());
    masm.Add(w0, w0, Operand(BaselineFrame::FramePointerOffset));

    // Store frame size without VMFunction arguments for GC marking.
    {
        vixl::UseScratchRegisterScope temps(&masm.asVIXL());
        const ARMRegister scratch32 = temps.AcquireW();

        masm.Sub(scratch32, w0, Operand(argSize));
        masm.store32(scratch32.asUnsized(),
                     Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));
    }

    // Push frame descriptor (minus the return address) and perform the tail call.
    MOZ_ASSERT(ICTailCallReg == lr);
    masm.makeFrameDescriptor(r0, JitFrame_BaselineJS);
    masm.push(r0);

    // The return address will be pushed by the VM wrapper, for compatibility
    // with direct calls. Refer to the top of generateVMWrapper().
    // ICTailCallReg (lr) already contains the return address (as we keep
    // it there through the stub calls).

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
    ARMRegister reg64(reg, 64);

    // Compute stub frame size.
    masm.Sub(reg64, masm.GetStackPointer64(), Operand(sizeof(void*) * 2));
    masm.Sub(reg64, BaselineFrameReg64, reg64);

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
    MOZ_CRASH("Not implemented yet.");
}

// Size of values pushed by EmitEnterStubFrame.
static const uint32_t STUB_FRAME_SIZE = 4 * sizeof(void*);
static const uint32_t STUB_FRAME_SAVED_STUB_OFFSET = sizeof(void*);

inline void
EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch)
{
    MOZ_ASSERT(scratch != ICTailCallReg);

    // Compute frame size.
    masm.Add(ARMRegister(scratch, 64), BaselineFrameReg64, Operand(BaselineFrame::FramePointerOffset));
    masm.Sub(ARMRegister(scratch, 64), ARMRegister(scratch, 64), masm.GetStackPointer64());

    masm.store32(scratch, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Note: when making changes here, don't forget to update STUB_FRAME_SIZE.

    // Push frame descriptor and return address.
    // Save old frame pointer, stack pointer, and stub reg.
    masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS);
    masm.Push(scratch, ICTailCallReg, ICStubReg, BaselineFrameReg);

    // Update the frame register.
    masm.Mov(BaselineFrameReg64, masm.GetStackPointer64());

    // Stack should remain 16-byte aligned.
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
    vixl::UseScratchRegisterScope temps(&masm.asVIXL());
    const ARMRegister scratch64 = temps.AcquireX();

    // Ion frames do not save and restore the frame pointer. If we called
    // into Ion, we have to restore the stack pointer from the frame descriptor.
    // If we performed a VM call, the descriptor has been popped already so
    // in that case we use the frame pointer.
    if (calledIntoIon) {
        masm.pop(scratch64.asUnsized());
        masm.Lsr(scratch64, scratch64, FRAMESIZE_SHIFT);
        masm.Add(masm.GetStackPointer64(), masm.GetStackPointer64(), scratch64);
    } else {
        masm.Mov(masm.GetStackPointer64(), BaselineFrameReg64);
    }

    // Pop values, discarding the frame descriptor.
    masm.pop(BaselineFrameReg, ICStubReg, ICTailCallReg, scratch64.asUnsized());

    // Stack should remain 16-byte aligned.
    masm.checkStackAlignment();
}

inline void
EmitIonLeaveStubFrame(MacroAssembler& masm)
{
    MOZ_CRASH("Not implemented yet.");
}

inline void
EmitStowICValues(MacroAssembler& masm, int values)
{
    switch (values) {
      case 1:
        // Stow R0.
        masm.Push(R0);
        break;
      case 2:
        // Stow R0 and R1.
        masm.Push(R0.valueReg());
        masm.Push(R1.valueReg());
        break;
      default:
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Expected 1 or 2 values");
    }
}

inline void
EmitUnstowICValues(MacroAssembler& masm, int values, bool discard = false)
{
    MOZ_ASSERT(values >= 0 && values <= 2);
    switch (values) {
      case 1:
        // Unstow R0.
        if (discard)
            masm.Drop(Operand(sizeof(Value)));
        else
            masm.popValue(R0);
        break;
      case 2:
        // Unstow R0 and R1.
        if (discard)
            masm.Drop(Operand(sizeof(Value) * 2));
        else
            masm.pop(R1.valueReg(), R0.valueReg());
        break;
      default:
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Expected 1 or 2 values");
    }
    masm.adjustFrame(-values * sizeof(Value));
}

inline void
EmitCallTypeUpdateIC(MacroAssembler& masm, JitCode* code, uint32_t objectOffset)
{
    // R0 contains the value that needs to be typechecked.
    // The object we're updating is a boxed Value on the stack, at offset
    // objectOffset from stack top, excluding the return address.
    MOZ_ASSERT(R2 == ValueOperand(r0));

    // Save the current ICStubReg to stack, as well as the TailCallReg,
    // since on AArch64, the LR is live.
    masm.push(ICStubReg, ICTailCallReg);

    // This is expected to be called from within an IC, when ICStubReg
    // is properly initialized to point to the stub.
    masm.loadPtr(Address(ICStubReg, (int32_t)ICUpdatedStub::offsetOfFirstUpdateStub()),
                 ICStubReg);

    // Load stubcode pointer from ICStubReg into ICTailCallReg.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), ICTailCallReg);

    // Call the stubcode.
    masm.Blr(ARMRegister(ICTailCallReg, 64));

    // Restore the old stub reg and tailcall reg.
    masm.pop(ICTailCallReg, ICStubReg);

    // The update IC will store 0 or 1 in R1.scratchReg() reflecting if the
    // value in R0 type-checked properly or not.
    Label success;
    masm.cmp32(R1.scratchReg(), Imm32(1));
    masm.j(Assembler::Equal, &success);

    // If the IC failed, then call the update fallback function.
    EmitBaselineEnterStubFrame(masm, R1.scratchReg());

    masm.loadValue(Address(masm.getStackPointer(), STUB_FRAME_SIZE + objectOffset), R1);
    masm.Push(R0.valueReg());
    masm.Push(R1.valueReg());
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
    // On AArch64, lr is clobbered by patchableCallPreBarrier. Save it first.
    masm.push(lr);
    masm.patchableCallPreBarrier(addr, type);
    masm.pop(lr);
}

inline void
EmitStubGuardFailure(MacroAssembler& masm)
{
    // NOTE: This routine assumes that the stub guard code left the stack in the
    // same state it was in when it was entered.

    // BaselineStubEntry points to the current stub.

    // Load next stub into ICStubReg.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfNext()), ICStubReg);

    // Load stubcode pointer from BaselineStubEntry into scratch register.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), r0);

    // Return address is already loaded, just jump to the next stubcode.
    masm.Br(x0);
}

} // namespace jit
} // namespace js

#endif // jit_arm64_SharedICHelpers_arm64_h

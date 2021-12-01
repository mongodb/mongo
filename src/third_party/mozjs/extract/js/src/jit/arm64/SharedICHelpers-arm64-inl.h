/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_SharedICHelpers_arm64_inl_h
#define jit_arm64_SharedICHelpers_arm64_inl_h

#include "jit/SharedICHelpers.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

inline void
EmitBaselineTailCallVM(TrampolinePtr target, MacroAssembler& masm, uint32_t argSize)
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
    masm.makeFrameDescriptor(r0, JitFrame_BaselineJS, ExitFrameLayout::Size());
    masm.push(r0);

    // The return address will be pushed by the VM wrapper, for compatibility
    // with direct calls. Refer to the top of generateVMWrapper().
    // ICTailCallReg (lr) already contains the return address (as we keep
    // it there through the stub calls).

    masm.jump(target);
}

inline void
EmitIonTailCallVM(TrampolinePtr target, MacroAssembler& masm, uint32_t stackSize)
{
    MOZ_CRASH("Not implemented yet.");
}

inline void
EmitBaselineCreateStubFrameDescriptor(MacroAssembler& masm, Register reg, uint32_t headerSize)
{
    ARMRegister reg64(reg, 64);

    // Compute stub frame size.
    masm.Sub(reg64, masm.GetStackPointer64(), Operand(sizeof(void*) * 2));
    masm.Sub(reg64, BaselineFrameReg64, reg64);

    masm.makeFrameDescriptor(reg, JitFrame_BaselineStub, headerSize);
}

inline void
EmitBaselineCallVM(TrampolinePtr target, MacroAssembler& masm)
{
    EmitBaselineCreateStubFrameDescriptor(masm, r0, ExitFrameLayout::Size());
    masm.push(r0);
    masm.call(target);
}

// Size of values pushed by EmitBaselineEnterStubFrame.
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
    masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS, BaselineStubFrameLayout::Size());
    masm.Push(scratch, ICTailCallReg, ICStubReg, BaselineFrameReg);

    // Update the frame register.
    masm.Mov(BaselineFrameReg64, masm.GetStackPointer64());

    // Stack should remain 16-byte aligned.
    masm.checkStackAlignment();
}

} // namespace jit
} // namespace js

#endif // jit_arm64_SharedICHelpers_arm64_inl_h

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_SharedICHelpers_x86_inl_h
#define jit_x86_SharedICHelpers_x86_inl_h

#include "jit/SharedICHelpers.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

inline void
EmitBaselineTailCallVM(TrampolinePtr target, MacroAssembler& masm, uint32_t argSize)
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
    masm.makeFrameDescriptor(eax, JitFrame_BaselineJS, ExitFrameLayout::Size());
    masm.push(eax);
    masm.push(ICTailCallReg);
    masm.jump(target);
}

inline void
EmitIonTailCallVM(TrampolinePtr target, MacroAssembler& masm, uint32_t stackSize)
{
    // For tail calls, find the already pushed JitFrame_IonJS signifying the
    // end of the Ion frame. Retrieve the length of the frame and repush
    // JitFrame_IonJS with the extra stacksize, rendering the original
    // JitFrame_IonJS obsolete.

    masm.loadPtr(Address(esp, stackSize), eax);
    masm.shrl(Imm32(FRAMESIZE_SHIFT), eax);
    masm.addl(Imm32(stackSize + JitStubFrameLayout::Size() - sizeof(intptr_t)), eax);

    // Push frame descriptor and perform the tail call.
    masm.makeFrameDescriptor(eax, JitFrame_IonJS, ExitFrameLayout::Size());
    masm.push(eax);
    masm.push(ICTailCallReg);
    masm.jump(target);
}

inline void
EmitBaselineCreateStubFrameDescriptor(MacroAssembler& masm, Register reg, uint32_t headerSize)
{
    // Compute stub frame size. We have to add two pointers: the stub reg and previous
    // frame pointer pushed by EmitEnterStubFrame.
    masm.movl(BaselineFrameReg, reg);
    masm.addl(Imm32(sizeof(void*) * 2), reg);
    masm.subl(BaselineStackReg, reg);

    masm.makeFrameDescriptor(reg, JitFrame_BaselineStub, headerSize);
}

inline void
EmitBaselineCallVM(TrampolinePtr target, MacroAssembler& masm)
{
    EmitBaselineCreateStubFrameDescriptor(masm, eax, ExitFrameLayout::Size());
    masm.push(eax);
    masm.call(target);
}

// Size of values pushed by EmitBaselineEnterStubFrame.
static const uint32_t STUB_FRAME_SIZE = 4 * sizeof(void*);
static const uint32_t STUB_FRAME_SAVED_STUB_OFFSET = sizeof(void*);

inline void
EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch)
{
    // Compute frame size. Because the return address is still on the stack,
    // this is:
    //
    //   BaselineFrameReg
    //   + BaselineFrame::FramePointerOffset
    //   - BaselineStackReg
    //   - sizeof(return address)
    //
    // The two constants cancel each other out, so we can just calculate
    // BaselineFrameReg - BaselineStackReg.

    static_assert(BaselineFrame::FramePointerOffset == sizeof(void*),
                  "FramePointerOffset must be the same as the return address size");

    masm.movl(BaselineFrameReg, scratch);
    masm.subl(BaselineStackReg, scratch);

    masm.store32(scratch, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Note: when making changes here,  don't forget to update STUB_FRAME_SIZE
    // if needed.

    // Push the return address that's currently on top of the stack.
    masm.Push(Operand(BaselineStackReg, 0));

    // Replace the original return address with the frame descriptor.
    masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS, BaselineStubFrameLayout::Size());
    masm.storePtr(scratch, Address(BaselineStackReg, sizeof(uintptr_t)));

    // Save old frame pointer, stack pointer and stub reg.
    masm.Push(ICStubReg);
    masm.Push(BaselineFrameReg);
    masm.mov(BaselineStackReg, BaselineFrameReg);
}

} // namespace jit
} // namespace js

#endif /* jit_x86_SharedICHelpers_x86_inl_h */

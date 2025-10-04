/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_SharedICHelpers_x86_inl_h
#define jit_x86_SharedICHelpers_x86_inl_h

#include "jit/BaselineFrame.h"
#include "jit/SharedICHelpers.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

inline void EmitBaselineTailCallVM(TrampolinePtr target, MacroAssembler& masm,
                                   uint32_t argSize) {
#ifdef DEBUG
  // We assume during this that R0 and R1 have been pushed.
  // Store frame size without VMFunction arguments for debug assertions.
  masm.movl(FramePointer, eax);
  masm.subl(StackPointer, eax);
  masm.subl(Imm32(argSize), eax);
  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(eax, frameSizeAddr);
#endif

  // Push frame descriptor and perform the tail call.
  masm.pushFrameDescriptor(FrameType::BaselineJS);
  masm.push(ICTailCallReg);
  masm.jump(target);
}

inline void EmitBaselineCallVM(TrampolinePtr target, MacroAssembler& masm) {
  masm.pushFrameDescriptor(FrameType::BaselineStub);
  masm.call(target);
}

inline void EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch) {
#ifdef DEBUG
  // Compute frame size. Because the return address is still on the stack,
  // this is:
  //
  //   FramePointer
  //   - StackPointer
  //   - sizeof(return address)

  masm.movl(FramePointer, scratch);
  masm.subl(StackPointer, scratch);
  masm.subl(Imm32(sizeof(void*)), scratch);  // Return address.

  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
#endif

  // Push the return address that's currently on top of the stack.
  masm.Push(Operand(StackPointer, 0));

  // Replace the original return address with the frame descriptor.
  masm.storePtr(ImmWord(MakeFrameDescriptor(FrameType::BaselineJS)),
                Address(StackPointer, sizeof(uintptr_t)));

  // Save old frame pointer, stack pointer and stub reg.
  masm.Push(FramePointer);
  masm.mov(StackPointer, FramePointer);

  masm.Push(ICStubReg);
}

}  // namespace jit
}  // namespace js

#endif /* jit_x86_SharedICHelpers_x86_inl_h */

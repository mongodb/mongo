/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_SharedICHelpers_arm_inl_h
#define jit_arm_SharedICHelpers_arm_inl_h

#include "jit/BaselineFrame.h"
#include "jit/SharedICHelpers.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

inline void EmitBaselineTailCallVM(TrampolinePtr target, MacroAssembler& masm,
                                   uint32_t argSize) {
#ifdef DEBUG
  // We assume during this that R0 and R1 have been pushed, and that R2 is
  // unused.
  static_assert(R2 == ValueOperand(r1, r0));

  // Store frame size without VMFunction arguments for debug assertions.
  masm.movePtr(FramePointer, r0);
  masm.ma_sub(StackPointer, r0);
  masm.sub32(Imm32(argSize), r0);
  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(r0, frameSizeAddr);
#endif

  // Push frame descriptor and perform the tail call.
  masm.pushFrameDescriptor(FrameType::BaselineJS);

  static_assert(ICTailCallReg == lr);
  // The return address will be pushed by the VM wrapper, for compatibility
  // with direct calls. Refer to the top of generateVMWrapper().
  // ICTailCallReg (lr) already contains the return address (as we keep
  // it there through the stub calls).

  masm.jump(target);
}

inline void EmitBaselineCallVM(TrampolinePtr target, MacroAssembler& masm) {
  masm.pushFrameDescriptor(FrameType::BaselineStub);
  masm.call(target);
}

inline void EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch) {
  MOZ_ASSERT(scratch != ICTailCallReg);

#ifdef DEBUG
  // Compute frame size.
  masm.mov(FramePointer, scratch);
  masm.ma_sub(StackPointer, scratch);

  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
#endif

  // Push frame descriptor and return address.
  masm.PushFrameDescriptor(FrameType::BaselineJS);
  masm.Push(ICTailCallReg);

  // Save old frame pointer, stack pointer and stub reg.
  masm.Push(FramePointer);
  masm.mov(StackPointer, FramePointer);

  masm.Push(ICStubReg);

  // We pushed 4 words, so the stack is still aligned to 8 bytes.
  masm.checkStackAlignment();
}

}  // namespace jit
}  // namespace js

#endif /* jit_arm_SharedICHelpers_arm_inl_h */

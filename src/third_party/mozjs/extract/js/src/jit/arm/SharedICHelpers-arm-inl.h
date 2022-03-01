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
  // We assume during this that R0 and R1 have been pushed, and that R2 is
  // unused.
  static_assert(R2 == ValueOperand(r1, r0));

  // Compute frame size.
  masm.movePtr(BaselineFrameReg, r0);
  masm.as_add(r0, r0, Imm8(BaselineFrame::FramePointerOffset));
  masm.ma_sub(BaselineStackReg, r0);

#ifdef DEBUG
  // Store frame size without VMFunction arguments for debug assertions.
  {
    ScratchRegisterScope scratch(masm);
    masm.ma_sub(r0, Imm32(argSize), r1, scratch);
  }
  Address frameSizeAddr(BaselineFrameReg,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(r1, frameSizeAddr);
#endif

  // Push frame descriptor and perform the tail call.
  // ICTailCallReg (lr) already contains the return address (as we keep
  // it there through the stub calls), but the VMWrapper code being called
  // expects the return address to also be pushed on the stack.
  static_assert(ICTailCallReg == lr);
  masm.makeFrameDescriptor(r0, FrameType::BaselineJS, ExitFrameLayout::Size());
  masm.push(r0);
  masm.push(lr);
  masm.jump(target);
}

inline void EmitBaselineCreateStubFrameDescriptor(MacroAssembler& masm,
                                                  Register reg,
                                                  uint32_t headerSize) {
  // Compute stub frame size. We have to add two pointers: the stub reg and
  // previous frame pointer pushed by EmitEnterStubFrame.
  masm.mov(BaselineFrameReg, reg);
  masm.as_add(reg, reg, Imm8(sizeof(void*) * 2));
  masm.ma_sub(BaselineStackReg, reg);

  masm.makeFrameDescriptor(reg, FrameType::BaselineStub, headerSize);
}

inline void EmitBaselineCallVM(TrampolinePtr target, MacroAssembler& masm) {
  EmitBaselineCreateStubFrameDescriptor(masm, r0, ExitFrameLayout::Size());
  masm.push(r0);
  masm.call(target);
}

// Size of values pushed by EmitBaselineEnterStubFrame.
static const uint32_t STUB_FRAME_SIZE = 4 * sizeof(void*);
static const uint32_t STUB_FRAME_SAVED_STUB_OFFSET = sizeof(void*);

inline void EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch) {
  MOZ_ASSERT(scratch != ICTailCallReg);

  // Compute frame size.
  masm.mov(BaselineFrameReg, scratch);
  masm.as_add(scratch, scratch, Imm8(BaselineFrame::FramePointerOffset));
  masm.ma_sub(BaselineStackReg, scratch);

#ifdef DEBUG
  Address frameSizeAddr(BaselineFrameReg,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
#endif

  // Note: when making changes here, don't forget to update STUB_FRAME_SIZE if
  // needed.

  // Push frame descriptor and return address.
  masm.makeFrameDescriptor(scratch, FrameType::BaselineJS,
                           BaselineStubFrameLayout::Size());
  masm.Push(scratch);
  masm.Push(ICTailCallReg);

  // Save old frame pointer, stack pointer and stub reg.
  masm.Push(ICStubReg);
  masm.Push(BaselineFrameReg);
  masm.mov(BaselineStackReg, BaselineFrameReg);

  // We pushed 4 words, so the stack is still aligned to 8 bytes.
  masm.checkStackAlignment();
}

}  // namespace jit
}  // namespace js

#endif /* jit_arm_SharedICHelpers_arm_inl_h */

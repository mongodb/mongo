/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_riscv64_SharedICHelpers_riscv64_inl_h
#define jit_riscv64_SharedICHelpers_riscv64_inl_h

#include "jit/SharedICHelpers.h"

namespace js {
namespace jit {

inline void EmitBaselineTailCallVM(TrampolinePtr target, MacroAssembler& masm,
                                   uint32_t argSize) {
#ifdef DEBUG
  Register scratch = R2.scratchReg();

  // Compute frame size.
  masm.movePtr(FramePointer, scratch);
  masm.subPtr(StackPointer, scratch);

  // Store frame size without VMFunction arguments for debug assertions.
  masm.subPtr(Imm32(argSize), scratch);
  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
  masm.addPtr(Imm32(argSize), scratch);
#endif

  // Push frame descriptor and perform the tail call.
  masm.pushFrameDescriptor(FrameType::BaselineJS);

  MOZ_ASSERT(ICTailCallReg == ra);
  // The return address will be pushed by the VM wrapper, for compatibility
  // with direct calls. Refer to the top of generateVMWrapper().
  // ICTailCallReg (ra) already contains the return address (as we keep
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
  masm.movePtr(FramePointer, scratch);
  masm.subPtr(StackPointer, scratch);

  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
#endif

  // Note: when making changes here, don't forget to update
  // BaselineStubFrame if needed.

  // Push frame descriptor and return address.
  masm.PushFrameDescriptor(FrameType::BaselineJS);
  masm.Push(ICTailCallReg);

  // Save old frame pointer, stack pointer and stub reg.
  masm.Push(FramePointer);
  masm.movePtr(StackPointer, FramePointer);
  masm.Push(ICStubReg);

  // Stack should remain aligned.
  masm.assertStackAlignment(sizeof(Value), 0);
}

}  // namespace jit
}  // namespace js

#endif /* jit_riscv64_SharedICHelpers_riscv64_inl_h */

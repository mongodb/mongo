/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_SharedICHelpers_arm64_inl_h
#define jit_arm64_SharedICHelpers_arm64_inl_h

#include "jit/BaselineFrame.h"
#include "jit/SharedICHelpers.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

inline void EmitBaselineTailCallVM(TrampolinePtr target, MacroAssembler& masm,
                                   uint32_t argSize) {
#ifdef DEBUG
  // We assume that R0 has been pushed, and R2 is unused.
  static_assert(R2 == ValueOperand(r0));

  // Store frame size without VMFunction arguments for debug assertions.
  masm.Sub(x0, FramePointer64, masm.GetStackPointer64());
  masm.Sub(w0, w0, Operand(argSize));
  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(w0.asUnsized(), frameSizeAddr);
#endif

  // Push frame descriptor (minus the return address) and perform the tail call.
  static_assert(ICTailCallReg == lr);
  masm.pushFrameDescriptor(FrameType::BaselineJS);

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
  masm.Sub(ARMRegister(scratch, 64), FramePointer64, masm.GetStackPointer64());

  Address frameSizeAddr(FramePointer,
                        BaselineFrame::reverseOffsetOfDebugFrameSize());
  masm.store32(scratch, frameSizeAddr);
#endif

  // Push frame descriptor and return address.
  // Save old frame pointer, stack pointer, and stub reg.
  masm.PushFrameDescriptor(FrameType::BaselineJS);
  masm.Push(ICTailCallReg);
  masm.Push(FramePointer);

  // Update the frame register.
  masm.Mov(FramePointer64, masm.GetStackPointer64());

  masm.Push(ICStubReg);

  // Stack should remain 16-byte aligned.
  masm.checkStackAlignment();
}

}  // namespace jit
}  // namespace js

#endif  // jit_arm64_SharedICHelpers_arm64_inl_h

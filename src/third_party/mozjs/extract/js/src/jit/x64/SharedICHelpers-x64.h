/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_SharedICHelpers_x64_h
#define jit_x64_SharedICHelpers_x64_h

#include "jit/BaselineIC.h"
#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICRegisters.h"

namespace js {
namespace jit {

// Distance from Stack top to the top Value inside an IC stub (this is the
// return address).
static const size_t ICStackValueOffset = sizeof(void*);

inline void EmitRestoreTailCallReg(MacroAssembler& masm) {
  masm.Pop(ICTailCallReg);
}

inline void EmitRepushTailCallReg(MacroAssembler& masm) {
  masm.Push(ICTailCallReg);
}

inline void EmitCallIC(MacroAssembler& masm, CodeOffset* callOffset) {
  // The stub pointer must already be in ICStubReg.
  // Call the stubcode.
  masm.call(Address(ICStubReg, ICStub::offsetOfStubCode()));
  *callOffset = CodeOffset(masm.currentOffset());
}

inline void EmitReturnFromIC(MacroAssembler& masm) { masm.ret(); }

inline void EmitBaselineLeaveStubFrame(MacroAssembler& masm) {
  Address stubAddr(FramePointer, BaselineStubFrameLayout::ICStubOffsetFromFP);
  masm.loadPtr(stubAddr, ICStubReg);

  masm.mov(FramePointer, StackPointer);
  masm.Pop(FramePointer);

  // The return address is on top of the stack, followed by the frame
  // descriptor. Use a pop instruction to overwrite the frame descriptor
  // with the return address. Note that pop increments the stack pointer
  // before computing the address.
  masm.Pop(Operand(StackPointer, 0));
}

template <typename AddrType>
inline void EmitPreBarrier(MacroAssembler& masm, const AddrType& addr,
                           MIRType type) {
  masm.guardedCallPreBarrier(addr, type);
}

inline void EmitStubGuardFailure(MacroAssembler& masm) {
  // Load next stub into ICStubReg
  masm.loadPtr(Address(ICStubReg, ICCacheIRStub::offsetOfNext()), ICStubReg);

  // Return address is already loaded, just jump to the next stubcode.
  masm.jmp(Operand(ICStubReg, ICStub::offsetOfStubCode()));
}

}  // namespace jit
}  // namespace js

#endif /* jit_x64_SharedICHelpers_x64_h */

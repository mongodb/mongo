/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_riscv64_SharedICHelpers_riscv64_h
#define jit_riscv64_SharedICHelpers_riscv64_h
#include "jit/BaselineIC.h"
#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICRegisters.h"
namespace js {
namespace jit {

static const size_t ICStackValueOffset = 0;

inline void EmitRestoreTailCallReg(MacroAssembler& masm) {
  // No-op on RISC-V because ra register is always holding the return address.
}

inline void EmitRepushTailCallReg(MacroAssembler& masm) {
  // No-op on RISC-V because ra register is always holding the return address.
}
inline void EmitCallIC(MacroAssembler& masm, CodeOffset* callOffset) {
  // The stub pointer must already be in ICStubReg.
  // Load stubcode pointer from the ICStub.
  // R2 won't be active when we call ICs, so we can use it as scratch.
  masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), R2.scratchReg());

  // Call the stubcode via a direct jump-and-link
  masm.call(R2.scratchReg());
  *callOffset = CodeOffset(masm.currentOffset());
}
inline void EmitReturnFromIC(MacroAssembler& masm) { masm.branch(ra); }
inline void EmitBaselineLeaveStubFrame(MacroAssembler& masm) {
  masm.loadPtr(
      Address(FramePointer, BaselineStubFrameLayout::ICStubOffsetFromFP),
      ICStubReg);

  masm.movePtr(FramePointer, StackPointer);
  masm.Pop(FramePointer);

  // Load the return address.
  masm.Pop(ICTailCallReg);

  // Discard the frame descriptor.
  {
    UseScratchRegisterScope temps(&masm);
    Register scratch2 = temps.Acquire();
    masm.Pop(scratch2);
  }

  masm.checkStackAlignment();
}

inline void EmitStubGuardFailure(MacroAssembler& masm) {
  // Load next stub into ICStubReg
  masm.loadPtr(Address(ICStubReg, ICCacheIRStub::offsetOfNext()), ICStubReg);

  // Return address is already loaded, just jump to the next stubcode.
  MOZ_ASSERT(ICTailCallReg == ra);
  masm.jump(Address(ICStubReg, ICStub::offsetOfStubCode()));
}
template <typename AddrType>
inline void EmitPreBarrier(MacroAssembler& masm, const AddrType& addr,
                           MIRType type) {
  // On RISC-V, $ra is clobbered by guardedCallPreBarrier. Save it first.
  masm.push(ra);
  masm.guardedCallPreBarrier(addr, type);
  masm.pop(ra);
}

}  // namespace jit
}  // namespace js

#endif /* jit_riscv64_SharedICHelpers_riscv64_h */

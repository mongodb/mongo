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

template <typename AddrType>
inline void
EmitPreBarrier(MacroAssembler& masm, const AddrType& addr, MIRType type)
{
    // On AArch64, lr is clobbered by guardedCallPreBarrier. Save it first.
    masm.push(lr);
    masm.guardedCallPreBarrier(addr, type);
    masm.pop(lr);
}

inline void
EmitStubGuardFailure(MacroAssembler& masm)
{
    // Load next stub into ICStubReg.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfNext()), ICStubReg);

    // Return address is already loaded, just jump to the next stubcode.
    masm.jump(Address(ICStubReg, ICStub::offsetOfStubCode()));
}

} // namespace jit
} // namespace js

#endif // jit_arm64_SharedICHelpers_arm64_h

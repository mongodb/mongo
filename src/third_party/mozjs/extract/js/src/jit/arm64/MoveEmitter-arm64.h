/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_MoveEmitter_arm64_h
#define jit_arm64_MoveEmitter_arm64_h

#include "jit/arm64/Assembler-arm64.h"
#include "jit/MacroAssembler.h"
#include "jit/MoveResolver.h"

namespace js {
namespace jit {

class CodeGenerator;

class MoveEmitterARM64
{
    bool inCycle_;
    MacroAssembler& masm;

    // Original stack push value.
    uint32_t pushedAtStart_;

    // This stores a stack offset to a spill location, snapshotting
    // codegen->framePushed_ at the time it was allocated. It is -1 if no
    // stack space has been allocated for that particular spill.
    int32_t pushedAtCycle_;

    void assertDone() {
        MOZ_ASSERT(!inCycle_);
    }

    MemOperand cycleSlot();
    MemOperand toMemOperand(const MoveOperand& operand) const;
    ARMRegister toARMReg32(const MoveOperand& operand) const {
        MOZ_ASSERT(operand.isGeneralReg());
        return ARMRegister(operand.reg(), 32);
    }
    ARMRegister toARMReg64(const MoveOperand& operand) const {
        if (operand.isGeneralReg())
            return ARMRegister(operand.reg(), 64);
        else
            return ARMRegister(operand.base(), 64);
    }
    ARMFPRegister toFPReg(const MoveOperand& operand, MoveOp::Type t) const {
        MOZ_ASSERT(operand.isFloatReg());
        return ARMFPRegister(operand.floatReg().encoding(), t == MoveOp::FLOAT32 ? 32 : 64);
    }

    void emitFloat32Move(const MoveOperand& from, const MoveOperand& to);
    void emitDoubleMove(const MoveOperand& from, const MoveOperand& to);
    void emitInt32Move(const MoveOperand& from, const MoveOperand& to);
    void emitGeneralMove(const MoveOperand& from, const MoveOperand& to);

    void emitMove(const MoveOp& move);
    void breakCycle(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type);
    void completeCycle(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type);

  public:
    MoveEmitterARM64(MacroAssembler& masm)
      : inCycle_(false),
        masm(masm),
        pushedAtStart_(masm.framePushed()),
        pushedAtCycle_(-1)
    { }

    ~MoveEmitterARM64() {
        assertDone();
    }

    void emit(const MoveResolver& moves);
    void finish();
    void setScratchRegister(Register reg) {}
};

typedef MoveEmitterARM64 MoveEmitter;

} // namespace jit
} // namespace js

#endif /* jit_arm64_MoveEmitter_arm64_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_MoveEmitter_mips_h
#define jit_mips_MoveEmitter_mips_h

#include "jit/MacroAssembler.h"
#include "jit/MoveResolver.h"

namespace js {
namespace jit {

class CodeGenerator;

class MoveEmitterMIPS
{
    uint32_t inCycle_;
    MacroAssemblerMIPSCompat& masm;

    // Original stack push value.
    uint32_t pushedAtStart_;

    // These store stack offsets to spill locations, snapshotting
    // codegen->framePushed_ at the time they were allocated. They are -1 if no
    // stack space has been allocated for that particular spill.
    int32_t pushedAtCycle_;
    int32_t pushedAtSpill_;

    // These are registers that are available for temporary use. They may be
    // assigned InvalidReg. If no corresponding spill space has been assigned,
    // then these registers do not need to be spilled.
    Register spilledReg_;
    FloatRegister spilledFloatReg_;

    void assertDone();
    Register tempReg();
    FloatRegister tempFloatReg();
    Address cycleSlot(uint32_t slot, uint32_t subslot) const;
    int32_t getAdjustedOffset(const MoveOperand& operand);
    Address getAdjustedAddress(const MoveOperand& operand);

    void emitMove(const MoveOperand& from, const MoveOperand& to);
    void emitFloat32Move(const MoveOperand& from, const MoveOperand& to);
    void emitDoubleMove(const MoveOperand& from, const MoveOperand& to);
    void breakCycle(const MoveOperand& from, const MoveOperand& to,
                    MoveOp::Type type, uint32_t slot);
    void completeCycle(const MoveOperand& from, const MoveOperand& to,
                       MoveOp::Type type, uint32_t slot);
    void emit(const MoveOp& move);

  public:
    MoveEmitterMIPS(MacroAssemblerMIPSCompat& masm);
    ~MoveEmitterMIPS();
    void emit(const MoveResolver& moves);
    void finish();

    void setScratchRegister(Register reg) {}
};

typedef MoveEmitterMIPS MoveEmitter;

} // namespace jit
} // namespace js

#endif /* jit_mips_MoveEmitter_mips_h */

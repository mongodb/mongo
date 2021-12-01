/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MoveEmitter_x86_shared_h
#define jit_MoveEmitter_x86_shared_h

#include "jit/MacroAssembler.h"
#include "jit/MoveResolver.h"

namespace js {
namespace jit {

class MoveEmitterX86
{
    bool inCycle_;
    MacroAssembler& masm;

    // Original stack push value.
    uint32_t pushedAtStart_;

    // This is a store stack offset for the cycle-break spill slot, snapshotting
    // codegen->framePushed_ at the time it is allocated. -1 if not allocated.
    int32_t pushedAtCycle_;

#ifdef JS_CODEGEN_X86
    // Optional scratch register for performing moves.
    mozilla::Maybe<Register> scratchRegister_;
#endif

    void assertDone();
    Address cycleSlot();
    Address toAddress(const MoveOperand& operand) const;
    Operand toOperand(const MoveOperand& operand) const;
    Operand toPopOperand(const MoveOperand& operand) const;

    size_t characterizeCycle(const MoveResolver& moves, size_t i,
                             bool* allGeneralRegs, bool* allFloatRegs);
    bool maybeEmitOptimizedCycle(const MoveResolver& moves, size_t i,
                                 bool allGeneralRegs, bool allFloatRegs, size_t swapCount);
    void emitInt32Move(const MoveOperand& from, const MoveOperand& to,
                       const MoveResolver& moves, size_t i);
    void emitGeneralMove(const MoveOperand& from, const MoveOperand& to,
                         const MoveResolver& moves, size_t i);
    void emitFloat32Move(const MoveOperand& from, const MoveOperand& to);
    void emitDoubleMove(const MoveOperand& from, const MoveOperand& to);
    void emitSimd128FloatMove(const MoveOperand& from, const MoveOperand& to);
    void emitSimd128IntMove(const MoveOperand& from, const MoveOperand& to);
    void breakCycle(const MoveOperand& to, MoveOp::Type type);
    void completeCycle(const MoveOperand& to, MoveOp::Type type);

  public:
    explicit MoveEmitterX86(MacroAssembler& masm);
    ~MoveEmitterX86();
    void emit(const MoveResolver& moves);
    void finish();

    void setScratchRegister(Register reg) {
#ifdef JS_CODEGEN_X86
        scratchRegister_.emplace(reg);
#endif
    }

    mozilla::Maybe<Register> findScratchRegister(const MoveResolver& moves, size_t i);
};

typedef MoveEmitterX86 MoveEmitter;

} // namespace jit
} // namespace js

#endif /* jit_MoveEmitter_x86_shared_h */

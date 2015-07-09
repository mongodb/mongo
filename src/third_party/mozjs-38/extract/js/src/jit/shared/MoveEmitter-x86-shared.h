/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MoveEmitter_x86_shared_h
#define jit_MoveEmitter_x86_shared_h

#if defined(JS_CODEGEN_X86)
# include "jit/x86/MacroAssembler-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/MacroAssembler-x64.h"
#else
# error "Wrong architecture. Only x86 and x64 should build this file!"
#endif
#include "jit/MoveResolver.h"

namespace js {
namespace jit {

class CodeGenerator;

class MoveEmitterX86
{
    bool inCycle_;
    MacroAssemblerSpecific& masm;

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
    void emitInt32Move(const MoveOperand& from, const MoveOperand& to);
    void emitGeneralMove(const MoveOperand& from, const MoveOperand& to);
    void emitFloat32Move(const MoveOperand& from, const MoveOperand& to);
    void emitDoubleMove(const MoveOperand& from, const MoveOperand& to);
    void emitFloat32X4Move(const MoveOperand& from, const MoveOperand& to);
    void emitInt32X4Move(const MoveOperand& from, const MoveOperand& to);
    void breakCycle(const MoveOperand& to, MoveOp::Type type);
    void completeCycle(const MoveOperand& to, MoveOp::Type type);

  public:
    explicit MoveEmitterX86(MacroAssemblerSpecific& masm);
    ~MoveEmitterX86();
    void emit(const MoveResolver& moves);
    void finish();

    void setScratchRegister(Register reg) {
#ifdef JS_CODEGEN_X86
        scratchRegister_.emplace(reg);
#endif
    }

    bool hasScratchRegister() {
#ifdef JS_CODEGEN_X86
        return scratchRegister_.isSome();
#else
        return true;
#endif
    }

    Register scratchRegister() {
        MOZ_ASSERT(hasScratchRegister());
#ifdef JS_CODEGEN_X86
        return scratchRegister_.value();
#else
        return ScratchReg;
#endif
    }
};

typedef MoveEmitterX86 MoveEmitter;

} // ion
} // js

#endif /* jit_MoveEmitter_x86_shared_h */

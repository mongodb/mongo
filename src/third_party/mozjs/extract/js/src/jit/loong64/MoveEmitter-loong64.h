/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_loong64_MoveEmitter_loong64_h
#define jit_loong64_MoveEmitter_loong64_h

#include "jit/MacroAssembler.h"
#include "jit/MoveResolver.h"

namespace js {
namespace jit {

class MoveEmitterLOONG64 {
  void emitDoubleMove(const MoveOperand& from, const MoveOperand& to);
  void breakCycle(const MoveOperand& from, const MoveOperand& to,
                  MoveOp::Type type, uint32_t slot);
  void completeCycle(const MoveOperand& from, const MoveOperand& to,
                     MoveOp::Type type, uint32_t slot);

 protected:
  uint32_t inCycle_;
  MacroAssembler& masm;

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
  Address cycleSlot(uint32_t slot, uint32_t subslot = 0) const;
  int32_t getAdjustedOffset(const MoveOperand& operand);
  Address getAdjustedAddress(const MoveOperand& operand);

  void emitMove(const MoveOperand& from, const MoveOperand& to);
  void emitInt32Move(const MoveOperand& from, const MoveOperand& to);
  void emitFloat32Move(const MoveOperand& from, const MoveOperand& to);
  void emit(const MoveOp& move);

 public:
  MoveEmitterLOONG64(MacroAssembler& masm)
      : inCycle_(0),
        masm(masm),
        pushedAtStart_(masm.framePushed()),
        pushedAtCycle_(-1),
        pushedAtSpill_(-1),
        spilledReg_(InvalidReg),
        spilledFloatReg_(InvalidFloatReg) {}

  ~MoveEmitterLOONG64() { assertDone(); }

  void emit(const MoveResolver& moves);
  void finish();
  void setScratchRegister(Register reg) {}
};

typedef MoveEmitterLOONG64 MoveEmitter;

}  // namespace jit
}  // namespace js

#endif /* jit_loong64_MoveEmitter_loong64_h */

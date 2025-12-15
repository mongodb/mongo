/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips-shared/MoveEmitter-mips-shared.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

void MoveEmitterMIPSShared::emit(const MoveResolver& moves) {
  if (moves.numCycles()) {
    // Reserve stack for cycle resolution
    static_assert(SpillSlotSize == 8);
    masm.reserveStack(moves.numCycles() * SpillSlotSize);
    pushedAtCycle_ = masm.framePushed();
  }

  for (size_t i = 0; i < moves.numMoves(); i++) {
    emit(moves.getMove(i));
  }
}

Address MoveEmitterMIPSShared::cycleSlot(uint32_t slot,
                                         uint32_t subslot) const {
  int32_t offset = masm.framePushed() - pushedAtCycle_;
  MOZ_ASSERT(Imm16::IsInSignedRange(offset));
  return Address(StackPointer, offset + slot * sizeof(double) + subslot);
}

int32_t MoveEmitterMIPSShared::getAdjustedOffset(const MoveOperand& operand) {
  MOZ_ASSERT(operand.isMemoryOrEffectiveAddress());
  if (operand.base() != StackPointer) {
    return operand.disp();
  }

  // Adjust offset if stack pointer has been moved.
  return operand.disp() + masm.framePushed() - pushedAtStart_;
}

Address MoveEmitterMIPSShared::getAdjustedAddress(const MoveOperand& operand) {
  return Address(operand.base(), getAdjustedOffset(operand));
}

Register MoveEmitterMIPSShared::tempReg() {
  spilledReg_ = SecondScratchReg;
  return SecondScratchReg;
}

void MoveEmitterMIPSShared::emitMove(const MoveOperand& from,
                                     const MoveOperand& to) {
  if (from.isGeneralReg()) {
    // Second scratch register should not be moved by MoveEmitter.
    MOZ_ASSERT(from.reg() != spilledReg_);

    if (to.isGeneralReg()) {
      masm.movePtr(from.reg(), to.reg());
    } else if (to.isMemory()) {
      masm.storePtr(from.reg(), getAdjustedAddress(to));
    } else {
      MOZ_CRASH("Invalid emitMove arguments.");
    }
  } else if (from.isMemory()) {
    if (to.isGeneralReg()) {
      masm.loadPtr(getAdjustedAddress(from), to.reg());
    } else if (to.isMemory()) {
      masm.loadPtr(getAdjustedAddress(from), tempReg());
      masm.storePtr(tempReg(), getAdjustedAddress(to));
    } else {
      MOZ_CRASH("Invalid emitMove arguments.");
    }
  } else if (from.isEffectiveAddress()) {
    if (to.isGeneralReg()) {
      masm.computeEffectiveAddress(getAdjustedAddress(from), to.reg());
    } else if (to.isMemory()) {
      masm.computeEffectiveAddress(getAdjustedAddress(from), tempReg());
      masm.storePtr(tempReg(), getAdjustedAddress(to));
    } else {
      MOZ_CRASH("Invalid emitMove arguments.");
    }
  } else {
    MOZ_CRASH("Invalid emitMove arguments.");
  }
}

void MoveEmitterMIPSShared::emitInt32Move(const MoveOperand& from,
                                          const MoveOperand& to) {
  if (from.isGeneralReg()) {
    // Second scratch register should not be moved by MoveEmitter.
    MOZ_ASSERT(from.reg() != spilledReg_);

    if (to.isGeneralReg()) {
      masm.move32(from.reg(), to.reg());
    } else if (to.isMemory()) {
      masm.store32(from.reg(), getAdjustedAddress(to));
    } else {
      MOZ_CRASH("Invalid emitInt32Move arguments.");
    }
  } else if (from.isMemory()) {
    if (to.isGeneralReg()) {
      masm.load32(getAdjustedAddress(from), to.reg());
    } else if (to.isMemory()) {
      masm.load32(getAdjustedAddress(from), tempReg());
      masm.store32(tempReg(), getAdjustedAddress(to));
    } else {
      MOZ_CRASH("Invalid emitInt32Move arguments.");
    }
  } else if (from.isEffectiveAddress()) {
    if (to.isGeneralReg()) {
      masm.computeEffectiveAddress(getAdjustedAddress(from), to.reg());
    } else if (to.isMemory()) {
      masm.computeEffectiveAddress(getAdjustedAddress(from), tempReg());
      masm.store32(tempReg(), getAdjustedAddress(to));
    } else {
      MOZ_CRASH("Invalid emitInt32Move arguments.");
    }
  } else {
    MOZ_CRASH("Invalid emitInt32Move arguments.");
  }
}

void MoveEmitterMIPSShared::emitFloat32Move(const MoveOperand& from,
                                            const MoveOperand& to) {
  if (from.isFloatReg()) {
    if (to.isFloatReg()) {
      masm.moveFloat32(from.floatReg(), to.floatReg());
    } else if (to.isGeneralReg()) {
      // This should only be used when passing float parameter in a1,a2,a3
      MOZ_ASSERT(to.reg() == a1 || to.reg() == a2 || to.reg() == a3);
      masm.moveFromFloat32(from.floatReg(), to.reg());
    } else {
      MOZ_ASSERT(to.isMemory());
      masm.storeFloat32(from.floatReg(), getAdjustedAddress(to));
    }
  } else if (to.isFloatReg()) {
    MOZ_ASSERT(from.isMemory());
    masm.loadFloat32(getAdjustedAddress(from), to.floatReg());
  } else if (to.isGeneralReg()) {
    MOZ_ASSERT(from.isMemory());
    // This should only be used when passing float parameter in a1,a2,a3
    MOZ_ASSERT(to.reg() == a1 || to.reg() == a2 || to.reg() == a3);
    masm.loadPtr(getAdjustedAddress(from), to.reg());
  } else {
    MOZ_ASSERT(from.isMemory());
    MOZ_ASSERT(to.isMemory());
    masm.loadFloat32(getAdjustedAddress(from), ScratchFloat32Reg);
    masm.storeFloat32(ScratchFloat32Reg, getAdjustedAddress(to));
  }
}

void MoveEmitterMIPSShared::emit(const MoveOp& move) {
  const MoveOperand& from = move.from();
  const MoveOperand& to = move.to();

  if (move.isCycleEnd() && move.isCycleBegin()) {
    // A fun consequence of aliased registers is you can have multiple
    // cycles at once, and one can end exactly where another begins.
    breakCycle(from, to, move.endCycleType(), move.cycleBeginSlot());
    completeCycle(from, to, move.type(), move.cycleEndSlot());
    return;
  }

  if (move.isCycleEnd()) {
    MOZ_ASSERT(inCycle_);
    completeCycle(from, to, move.type(), move.cycleEndSlot());
    MOZ_ASSERT(inCycle_ > 0);
    inCycle_--;
    return;
  }

  if (move.isCycleBegin()) {
    breakCycle(from, to, move.endCycleType(), move.cycleBeginSlot());
    inCycle_++;
  }

  switch (move.type()) {
    case MoveOp::FLOAT32:
      emitFloat32Move(from, to);
      break;
    case MoveOp::DOUBLE:
      emitDoubleMove(from, to);
      break;
    case MoveOp::INT32:
      emitInt32Move(from, to);
      break;
    case MoveOp::GENERAL:
      emitMove(from, to);
      break;
    default:
      MOZ_CRASH("Unexpected move type");
  }
}

void MoveEmitterMIPSShared::assertDone() { MOZ_ASSERT(inCycle_ == 0); }

void MoveEmitterMIPSShared::finish() {
  assertDone();

  masm.freeStack(masm.framePushed() - pushedAtStart_);
}

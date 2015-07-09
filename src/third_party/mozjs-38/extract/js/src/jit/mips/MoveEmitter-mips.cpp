/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips/MoveEmitter-mips.h"

using namespace js;
using namespace js::jit;

MoveEmitterMIPS::MoveEmitterMIPS(MacroAssemblerMIPSCompat& masm)
  : inCycle_(0),
    masm(masm),
    pushedAtCycle_(-1),
    pushedAtSpill_(-1),
    spilledReg_(InvalidReg),
    spilledFloatReg_(InvalidFloatReg)
{
    pushedAtStart_ = masm.framePushed();
}

void
MoveEmitterMIPS::emit(const MoveResolver& moves)
{
    if (moves.numCycles()) {
        // Reserve stack for cycle resolution
        masm.reserveStack(moves.numCycles() * sizeof(double));
        pushedAtCycle_ = masm.framePushed();
    }

    for (size_t i = 0; i < moves.numMoves(); i++)
        emit(moves.getMove(i));
}

MoveEmitterMIPS::~MoveEmitterMIPS()
{
    assertDone();
}

Address
MoveEmitterMIPS::cycleSlot(uint32_t slot, uint32_t subslot) const
{
    int32_t offset = masm.framePushed() - pushedAtCycle_;
    MOZ_ASSERT(Imm16::IsInSignedRange(offset));
    return Address(StackPointer, offset + slot * sizeof(double) + subslot);
}

int32_t
MoveEmitterMIPS::getAdjustedOffset(const MoveOperand& operand)
{
    MOZ_ASSERT(operand.isMemoryOrEffectiveAddress());
    if (operand.base() != StackPointer)
        return operand.disp();

    // Adjust offset if stack pointer has been moved.
    return operand.disp() + masm.framePushed() - pushedAtStart_;
}

Address
MoveEmitterMIPS::getAdjustedAddress(const MoveOperand& operand)
{
    return Address(operand.base(), getAdjustedOffset(operand));
}


Register
MoveEmitterMIPS::tempReg()
{
    spilledReg_ = SecondScratchReg;
    return SecondScratchReg;
}

void
MoveEmitterMIPS::breakCycle(const MoveOperand& from, const MoveOperand& to,
                            MoveOp::Type type, uint32_t slotId)
{
    // There is some pattern:
    //   (A -> B)
    //   (B -> A)
    //
    // This case handles (A -> B), which we reach first. We save B, then allow
    // the original move to continue.
    switch (type) {
      case MoveOp::FLOAT32:
        if (to.isMemory()) {
            FloatRegister temp = ScratchFloat32Reg;
            masm.loadFloat32(getAdjustedAddress(to), temp);
            // Since it is uncertain if the load will be aligned or not
            // just fill both of them with the same value.
            masm.storeFloat32(temp, cycleSlot(slotId, 0));
            masm.storeFloat32(temp, cycleSlot(slotId, 4));
        } else {
            // Just always store the largest possible size.
            masm.storeDouble(to.floatReg().doubleOverlay(), cycleSlot(slotId, 0));
        }
        break;
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            FloatRegister temp = ScratchDoubleReg;
            masm.loadDouble(getAdjustedAddress(to), temp);
            masm.storeDouble(temp, cycleSlot(slotId, 0));
        } else {
            masm.storeDouble(to.floatReg(), cycleSlot(slotId, 0));
        }
        break;
      case MoveOp::INT32:
        MOZ_ASSERT(sizeof(uintptr_t) == sizeof(int32_t));
      case MoveOp::GENERAL:
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.loadPtr(getAdjustedAddress(to), temp);
            masm.storePtr(temp, cycleSlot(0, 0));
        } else {
            // Second scratch register should not be moved by MoveEmitter.
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.storePtr(to.reg(), cycleSlot(0, 0));
        }
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterMIPS::completeCycle(const MoveOperand& from, const MoveOperand& to,
                               MoveOp::Type type, uint32_t slotId)
{
    // There is some pattern:
    //   (A -> B)
    //   (B -> A)
    //
    // This case handles (B -> A), which we reach last. We emit a move from the
    // saved value of B, to A.
    switch (type) {
      case MoveOp::FLOAT32:
        if (to.isMemory()) {
            FloatRegister temp = ScratchFloat32Reg;
            masm.loadFloat32(cycleSlot(slotId, 0), temp);
            masm.storeFloat32(temp, getAdjustedAddress(to));
        } else {
            uint32_t offset = 0;
            if (from.floatReg().numAlignedAliased() == 1)
                offset = sizeof(float);
            masm.loadFloat32(cycleSlot(slotId, offset), to.floatReg());
        }
        break;
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            FloatRegister temp = ScratchDoubleReg;
            masm.loadDouble(cycleSlot(slotId, 0), temp);
            masm.storeDouble(temp, getAdjustedAddress(to));
        } else {
            masm.loadDouble(cycleSlot(slotId, 0), to.floatReg());
        }
        break;
      case MoveOp::INT32:
        MOZ_ASSERT(sizeof(uintptr_t) == sizeof(int32_t));
      case MoveOp::GENERAL:
        MOZ_ASSERT(slotId == 0);
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.loadPtr(cycleSlot(0, 0), temp);
            masm.storePtr(temp, getAdjustedAddress(to));
        } else {
            // Second scratch register should not be moved by MoveEmitter.
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.loadPtr(cycleSlot(0, 0), to.reg());
        }
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterMIPS::emitMove(const MoveOperand& from, const MoveOperand& to)
{
    if (from.isGeneralReg()) {
        // Second scratch register should not be moved by MoveEmitter.
        MOZ_ASSERT(from.reg() != spilledReg_);

        if (to.isGeneralReg())
            masm.movePtr(from.reg(), to.reg());
        else if (to.isMemory())
            masm.storePtr(from.reg(), getAdjustedAddress(to));
        else
            MOZ_CRASH("Invalid emitMove arguments.");
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

void
MoveEmitterMIPS::emitFloat32Move(const MoveOperand& from, const MoveOperand& to)
{
    // Ensure that we can use ScratchFloat32Reg in memory move.
    MOZ_ASSERT_IF(from.isFloatReg(), from.floatReg() != ScratchFloat32Reg);
    MOZ_ASSERT_IF(to.isFloatReg(), to.floatReg() != ScratchFloat32Reg);

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

void
MoveEmitterMIPS::emitDoubleMove(const MoveOperand& from, const MoveOperand& to)
{
    // Ensure that we can use ScratchDoubleReg in memory move.
    MOZ_ASSERT_IF(from.isFloatReg(), from.floatReg() != ScratchDoubleReg);
    MOZ_ASSERT_IF(to.isFloatReg(), to.floatReg() != ScratchDoubleReg);

    if (from.isFloatReg()) {
        if (to.isFloatReg()) {
            masm.moveDouble(from.floatReg(), to.floatReg());
        } else if (to.isGeneralReg()) {
            // Used for passing double parameter in a2,a3 register pair.
            // Two moves are added for one double parameter by
            // MacroAssemblerMIPSCompat::passABIArg
            if(to.reg() == a2)
                masm.moveFromDoubleLo(from.floatReg(), a2);
            else if(to.reg() == a3)
                masm.moveFromDoubleHi(from.floatReg(), a3);
            else
                MOZ_CRASH("Invalid emitDoubleMove arguments.");
        } else {
            MOZ_ASSERT(to.isMemory());
            masm.storeDouble(from.floatReg(), getAdjustedAddress(to));
        }
    } else if (to.isFloatReg()) {
        MOZ_ASSERT(from.isMemory());
        masm.loadDouble(getAdjustedAddress(from), to.floatReg());
    } else if (to.isGeneralReg()) {
        // Used for passing double parameter in a2,a3 register pair.
        // Two moves are added for one double parameter by
        // MacroAssemblerMIPSCompat::passABIArg
        if (from.isMemory()) {
            if(to.reg() == a2)
                masm.loadPtr(getAdjustedAddress(from), a2);
            else if(to.reg() == a3)
                masm.loadPtr(Address(from.base(), getAdjustedOffset(from) + sizeof(uint32_t)), a3);
            else
                MOZ_CRASH("Invalid emitDoubleMove arguments.");
        } else {
            // Used for moving a double parameter from the same source. See Bug 1123874.
            if(to.reg() == a2 || to.reg() == a3)
                masm.ma_move(to.reg(), from.reg());
            else
                MOZ_CRASH("Invalid emitDoubleMove arguments.");
        }
    } else {
        MOZ_ASSERT(from.isMemory());
        MOZ_ASSERT(to.isMemory());
        masm.loadDouble(getAdjustedAddress(from), ScratchDoubleReg);
        masm.storeDouble(ScratchDoubleReg, getAdjustedAddress(to));
    }
}

void
MoveEmitterMIPS::emit(const MoveOp& move)
{
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
        MOZ_ASSERT(sizeof(uintptr_t) == sizeof(int32_t));
      case MoveOp::GENERAL:
        emitMove(from, to);
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterMIPS::assertDone()
{
    MOZ_ASSERT(inCycle_ == 0);
}

void
MoveEmitterMIPS::finish()
{
    assertDone();

    masm.freeStack(masm.framePushed() - pushedAtStart_);
}

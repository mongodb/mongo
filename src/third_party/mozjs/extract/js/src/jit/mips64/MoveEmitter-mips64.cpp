/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips64/MoveEmitter-mips64.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

void
MoveEmitterMIPS64::breakCycle(const MoveOperand& from, const MoveOperand& to,
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
            masm.storeFloat32(temp, cycleSlot(slotId));
        } else {
            masm.storeFloat32(to.floatReg(), cycleSlot(slotId));
        }
        break;
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            FloatRegister temp = ScratchDoubleReg;
            masm.loadDouble(getAdjustedAddress(to), temp);
            masm.storeDouble(temp, cycleSlot(slotId));
        } else {
            masm.storeDouble(to.floatReg(), cycleSlot(slotId));
        }
        break;
      case MoveOp::INT32:
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.load32(getAdjustedAddress(to), temp);
            masm.store32(temp, cycleSlot(0));
        } else {
            // Second scratch register should not be moved by MoveEmitter.
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.store32(to.reg(), cycleSlot(0));
        }
        break;
      case MoveOp::GENERAL:
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.loadPtr(getAdjustedAddress(to), temp);
            masm.storePtr(temp, cycleSlot(0));
        } else {
            // Second scratch register should not be moved by MoveEmitter.
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.storePtr(to.reg(), cycleSlot(0));
        }
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterMIPS64::completeCycle(const MoveOperand& from, const MoveOperand& to,
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
            masm.loadFloat32(cycleSlot(slotId), temp);
            masm.storeFloat32(temp, getAdjustedAddress(to));
        } else {
            masm.loadFloat32(cycleSlot(slotId), to.floatReg());
        }
        break;
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            FloatRegister temp = ScratchDoubleReg;
            masm.loadDouble(cycleSlot(slotId), temp);
            masm.storeDouble(temp, getAdjustedAddress(to));
        } else {
            masm.loadDouble(cycleSlot(slotId), to.floatReg());
        }
        break;
      case MoveOp::INT32:
        MOZ_ASSERT(slotId == 0);
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.load32(cycleSlot(0), temp);
            masm.store32(temp, getAdjustedAddress(to));
        } else {
            // Second scratch register should not be moved by MoveEmitter.
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.load32(cycleSlot(0), to.reg());
        }
        break;
      case MoveOp::GENERAL:
        MOZ_ASSERT(slotId == 0);
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.loadPtr(cycleSlot(0), temp);
            masm.storePtr(temp, getAdjustedAddress(to));
        } else {
            // Second scratch register should not be moved by MoveEmitter.
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.loadPtr(cycleSlot(0), to.reg());
        }
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterMIPS64::emitDoubleMove(const MoveOperand& from, const MoveOperand& to)
{
    if (from.isFloatReg()) {
        if (to.isFloatReg()) {
            masm.moveDouble(from.floatReg(), to.floatReg());
        } else if (to.isGeneralReg()) {
            masm.moveFromDouble(from.floatReg(), to.reg());
        } else {
            MOZ_ASSERT(to.isMemory());
            masm.storeDouble(from.floatReg(), getAdjustedAddress(to));
        }
    } else if (to.isFloatReg()) {
        if (from.isMemory())
          masm.loadDouble(getAdjustedAddress(from), to.floatReg());
        else
          masm.moveToDouble(from.reg(), to.floatReg());
    } else {
        MOZ_ASSERT(from.isMemory());
        MOZ_ASSERT(to.isMemory());
        masm.loadDouble(getAdjustedAddress(from), ScratchDoubleReg);
        masm.storeDouble(ScratchDoubleReg, getAdjustedAddress(to));
    }
}

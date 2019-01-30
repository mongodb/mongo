/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips32/MoveEmitter-mips32.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

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
        MOZ_FALLTHROUGH;
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
        MOZ_FALLTHROUGH;
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
MoveEmitterMIPS::emitDoubleMove(const MoveOperand& from, const MoveOperand& to)
{
    if (from.isFloatReg()) {
        if (to.isFloatReg()) {
            masm.moveDouble(from.floatReg(), to.floatReg());
        } else if (to.isGeneralRegPair()) {
            // Used for passing double parameter in a2,a3 register pair.
            // Two moves are added for one double parameter by
            // MacroAssembler::passABIArg
            MOZ_ASSERT(to.evenReg() == a2 && to.oddReg() == a3,
                       "Invalid emitDoubleMove arguments.");
            masm.moveFromDoubleLo(from.floatReg(), a2);
            masm.moveFromDoubleHi(from.floatReg(), a3);
        } else {
            MOZ_ASSERT(to.isMemory());
            masm.storeDouble(from.floatReg(), getAdjustedAddress(to));
        }
    } else if (to.isFloatReg()) {
        MOZ_ASSERT(from.isMemory());
        masm.loadDouble(getAdjustedAddress(from), to.floatReg());
    } else if (to.isGeneralRegPair()) {
        // Used for passing double parameter in a2,a3 register pair.
        // Two moves are added for one double parameter by
        // MacroAssembler::passABIArg
        MOZ_ASSERT(from.isMemory());
        MOZ_ASSERT(to.evenReg() == a2 && to.oddReg() == a3,
                   "Invalid emitDoubleMove arguments.");
        masm.loadPtr(getAdjustedAddress(from), a2);
        masm.loadPtr(Address(from.base(), getAdjustedOffset(from) + sizeof(uint32_t)), a3);
    } else {
        MOZ_ASSERT(from.isMemory());
        MOZ_ASSERT(to.isMemory());
        masm.loadDouble(getAdjustedAddress(from), ScratchDoubleReg);
        masm.storeDouble(ScratchDoubleReg, getAdjustedAddress(to));
    }
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/MoveEmitter-arm.h"

using namespace js;
using namespace js::jit;

MoveEmitterARM::MoveEmitterARM(MacroAssemblerARMCompat& masm)
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
MoveEmitterARM::emit(const MoveResolver& moves)
{
    if (moves.numCycles()) {
        // Reserve stack for cycle resolution
        masm.reserveStack(moves.numCycles() * sizeof(double));
        pushedAtCycle_ = masm.framePushed();
    }

    for (size_t i = 0; i < moves.numMoves(); i++)
        emit(moves.getMove(i));
}

MoveEmitterARM::~MoveEmitterARM()
{
    assertDone();
}

Operand
MoveEmitterARM::cycleSlot(uint32_t slot, uint32_t subslot) const
{
    int32_t offset =  masm.framePushed() - pushedAtCycle_;
    MOZ_ASSERT(offset < 4096 && offset > -4096);
    return Operand(StackPointer, offset + slot * sizeof(double) + subslot);
}

// THIS IS ALWAYS AN LDRAddr. It should not be wrapped in an operand, methinks.
Operand
MoveEmitterARM::spillSlot() const
{
    int32_t offset =  masm.framePushed() - pushedAtSpill_;
    MOZ_ASSERT(offset < 4096 && offset > -4096);
    return Operand(StackPointer, offset);
}

Operand
MoveEmitterARM::toOperand(const MoveOperand& operand, bool isFloat) const
{
    if (operand.isMemoryOrEffectiveAddress()) {
        if (operand.base() != StackPointer) {
            MOZ_ASSERT(operand.disp() < 1024 && operand.disp() > -1024);
            return Operand(operand.base(), operand.disp());
        }

        MOZ_ASSERT(operand.disp() >= 0);

        // Otherwise, the stack offset may need to be adjusted.
        return Operand(StackPointer, operand.disp() + (masm.framePushed() - pushedAtStart_));
    }

    if (operand.isGeneralReg())
        return Operand(operand.reg());

    MOZ_ASSERT(operand.isFloatReg());
    return Operand(operand.floatReg());
}

Register
MoveEmitterARM::tempReg()
{
    if (spilledReg_ != InvalidReg)
        return spilledReg_;

    // For now, just pick r12/ip as the eviction point. This is totally random,
    // and if it ends up being bad, we can use actual heuristics later. r12 is
    // actually a bad choice. It is the scratch register, which is frequently
    // used for address computations, such as those found when we attempt to
    // access values more than 4096 off of the stack pointer. Instead, use lr,
    // the LinkRegister.
    spilledReg_ = r14;
    if (pushedAtSpill_ == -1) {
        masm.Push(spilledReg_);
        pushedAtSpill_ = masm.framePushed();
    } else {
        masm.ma_str(spilledReg_, spillSlot());
    }
    return spilledReg_;
}

void
MoveEmitterARM::breakCycle(const MoveOperand& from, const MoveOperand& to,
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
            VFPRegister temp = ScratchFloat32Reg;
            masm.ma_vldr(toOperand(to, true), temp);
            // Since it is uncertain if the load will be aligned or not
            // just fill both of them with the same value.
            masm.ma_vstr(temp, cycleSlot(slotId, 0));
            masm.ma_vstr(temp, cycleSlot(slotId, 4));
        } else {
            FloatRegister src = to.floatReg();
            // Just always store the largest possible size. Currently, this is
            // a double. When SIMD is added, two doubles will need to be stored.
            masm.ma_vstr(src.doubleOverlay(), cycleSlot(slotId, 0));
        }
        break;
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            FloatRegister temp = ScratchDoubleReg;
            masm.ma_vldr(toOperand(to, true), temp);
            masm.ma_vstr(temp, cycleSlot(slotId, 0));
        } else {
            masm.ma_vstr(to.floatReg().doubleOverlay(), cycleSlot(slotId, 0));
        }
        break;
      case MoveOp::INT32:
      case MoveOp::GENERAL:
        // an non-vfp value
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.ma_ldr(toOperand(to, false), temp);
            masm.ma_str(temp, cycleSlot(0,0));
        } else {
            if (to.reg() == spilledReg_) {
                // If the destination was spilled, restore it first.
                masm.ma_ldr(spillSlot(), spilledReg_);
                spilledReg_ = InvalidReg;
            }
            masm.ma_str(to.reg(), cycleSlot(0,0));
        }
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterARM::completeCycle(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type, uint32_t slotId)
{
    // There is some pattern:
    //   (A -> B)
    //   (B -> A)
    //
    // This case handles (B -> A), which we reach last. We emit a move from the
    // saved value of B, to A.
    switch (type) {
      case MoveOp::FLOAT32:
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            FloatRegister temp = ScratchDoubleReg;
            masm.ma_vldr(cycleSlot(slotId, 0), temp);
            masm.ma_vstr(temp, toOperand(to, true));
        } else {
            uint32_t offset = 0;
            if ((!from.isMemory()) && from.floatReg().numAlignedAliased() == 1)
                offset = sizeof(float);
            masm.ma_vldr(cycleSlot(slotId, offset), to.floatReg());
        }
        break;
      case MoveOp::INT32:
      case MoveOp::GENERAL:
        MOZ_ASSERT(slotId == 0);
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.ma_ldr(cycleSlot(slotId, 0), temp);
            masm.ma_str(temp, toOperand(to, false));
        } else {
            if (to.reg() == spilledReg_) {
                // Make sure we don't re-clobber the spilled register later.
                spilledReg_ = InvalidReg;
            }
            masm.ma_ldr(cycleSlot(slotId, 0), to.reg());
        }
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterARM::emitMove(const MoveOperand& from, const MoveOperand& to)
{
    if (to.isGeneralReg() && to.reg() == spilledReg_) {
        // If the destination is the spilled register, make sure we
        // don't re-clobber its value.
        spilledReg_ = InvalidReg;
    }

    if (from.isGeneralReg()) {
        if (from.reg() == spilledReg_) {
            // If the source is a register that has been spilled, make sure
            // to load the source back into that register.
            masm.ma_ldr(spillSlot(), spilledReg_);
            spilledReg_ = InvalidReg;
        }
        switch (toOperand(to, false).getTag()) {
          case Operand::OP2:
            // secretly must be a register
            masm.ma_mov(from.reg(), to.reg());
            break;
          case Operand::MEM:
            masm.ma_str(from.reg(), toOperand(to, false));
            break;
          default:
            MOZ_CRASH("strange move!");
        }
    } else if (to.isGeneralReg()) {
        MOZ_ASSERT(from.isMemoryOrEffectiveAddress());
        if (from.isMemory())
            masm.ma_ldr(toOperand(from, false), to.reg());
        else
            masm.ma_add(from.base(), Imm32(from.disp()), to.reg());
    } else {
        // Memory to memory gpr move.
        Register reg = tempReg();

        MOZ_ASSERT(from.isMemoryOrEffectiveAddress());
        if (from.isMemory())
            masm.ma_ldr(toOperand(from, false), reg);
        else
            masm.ma_add(from.base(), Imm32(from.disp()), reg);
        MOZ_ASSERT(to.base() != reg);
        masm.ma_str(reg, toOperand(to, false));
    }
}

void
MoveEmitterARM::emitFloat32Move(const MoveOperand& from, const MoveOperand& to)
{
    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.ma_vmov_f32(from.floatReg(), to.floatReg());
        else
            masm.ma_vstr(VFPRegister(from.floatReg()).singleOverlay(),
                         toOperand(to, true));
    } else if (to.isFloatReg()) {
        masm.ma_vldr(toOperand(from, true),
                     VFPRegister(to.floatReg()).singleOverlay());
    } else {
        // Memory to memory move.
        MOZ_ASSERT(from.isMemory());
        FloatRegister reg = ScratchFloat32Reg;
        masm.ma_vldr(toOperand(from, true),
                     VFPRegister(reg).singleOverlay());
        masm.ma_vstr(VFPRegister(reg).singleOverlay(),
                     toOperand(to, true));
    }
}

void
MoveEmitterARM::emitDoubleMove(const MoveOperand& from, const MoveOperand& to)
{
    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.ma_vmov(from.floatReg(), to.floatReg());
        else
            masm.ma_vstr(from.floatReg(), toOperand(to, true));
    } else if (to.isFloatReg()) {
        masm.ma_vldr(toOperand(from, true), to.floatReg());
    } else {
        // Memory to memory move.
        MOZ_ASSERT(from.isMemory());
        FloatRegister reg = ScratchDoubleReg;
        masm.ma_vldr(toOperand(from, true), reg);
        masm.ma_vstr(reg, toOperand(to, true));
    }
}

void
MoveEmitterARM::emit(const MoveOp& move)
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
      case MoveOp::GENERAL:
        emitMove(from, to);
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterARM::assertDone()
{
    MOZ_ASSERT(inCycle_ == 0);
}

void
MoveEmitterARM::finish()
{
    assertDone();

    if (pushedAtSpill_ != -1 && spilledReg_ != InvalidReg)
        masm.ma_ldr(spillSlot(), spilledReg_);
    masm.freeStack(masm.framePushed() - pushedAtStart_);
}

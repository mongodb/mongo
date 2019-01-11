/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/MoveEmitter-arm.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

MoveEmitterARM::MoveEmitterARM(MacroAssembler& masm)
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

Address
MoveEmitterARM::cycleSlot(uint32_t slot, uint32_t subslot) const
{
    int32_t offset =  masm.framePushed() - pushedAtCycle_;
    MOZ_ASSERT(offset < 4096 && offset > -4096);
    return Address(StackPointer, offset + slot * sizeof(double) + subslot);
}

Address
MoveEmitterARM::spillSlot() const
{
    int32_t offset =  masm.framePushed() - pushedAtSpill_;
    MOZ_ASSERT(offset < 4096 && offset > -4096);
    return Address(StackPointer, offset);
}

Address
MoveEmitterARM::toAddress(const MoveOperand& operand) const
{
    MOZ_ASSERT(operand.isMemoryOrEffectiveAddress());

    if (operand.base() != StackPointer) {
        MOZ_ASSERT(operand.disp() < 1024 && operand.disp() > -1024);
        return Operand(operand.base(), operand.disp()).toAddress();
    }

    MOZ_ASSERT(operand.disp() >= 0);

    // Otherwise, the stack offset may need to be adjusted.
    return Address(StackPointer, operand.disp() + (masm.framePushed() - pushedAtStart_));
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
        ScratchRegisterScope scratch(masm);
        masm.ma_str(spilledReg_, spillSlot(), scratch);
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

    ScratchRegisterScope scratch(masm);

    switch (type) {
      case MoveOp::FLOAT32:
        if (to.isMemory()) {
            ScratchFloat32Scope scratchFloat32(masm);
            masm.ma_vldr(toAddress(to), scratchFloat32, scratch);
            // Since it is uncertain if the load will be aligned or not
            // just fill both of them with the same value.
            masm.ma_vstr(scratchFloat32, cycleSlot(slotId, 0), scratch);
            masm.ma_vstr(scratchFloat32, cycleSlot(slotId, 4), scratch);
        } else if (to.isGeneralReg()) {
            // Since it is uncertain if the load will be aligned or not
            // just fill both of them with the same value.
            masm.ma_str(to.reg(), cycleSlot(slotId, 0), scratch);
            masm.ma_str(to.reg(), cycleSlot(slotId, 4), scratch);
        } else {
            FloatRegister src = to.floatReg();
            // Just always store the largest possible size. Currently, this is
            // a double. When SIMD is added, two doubles will need to be stored.
            masm.ma_vstr(src.doubleOverlay(), cycleSlot(slotId, 0), scratch);
        }
        break;
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            ScratchDoubleScope scratchDouble(masm);
            masm.ma_vldr(toAddress(to), scratchDouble, scratch);
            masm.ma_vstr(scratchDouble, cycleSlot(slotId, 0), scratch);
        } else if (to.isGeneralRegPair()) {
            ScratchDoubleScope scratchDouble(masm);
            masm.ma_vxfer(to.evenReg(), to.oddReg(), scratchDouble);
            masm.ma_vstr(scratchDouble, cycleSlot(slotId, 0), scratch);
        } else {
            masm.ma_vstr(to.floatReg().doubleOverlay(), cycleSlot(slotId, 0), scratch);
        }
        break;
      case MoveOp::INT32:
      case MoveOp::GENERAL:
        // an non-vfp value
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.ma_ldr(toAddress(to), temp, scratch);
            masm.ma_str(temp, cycleSlot(0,0), scratch);
        } else {
            if (to.reg() == spilledReg_) {
                // If the destination was spilled, restore it first.
                masm.ma_ldr(spillSlot(), spilledReg_, scratch);
                spilledReg_ = InvalidReg;
            }
            masm.ma_str(to.reg(), cycleSlot(0,0), scratch);
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

    ScratchRegisterScope scratch(masm);

    switch (type) {
      case MoveOp::FLOAT32:
        MOZ_ASSERT(!to.isGeneralRegPair());
        if (to.isMemory()) {
            ScratchFloat32Scope scratchFloat32(masm);
            masm.ma_vldr(cycleSlot(slotId, 0), scratchFloat32, scratch);
            masm.ma_vstr(scratchFloat32, toAddress(to), scratch);
        } else if (to.isGeneralReg()) {
            MOZ_ASSERT(type == MoveOp::FLOAT32);
            masm.ma_ldr(toAddress(from), to.reg(), scratch);
        } else {
            uint32_t offset = 0;
            if ((!from.isMemory()) && from.floatReg().numAlignedAliased() == 1)
                offset = sizeof(float);
            masm.ma_vldr(cycleSlot(slotId, offset), to.floatReg(), scratch);
        }
        break;
      case MoveOp::DOUBLE:
        MOZ_ASSERT(!to.isGeneralReg());
        if (to.isMemory()) {
            ScratchDoubleScope scratchDouble(masm);
            masm.ma_vldr(cycleSlot(slotId, 0), scratchDouble, scratch);
            masm.ma_vstr(scratchDouble, toAddress(to), scratch);
        } else if (to.isGeneralRegPair()) {
            MOZ_ASSERT(type == MoveOp::DOUBLE);
            ScratchDoubleScope scratchDouble(masm);
            masm.ma_vldr(toAddress(from), scratchDouble, scratch);
            masm.ma_vxfer(scratchDouble, to.evenReg(), to.oddReg());
        } else {
            uint32_t offset = 0;
            if ((!from.isMemory()) && from.floatReg().numAlignedAliased() == 1)
                offset = sizeof(float);
            masm.ma_vldr(cycleSlot(slotId, offset), to.floatReg(), scratch);
        }
        break;
      case MoveOp::INT32:
      case MoveOp::GENERAL:
        MOZ_ASSERT(slotId == 0);
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.ma_ldr(cycleSlot(slotId, 0), temp, scratch);
            masm.ma_str(temp, toAddress(to), scratch);
        } else {
            if (to.reg() == spilledReg_) {
                // Make sure we don't re-clobber the spilled register later.
                spilledReg_ = InvalidReg;
            }
            masm.ma_ldr(cycleSlot(slotId, 0), to.reg(), scratch);
        }
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterARM::emitMove(const MoveOperand& from, const MoveOperand& to)
{
    // Register pairs are used to store Double values during calls.
    MOZ_ASSERT(!from.isGeneralRegPair());
    MOZ_ASSERT(!to.isGeneralRegPair());

    ScratchRegisterScope scratch(masm);

    if (to.isGeneralReg() && to.reg() == spilledReg_) {
        // If the destination is the spilled register, make sure we
        // don't re-clobber its value.
        spilledReg_ = InvalidReg;
    }

    if (from.isGeneralReg()) {
        if (from.reg() == spilledReg_) {
            // If the source is a register that has been spilled, make sure
            // to load the source back into that register.
            masm.ma_ldr(spillSlot(), spilledReg_, scratch);
            spilledReg_ = InvalidReg;
        }
        if (to.isMemoryOrEffectiveAddress())
            masm.ma_str(from.reg(), toAddress(to), scratch);
        else
            masm.ma_mov(from.reg(), to.reg());
    } else if (to.isGeneralReg()) {
        MOZ_ASSERT(from.isMemoryOrEffectiveAddress());
        if (from.isMemory())
            masm.ma_ldr(toAddress(from), to.reg(), scratch);
        else
            masm.ma_add(from.base(), Imm32(from.disp()), to.reg(), scratch);
    } else {
        // Memory to memory gpr move.
        Register reg = tempReg();

        MOZ_ASSERT(from.isMemoryOrEffectiveAddress());
        if (from.isMemory())
            masm.ma_ldr(toAddress(from), reg, scratch);
        else
            masm.ma_add(from.base(), Imm32(from.disp()), reg, scratch);
        MOZ_ASSERT(to.base() != reg);
        masm.ma_str(reg, toAddress(to), scratch);
    }
}

void
MoveEmitterARM::emitFloat32Move(const MoveOperand& from, const MoveOperand& to)
{
    // Register pairs are used to store Double values during calls.
    MOZ_ASSERT(!from.isGeneralRegPair());
    MOZ_ASSERT(!to.isGeneralRegPair());

    ScratchRegisterScope scratch(masm);

    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.ma_vmov_f32(from.floatReg(), to.floatReg());
        else if (to.isGeneralReg())
            masm.ma_vxfer(from.floatReg(), to.reg());
        else
            masm.ma_vstr(VFPRegister(from.floatReg()).singleOverlay(), toAddress(to), scratch);
    } else if (from.isGeneralReg()) {
        if (to.isFloatReg()) {
            masm.ma_vxfer(from.reg(), to.floatReg());
        } else if (to.isGeneralReg()) {
            masm.ma_mov(from.reg(), to.reg());
        } else {
            masm.ma_str(from.reg(), toAddress(to), scratch);
        }
    } else if (to.isFloatReg()) {
        masm.ma_vldr(toAddress(from), VFPRegister(to.floatReg()).singleOverlay(), scratch);
    } else if (to.isGeneralReg()) {
        masm.ma_ldr(toAddress(from), to.reg(), scratch);
    } else {
        // Memory to memory move.
        MOZ_ASSERT(from.isMemory());
        ScratchFloat32Scope scratchFloat32(masm);
        masm.ma_vldr(toAddress(from), scratchFloat32, scratch);
        masm.ma_vstr(scratchFloat32, toAddress(to), scratch);
    }
}

void
MoveEmitterARM::emitDoubleMove(const MoveOperand& from, const MoveOperand& to)
{
    // Registers are used to store pointers / int32 / float32 values.
    MOZ_ASSERT(!from.isGeneralReg());
    MOZ_ASSERT(!to.isGeneralReg());

    ScratchRegisterScope scratch(masm);

    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.ma_vmov(from.floatReg(), to.floatReg());
        else if (to.isGeneralRegPair())
            masm.ma_vxfer(from.floatReg(), to.evenReg(), to.oddReg());
        else
            masm.ma_vstr(from.floatReg(), toAddress(to), scratch);
    } else if (from.isGeneralRegPair()) {
        if (to.isFloatReg())
            masm.ma_vxfer(from.evenReg(), from.oddReg(), to.floatReg());
        else if (to.isGeneralRegPair()) {
            MOZ_ASSERT(!from.aliases(to));
            masm.ma_mov(from.evenReg(), to.evenReg());
            masm.ma_mov(from.oddReg(), to.oddReg());
        } else {
            ScratchDoubleScope scratchDouble(masm);
            masm.ma_vxfer(from.evenReg(), from.oddReg(), scratchDouble);
            masm.ma_vstr(scratchDouble, toAddress(to), scratch);
        }
    } else if (to.isFloatReg()) {
        masm.ma_vldr(toAddress(from), to.floatReg(), scratch);
    } else if (to.isGeneralRegPair()) {
        MOZ_ASSERT(from.isMemory());
        Address src = toAddress(from);
        // Note: We can safely use the MoveOperand's displacement here,
        // even if the base is SP: MoveEmitter::toOperand adjusts
        // SP-relative operands by the difference between the current
        // stack usage and stackAdjust, which emitter.finish() resets to
        // 0.
        //
        // Warning: if the offset isn't within [-255,+255] then this
        // will assert-fail (or, if non-debug, load the wrong words).
        // Nothing uses such an offset at the time of this writing.
        masm.ma_ldrd(EDtrAddr(src.base, EDtrOffImm(src.offset)), to.evenReg(), to.oddReg());
    } else {
        // Memory to memory move.
        MOZ_ASSERT(from.isMemory());
        ScratchDoubleScope scratchDouble(masm);
        masm.ma_vldr(toAddress(from), scratchDouble, scratch);
        masm.ma_vstr(scratchDouble, toAddress(to), scratch);
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

    if (pushedAtSpill_ != -1 && spilledReg_ != InvalidReg) {
        ScratchRegisterScope scratch(masm);
        masm.ma_ldr(spillSlot(), spilledReg_, scratch);
    }
    masm.freeStack(masm.framePushed() - pushedAtStart_);
}

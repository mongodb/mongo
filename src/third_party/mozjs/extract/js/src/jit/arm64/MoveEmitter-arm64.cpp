/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm64/MoveEmitter-arm64.h"
#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

MemOperand MoveEmitterARM64::toMemOperand(const MoveOperand& operand) const {
  MOZ_ASSERT(operand.isMemory());
  ARMRegister base(operand.base(), 64);
  if (operand.base() == masm.getStackPointer()) {
    return MemOperand(base,
                      operand.disp() + (masm.framePushed() - pushedAtStart_));
  }
  return MemOperand(base, operand.disp());
}

void MoveEmitterARM64::emit(const MoveResolver& moves) {
  vixl::UseScratchRegisterScope temps(&masm.asVIXL());
  // We have two scratch general registers, so use one as temporary storage for
  // breaking cycles and leave the other available for memory to memory moves.
  //
  // This register is used when breaking GENERAL, INT32, FLOAT32, and DOUBLE
  // move cycles. For FLOAT32/DOUBLE, this involves a fmov between float and
  // general registers. We could avoid this if we had an extra scratch float
  // register, otherwise we need the scratch float register for memory to
  // memory moves that may happen in the cycle. We cannot use the scratch
  // general register for SIMD128 cycles as it is not large enough.
  cycleGeneralReg_ = temps.AcquireX();

  for (size_t i = 0; i < moves.numMoves(); i++) {
    emitMove(moves.getMove(i));
  }

  cycleGeneralReg_ = ARMRegister();
}

void MoveEmitterARM64::finish() {
  assertDone();
  masm.freeStack(masm.framePushed() - pushedAtStart_);
  MOZ_ASSERT(masm.framePushed() == pushedAtStart_);
}

void MoveEmitterARM64::emitMove(const MoveOp& move) {
  const MoveOperand& from = move.from();
  const MoveOperand& to = move.to();

  if (move.isCycleBegin()) {
    MOZ_ASSERT(!inCycle_ && !move.isCycleEnd());
    breakCycle(from, to, move.endCycleType());
    inCycle_ = true;
  } else if (move.isCycleEnd()) {
    MOZ_ASSERT(inCycle_);
    completeCycle(from, to, move.type());
    inCycle_ = false;
    return;
  }

  switch (move.type()) {
    case MoveOp::FLOAT32:
      emitFloat32Move(from, to);
      break;
    case MoveOp::DOUBLE:
      emitDoubleMove(from, to);
      break;
    case MoveOp::SIMD128:
      emitSimd128Move(from, to);
      break;
    case MoveOp::INT32:
      emitInt32Move(from, to);
      break;
    case MoveOp::GENERAL:
      emitGeneralMove(from, to);
      break;
    default:
      MOZ_CRASH("Unexpected move type");
  }
}

void MoveEmitterARM64::emitFloat32Move(const MoveOperand& from,
                                       const MoveOperand& to) {
  if (from.isFloatReg()) {
    if (to.isFloatReg()) {
      masm.Fmov(toFPReg(to, MoveOp::FLOAT32), toFPReg(from, MoveOp::FLOAT32));
    } else {
      masm.Str(toFPReg(from, MoveOp::FLOAT32), toMemOperand(to));
    }
    return;
  }

  if (to.isFloatReg()) {
    masm.Ldr(toFPReg(to, MoveOp::FLOAT32), toMemOperand(from));
    return;
  }

  vixl::UseScratchRegisterScope temps(&masm.asVIXL());
  const ARMFPRegister scratch32 = temps.AcquireS();
  masm.Ldr(scratch32, toMemOperand(from));
  masm.Str(scratch32, toMemOperand(to));
}

void MoveEmitterARM64::emitDoubleMove(const MoveOperand& from,
                                      const MoveOperand& to) {
  if (from.isFloatReg()) {
    if (to.isFloatReg()) {
      masm.Fmov(toFPReg(to, MoveOp::DOUBLE), toFPReg(from, MoveOp::DOUBLE));
    } else {
      masm.Str(toFPReg(from, MoveOp::DOUBLE), toMemOperand(to));
    }
    return;
  }

  if (to.isFloatReg()) {
    masm.Ldr(toFPReg(to, MoveOp::DOUBLE), toMemOperand(from));
    return;
  }

  vixl::UseScratchRegisterScope temps(&masm.asVIXL());
  const ARMFPRegister scratch = temps.AcquireD();
  masm.Ldr(scratch, toMemOperand(from));
  masm.Str(scratch, toMemOperand(to));
}

void MoveEmitterARM64::emitSimd128Move(const MoveOperand& from,
                                       const MoveOperand& to) {
  if (from.isFloatReg()) {
    if (to.isFloatReg()) {
      masm.Mov(toFPReg(to, MoveOp::SIMD128), toFPReg(from, MoveOp::SIMD128));
    } else {
      masm.Str(toFPReg(from, MoveOp::SIMD128), toMemOperand(to));
    }
    return;
  }

  if (to.isFloatReg()) {
    masm.Ldr(toFPReg(to, MoveOp::SIMD128), toMemOperand(from));
    return;
  }

  vixl::UseScratchRegisterScope temps(&masm.asVIXL());
  const ARMFPRegister scratch = temps.AcquireQ();
  masm.Ldr(scratch, toMemOperand(from));
  masm.Str(scratch, toMemOperand(to));
}

void MoveEmitterARM64::emitInt32Move(const MoveOperand& from,
                                     const MoveOperand& to) {
  if (from.isGeneralReg()) {
    if (to.isGeneralReg()) {
      masm.Mov(toARMReg32(to), toARMReg32(from));
    } else {
      masm.Str(toARMReg32(from), toMemOperand(to));
    }
    return;
  }

  if (to.isGeneralReg()) {
    masm.Ldr(toARMReg32(to), toMemOperand(from));
    return;
  }

  vixl::UseScratchRegisterScope temps(&masm.asVIXL());
  const ARMRegister scratch32 = temps.AcquireW();
  masm.Ldr(scratch32, toMemOperand(from));
  masm.Str(scratch32, toMemOperand(to));
}

void MoveEmitterARM64::emitGeneralMove(const MoveOperand& from,
                                       const MoveOperand& to) {
  if (from.isGeneralReg()) {
    MOZ_ASSERT(to.isGeneralReg() || to.isMemory());
    if (to.isGeneralReg()) {
      masm.Mov(toARMReg64(to), toARMReg64(from));
    } else {
      masm.Str(toARMReg64(from), toMemOperand(to));
    }
    return;
  }

  // {Memory OR EffectiveAddress} -> Register move.
  if (to.isGeneralReg()) {
    MOZ_ASSERT(from.isMemoryOrEffectiveAddress());
    if (from.isMemory()) {
      masm.Ldr(toARMReg64(to), toMemOperand(from));
    } else {
      masm.Add(toARMReg64(to), toARMReg64(from), Operand(from.disp()));
    }
    return;
  }

  vixl::UseScratchRegisterScope temps(&masm.asVIXL());
  const ARMRegister scratch64 = temps.AcquireX();

  // Memory -> Memory move.
  if (from.isMemory()) {
    MOZ_ASSERT(to.isMemory());
    masm.Ldr(scratch64, toMemOperand(from));
    masm.Str(scratch64, toMemOperand(to));
    return;
  }

  // EffectiveAddress -> Memory move.
  MOZ_ASSERT(from.isEffectiveAddress());
  MOZ_ASSERT(to.isMemory());
  masm.Add(scratch64, toARMReg64(from), Operand(from.disp()));
  masm.Str(scratch64, toMemOperand(to));
}

MemOperand MoveEmitterARM64::cycleSlot() {
  // Using SP as stack pointer requires alignment preservation below.
  MOZ_ASSERT(!masm.GetStackPointer64().Is(sp));

  // Allocate a slot for breaking cycles if we have not already
  if (pushedAtCycle_ == -1) {
    static_assert(SpillSlotSize == 16);
    masm.reserveStack(SpillSlotSize);
    pushedAtCycle_ = masm.framePushed();
  }

  return MemOperand(masm.GetStackPointer64(),
                    masm.framePushed() - pushedAtCycle_);
}

void MoveEmitterARM64::breakCycle(const MoveOperand& from,
                                  const MoveOperand& to, MoveOp::Type type) {
  switch (type) {
    case MoveOp::FLOAT32:
      if (to.isMemory()) {
        masm.Ldr(cycleGeneralReg_.W(), toMemOperand(to));
      } else {
        masm.Fmov(cycleGeneralReg_.W(), toFPReg(to, type));
      }
      break;

    case MoveOp::DOUBLE:
      if (to.isMemory()) {
        masm.Ldr(cycleGeneralReg_.X(), toMemOperand(to));
      } else {
        masm.Fmov(cycleGeneralReg_.X(), toFPReg(to, type));
      }
      break;

    case MoveOp::SIMD128:
      if (to.isMemory()) {
        vixl::UseScratchRegisterScope temps(&masm.asVIXL());
        const ARMFPRegister scratch128 = temps.AcquireQ();
        masm.Ldr(scratch128, toMemOperand(to));
        masm.Str(scratch128, cycleSlot());
      } else {
        masm.Str(toFPReg(to, type), cycleSlot());
      }
      break;

    case MoveOp::INT32:
      if (to.isMemory()) {
        masm.Ldr(cycleGeneralReg_.W(), toMemOperand(to));
      } else {
        masm.Mov(cycleGeneralReg_.W(), toARMReg32(to));
      }
      break;

    case MoveOp::GENERAL:
      if (to.isMemory()) {
        masm.Ldr(cycleGeneralReg_.X(), toMemOperand(to));
      } else {
        masm.Mov(cycleGeneralReg_.X(), toARMReg64(to));
      }
      break;

    default:
      MOZ_CRASH("Unexpected move type");
  }
}

void MoveEmitterARM64::completeCycle(const MoveOperand& from,
                                     const MoveOperand& to, MoveOp::Type type) {
  switch (type) {
    case MoveOp::FLOAT32:
      if (to.isMemory()) {
        masm.Str(cycleGeneralReg_.W(), toMemOperand(to));
      } else {
        masm.Fmov(toFPReg(to, type), cycleGeneralReg_.W());
      }
      break;

    case MoveOp::DOUBLE:
      if (to.isMemory()) {
        masm.Str(cycleGeneralReg_.X(), toMemOperand(to));
      } else {
        masm.Fmov(toFPReg(to, type), cycleGeneralReg_.X());
      }
      break;

    case MoveOp::SIMD128:
      if (to.isMemory()) {
        vixl::UseScratchRegisterScope temps(&masm.asVIXL());
        const ARMFPRegister scratch = temps.AcquireQ();
        masm.Ldr(scratch, cycleSlot());
        masm.Str(scratch, toMemOperand(to));
      } else {
        masm.Ldr(toFPReg(to, type), cycleSlot());
      }
      break;

    case MoveOp::INT32:
      if (to.isMemory()) {
        masm.Str(cycleGeneralReg_.W(), toMemOperand(to));
      } else {
        masm.Mov(toARMReg32(to), cycleGeneralReg_.W());
      }
      break;

    case MoveOp::GENERAL:
      if (to.isMemory()) {
        masm.Str(cycleGeneralReg_.X(), toMemOperand(to));
      } else {
        masm.Mov(toARMReg64(to), cycleGeneralReg_.X());
      }
      break;

    default:
      MOZ_CRASH("Unexpected move type");
  }
}

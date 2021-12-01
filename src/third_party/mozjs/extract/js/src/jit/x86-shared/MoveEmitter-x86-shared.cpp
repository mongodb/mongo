/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86-shared/MoveEmitter-x86-shared.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Maybe;

MoveEmitterX86::MoveEmitterX86(MacroAssembler& masm)
  : inCycle_(false),
    masm(masm),
    pushedAtCycle_(-1)
{
    pushedAtStart_ = masm.framePushed();
}

// Examine the cycle in moves starting at position i. Determine if it's a
// simple cycle consisting of all register-to-register moves in a single class,
// and whether it can be implemented entirely by swaps.
size_t
MoveEmitterX86::characterizeCycle(const MoveResolver& moves, size_t i,
                                  bool* allGeneralRegs, bool* allFloatRegs)
{
    size_t swapCount = 0;

    for (size_t j = i; ; j++) {
        const MoveOp& move = moves.getMove(j);

        // If it isn't a cycle of registers of the same kind, we won't be able
        // to optimize it.
        if (!move.to().isGeneralReg())
            *allGeneralRegs = false;
        if (!move.to().isFloatReg())
            *allFloatRegs = false;
        if (!*allGeneralRegs && !*allFloatRegs)
            return -1;

        // Stop iterating when we see the last one.
        if (j != i && move.isCycleEnd())
            break;

        // Check that this move is actually part of the cycle. This is
        // over-conservative when there are multiple reads from the same source,
        // but that's expected to be rare.
        if (move.from() != moves.getMove(j + 1).to()) {
            *allGeneralRegs = false;
            *allFloatRegs = false;
            return -1;
        }

        swapCount++;
    }

    // Check that the last move cycles back to the first move.
    const MoveOp& move = moves.getMove(i + swapCount);
    if (move.from() != moves.getMove(i).to()) {
        *allGeneralRegs = false;
        *allFloatRegs = false;
        return -1;
    }

    return swapCount;
}

// If we can emit optimized code for the cycle in moves starting at position i,
// do so, and return true.
bool
MoveEmitterX86::maybeEmitOptimizedCycle(const MoveResolver& moves, size_t i,
                                        bool allGeneralRegs, bool allFloatRegs, size_t swapCount)
{
    if (allGeneralRegs && swapCount <= 2) {
        // Use x86's swap-integer-registers instruction if we only have a few
        // swaps. (x86 also has a swap between registers and memory but it's
        // slow.)
        for (size_t k = 0; k < swapCount; k++)
            masm.xchg(moves.getMove(i + k).to().reg(), moves.getMove(i + k + 1).to().reg());
        return true;
    }

    if (allFloatRegs && swapCount == 1) {
        // There's no xchg for xmm registers, but if we only need a single swap,
        // it's cheap to do an XOR swap.
        FloatRegister a = moves.getMove(i).to().floatReg();
        FloatRegister b = moves.getMove(i + 1).to().floatReg();
        masm.vxorpd(a, b, b);
        masm.vxorpd(b, a, a);
        masm.vxorpd(a, b, b);
        return true;
    }

    return false;
}

void
MoveEmitterX86::emit(const MoveResolver& moves)
{
#if defined(JS_CODEGEN_X86) && defined(DEBUG)
    // Clobber any scratch register we have, to make regalloc bugs more visible.
    if (scratchRegister_.isSome())
        masm.mov(ImmWord(0xdeadbeef), scratchRegister_.value());
#endif

    for (size_t i = 0; i < moves.numMoves(); i++) {
#if defined(JS_CODEGEN_X86) && defined(DEBUG)
        if (!scratchRegister_.isSome()) {
            Maybe<Register> reg = findScratchRegister(moves, i);
            if (reg.isSome())
                masm.mov(ImmWord(0xdeadbeef), reg.value());
        }
#endif

        const MoveOp& move = moves.getMove(i);
        const MoveOperand& from = move.from();
        const MoveOperand& to = move.to();

        if (move.isCycleEnd()) {
            MOZ_ASSERT(inCycle_);
            completeCycle(to, move.type());
            inCycle_ = false;
            continue;
        }

        if (move.isCycleBegin()) {
            MOZ_ASSERT(!inCycle_);

            // Characterize the cycle.
            bool allGeneralRegs = true, allFloatRegs = true;
            size_t swapCount = characterizeCycle(moves, i, &allGeneralRegs, &allFloatRegs);

            // Attempt to optimize it to avoid using the stack.
            if (maybeEmitOptimizedCycle(moves, i, allGeneralRegs, allFloatRegs, swapCount)) {
                i += swapCount;
                continue;
            }

            // Otherwise use the stack.
            breakCycle(to, move.endCycleType());
            inCycle_ = true;
        }

        // A normal move which is not part of a cycle.
        switch (move.type()) {
          case MoveOp::FLOAT32:
            emitFloat32Move(from, to);
            break;
          case MoveOp::DOUBLE:
            emitDoubleMove(from, to);
            break;
          case MoveOp::INT32:
            emitInt32Move(from, to, moves, i);
            break;
          case MoveOp::GENERAL:
            emitGeneralMove(from, to, moves, i);
            break;
          case MoveOp::SIMD128INT:
            emitSimd128IntMove(from, to);
            break;
          case MoveOp::SIMD128FLOAT:
            emitSimd128FloatMove(from, to);
            break;
          default:
            MOZ_CRASH("Unexpected move type");
        }
    }
}

MoveEmitterX86::~MoveEmitterX86()
{
    assertDone();
}

Address
MoveEmitterX86::cycleSlot()
{
    if (pushedAtCycle_ == -1) {
        // Reserve stack for cycle resolution
        masm.reserveStack(Simd128DataSize);
        pushedAtCycle_ = masm.framePushed();
    }

    return Address(StackPointer, masm.framePushed() - pushedAtCycle_);
}

Address
MoveEmitterX86::toAddress(const MoveOperand& operand) const
{
    if (operand.base() != StackPointer)
        return Address(operand.base(), operand.disp());

    MOZ_ASSERT(operand.disp() >= 0);

    // Otherwise, the stack offset may need to be adjusted.
    return Address(StackPointer, operand.disp() + (masm.framePushed() - pushedAtStart_));
}

// Warning, do not use the resulting operand with pop instructions, since they
// compute the effective destination address after altering the stack pointer.
// Use toPopOperand if an Operand is needed for a pop.
Operand
MoveEmitterX86::toOperand(const MoveOperand& operand) const
{
    if (operand.isMemoryOrEffectiveAddress())
        return Operand(toAddress(operand));
    if (operand.isGeneralReg())
        return Operand(operand.reg());

    MOZ_ASSERT(operand.isFloatReg());
    return Operand(operand.floatReg());
}

// This is the same as toOperand except that it computes an Operand suitable for
// use in a pop.
Operand
MoveEmitterX86::toPopOperand(const MoveOperand& operand) const
{
    if (operand.isMemory()) {
        if (operand.base() != StackPointer)
            return Operand(operand.base(), operand.disp());

        MOZ_ASSERT(operand.disp() >= 0);

        // Otherwise, the stack offset may need to be adjusted.
        // Note the adjustment by the stack slot here, to offset for the fact that pop
        // computes its effective address after incrementing the stack pointer.
        return Operand(StackPointer,
                       operand.disp() + (masm.framePushed() - sizeof(void*) - pushedAtStart_));
    }
    if (operand.isGeneralReg())
        return Operand(operand.reg());

    MOZ_ASSERT(operand.isFloatReg());
    return Operand(operand.floatReg());
}

void
MoveEmitterX86::breakCycle(const MoveOperand& to, MoveOp::Type type)
{
    // There is some pattern:
    //   (A -> B)
    //   (B -> A)
    //
    // This case handles (A -> B), which we reach first. We save B, then allow
    // the original move to continue.
    switch (type) {
      case MoveOp::SIMD128INT:
        if (to.isMemory()) {
            ScratchSimd128Scope scratch(masm);
            masm.loadAlignedSimd128Int(toAddress(to), scratch);
            masm.storeAlignedSimd128Int(scratch, cycleSlot());
        } else {
            masm.storeAlignedSimd128Int(to.floatReg(), cycleSlot());
        }
        break;
      case MoveOp::SIMD128FLOAT:
        if (to.isMemory()) {
            ScratchSimd128Scope scratch(masm);
            masm.loadAlignedSimd128Float(toAddress(to), scratch);
            masm.storeAlignedSimd128Float(scratch, cycleSlot());
        } else {
            masm.storeAlignedSimd128Float(to.floatReg(), cycleSlot());
        }
        break;
      case MoveOp::FLOAT32:
        if (to.isMemory()) {
            ScratchFloat32Scope scratch(masm);
            masm.loadFloat32(toAddress(to), scratch);
            masm.storeFloat32(scratch, cycleSlot());
        } else {
            masm.storeFloat32(to.floatReg(), cycleSlot());
        }
        break;
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            ScratchDoubleScope scratch(masm);
            masm.loadDouble(toAddress(to), scratch);
            masm.storeDouble(scratch, cycleSlot());
        } else {
            masm.storeDouble(to.floatReg(), cycleSlot());
        }
        break;
      case MoveOp::INT32:
#ifdef JS_CODEGEN_X64
        // x64 can't pop to a 32-bit destination, so don't push.
        if (to.isMemory()) {
            masm.load32(toAddress(to), ScratchReg);
            masm.store32(ScratchReg, cycleSlot());
        } else {
            masm.store32(to.reg(), cycleSlot());
        }
        break;
#endif
      case MoveOp::GENERAL:
        masm.Push(toOperand(to));
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterX86::completeCycle(const MoveOperand& to, MoveOp::Type type)
{
    // There is some pattern:
    //   (A -> B)
    //   (B -> A)
    //
    // This case handles (B -> A), which we reach last. We emit a move from the
    // saved value of B, to A.
    switch (type) {
      case MoveOp::SIMD128INT:
        MOZ_ASSERT(pushedAtCycle_ != -1);
        MOZ_ASSERT(pushedAtCycle_ - pushedAtStart_ >= Simd128DataSize);
        if (to.isMemory()) {
            ScratchSimd128Scope scratch(masm);
            masm.loadAlignedSimd128Int(cycleSlot(), scratch);
            masm.storeAlignedSimd128Int(scratch, toAddress(to));
        } else {
            masm.loadAlignedSimd128Int(cycleSlot(), to.floatReg());
        }
        break;
      case MoveOp::SIMD128FLOAT:
        MOZ_ASSERT(pushedAtCycle_ != -1);
        MOZ_ASSERT(pushedAtCycle_ - pushedAtStart_ >= Simd128DataSize);
        if (to.isMemory()) {
            ScratchSimd128Scope scratch(masm);
            masm.loadAlignedSimd128Float(cycleSlot(), scratch);
            masm.storeAlignedSimd128Float(scratch, toAddress(to));
        } else {
            masm.loadAlignedSimd128Float(cycleSlot(), to.floatReg());
        }
        break;
      case MoveOp::FLOAT32:
        MOZ_ASSERT(pushedAtCycle_ != -1);
        MOZ_ASSERT(pushedAtCycle_ - pushedAtStart_ >= sizeof(float));
        if (to.isMemory()) {
            ScratchFloat32Scope scratch(masm);
            masm.loadFloat32(cycleSlot(), scratch);
            masm.storeFloat32(scratch, toAddress(to));
        } else {
            masm.loadFloat32(cycleSlot(), to.floatReg());
        }
        break;
      case MoveOp::DOUBLE:
        MOZ_ASSERT(pushedAtCycle_ != -1);
        MOZ_ASSERT(pushedAtCycle_ - pushedAtStart_ >= sizeof(double));
        if (to.isMemory()) {
            ScratchDoubleScope scratch(masm);
            masm.loadDouble(cycleSlot(), scratch);
            masm.storeDouble(scratch, toAddress(to));
        } else {
            masm.loadDouble(cycleSlot(), to.floatReg());
        }
        break;
      case MoveOp::INT32:
#ifdef JS_CODEGEN_X64
        MOZ_ASSERT(pushedAtCycle_ != -1);
        MOZ_ASSERT(pushedAtCycle_ - pushedAtStart_ >= sizeof(int32_t));
        // x64 can't pop to a 32-bit destination.
        if (to.isMemory()) {
            masm.load32(cycleSlot(), ScratchReg);
            masm.store32(ScratchReg, toAddress(to));
        } else {
            masm.load32(cycleSlot(), to.reg());
        }
        break;
#endif
      case MoveOp::GENERAL:
        MOZ_ASSERT(masm.framePushed() - pushedAtStart_ >= sizeof(intptr_t));
        masm.Pop(toPopOperand(to));
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterX86::emitInt32Move(const MoveOperand& from, const MoveOperand& to,
                              const MoveResolver& moves, size_t i)
{
    if (from.isGeneralReg()) {
        masm.move32(from.reg(), toOperand(to));
    } else if (to.isGeneralReg()) {
        MOZ_ASSERT(from.isMemory());
        masm.load32(toAddress(from), to.reg());
    } else {
        // Memory to memory gpr move.
        MOZ_ASSERT(from.isMemory());
        Maybe<Register> reg = findScratchRegister(moves, i);
        if (reg.isSome()) {
            masm.load32(toAddress(from), reg.value());
            masm.move32(reg.value(), toOperand(to));
        } else {
            // No scratch register available; bounce it off the stack.
            masm.Push(toOperand(from));
            masm.Pop(toPopOperand(to));
        }
    }
}

void
MoveEmitterX86::emitGeneralMove(const MoveOperand& from, const MoveOperand& to,
                                const MoveResolver& moves, size_t i)
{
    if (from.isGeneralReg()) {
        masm.mov(from.reg(), toOperand(to));
    } else if (to.isGeneralReg()) {
        MOZ_ASSERT(from.isMemoryOrEffectiveAddress());
        if (from.isMemory())
            masm.loadPtr(toAddress(from), to.reg());
        else
            masm.lea(toOperand(from), to.reg());
    } else if (from.isMemory()) {
        // Memory to memory gpr move.
        Maybe<Register> reg = findScratchRegister(moves, i);
        if (reg.isSome()) {
            masm.loadPtr(toAddress(from), reg.value());
            masm.mov(reg.value(), toOperand(to));
        } else {
            // No scratch register available; bounce it off the stack.
            masm.Push(toOperand(from));
            masm.Pop(toPopOperand(to));
        }
    } else {
        // Effective address to memory move.
        MOZ_ASSERT(from.isEffectiveAddress());
        Maybe<Register> reg = findScratchRegister(moves, i);
        if (reg.isSome()) {
            masm.lea(toOperand(from), reg.value());
            masm.mov(reg.value(), toOperand(to));
        } else {
            // This is tricky without a scratch reg. We can't do an lea. Bounce the
            // base register off the stack, then add the offset in place. Note that
            // this clobbers FLAGS!
            masm.Push(from.base());
            masm.Pop(toPopOperand(to));
            MOZ_ASSERT(to.isMemoryOrEffectiveAddress());
            masm.addPtr(Imm32(from.disp()), toAddress(to));
        }
    }
}

void
MoveEmitterX86::emitFloat32Move(const MoveOperand& from, const MoveOperand& to)
{
    MOZ_ASSERT_IF(from.isFloatReg(), from.floatReg().isSingle());
    MOZ_ASSERT_IF(to.isFloatReg(), to.floatReg().isSingle());

    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.moveFloat32(from.floatReg(), to.floatReg());
        else
            masm.storeFloat32(from.floatReg(), toAddress(to));
    } else if (to.isFloatReg()) {
        masm.loadFloat32(toAddress(from), to.floatReg());
    } else {
        // Memory to memory move.
        MOZ_ASSERT(from.isMemory());
        ScratchFloat32Scope scratch(masm);
        masm.loadFloat32(toAddress(from), scratch);
        masm.storeFloat32(scratch, toAddress(to));
    }
}

void
MoveEmitterX86::emitDoubleMove(const MoveOperand& from, const MoveOperand& to)
{
    MOZ_ASSERT_IF(from.isFloatReg(), from.floatReg().isDouble());
    MOZ_ASSERT_IF(to.isFloatReg(), to.floatReg().isDouble());

    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.moveDouble(from.floatReg(), to.floatReg());
        else
            masm.storeDouble(from.floatReg(), toAddress(to));
    } else if (to.isFloatReg()) {
        masm.loadDouble(toAddress(from), to.floatReg());
    } else {
        // Memory to memory move.
        MOZ_ASSERT(from.isMemory());
        ScratchDoubleScope scratch(masm);
        masm.loadDouble(toAddress(from), scratch);
        masm.storeDouble(scratch, toAddress(to));
    }
}

void
MoveEmitterX86::emitSimd128IntMove(const MoveOperand& from, const MoveOperand& to)
{
    MOZ_ASSERT_IF(from.isFloatReg(), from.floatReg().isSimd128());
    MOZ_ASSERT_IF(to.isFloatReg(), to.floatReg().isSimd128());

    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.moveSimd128Int(from.floatReg(), to.floatReg());
        else
            masm.storeAlignedSimd128Int(from.floatReg(), toAddress(to));
    } else if (to.isFloatReg()) {
        masm.loadAlignedSimd128Int(toAddress(from), to.floatReg());
    } else {
        // Memory to memory move.
        MOZ_ASSERT(from.isMemory());
        ScratchSimd128Scope scratch(masm);
        masm.loadAlignedSimd128Int(toAddress(from), scratch);
        masm.storeAlignedSimd128Int(scratch, toAddress(to));
    }
}

void
MoveEmitterX86::emitSimd128FloatMove(const MoveOperand& from, const MoveOperand& to)
{
    MOZ_ASSERT_IF(from.isFloatReg(), from.floatReg().isSimd128());
    MOZ_ASSERT_IF(to.isFloatReg(), to.floatReg().isSimd128());

    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.moveSimd128Float(from.floatReg(), to.floatReg());
        else
            masm.storeAlignedSimd128Float(from.floatReg(), toAddress(to));
    } else if (to.isFloatReg()) {
        masm.loadAlignedSimd128Float(toAddress(from), to.floatReg());
    } else {
        // Memory to memory move.
        MOZ_ASSERT(from.isMemory());
        ScratchSimd128Scope scratch(masm);
        masm.loadAlignedSimd128Float(toAddress(from), scratch);
        masm.storeAlignedSimd128Float(scratch, toAddress(to));
    }
}

void
MoveEmitterX86::assertDone()
{
    MOZ_ASSERT(!inCycle_);
}

void
MoveEmitterX86::finish()
{
    assertDone();

    masm.freeStack(masm.framePushed() - pushedAtStart_);
}

Maybe<Register>
MoveEmitterX86::findScratchRegister(const MoveResolver& moves, size_t initial)
{
#ifdef JS_CODEGEN_X86
    if (scratchRegister_.isSome())
        return scratchRegister_;

    // All registers are either in use by this move group or are live
    // afterwards. Look through the remaining moves for a register which is
    // clobbered before it is used, and is thus dead at this point.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    for (size_t i = initial; i < moves.numMoves(); i++) {
        const MoveOp& move = moves.getMove(i);
        if (move.from().isGeneralReg())
            regs.takeUnchecked(move.from().reg());
        else if (move.from().isMemoryOrEffectiveAddress())
            regs.takeUnchecked(move.from().base());
        if (move.to().isGeneralReg()) {
            if (i != initial && !move.isCycleBegin() && regs.has(move.to().reg()))
                return mozilla::Some(move.to().reg());
            regs.takeUnchecked(move.to().reg());
        } else if (move.to().isMemoryOrEffectiveAddress()) {
            regs.takeUnchecked(move.to().base());
        }
    }

    return mozilla::Nothing();
#else
    return mozilla::Some(ScratchReg);
#endif
}

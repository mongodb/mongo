/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MoveResolver.h"

#include "jit/MacroAssembler.h"
#include "jit/RegisterSets.h"

using namespace js;
using namespace js::jit;

MoveOperand::MoveOperand(MacroAssembler& masm, const ABIArg& arg)
{
    switch (arg.kind()) {
      case ABIArg::GPR:
        kind_ = REG;
        code_ = arg.gpr().code();
        break;
#ifdef JS_CODEGEN_REGISTER_PAIR
      case ABIArg::GPR_PAIR:
        kind_ = REG_PAIR;
        code_ = arg.evenGpr().code();
        MOZ_ASSERT(code_ % 2 == 0);
        MOZ_ASSERT(code_ + 1 == arg.oddGpr().code());
        break;
#endif
      case ABIArg::FPU:
        kind_ = FLOAT_REG;
        code_ = arg.fpu().code();
        break;
      case ABIArg::Stack:
        kind_ = MEMORY;
        code_ = masm.getStackPointer().code();
        disp_ = arg.offsetFromArgBase();
        break;
    }
}

MoveResolver::MoveResolver()
  : numCycles_(0), curCycles_(0)
{
}

void
MoveResolver::resetState()
{
    numCycles_ = 0;
    curCycles_ = 0;
}

bool
MoveResolver::addMove(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type)
{
    // Assert that we're not doing no-op moves.
    MOZ_ASSERT(!(from == to));
    PendingMove* pm = movePool_.allocate();
    if (!pm)
        return false;
    new (pm) PendingMove(from, to, type);
    pending_.pushBack(pm);
    return true;
}

// Given move (A -> B), this function attempts to find any move (B -> *) in the
// pending move list, and returns the first one.
MoveResolver::PendingMove*
MoveResolver::findBlockingMove(const PendingMove* last)
{
    for (PendingMoveIterator iter = pending_.begin(); iter != pending_.end(); iter++) {
        PendingMove* other = *iter;

        if (other->from().aliases(last->to())) {
            // We now have pairs in the form (A -> X) (X -> y). The second pair
            // blocks the move in the first pair, so return it.
            return other;
        }
    }

    // No blocking moves found.
    return nullptr;
}

// Given move (A -> B), this function attempts to find any move (B -> *) in the
// move list iterator, and returns the first one.
// N.B. It is unclear if a single move can complete more than one cycle, so to be
// conservative, this function operates on iterators, so the caller can process all
// instructions that start a cycle.
MoveResolver::PendingMove*
MoveResolver::findCycledMove(PendingMoveIterator* iter, PendingMoveIterator end, const PendingMove* last)
{
    for (; *iter != end; (*iter)++) {
        PendingMove* other = **iter;
        if (other->from().aliases(last->to())) {
            // We now have pairs in the form (A -> X) (X -> y). The second pair
            // blocks the move in the first pair, so return it.
            (*iter)++;
            return other;
        }
    }
    // No blocking moves found.
    return nullptr;
}

bool
MoveResolver::resolve()
{
    resetState();
    orderedMoves_.clear();

    InlineList<PendingMove> stack;

    // This is a depth-first-search without recursion, which tries to find
    // cycles in a list of moves.
    //
    // Algorithm.
    //
    // S = Traversal stack.
    // P = Pending move list.
    // O = Ordered list of moves.
    //
    // As long as there are pending moves in P:
    //      Let |root| be any pending move removed from P
    //      Add |root| to the traversal stack.
    //      As long as S is not empty:
    //          Let |L| be the most recent move added to S.
    //
    //          Find any pending move M whose source is L's destination, thus
    //          preventing L's move until M has completed.
    //
    //          If a move M was found,
    //              Remove M from the pending list.
    //              If M's destination is |root|,
    //                  Annotate M and |root| as cycles.
    //                  Add M to S.
    //                  do not Add M to O, since M may have other conflictors in P that have not yet been processed.
    //              Otherwise,
    //                  Add M to S.
    //         Otherwise,
    //              Remove L from S.
    //              Add L to O.
    //
    while (!pending_.empty()) {
        PendingMove* pm = pending_.popBack();

        // Add this pending move to the cycle detection stack.
        stack.pushBack(pm);

        while (!stack.empty()) {
            PendingMove* blocking = findBlockingMove(stack.peekBack());

            if (blocking) {
                PendingMoveIterator stackiter = stack.begin();
                PendingMove* cycled = findCycledMove(&stackiter, stack.end(), blocking);
                if (cycled) {
                    // Find the cycle's start.
                    // We annotate cycles at each move in the cycle, and
                    // assert that we do not find two cycles in one move chain
                    // traversal (which would indicate two moves to the same
                    // destination).
                    // Since there can be more than one cycle, find them all.
                    do {
                        cycled->setCycleEnd(curCycles_);
                        cycled = findCycledMove(&stackiter, stack.end(), blocking);
                    } while (cycled);

                    blocking->setCycleBegin(pm->type(), curCycles_);
                    curCycles_++;
                    pending_.remove(blocking);
                    stack.pushBack(blocking);
                } else {
                    // This is a new link in the move chain, so keep
                    // searching for a cycle.
                    pending_.remove(blocking);
                    stack.pushBack(blocking);
                }
            } else {
                // Otherwise, pop the last move on the search stack because it's
                // complete and not participating in a cycle. The resulting
                // move can safely be added to the ordered move list.
                PendingMove* done = stack.popBack();
                if (!addOrderedMove(*done))
                    return false;
                movePool_.free(done);
            }
        }
        // If the current queue is empty, it is certain that there are
        // all previous cycles cannot conflict with future cycles,
        // so re-set the counter of pending cycles, while keeping a high-water mark.
        if (numCycles_ < curCycles_)
            numCycles_ = curCycles_;
        curCycles_ = 0;
    }

    return true;
}

bool
MoveResolver::addOrderedMove(const MoveOp& move)
{
    // Sometimes the register allocator generates move groups where multiple
    // moves have the same source. Try to optimize these cases when the source
    // is in memory and the target of one of the moves is in a register.
    MOZ_ASSERT(!move.from().aliases(move.to()));

    if (!move.from().isMemory() || move.isCycleBegin() || move.isCycleEnd())
        return orderedMoves_.append(move);

    // Look for an earlier move with the same source, where no intervening move
    // touches either the source or destination of the new move.
    for (int i = orderedMoves_.length() - 1; i >= 0; i--) {
        const MoveOp& existing = orderedMoves_[i];

        if (existing.from() == move.from() &&
            !existing.to().aliases(move.to()) &&
            existing.type() == move.type() &&
            !existing.isCycleBegin() &&
            !existing.isCycleEnd())
        {
            MoveOp* after = orderedMoves_.begin() + i + 1;
            if (existing.to().isGeneralReg() || existing.to().isFloatReg()) {
                MoveOp nmove(existing.to(), move.to(), move.type());
                return orderedMoves_.insert(after, nmove);
            } else if (move.to().isGeneralReg() || move.to().isFloatReg()) {
                MoveOp nmove(move.to(), existing.to(), move.type());
                orderedMoves_[i] = move;
                return orderedMoves_.insert(after, nmove);
            }
        }

        if (existing.aliases(move))
            break;
    }

    return orderedMoves_.append(move);
}

void
MoveResolver::reorderMove(size_t from, size_t to)
{
    MOZ_ASSERT(from != to);

    MoveOp op = orderedMoves_[from];
    if (from < to) {
        for (size_t i = from; i < to; i++)
            orderedMoves_[i] = orderedMoves_[i + 1];
    } else {
        for (size_t i = from; i > to; i--)
            orderedMoves_[i] = orderedMoves_[i - 1];
    }
    orderedMoves_[to] = op;
}

void
MoveResolver::sortMemoryToMemoryMoves()
{
    // Try to reorder memory->memory moves so that they are executed right
    // before a move that clobbers some register. This will allow the move
    // emitter to use that clobbered register as a scratch register for the
    // memory->memory move, if necessary.
    for (size_t i = 0; i < orderedMoves_.length(); i++) {
        const MoveOp& base = orderedMoves_[i];
        if (!base.from().isMemory() || !base.to().isMemory())
            continue;
        if (base.type() != MoveOp::GENERAL && base.type() != MoveOp::INT32)
            continue;

        // Look for an earlier move clobbering a register.
        bool found = false;
        for (int j = i - 1; j >= 0; j--) {
            const MoveOp& previous = orderedMoves_[j];
            if (previous.aliases(base) || previous.isCycleBegin() || previous.isCycleEnd())
                break;

            if (previous.to().isGeneralReg()) {
                reorderMove(i, j);
                found = true;
                break;
            }
        }
        if (found)
            continue;

        // Look for a later move clobbering a register.
        if (i + 1 < orderedMoves_.length()) {
            bool found = false, skippedRegisterUse = false;
            for (size_t j = i + 1; j < orderedMoves_.length(); j++) {
                const MoveOp& later = orderedMoves_[j];
                if (later.aliases(base) || later.isCycleBegin() || later.isCycleEnd())
                    break;

                if (later.to().isGeneralReg()) {
                    if (skippedRegisterUse) {
                        reorderMove(i, j);
                        found = true;
                    } else {
                        // There is no move that uses a register between the
                        // original memory->memory move and this move that
                        // clobbers a register. The move should already be able
                        // to use a scratch register, so don't shift anything
                        // around.
                    }
                    break;
                }

                if (later.from().isGeneralReg())
                    skippedRegisterUse = true;
            }

            if (found) {
                // Redo the search for memory->memory moves at the current
                // index, so we don't skip the move just shifted back.
                i--;
            }
        }
    }
}

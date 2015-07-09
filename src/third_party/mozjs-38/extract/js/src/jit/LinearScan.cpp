/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/LinearScan.h"

#include "mozilla/DebugOnly.h"

#include "jit/BitSet.h"
#include "jit/JitSpewer.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

/*
 * Merge virtual register intervals into the UnhandledQueue, taking advantage
 * of their nearly-sorted ordering.
 */
void
LinearScanAllocator::enqueueVirtualRegisterIntervals()
{
    // Cursor into the unhandled queue, iterating through start positions.
    IntervalReverseIterator curr = unhandled.rbegin();

    // Start position is non-monotonically increasing by virtual register number.
    for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
        LiveInterval* live = vregs[i].getInterval(0);
        if (live->numRanges() > 0) {
            setIntervalRequirement(live);

            // Iterate backward until the next highest class of start position.
            for (; curr != unhandled.rend(); curr++) {
                if (curr->start() > live->start())
                    break;
            }

            // Insert forward from the current position, thereby
            // sorting by priority within the start class.
            unhandled.enqueueForward(*curr, live);
        }
    }
}

/*
 * This function performs a preliminary allocation on the already-computed
 * live intervals, storing the result in the allocation field of the intervals
 * themselves.
 *
 * The algorithm is based on the one published in:
 *
 * Wimmer, Christian, and Hanspeter Mössenböck. "Optimized Interval Splitting
 *     in a Linear Scan Register Allocator." Proceedings of the First
 *     ACM/USENIX Conference on Virtual Execution Environments. Chicago, IL,
 *     USA, ACM. 2005. PDF.
 *
 * The algorithm proceeds over each interval in order of their start position.
 * If a free register is available, the register that is free for the largest
 * portion of the current interval is allocated. Otherwise, the interval
 * with the farthest-away next use is spilled to make room for this one. In all
 * cases, intervals which conflict either with other intervals or with
 * use or definition constraints are split at the point of conflict to be
 * assigned a compatible allocation later.
 */
bool
LinearScanAllocator::allocateRegisters()
{
    // The unhandled queue currently contains only spill intervals, in sorted
    // order. Intervals for virtual registers exist in sorted order based on
    // start position by vreg ID, but may have priorities that require them to
    // be reordered when adding to the unhandled queue.
    enqueueVirtualRegisterIntervals();
    unhandled.assertSorted();

    // Add fixed intervals with ranges to fixed.
    for (size_t i = 0; i < AnyRegister::Total; i++) {
        if (fixedIntervals[i]->numRanges() > 0)
            fixed.pushBack(fixedIntervals[i]);
    }

    // Iterate through all intervals in ascending start order.
    CodePosition prevPosition = CodePosition::MIN;
    while ((current = unhandled.dequeue()) != nullptr) {
        MOZ_ASSERT(current->getAllocation()->isBogus());
        MOZ_ASSERT(current->numRanges() > 0);

        if (mir->shouldCancel("LSRA Allocate Registers (main loop)"))
            return false;

        CodePosition position = current->start();
        const Requirement* req = current->requirement();
        const Requirement* hint = current->hint();

        JitSpew(JitSpew_RegAlloc, "Processing %d = [%u, %u] (pri=%d)",
                current->hasVreg() ? current->vreg() : 0, current->start().bits(),
                current->end().bits(), current->requirement()->priority());

        // Shift active intervals to the inactive or handled sets as appropriate
        if (position != prevPosition) {
            MOZ_ASSERT(position > prevPosition);
            prevPosition = position;

            for (IntervalIterator i(active.begin()); i != active.end(); ) {
                LiveInterval* it = *i++;
                MOZ_ASSERT(it->numRanges() > 0);

                if (it->end() <= position) {
                    active.remove(it);
                    finishInterval(it);
                } else if (!it->covers(position)) {
                    active.remove(it);
                    inactive.pushBack(it);
                }
            }

            // Shift inactive intervals to the active or handled sets as appropriate
            for (IntervalIterator i(inactive.begin()); i != inactive.end(); ) {
                LiveInterval* it = *i++;
                MOZ_ASSERT(it->numRanges() > 0);

                if (it->end() <= position) {
                    inactive.remove(it);
                    finishInterval(it);
                } else if (it->covers(position)) {
                    inactive.remove(it);
                    active.pushBack(it);
                }
            }
        }

        // Sanity check all intervals in all sets
        validateIntervals();

        // If the interval has a hard requirement, grant it.
        if (req->kind() == Requirement::FIXED) {
            MOZ_ASSERT(!req->allocation().isRegister());
            if (!assign(req->allocation()))
                return false;
            continue;
        }

        // If we don't really need this in a register, don't allocate one
        if (req->kind() != Requirement::REGISTER && hint->kind() == Requirement::NONE) {
            JitSpew(JitSpew_RegAlloc, "  Eagerly spilling virtual register %d",
                    current->hasVreg() ? current->vreg() : 0);
            if (!spill())
                return false;
            continue;
        }

        // Try to allocate a free register
        JitSpew(JitSpew_RegAlloc, " Attempting free register allocation");
        CodePosition bestFreeUntil;
        AnyRegister::Code bestCode = findBestFreeRegister(&bestFreeUntil);
        if (bestCode != AnyRegister::Invalid) {
            AnyRegister best = AnyRegister::FromCode(bestCode);
            JitSpew(JitSpew_RegAlloc, "  Decided best register was %s", best.name());

            // Split when the register is next needed if necessary
            if (bestFreeUntil < current->end()) {
                if (!splitInterval(current, bestFreeUntil))
                    return false;
            }
            if (!assign(LAllocation(best)))
                return false;

            continue;
        }

        JitSpew(JitSpew_RegAlloc, " Attempting blocked register allocation");

        // If we absolutely need a register or our next use is closer than the
        // selected blocking register then we spill the blocker. Otherwise, we
        // spill the current interval.
        CodePosition bestNextUsed;
        bestCode = findBestBlockedRegister(&bestNextUsed);
        if (bestCode != AnyRegister::Invalid &&
            (req->kind() == Requirement::REGISTER || hint->pos() < bestNextUsed))
        {
            AnyRegister best = AnyRegister::FromCode(bestCode);
            JitSpew(JitSpew_RegAlloc, "  Decided best register was %s", best.name());

            if (!splitBlockingIntervals(best))
                return false;
            if (!assign(LAllocation(best)))
                return false;

            continue;
        }

        JitSpew(JitSpew_RegAlloc, "  No registers available to spill");
        MOZ_ASSERT(req->kind() == Requirement::NONE);

        if (!spill())
            return false;
    }

    validateAllocations();
    validateVirtualRegisters();

    return true;
}

/*
 * This function iterates over control flow edges in the function and resolves
 * conflicts wherein two predecessors of a block have different allocations
 * for a virtual register than the block itself. It also turns phis into moves.
 *
 * The algorithm is based on the one published in "Linear Scan Register
 * Allocation on SSA Form" by C. Wimmer et al., for which the full citation
 * appears in LiveRangeAllocator.cpp.
 */
bool
LinearScanAllocator::resolveControlFlow()
{
    for (size_t i = 0; i < graph.numBlocks(); i++) {
        if (mir->shouldCancel("LSRA Resolve Control Flow (main loop)"))
            return false;

        LBlock* successor = graph.getBlock(i);
        MBasicBlock* mSuccessor = successor->mir();
        if (mSuccessor->numPredecessors() < 1)
            continue;

        // Resolve phis to moves
        for (size_t j = 0; j < successor->numPhis(); j++) {
            LPhi* phi = successor->getPhi(j);
            MOZ_ASSERT(phi->numDefs() == 1);
            LDefinition* def = phi->getDef(0);
            LinearScanVirtualRegister* vreg = &vregs[def];
            LiveInterval* to = vreg->intervalFor(entryOf(successor));
            MOZ_ASSERT(to);

            for (size_t k = 0; k < mSuccessor->numPredecessors(); k++) {
                LBlock* predecessor = mSuccessor->getPredecessor(k)->lir();
                MOZ_ASSERT(predecessor->mir()->numSuccessors() == 1);

                LAllocation* input = phi->getOperand(k);
                LiveInterval* from = vregs[input].intervalFor(exitOf(predecessor));
                MOZ_ASSERT(from);

                if (!moveAtExit(predecessor, from, to, def->type()))
                    return false;
            }

            if (vreg->mustSpillAtDefinition() && !to->isSpill()) {
                // Make sure this phi is spilled at the loop header.
                LMoveGroup* moves = successor->getEntryMoveGroup(alloc());
                if (!moves->add(to->getAllocation(), vregs[to->vreg()].canonicalSpill(),
                                def->type()))
                    return false;
            }
        }

        // Resolve split intervals with moves
        BitSet& live = liveIn[mSuccessor->id()];

        for (BitSet::Iterator liveRegId(live); liveRegId; ++liveRegId) {
            LinearScanVirtualRegister* vreg = &vregs[*liveRegId];
            LiveInterval* to = vreg->intervalFor(entryOf(successor));
            MOZ_ASSERT(to);

            for (size_t j = 0; j < mSuccessor->numPredecessors(); j++) {
                LBlock* predecessor = mSuccessor->getPredecessor(j)->lir();
                LiveInterval* from = vregs[*liveRegId].intervalFor(exitOf(predecessor));
                MOZ_ASSERT(from);

                if (*from->getAllocation() == *to->getAllocation())
                    continue;

                // If this value is spilled at its definition, other stores
                // are redundant.
                if (vreg->mustSpillAtDefinition() && to->getAllocation()->isStackSlot()) {
                    MOZ_ASSERT(vreg->canonicalSpill());
                    MOZ_ASSERT(*vreg->canonicalSpill() == *to->getAllocation());
                    continue;
                }

                if (mSuccessor->numPredecessors() > 1) {
                    MOZ_ASSERT(predecessor->mir()->numSuccessors() == 1);
                    if (!moveAtExit(predecessor, from, to, vreg->type()))
                        return false;
                } else {
                    if (!moveAtEntry(successor, from, to, vreg->type()))
                        return false;
                }
            }
        }
    }

    return true;
}

bool
LinearScanAllocator::moveInputAlloc(LInstruction* ins, LAllocation* from, LAllocation* to,
                                    LDefinition::Type type)
{
    if (*from == *to)
        return true;
    LMoveGroup* moves = getInputMoveGroup(ins);
    return moves->add(from, to, type);
}

static inline void
SetOsiPointUses(LiveInterval* interval, CodePosition defEnd, const LAllocation& allocation)
{
    // Moves are inserted after OsiPoint instructions. This function sets
    // any OsiPoint uses of this interval to the allocation of the value
    // before the move.

    MOZ_ASSERT(interval->index() == 0);

    for (UsePositionIterator usePos(interval->usesBegin());
         usePos != interval->usesEnd();
         usePos++)
    {
        if (usePos->pos > defEnd)
            break;
        *static_cast<LAllocation*>(usePos->use) = allocation;
    }
}

/*
 * This function takes the allocations assigned to the live intervals and
 * erases all virtual registers in the function with the allocations
 * corresponding to the appropriate interval.
 */
bool
LinearScanAllocator::reifyAllocations()
{
    // Iterate over each interval, ensuring that definitions are visited before uses.
    for (size_t j = 1; j < graph.numVirtualRegisters(); j++) {
        LinearScanVirtualRegister* reg = &vregs[j];
        if (mir->shouldCancel("LSRA Reification (main loop)"))
            return false;

    for (size_t k = 0; k < reg->numIntervals(); k++) {
        LiveInterval* interval = reg->getInterval(k);
        MOZ_ASSERT(reg == &vregs[interval->vreg()]);
        if (!interval->numRanges())
            continue;

        UsePositionIterator usePos(interval->usesBegin());
        for (; usePos != interval->usesEnd(); usePos++) {
            if (usePos->use->isFixedRegister()) {
                LiveInterval* to = fixedIntervals[GetFixedRegister(reg->def(), usePos->use).code()];

                *static_cast<LAllocation*>(usePos->use) = *to->getAllocation();
                if (!moveInput(insData[usePos->pos]->toInstruction(), interval, to, reg->type()))
                    return false;
            } else {
                MOZ_ASSERT(UseCompatibleWith(usePos->use, *interval->getAllocation()));
                *static_cast<LAllocation*>(usePos->use) = *interval->getAllocation();
            }
        }

        // Erase the def of this interval if it's the first one
        if (interval->index() == 0)
        {
            LDefinition* def = reg->def();
            LAllocation* spillFrom;

            // Insert the moves after any OsiPoint or Nop instructions
            // following this one. See minimalDefEnd for more info.
            CodePosition defEnd = minimalDefEnd(reg->ins());

            if (def->policy() == LDefinition::FIXED && def->output()->isRegister()) {
                AnyRegister fixedReg = def->output()->toRegister();
                LiveInterval* from = fixedIntervals[fixedReg.code()];

                // If we insert the move after an OsiPoint that uses this vreg,
                // it should use the fixed register instead.
                SetOsiPointUses(interval, defEnd, LAllocation(fixedReg));

                if (!moveAfter(insData[defEnd]->toInstruction(), from, interval, def->type()))
                    return false;
                spillFrom = from->getAllocation();
            } else {
                if (def->policy() == LDefinition::MUST_REUSE_INPUT) {
                    LAllocation* inputAlloc = reg->ins()->getOperand(def->getReusedInput());
                    LAllocation* origAlloc = LAllocation::New(alloc(), *inputAlloc);

                    MOZ_ASSERT(!inputAlloc->isUse());

                    *inputAlloc = *interval->getAllocation();
                    if (!moveInputAlloc(reg->ins()->toInstruction(), origAlloc, inputAlloc, def->type()))
                        return false;
                }

                MOZ_ASSERT(DefinitionCompatibleWith(reg->ins(), def, *interval->getAllocation()));
                def->setOutput(*interval->getAllocation());

                spillFrom = interval->getAllocation();
            }

            if (reg->ins()->recoversInput()) {
                LSnapshot* snapshot = reg->ins()->toInstruction()->snapshot();
                for (size_t i = 0; i < snapshot->numEntries(); i++) {
                    LAllocation* entry = snapshot->getEntry(i);
                    if (entry->isUse() && entry->toUse()->policy() == LUse::RECOVERED_INPUT)
                        *entry = *def->output();
                }
            }

            if (reg->mustSpillAtDefinition() && !reg->ins()->isPhi() &&
                (*reg->canonicalSpill() != *spillFrom))
            {
                // If we move the spill after an OsiPoint, the OsiPoint should
                // use the original location instead.
                SetOsiPointUses(interval, defEnd, *spillFrom);

                // Insert a spill after this instruction (or after any OsiPoint
                // or Nop instructions). Note that we explicitly ignore phis,
                // which should have been handled in resolveControlFlow().
                LMoveGroup* moves = getMoveGroupAfter(insData[defEnd]->toInstruction());
                if (!moves->add(spillFrom, reg->canonicalSpill(), def->type()))
                    return false;
            }
        }
        else if (interval->start() > entryOf(insData[interval->start()]->block()) &&
                 (!reg->canonicalSpill() ||
                  (reg->canonicalSpill() == interval->getAllocation() &&
                   !reg->mustSpillAtDefinition()) ||
                  *reg->canonicalSpill() != *interval->getAllocation()))
        {
            // If this virtual register has no canonical spill location, this
            // is the first spill to that location, or this is a move to somewhere
            // completely different, we have to emit a move for this interval.
            // Don't do this if the interval starts at the first instruction of the
            // block; this case should have been handled by resolveControlFlow().
            //
            // If the interval starts at the output half of an instruction, we have to
            // emit the move *after* this instruction, to prevent clobbering an input
            // register.
            LiveInterval* prevInterval = reg->getInterval(interval->index() - 1);
            CodePosition start = interval->start();
            LInstruction* ins = insData[start]->toInstruction();

            MOZ_ASSERT(start == inputOf(ins) || start == outputOf(ins));

            if (start.subpos() == CodePosition::INPUT) {
                if (!moveInput(ins, prevInterval, interval, reg->type()))
                    return false;
            } else {
                if (!moveAfter(ins, prevInterval, interval, reg->type()))
                    return false;
            }

            // Mark this interval's spill position, if needed.
            if (reg->canonicalSpill() == interval->getAllocation() &&
                !reg->mustSpillAtDefinition())
            {
                reg->setSpillPosition(interval->start());
            }
        }

        addLiveRegistersForInterval(reg, interval);
    }} // Iteration over virtual register intervals.

    // Set the graph overall stack height
    graph.setLocalSlotCount(stackSlotAllocator.stackHeight());

    return true;
}

inline bool
LinearScanAllocator::isSpilledAt(LiveInterval* interval, CodePosition pos)
{
    LinearScanVirtualRegister* reg = &vregs[interval->vreg()];
    if (!reg->canonicalSpill() || !reg->canonicalSpill()->isStackSlot())
        return false;

    if (reg->mustSpillAtDefinition()) {
        MOZ_ASSERT(reg->spillPosition() <= pos);
        return true;
    }

    return interval->getAllocation() == reg->canonicalSpill();
}

bool
LinearScanAllocator::populateSafepoints()
{
    // Populate all safepoints with argument slots. These are never changed
    // by the allocator and are not necessarily populated by the code below.
    size_t nargs = graph.getBlock(0)->mir()->info().nargs();
    for (size_t i = 0; i < graph.numSafepoints(); i++) {
        LSafepoint* safepoint = graph.getSafepoint(i)->safepoint();
        for (size_t j = 0; j < nargs; j++) {
            if (!safepoint->addValueSlot(/* stack = */ false, (j + 1) * sizeof(Value)))
                return false;
        }
    }

    size_t firstSafepoint = 0;

    for (uint32_t i = 0; i < vregs.numVirtualRegisters(); i++) {
        LinearScanVirtualRegister* reg = &vregs[i];

        if (!reg->def() || (!IsTraceable(reg) && !IsSlotsOrElements(reg) && !IsNunbox(reg)))
            continue;

        firstSafepoint = findFirstSafepoint(reg->getInterval(0), firstSafepoint);
        if (firstSafepoint >= graph.numSafepoints())
            break;

        // Find the furthest endpoint.
        size_t lastInterval = reg->numIntervals() - 1;
        CodePosition end = reg->getInterval(lastInterval)->end();

        for (size_t j = firstSafepoint; j < graph.numSafepoints(); j++) {
            LInstruction* ins = graph.getSafepoint(j);

            // Stop processing safepoints if we know we're out of this virtual
            // register's range.
            if (end < inputOf(ins))
                break;

            // Include temps but not instruction outputs. Also make sure MUST_REUSE_INPUT
            // is not used with gcthings or nunboxes, or we would have to add the input reg
            // to this safepoint.
            if (ins == reg->ins() && !reg->isTemp()) {
                DebugOnly<LDefinition*> def = reg->def();
                MOZ_ASSERT_IF(def->policy() == LDefinition::MUST_REUSE_INPUT,
                              def->type() == LDefinition::GENERAL ||
                              def->type() == LDefinition::INT32 ||
                              def->type() == LDefinition::FLOAT32 ||
                              def->type() == LDefinition::DOUBLE);
                continue;
            }

            LSafepoint* safepoint = ins->safepoint();

            if (IsSlotsOrElements(reg)) {
                LiveInterval* interval = reg->intervalFor(inputOf(ins));
                if (!interval)
                    continue;

                LAllocation* a = interval->getAllocation();
                if (a->isGeneralReg() && !ins->isCall())
                    safepoint->addSlotsOrElementsRegister(a->toGeneralReg()->reg());

                if (isSpilledAt(interval, inputOf(ins))) {
                    if (!safepoint->addSlotsOrElementsSlot(true, reg->canonicalSpillSlot()))
                        return false;
                }
            } else if (!IsNunbox(reg)) {
                MOZ_ASSERT(IsTraceable(reg));

                LiveInterval* interval = reg->intervalFor(inputOf(ins));
                if (!interval)
                    continue;

                LAllocation* a = interval->getAllocation();
                if (a->isGeneralReg() && !ins->isCall()) {
#ifdef JS_PUNBOX64
                    if (reg->type() == LDefinition::BOX) {
                        safepoint->addValueRegister(a->toGeneralReg()->reg());
                    } else
#endif
                    {
                        safepoint->addGcRegister(a->toGeneralReg()->reg());
                    }
                }

                if (isSpilledAt(interval, inputOf(ins))) {
#ifdef JS_PUNBOX64
                    if (reg->type() == LDefinition::BOX) {
                        if (!safepoint->addValueSlot(true, reg->canonicalSpillSlot()))
                            return false;
                    } else
#endif
                    {
                        if (!safepoint->addGcSlot(true, reg->canonicalSpillSlot()))
                            return false;
                    }
                }
#ifdef JS_NUNBOX32
            } else {
                LinearScanVirtualRegister* other = otherHalfOfNunbox(reg);
                LinearScanVirtualRegister* type = (reg->type() == LDefinition::TYPE) ? reg : other;
                LinearScanVirtualRegister* payload = (reg->type() == LDefinition::PAYLOAD) ? reg : other;
                LiveInterval* typeInterval = type->intervalFor(inputOf(ins));
                LiveInterval* payloadInterval = payload->intervalFor(inputOf(ins));

                if (!typeInterval && !payloadInterval)
                    continue;

                LAllocation* typeAlloc = typeInterval->getAllocation();
                LAllocation* payloadAlloc = payloadInterval->getAllocation();

                // If the payload is an argument, we'll scan that explicitly as
                // part of the frame. It is therefore safe to not add any
                // safepoint entry, as long as the vreg does not have a stack
                // slot as canonical spill slot.
                if (payloadAlloc->isArgument() &&
                    (!payload->canonicalSpill() || payload->canonicalSpill() == payloadAlloc))
                {
                    continue;
                }

                if (isSpilledAt(typeInterval, inputOf(ins)) &&
                    isSpilledAt(payloadInterval, inputOf(ins)))
                {
                    // These two components of the Value are spilled
                    // contiguously, so simply keep track of the base slot.
                    uint32_t payloadSlot = payload->canonicalSpillSlot();
                    uint32_t slot = BaseOfNunboxSlot(LDefinition::PAYLOAD, payloadSlot);
                    if (!safepoint->addValueSlot(true, slot))
                        return false;
                }

                if (!ins->isCall() &&
                    (!isSpilledAt(typeInterval, inputOf(ins)) || payloadAlloc->isGeneralReg()))
                {
                    // Either the payload is on the stack but the type is
                    // in a register, or the payload is in a register. In
                    // both cases, we don't have a contiguous spill so we
                    // add a torn entry.
                    uint32_t typeVreg = type->def()->virtualRegister();
                    if (!safepoint->addNunboxParts(typeVreg, *typeAlloc, *payloadAlloc))
                        return false;

                    // If the nunbox is stored in multiple places, we need to
                    // trace all of them to allow the GC to relocate objects.
                    if (payloadAlloc->isGeneralReg() && isSpilledAt(payloadInterval, inputOf(ins))) {
                        if (!safepoint->addNunboxParts(typeVreg, *typeAlloc,
                                                       *payload->canonicalSpill()))
                        {
                            return false;
                        }
                    }
                }
#endif
            }
        }

#ifdef JS_NUNBOX32
        if (IsNunbox(reg)) {
            // Skip past the next half of this nunbox so we don't track the
            // same slot twice.
            MOZ_ASSERT(&vregs[reg->def()->virtualRegister() + 1] == otherHalfOfNunbox(reg));
            i++;
        }
#endif
    }

    return true;
}

/*
 * Split the given interval at the given position, and add the created
 * interval to the unhandled queue.
 */
bool
LinearScanAllocator::splitInterval(LiveInterval* interval, CodePosition pos)
{
    // Make sure we're actually splitting this interval, not some other
    // interval in the same virtual register.
    MOZ_ASSERT(interval->start() < pos && pos < interval->end());

    LinearScanVirtualRegister* reg = &vregs[interval->vreg()];

    // "Bogus" intervals cannot be split.
    MOZ_ASSERT(reg);

    // Do the split.
    LiveInterval* newInterval = LiveInterval::New(alloc(), interval->vreg(), interval->index() + 1);
    if (!interval->splitFrom(pos, newInterval))
        return false;

    MOZ_ASSERT(interval->numRanges() > 0);
    MOZ_ASSERT(newInterval->numRanges() > 0);

    if (!reg->addInterval(newInterval))
        return false;

    JitSpew(JitSpew_RegAlloc, "  Split interval to %u = [%u, %u]/[%u, %u]",
            interval->vreg(), interval->start().bits(),
            interval->end().bits(), newInterval->start().bits(),
            newInterval->end().bits());

    // We always want to enqueue the resulting split. We always split
    // forward, and we never want to handle something forward of our
    // current position.
    setIntervalRequirement(newInterval);

    // splitInterval() is usually called to split the node that has just been
    // popped from the unhandled queue. Therefore the split will likely be
    // closer to the lower start positions in the queue.
    unhandled.enqueueBackward(newInterval);

    return true;
}

bool
LinearScanAllocator::splitBlockingIntervals(AnyRegister allocatedReg)
{

    // Split current before the next fixed use.
    LiveInterval* fixed = fixedIntervals[allocatedReg.code()];
    if (fixed->numRanges() > 0) {
        CodePosition fixedPos = current->intersect(fixed);
        if (fixedPos != CodePosition::MIN) {
            MOZ_ASSERT(fixedPos > current->start());
            MOZ_ASSERT(fixedPos < current->end());
            if (!splitInterval(current, fixedPos))
                return false;
        }
    }

    // Split the blocking interval if it exists.

    for (IntervalIterator i(active.begin()); i != active.end();) {
        LiveInterval* it = *i++;
        if (it->getAllocation()->isRegister() &&
            it->getAllocation()->toRegister().aliases(allocatedReg))
        {
            JitSpew(JitSpew_RegAlloc, " Splitting active interval %u = [%u, %u]",
                    vregs[it->vreg()].ins()->id(), it->start().bits(), it->end().bits());

            MOZ_ASSERT(it->start() != current->start());
            MOZ_ASSERT(it->covers(current->start()));
            MOZ_ASSERT(it->start() != current->start());

            if (!splitInterval(it, current->start()))
                return false;

            active.remove(it);
            finishInterval(it);
            if (allocatedReg.numAliased() == 1)
                break;
        } else {
            JitSpew(JitSpew_RegAlloc, " Not touching active interval %u = [%u, %u]",
                    vregs[it->vreg()].ins()->id(), it->start().bits(), it->end().bits());
        }
    }
    // Split any inactive intervals at the next live point.
    for (IntervalIterator i(inactive.begin()); i != inactive.end(); ) {
        LiveInterval* it = *i++;
        if (it->getAllocation()->isRegister() &&
            it->getAllocation()->toRegister().aliases(allocatedReg))
        {
            JitSpew(JitSpew_RegAlloc, " Splitting inactive interval %u = [%u, %u]",
                    vregs[it->vreg()].ins()->id(), it->start().bits(), it->end().bits());

            CodePosition nextActive = it->nextCoveredAfter(current->start());
            MOZ_ASSERT(nextActive != CodePosition::MIN);

            if (!splitInterval(it, nextActive))
                return false;
            inactive.remove(it);
            finishInterval(it);
        }
    }

    return true;
}

/*
 * Assign the current interval the given allocation, splitting conflicting
 * intervals as necessary to make the allocation stick.
 */
bool
LinearScanAllocator::assign(LAllocation allocation)
{
    if (allocation.isRegister())
        JitSpew(JitSpew_RegAlloc, "Assigning register %s", allocation.toRegister().name());
    current->setAllocation(allocation);

    // Split this interval at the next incompatible one
    LinearScanVirtualRegister* reg = &vregs[current->vreg()];
    if (reg) {
        CodePosition splitPos = current->firstIncompatibleUse(allocation);
        if (splitPos != CodePosition::MAX) {
            // Split before the incompatible use. This ensures the use position is
            // part of the second half of the interval and guarantees we never split
            // at the end (zero-length intervals are invalid).
            splitPos = splitPos.previous();
            MOZ_ASSERT(splitPos < current->end());
            if (!splitInterval(current, splitPos))
                return false;
        }
    }

    bool useAsCanonicalSpillSlot = allocation.isMemory();
    // Only canonically spill argument values when frame arguments are not
    // modified in the body.
    if (mir->modifiesFrameArguments())
        useAsCanonicalSpillSlot = allocation.isStackSlot();

    if (reg && useAsCanonicalSpillSlot) {
        if (reg->canonicalSpill()) {
            MOZ_ASSERT(allocation == *reg->canonicalSpill());

            // This interval is spilled more than once, so just always spill
            // it at its definition.
            reg->setSpillAtDefinition(outputOf(reg->ins()));
        } else {
            reg->setCanonicalSpill(current->getAllocation());

            // If this spill is inside a loop, and the definition is outside
            // the loop, instead move the spill to outside the loop.
            LNode* other = insData[current->start()];
            uint32_t loopDepthAtDef = reg->block()->mir()->loopDepth();
            uint32_t loopDepthAtSpill = other->block()->mir()->loopDepth();
            if (loopDepthAtSpill > loopDepthAtDef)
                reg->setSpillAtDefinition(outputOf(reg->ins()));
        }
    }

    active.pushBack(current);

    return true;
}

uint32_t
LinearScanAllocator::allocateSlotFor(const LiveInterval* interval)
{
    LinearScanVirtualRegister* reg = &vregs[interval->vreg()];

    SlotList* freed;
    if (reg->def()->isSimdType())
        freed = &finishedQuadSlots_;
    else if (reg->type() == LDefinition::DOUBLE)
        freed = &finishedDoubleSlots_;
#if JS_BITS_PER_WORD == 64
    else if (reg->type() == LDefinition::GENERAL ||
             reg->type() == LDefinition::OBJECT ||
             reg->type() == LDefinition::SLOTS)
        freed = &finishedDoubleSlots_;
#endif
#ifdef JS_PUNBOX64
    else if (reg->type() == LDefinition::BOX)
        freed = &finishedDoubleSlots_;
#endif
#ifdef JS_NUNBOX32
    else if (IsNunbox(reg))
        freed = &finishedNunboxSlots_;
#endif
    else
        freed = &finishedSlots_;

    if (!freed->empty()) {
        LiveInterval* maybeDead = freed->back();
        if (maybeDead->end() < reg->getInterval(0)->start()) {
            // This spill slot is dead before the start of the interval trying
            // to reuse the slot, so reuse is safe. Otherwise, we could
            // encounter a situation where a stack slot is allocated and freed
            // inside a loop, but the same allocation is then used to hold a
            // loop-carried value.
            //
            // Note that we don't reuse the dead slot if its interval ends right
            // before the current interval, to avoid conflicting slot -> reg and
            // reg -> slot moves in the same movegroup.
            freed->popBack();
            LinearScanVirtualRegister* dead = &vregs[maybeDead->vreg()];
#ifdef JS_NUNBOX32
            if (IsNunbox(dead))
                return BaseOfNunboxSlot(dead->type(), dead->canonicalSpillSlot());
#endif
            return dead->canonicalSpillSlot();
        }
    }

    return stackSlotAllocator.allocateSlot(reg->type());
}

bool
LinearScanAllocator::spill()
{
    JitSpew(JitSpew_RegAlloc, "  Decided to spill current interval");

    // We can't spill bogus intervals
    MOZ_ASSERT(current->hasVreg());

    LinearScanVirtualRegister* reg = &vregs[current->vreg()];

    if (reg->canonicalSpill()) {
        JitSpew(JitSpew_RegAlloc, "  Allocating canonical spill location");

        return assign(*reg->canonicalSpill());
    }

    uint32_t stackSlot;
#if defined JS_NUNBOX32
    if (IsNunbox(reg)) {
        LinearScanVirtualRegister* other = otherHalfOfNunbox(reg);

        if (other->canonicalSpill()) {
            // The other half of this nunbox already has a spill slot. To
            // ensure the Value is spilled contiguously, use the other half (it
            // was allocated double-wide).
            MOZ_ASSERT(other->canonicalSpill()->isStackSlot());
            stackSlot = BaseOfNunboxSlot(other->type(), other->canonicalSpillSlot());
        } else {
            // No canonical spill location exists for this nunbox yet. Allocate
            // one.
            stackSlot = allocateSlotFor(current);
        }
        stackSlot -= OffsetOfNunboxSlot(reg->type());
    } else
#endif
    {
        stackSlot = allocateSlotFor(current);
    }
    MOZ_ASSERT(stackSlot <= stackSlotAllocator.stackHeight());

    return assign(LStackSlot(stackSlot));
}

void
LinearScanAllocator::freeAllocation(LiveInterval* interval, LAllocation* alloc)
{
    LinearScanVirtualRegister* mine = &vregs[interval->vreg()];
    if (!IsNunbox(mine)) {
        if (alloc->isStackSlot()) {
            if (mine->def()->isSimdType())
                finishedQuadSlots_.append(interval);
            else if (mine->type() == LDefinition::DOUBLE)
                finishedDoubleSlots_.append(interval);
#if JS_BITS_PER_WORD == 64
            else if (mine->type() == LDefinition::GENERAL ||
                     mine->type() == LDefinition::OBJECT ||
                     mine->type() == LDefinition::SLOTS)
                finishedDoubleSlots_.append(interval);
#endif
#ifdef JS_PUNBOX64
            else if (mine->type() == LDefinition::BOX)
                finishedDoubleSlots_.append(interval);
#endif
            else
                finishedSlots_.append(interval);
        }
        return;
    }

#ifdef JS_NUNBOX32
    // Special handling for nunboxes. We can only free the stack slot once we
    // know both intervals have been finished.
    LinearScanVirtualRegister* other = otherHalfOfNunbox(mine);
    if (other->finished()) {
        if (!mine->canonicalSpill() && !other->canonicalSpill())
            return;

        MOZ_ASSERT_IF(mine->canonicalSpill() && other->canonicalSpill(),
                      mine->canonicalSpill()->isStackSlot() == other->canonicalSpill()->isStackSlot());

        LinearScanVirtualRegister* candidate = mine->canonicalSpill() ? mine : other;
        if (!candidate->canonicalSpill()->isStackSlot())
            return;

        finishedNunboxSlots_.append(candidate->lastInterval());
    }
#endif
}

void
LinearScanAllocator::finishInterval(LiveInterval* interval)
{
    LAllocation* alloc = interval->getAllocation();
    MOZ_ASSERT(!alloc->isUse());

    // Toss out the bogus interval now that it's run its course
    if (!interval->hasVreg())
        return;

    LinearScanVirtualRegister* reg = &vregs[interval->vreg()];

    // All spills should be equal to the canonical spill location.
    MOZ_ASSERT_IF(alloc->isStackSlot(), *alloc == *reg->canonicalSpill());

    bool lastInterval = interval->index() == (reg->numIntervals() - 1);
    if (lastInterval) {
        freeAllocation(interval, alloc);
        reg->setFinished();
    }

    handled.pushBack(interval);
}

/*
 * This function locates a register which may be assigned by the register
 * and is not assigned to any active interval. The register which is free
 * for the longest period of time is then returned.
 */
AnyRegister::Code
LinearScanAllocator::findBestFreeRegister(CodePosition* freeUntil)
{
    JitSpew(JitSpew_RegAlloc, "  Computing freeUntilPos");

    // Compute free-until positions for all registers
    CodePosition freeUntilPos[AnyRegister::Total];
    bool needFloat = vregs[current->vreg()].isFloatReg();
    for (RegisterSet regs(allRegisters_); !regs.empty(needFloat); ) {
        // If the requested register is a FP, we may need to look at
        // all of the float32 and float64 registers.
        AnyRegister reg = regs.takeAny(needFloat);
        freeUntilPos[reg.code()] = CodePosition::MAX;
    }
    for (IntervalIterator i(active.begin()); i != active.end(); i++) {
        LAllocation* alloc = i->getAllocation();
        if (alloc->isRegister(needFloat)) {
            AnyRegister reg = alloc->toRegister();
            for (size_t a = 0; a < reg.numAliased(); a++) {
                JitSpew(JitSpew_RegAlloc, "   Register %s not free", reg.aliased(a).name());
                freeUntilPos[reg.aliased(a).code()] = CodePosition::MIN;
            }
        }
    }
    for (IntervalIterator i(inactive.begin()); i != inactive.end(); i++) {
        LAllocation* alloc = i->getAllocation();
        if (alloc->isRegister(needFloat)) {
            AnyRegister reg = alloc->toRegister();
            CodePosition pos = current->intersect(*i);
            for (size_t a = 0; a < reg.numAliased(); a++) {
                if (pos != CodePosition::MIN && pos < freeUntilPos[reg.aliased(a).code()]) {
                    freeUntilPos[reg.aliased(a).code()] = pos;
                    JitSpew(JitSpew_RegAlloc, "   Register %s free until %u", reg.aliased(a).name(), pos.bits());
                }
            }
        }
    }

    CodePosition fixedPos = fixedIntervalsUnion->intersect(current);
    if (fixedPos != CodePosition::MIN) {
        for (IntervalIterator i(fixed.begin()); i != fixed.end(); i++) {
            AnyRegister reg = i->getAllocation()->toRegister();
            for (size_t a = 0; a < reg.numAliased(); a++) {
                AnyRegister areg = reg.aliased(a);
                if (freeUntilPos[areg.code()] != CodePosition::MIN) {
                    CodePosition pos = current->intersect(*i);
                    if (pos != CodePosition::MIN && pos < freeUntilPos[areg.code()]) {
                        freeUntilPos[areg.code()] = (pos == current->start()) ? CodePosition::MIN : pos;
                        JitSpew(JitSpew_RegAlloc, "   Register %s free until %u", areg.name(), pos.bits());
                    }
                }
            }
        }
    }

    AnyRegister::Code bestCode = AnyRegister::Invalid;
    if (current->index()) {
        // As an optimization, use the allocation from the previous interval if
        // it is available.
        LiveInterval* previous = vregs[current->vreg()].getInterval(current->index() - 1);
        LAllocation* alloc = previous->getAllocation();
        if (alloc->isRegister(needFloat)) {
            AnyRegister prevReg = alloc->toRegister();
            bool useit = true;
            for (size_t a = 0; a < prevReg.numAliased(); a++) {
                AnyRegister aprevReg = prevReg.aliased(a);
                if (freeUntilPos[aprevReg.code()] == CodePosition::MIN) {
                    useit = false;
                    break;
                }
            }
            if (useit)
                bestCode = prevReg.code();
        }
    }

    // Assign the register suggested by the hint if it's free.
    const Requirement* hint = current->hint();
    if (hint->kind() == Requirement::FIXED && hint->allocation().isRegister()) {
        AnyRegister hintReg = hint->allocation().toRegister();
        bool useit = true;
        for (size_t a = 0; a < hintReg.numAliased(); a++) {
            if (freeUntilPos[hintReg.aliased(a).code()] <= hint->pos()) {
                useit = false;
                break;
            }
        }
        if (useit)
            bestCode = hintReg.code();

    } else if (hint->kind() == Requirement::MUST_REUSE_INPUT) {
        LiveInterval* other = vregs[hint->virtualRegister()].intervalFor(hint->pos());
        if (other && other->getAllocation()->isRegister()) {
            AnyRegister hintReg = other->getAllocation()->toRegister();
            bool useit = true;
            for (size_t a = 0; a < hintReg.numAliased(); a++) {
                if (freeUntilPos[hintReg.aliased(a).code()] <= hint->pos()) {
                    useit = false;
                    break;
                }
            }
            if (useit)
                bestCode = hintReg.code();
        }
    }

    if (bestCode == AnyRegister::Invalid) {
        // If all else fails, search freeUntilPos for largest value
        for (uint32_t i = 0; i < AnyRegister::Total; i++) {
            if (freeUntilPos[i] == CodePosition::MIN)
                continue;
            AnyRegister cur = AnyRegister::FromCode(i);
            if (!vregs[current->vreg()].isCompatibleReg(cur))
                continue;
            if (bestCode == AnyRegister::Invalid || freeUntilPos[i] > freeUntilPos[bestCode])
                bestCode = AnyRegister::Code(i);
        }
    }

    if (bestCode != AnyRegister::Invalid)
        *freeUntil = freeUntilPos[bestCode];
    return bestCode;
}

/*
 * This function locates a register which is assigned to an active interval,
 * and returns the one with the furthest away next use. As a side effect,
 * the nextUsePos array is updated with the next use position of all active
 * intervals for use elsewhere in the algorithm.
 */
AnyRegister::Code
LinearScanAllocator::findBestBlockedRegister(CodePosition* nextUsed)
{
    JitSpew(JitSpew_RegAlloc, "  Computing nextUsePos");

    // Compute next-used positions for all registers
    CodePosition nextUsePos[AnyRegister::Total];
    bool needFloat = vregs[current->vreg()].isFloatReg();
    for (RegisterSet regs(allRegisters_); !regs.empty(needFloat); ) {
        AnyRegister reg = regs.takeAny(needFloat);
        nextUsePos[reg.code()] = CodePosition::MAX;
    }
    for (IntervalIterator i(active.begin()); i != active.end(); i++) {
        LAllocation* alloc = i->getAllocation();
        if (alloc->isRegister(needFloat)) {
            AnyRegister fullreg = alloc->toRegister();
            for (size_t a = 0; a < fullreg.numAliased(); a++) {
                AnyRegister reg = fullreg.aliased(a);
                if (i->start() == current->start()) {
                    nextUsePos[reg.code()] = CodePosition::MIN;
                    JitSpew(JitSpew_RegAlloc, "   Disqualifying %s due to recency", reg.name());
                } else if (nextUsePos[reg.code()] != CodePosition::MIN) {
                    nextUsePos[reg.code()] = i->nextUsePosAfter(current->start());
                    JitSpew(JitSpew_RegAlloc, "   Register %s next used %u", reg.name(),
                            nextUsePos[reg.code()].bits());
                }
            }
        }
    }
    for (IntervalIterator i(inactive.begin()); i != inactive.end(); i++) {
        LAllocation* alloc = i->getAllocation();
        if (alloc->isRegister(needFloat)) {
            AnyRegister reg = alloc->toRegister();
            CodePosition pos = i->nextUsePosAfter(current->start());
            for (size_t a = 0; a < reg.numAliased(); a++) {
                if (pos < nextUsePos[reg.aliased(a).code()]) {
                    nextUsePos[reg.aliased(a).code()] = pos;
                    JitSpew(JitSpew_RegAlloc, "   Register %s next used %u", reg.aliased(a).name(), pos.bits());
                }
            }
        }
    }

    CodePosition fixedPos = fixedIntervalsUnion->intersect(current);
    if (fixedPos != CodePosition::MIN) {
        for (IntervalIterator i(fixed.begin()); i != fixed.end(); i++) {
            AnyRegister fullreg = i->getAllocation()->toRegister();
            for (size_t a = 0; a < fullreg.numAliased(); a++) {
                AnyRegister reg = fullreg.aliased(a);
                if (nextUsePos[reg.code()] != CodePosition::MIN) {
                    CodePosition pos = i->intersect(current);
                    if (pos != CodePosition::MIN && pos < nextUsePos[reg.code()]) {
                        nextUsePos[reg.code()] = (pos == current->start()) ? CodePosition::MIN : pos;
                        JitSpew(JitSpew_RegAlloc, "   Register %s next used %u (fixed)", reg.name(), pos.bits());
                    }
                }
            }
        }
    }

    // Search nextUsePos for largest value
    AnyRegister::Code bestCode = AnyRegister::Invalid;
    for (size_t i = 0; i < AnyRegister::Total; i++) {
        if (nextUsePos[i] == CodePosition::MIN)
            continue;
        AnyRegister cur = AnyRegister::FromCode(i);
        if (!vregs[current->vreg()].isCompatibleReg(cur))
            continue;

        if (bestCode == AnyRegister::Invalid || nextUsePos[i] > nextUsePos[bestCode])
            bestCode = AnyRegister::Code(i);
    }

    if (bestCode != AnyRegister::Invalid)
        *nextUsed = nextUsePos[bestCode];
    return bestCode;
}

/*
 * Two intervals can coexist if any of the following conditions is met:
 *
 *   - The intervals do not intersect.
 *   - The intervals have different allocations.
 *   - The intervals have the same allocation, but the allocation may be
 *     shared.
 *
 * Intuitively, it is a bug if any allocated intervals exist which can not
 * coexist.
 */
bool
LinearScanAllocator::canCoexist(LiveInterval* a, LiveInterval* b)
{
    LAllocation* aa = a->getAllocation();
    LAllocation* ba = b->getAllocation();
    if (aa->isRegister() && ba->isRegister() && aa->toRegister().aliases(ba->toRegister()))
        return a->intersect(b) == CodePosition::MIN;
    return true;
}

#ifdef DEBUG
/*
 * Ensure intervals appear in exactly the appropriate one of {active,inactive,
 * handled}, and that active and inactive intervals do not conflict. Handled
 * intervals are checked for conflicts in validateAllocations for performance
 * reasons.
 */
void
LinearScanAllocator::validateIntervals()
{
    if (!js_JitOptions.checkGraphConsistency)
        return;

    for (IntervalIterator i(active.begin()); i != active.end(); i++) {
        MOZ_ASSERT(i->numRanges() > 0);
        MOZ_ASSERT(i->covers(current->start()));

        for (IntervalIterator j(active.begin()); j != i; j++)
            MOZ_ASSERT(canCoexist(*i, *j));
    }

    for (IntervalIterator i(inactive.begin()); i != inactive.end(); i++) {
        MOZ_ASSERT(i->numRanges() > 0);
        MOZ_ASSERT(i->end() >= current->start());
        MOZ_ASSERT(!i->covers(current->start()));

        for (IntervalIterator j(active.begin()); j != active.end(); j++) {
            MOZ_ASSERT(*i != *j);
            MOZ_ASSERT(canCoexist(*i, *j));
        }
        for (IntervalIterator j(inactive.begin()); j != i; j++)
            MOZ_ASSERT(canCoexist(*i, *j));
    }

    for (IntervalIterator i(handled.begin()); i != handled.end(); i++) {
        MOZ_ASSERT(!i->getAllocation()->isUse());
        MOZ_ASSERT(i->numRanges() > 0);
        if (i->getAllocation()->isRegister()) {
            MOZ_ASSERT(i->end() <= current->start());
            MOZ_ASSERT(!i->covers(current->start()));
        }

        for (IntervalIterator j(active.begin()); j != active.end(); j++)
            MOZ_ASSERT(*i != *j);
        for (IntervalIterator j(inactive.begin()); j != inactive.end(); j++)
            MOZ_ASSERT(*i != *j);
    }
}

/*
 * This function performs a nice, expensive check that all intervals
 * in the function can coexist with every other interval.
 */
void
LinearScanAllocator::validateAllocations()
{
    if (!js_JitOptions.checkGraphConsistency)
        return;

    for (IntervalIterator i(handled.begin()); i != handled.end(); i++) {
        for (IntervalIterator j(handled.begin()); j != i; j++) {
            MOZ_ASSERT(*i != *j);
            MOZ_ASSERT(canCoexist(*i, *j));
        }
        LinearScanVirtualRegister* reg = &vregs[i->vreg()];
        bool found = false;
        for (size_t j = 0; j < reg->numIntervals(); j++) {
            if (reg->getInterval(j) == *i) {
                MOZ_ASSERT(j == i->index());
                found = true;
                break;
            }
        }
        MOZ_ASSERT(found);
    }
}

#endif // DEBUG

bool
LinearScanAllocator::go()
{
    JitSpew(JitSpew_RegAlloc, "Beginning register allocation");

    if (!buildLivenessInfo())
        return false;

    if (mir->shouldCancel("LSRA Liveness"))
        return false;

    JitSpew(JitSpew_RegAlloc, "Beginning preliminary register allocation");
    if (!allocateRegisters())
        return false;
    JitSpew(JitSpew_RegAlloc, "Preliminary register allocation complete");

    if (mir->shouldCancel("LSRA Preliminary Regalloc"))
        return false;

    if (JitSpewEnabled(JitSpew_RegAlloc)) {
        fprintf(stderr, "Allocations by virtual register:\n");
        dumpVregs();
    }

    JitSpew(JitSpew_RegAlloc, "Beginning control flow resolution");
    if (!resolveControlFlow())
        return false;
    JitSpew(JitSpew_RegAlloc, "Control flow resolution complete");

    if (mir->shouldCancel("LSRA Control Flow"))
        return false;

    JitSpew(JitSpew_RegAlloc, "Beginning register allocation reification");
    if (!reifyAllocations())
        return false;
    JitSpew(JitSpew_RegAlloc, "Register allocation reification complete");

    if (mir->shouldCancel("LSRA Reification"))
        return false;

    JitSpew(JitSpew_RegAlloc, "Beginning safepoint population.");
    if (!populateSafepoints())
        return false;
    JitSpew(JitSpew_RegAlloc, "Safepoint population complete.");

    if (mir->shouldCancel("LSRA Safepoints"))
        return false;

    JitSpew(JitSpew_RegAlloc, "Register allocation complete");

    return true;
}

void
LinearScanAllocator::setIntervalRequirement(LiveInterval* interval)
{
    MOZ_ASSERT(interval->requirement()->kind() == Requirement::NONE);
    MOZ_ASSERT(interval->hint()->kind() == Requirement::NONE);

    // This function computes requirement by virtual register, other types of
    // interval should have requirements set manually
    LinearScanVirtualRegister* reg = &vregs[interval->vreg()];

    if (interval->index() == 0) {
        // The first interval is the definition, so deal with any definition
        // constraints/hints

        if (reg->def()->policy() == LDefinition::FIXED) {
            // Fixed policies get a FIXED requirement or hint.
            if (reg->def()->output()->isRegister())
                interval->setHint(Requirement(*reg->def()->output()));
            else
                interval->setRequirement(Requirement(*reg->def()->output()));
        } else if (reg->def()->policy() == LDefinition::MUST_REUSE_INPUT) {
            // Reuse policies get either a FIXED requirement or a SAME_AS hint.
            LUse* use = reg->ins()->getOperand(reg->def()->getReusedInput())->toUse();
            interval->setRequirement(Requirement(Requirement::REGISTER));
            interval->setHint(Requirement(use->virtualRegister(), interval->start().previous()));
        } else if (reg->ins()->isPhi()) {
            // Phis don't have any requirements, but they should prefer
            // their input allocations, so they get a SAME_AS hint of the
            // first input
            if (reg->ins()->toPhi()->numOperands() != 0) {
                LUse* use = reg->ins()->toPhi()->getOperand(0)->toUse();
                LBlock* predecessor = reg->block()->mir()->getPredecessor(0)->lir();
                CodePosition predEnd = exitOf(predecessor);
                interval->setHint(Requirement(use->virtualRegister(), predEnd));
            }
        } else {
            // Non-phis get a REGISTER requirement
            interval->setRequirement(Requirement(Requirement::REGISTER));
        }
    }

    UsePosition* fixedOp = nullptr;
    UsePosition* registerOp = nullptr;

    // Search uses at the start of the interval for requirements.
    UsePositionIterator usePos(interval->usesBegin());
    for (; usePos != interval->usesEnd(); usePos++) {
        if (interval->start().next() < usePos->pos)
            break;

        LUse::Policy policy = usePos->use->policy();
        if (policy == LUse::FIXED) {
            fixedOp = *usePos;
            interval->setRequirement(Requirement(Requirement::REGISTER));
            break;
        } else if (policy == LUse::REGISTER) {
            // Register uses get a REGISTER requirement
            interval->setRequirement(Requirement(Requirement::REGISTER));
        }
    }

    // Search other uses for hints. If the virtual register already has a
    // canonical spill location, we will eagerly spill this interval, so we
    // don't have to search for hints.
    if (!fixedOp && !vregs[interval->vreg()].canonicalSpill()) {
        for (; usePos != interval->usesEnd(); usePos++) {
            LUse::Policy policy = usePos->use->policy();
            if (policy == LUse::FIXED) {
                fixedOp = *usePos;
                break;
            } else if (policy == LUse::REGISTER) {
                if (!registerOp)
                    registerOp = *usePos;
            }
        }
    }

    if (fixedOp) {
        // Intervals with a fixed use now get a FIXED hint.
        AnyRegister required = GetFixedRegister(reg->def(), fixedOp->use);
        interval->setHint(Requirement(LAllocation(required), fixedOp->pos));
    } else if (registerOp) {
        // Intervals with register uses get a REGISTER hint. We may have already
        // assigned a SAME_AS hint, make sure we don't overwrite it with a weaker
        // hint.
        if (interval->hint()->kind() == Requirement::NONE)
            interval->setHint(Requirement(Requirement::REGISTER, registerOp->pos));
    }
}

/*
 * Enqueue by iteration starting from the node with the lowest start position.
 *
 * If we assign registers to intervals in order of their start positions
 * without regard to their requirements, we can end up in situations where
 * intervals that do not require registers block intervals that must have
 * registers from getting one. To avoid this, we ensure that intervals are
 * ordered by position and priority so intervals with more stringent
 * requirements are handled first.
 */
void
LinearScanAllocator::UnhandledQueue::enqueueBackward(LiveInterval* interval)
{
    IntervalReverseIterator i(rbegin());

    for (; i != rend(); i++) {
        if (i->start() > interval->start())
            break;
        if (i->start() == interval->start() &&
            i->requirement()->priority() >= interval->requirement()->priority())
        {
            break;
        }
    }
    insertAfter(*i, interval);
}

/*
 * Enqueue by iteration from high start position to low start position,
 * after a provided node.
 */
void
LinearScanAllocator::UnhandledQueue::enqueueForward(LiveInterval* after, LiveInterval* interval)
{
    IntervalIterator i(begin(after));
    i++; // Skip the initial node.

    for (; i != end(); i++) {
        if (i->start() < interval->start())
            break;
        if (i->start() == interval->start() &&
            i->requirement()->priority() < interval->requirement()->priority())
        {
            break;
        }
    }
    insertBefore(*i, interval);
}

void
LinearScanAllocator::UnhandledQueue::assertSorted()
{
#ifdef DEBUG
    LiveInterval* prev = nullptr;
    for (IntervalIterator i(begin()); i != end(); i++) {
        if (prev) {
            MOZ_ASSERT(prev->start() >= i->start());
            MOZ_ASSERT_IF(prev->start() == i->start(),
                          prev->requirement()->priority() >= i->requirement()->priority());
        }
        prev = *i;
    }
#endif
}

LiveInterval*
LinearScanAllocator::UnhandledQueue::dequeue()
{
    if (rbegin() == rend())
        return nullptr;

    LiveInterval* result = *rbegin();
    remove(result);
    return result;
}

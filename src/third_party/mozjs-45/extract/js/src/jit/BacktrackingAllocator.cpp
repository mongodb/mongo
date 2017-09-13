/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BacktrackingAllocator.h"

#include "jsprf.h"

#include "jit/BitSet.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

/////////////////////////////////////////////////////////////////////
// Utility
/////////////////////////////////////////////////////////////////////

static inline bool
SortBefore(UsePosition* a, UsePosition* b)
{
    return a->pos <= b->pos;
}

static inline bool
SortBefore(LiveRange::BundleLink* a, LiveRange::BundleLink* b)
{
    LiveRange* rangea = LiveRange::get(a);
    LiveRange* rangeb = LiveRange::get(b);
    MOZ_ASSERT(!rangea->intersects(rangeb));
    return rangea->from() < rangeb->from();
}

static inline bool
SortBefore(LiveRange::RegisterLink* a, LiveRange::RegisterLink* b)
{
    return LiveRange::get(a)->from() <= LiveRange::get(b)->from();
}

template <typename T>
static inline void
InsertSortedList(InlineForwardList<T> &list, T* value)
{
    if (list.empty()) {
        list.pushFront(value);
        return;
    }

    if (SortBefore(list.back(), value)) {
        list.pushBack(value);
        return;
    }

    T* prev = nullptr;
    for (InlineForwardListIterator<T> iter = list.begin(); iter; iter++) {
        if (SortBefore(value, *iter))
            break;
        prev = *iter;
    }

    if (prev)
        list.insertAfter(prev, value);
    else
        list.pushFront(value);
}

/////////////////////////////////////////////////////////////////////
// LiveRange
/////////////////////////////////////////////////////////////////////

void
LiveRange::addUse(UsePosition* use)
{
    MOZ_ASSERT(covers(use->pos));
    InsertSortedList(uses_, use);
}

void
LiveRange::distributeUses(LiveRange* other)
{
    MOZ_ASSERT(other->vreg() == vreg());
    MOZ_ASSERT(this != other);

    // Move over all uses which fit in |other|'s boundaries.
    for (UsePositionIterator iter = usesBegin(); iter; ) {
        UsePosition* use = *iter;
        if (other->covers(use->pos)) {
            uses_.removeAndIncrement(iter);
            other->addUse(use);
        } else {
            iter++;
        }
    }

    // Distribute the definition to |other| as well, if possible.
    if (hasDefinition() && from() == other->from())
        other->setHasDefinition();
}

bool
LiveRange::contains(LiveRange* other) const
{
    return from() <= other->from() && to() >= other->to();
}

void
LiveRange::intersect(LiveRange* other, Range* pre, Range* inside, Range* post) const
{
    MOZ_ASSERT(pre->empty() && inside->empty() && post->empty());

    CodePosition innerFrom = from();
    if (from() < other->from()) {
        if (to() < other->from()) {
            *pre = range_;
            return;
        }
        *pre = Range(from(), other->from());
        innerFrom = other->from();
    }

    CodePosition innerTo = to();
    if (to() > other->to()) {
        if (from() >= other->to()) {
            *post = range_;
            return;
        }
        *post = Range(other->to(), to());
        innerTo = other->to();
    }

    if (innerFrom != innerTo)
        *inside = Range(innerFrom, innerTo);
}

bool
LiveRange::intersects(LiveRange* other) const
{
    Range pre, inside, post;
    intersect(other, &pre, &inside, &post);
    return !inside.empty();
}

/////////////////////////////////////////////////////////////////////
// SpillSet
/////////////////////////////////////////////////////////////////////

void
SpillSet::setAllocation(LAllocation alloc)
{
    for (size_t i = 0; i < numSpilledBundles(); i++)
        spilledBundle(i)->setAllocation(alloc);
}

/////////////////////////////////////////////////////////////////////
// LiveBundle
/////////////////////////////////////////////////////////////////////

#ifdef DEBUG
size_t
LiveBundle::numRanges() const
{
    size_t count = 0;
    for (LiveRange::BundleLinkIterator iter = rangesBegin(); iter; iter++)
        count++;
    return count;
}
#endif // DEBUG

LiveRange*
LiveBundle::rangeFor(CodePosition pos) const
{
    for (LiveRange::BundleLinkIterator iter = rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        if (range->covers(pos))
            return range;
    }
    return nullptr;
}

void
LiveBundle::addRange(LiveRange* range)
{
    MOZ_ASSERT(!range->bundle());
    range->setBundle(this);
    InsertSortedList(ranges_, &range->bundleLink);
}

bool
LiveBundle::addRange(TempAllocator& alloc, uint32_t vreg, CodePosition from, CodePosition to)
{
    LiveRange* range = LiveRange::New(alloc, vreg, from, to);
    if (!range)
        return false;
    addRange(range);
    return true;
}

bool
LiveBundle::addRangeAndDistributeUses(TempAllocator& alloc, LiveRange* oldRange,
                                      CodePosition from, CodePosition to)
{
    LiveRange* range = LiveRange::New(alloc, oldRange->vreg(), from, to);
    if (!range)
        return false;
    addRange(range);
    oldRange->distributeUses(range);
    return true;
}

LiveRange*
LiveBundle::popFirstRange()
{
    LiveRange::BundleLinkIterator iter = rangesBegin();
    if (!iter)
        return nullptr;

    LiveRange* range = LiveRange::get(*iter);
    ranges_.removeAt(iter);

    range->setBundle(nullptr);
    return range;
}

void
LiveBundle::removeRange(LiveRange* range)
{
    for (LiveRange::BundleLinkIterator iter = rangesBegin(); iter; iter++) {
        LiveRange* existing = LiveRange::get(*iter);
        if (existing == range) {
            ranges_.removeAt(iter);
            return;
        }
    }
    MOZ_CRASH();
}

/////////////////////////////////////////////////////////////////////
// VirtualRegister
/////////////////////////////////////////////////////////////////////

bool
VirtualRegister::addInitialRange(TempAllocator& alloc, CodePosition from, CodePosition to)
{
    MOZ_ASSERT(from < to);

    // Mark [from,to) as a live range for this register during the initial
    // liveness analysis, coalescing with any existing overlapping ranges.

    LiveRange* prev = nullptr;
    LiveRange* merged = nullptr;
    for (LiveRange::RegisterLinkIterator iter(rangesBegin()); iter; ) {
        LiveRange* existing = LiveRange::get(*iter);

        if (from > existing->to()) {
            // The new range should go after this one.
            prev = existing;
            iter++;
            continue;
        }

        if (to.next() < existing->from()) {
            // The new range should go before this one.
            break;
        }

        if (!merged) {
            // This is the first old range we've found that overlaps the new
            // range. Extend this one to cover its union with the new range.
            merged = existing;

            if (from < existing->from())
                existing->setFrom(from);
            if (to > existing->to())
                existing->setTo(to);

            // Continue searching to see if any other old ranges can be
            // coalesced with the new merged range.
            iter++;
            continue;
        }

        // Coalesce this range into the previous range we merged into.
        MOZ_ASSERT(existing->from() >= merged->from());
        if (existing->to() > merged->to())
            merged->setTo(existing->to());

        MOZ_ASSERT(!existing->hasDefinition());
        existing->distributeUses(merged);
        MOZ_ASSERT(!existing->hasUses());

        ranges_.removeAndIncrement(iter);
    }

    if (!merged) {
        // The new range does not overlap any existing range for the vreg.
        LiveRange* range = LiveRange::New(alloc, vreg(), from, to);
        if (!range)
            return false;

        if (prev)
            ranges_.insertAfter(&prev->registerLink, &range->registerLink);
        else
            ranges_.pushFront(&range->registerLink);
    }

    return true;
}

void
VirtualRegister::addInitialUse(UsePosition* use)
{
    LiveRange::get(*rangesBegin())->addUse(use);
}

void
VirtualRegister::setInitialDefinition(CodePosition from)
{
    LiveRange* first = LiveRange::get(*rangesBegin());
    MOZ_ASSERT(from >= first->from());
    first->setFrom(from);
    first->setHasDefinition();
}

LiveRange*
VirtualRegister::rangeFor(CodePosition pos, bool preferRegister /* = false */) const
{
    LiveRange* found = nullptr;
    for (LiveRange::RegisterLinkIterator iter = rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        if (range->covers(pos)) {
            if (!preferRegister || range->bundle()->allocation().isRegister())
                return range;
            if (!found)
                found = range;
        }
    }
    return found;
}

void
VirtualRegister::addRange(LiveRange* range)
{
    InsertSortedList(ranges_, &range->registerLink);
}

void
VirtualRegister::removeRange(LiveRange* range)
{
    for (LiveRange::RegisterLinkIterator iter = rangesBegin(); iter; iter++) {
        LiveRange* existing = LiveRange::get(*iter);
        if (existing == range) {
            ranges_.removeAt(iter);
            return;
        }
    }
    MOZ_CRASH();
}

/////////////////////////////////////////////////////////////////////
// BacktrackingAllocator
/////////////////////////////////////////////////////////////////////

// This function pre-allocates and initializes as much global state as possible
// to avoid littering the algorithms with memory management cruft.
bool
BacktrackingAllocator::init()
{
    if (!RegisterAllocator::init())
        return false;

    liveIn = mir->allocate<BitSet>(graph.numBlockIds());
    if (!liveIn)
        return false;

    callRanges = LiveBundle::New(alloc(), nullptr, nullptr);

    size_t numVregs = graph.numVirtualRegisters();
    if (!vregs.init(mir->alloc(), numVregs))
        return false;
    memset(&vregs[0], 0, sizeof(VirtualRegister) * numVregs);
    for (uint32_t i = 0; i < numVregs; i++)
        new(&vregs[i]) VirtualRegister();

    // Build virtual register objects.
    for (size_t i = 0; i < graph.numBlocks(); i++) {
        if (mir->shouldCancel("Create data structures (main loop)"))
            return false;

        LBlock* block = graph.getBlock(i);
        for (LInstructionIterator ins = block->begin(); ins != block->end(); ins++) {
            for (size_t j = 0; j < ins->numDefs(); j++) {
                LDefinition* def = ins->getDef(j);
                if (def->isBogusTemp())
                    continue;
                vreg(def).init(*ins, def, /* isTemp = */ false);
            }

            for (size_t j = 0; j < ins->numTemps(); j++) {
                LDefinition* def = ins->getTemp(j);
                if (def->isBogusTemp())
                    continue;
                vreg(def).init(*ins, def, /* isTemp = */ true);
            }
        }
        for (size_t j = 0; j < block->numPhis(); j++) {
            LPhi* phi = block->getPhi(j);
            LDefinition* def = phi->getDef(0);
            vreg(def).init(phi, def, /* isTemp = */ false);
        }
    }

    LiveRegisterSet remainingRegisters(allRegisters_.asLiveSet());
    while (!remainingRegisters.emptyGeneral()) {
        AnyRegister reg = AnyRegister(remainingRegisters.takeAnyGeneral());
        registers[reg.code()].allocatable = true;
    }
    while (!remainingRegisters.emptyFloat()) {
        AnyRegister reg = AnyRegister(remainingRegisters.takeAnyFloat());
        registers[reg.code()].allocatable = true;
    }

    LifoAlloc* lifoAlloc = mir->alloc().lifoAlloc();
    for (size_t i = 0; i < AnyRegister::Total; i++) {
        registers[i].reg = AnyRegister::FromCode(i);
        registers[i].allocations.setAllocator(lifoAlloc);
    }

    hotcode.setAllocator(lifoAlloc);

    // Partition the graph into hot and cold sections, for helping to make
    // splitting decisions. Since we don't have any profiling data this is a
    // crapshoot, so just mark the bodies of inner loops as hot and everything
    // else as cold.

    LBlock* backedge = nullptr;
    for (size_t i = 0; i < graph.numBlocks(); i++) {
        LBlock* block = graph.getBlock(i);

        // If we see a loop header, mark the backedge so we know when we have
        // hit the end of the loop. Don't process the loop immediately, so that
        // if there is an inner loop we will ignore the outer backedge.
        if (block->mir()->isLoopHeader())
            backedge = block->mir()->backedge()->lir();

        if (block == backedge) {
            LBlock* header = block->mir()->loopHeaderOfBackedge()->lir();
            LiveRange* range = LiveRange::New(alloc(), 0, entryOf(header), exitOf(block).next());
            if (!range || !hotcode.insert(range))
                return false;
        }
    }

    return true;
}

bool
BacktrackingAllocator::addInitialFixedRange(AnyRegister reg, CodePosition from, CodePosition to)
{
    LiveRange* range = LiveRange::New(alloc(), 0, from, to);
    return range && registers[reg.code()].allocations.insert(range);
}

#ifdef DEBUG
// Returns true iff ins has a def/temp reusing the input allocation.
static bool
IsInputReused(LInstruction* ins, LUse* use)
{
    for (size_t i = 0; i < ins->numDefs(); i++) {
        if (ins->getDef(i)->policy() == LDefinition::MUST_REUSE_INPUT &&
            ins->getOperand(ins->getDef(i)->getReusedInput())->toUse() == use)
        {
            return true;
        }
    }

    for (size_t i = 0; i < ins->numTemps(); i++) {
        if (ins->getTemp(i)->policy() == LDefinition::MUST_REUSE_INPUT &&
            ins->getOperand(ins->getTemp(i)->getReusedInput())->toUse() == use)
        {
            return true;
        }
    }

    return false;
}
#endif

/*
 * This function builds up liveness ranges for all virtual registers
 * defined in the function. Additionally, it populates the liveIn array with
 * information about which registers are live at the beginning of a block, to
 * aid resolution and reification in a later phase.
 *
 * The algorithm is based on the one published in:
 *
 * Wimmer, Christian, and Michael Franz. "Linear Scan Register Allocation on
 *     SSA Form." Proceedings of the International Symposium on Code Generation
 *     and Optimization. Toronto, Ontario, Canada, ACM. 2010. 170-79. PDF.
 *
 * The algorithm operates on blocks ordered such that dominators of a block
 * are before the block itself, and such that all blocks of a loop are
 * contiguous. It proceeds backwards over the instructions in this order,
 * marking registers live at their uses, ending their live ranges at
 * definitions, and recording which registers are live at the top of every
 * block. To deal with loop backedges, registers live at the beginning of
 * a loop gain a range covering the entire loop.
 */
bool
BacktrackingAllocator::buildLivenessInfo()
{
    JitSpew(JitSpew_RegAlloc, "Beginning liveness analysis");

    Vector<MBasicBlock*, 1, SystemAllocPolicy> loopWorkList;
    BitSet loopDone(graph.numBlockIds());
    if (!loopDone.init(alloc()))
        return false;

    for (size_t i = graph.numBlocks(); i > 0; i--) {
        if (mir->shouldCancel("Build Liveness Info (main loop)"))
            return false;

        LBlock* block = graph.getBlock(i - 1);
        MBasicBlock* mblock = block->mir();

        BitSet& live = liveIn[mblock->id()];
        new (&live) BitSet(graph.numVirtualRegisters());
        if (!live.init(alloc()))
            return false;

        // Propagate liveIn from our successors to us.
        for (size_t i = 0; i < mblock->lastIns()->numSuccessors(); i++) {
            MBasicBlock* successor = mblock->lastIns()->getSuccessor(i);
            // Skip backedges, as we fix them up at the loop header.
            if (mblock->id() < successor->id())
                live.insertAll(liveIn[successor->id()]);
        }

        // Add successor phis.
        if (mblock->successorWithPhis()) {
            LBlock* phiSuccessor = mblock->successorWithPhis()->lir();
            for (unsigned int j = 0; j < phiSuccessor->numPhis(); j++) {
                LPhi* phi = phiSuccessor->getPhi(j);
                LAllocation* use = phi->getOperand(mblock->positionInPhiSuccessor());
                uint32_t reg = use->toUse()->virtualRegister();
                live.insert(reg);
                vreg(use).setUsedByPhi();
            }
        }

        // Registers are assumed alive for the entire block, a define shortens
        // the range to the point of definition.
        for (BitSet::Iterator liveRegId(live); liveRegId; ++liveRegId) {
            if (!vregs[*liveRegId].addInitialRange(alloc(), entryOf(block), exitOf(block).next()))
                return false;
        }

        // Shorten the front end of ranges for live variables to their point of
        // definition, if found.
        for (LInstructionReverseIterator ins = block->rbegin(); ins != block->rend(); ins++) {
            // Calls may clobber registers, so force a spill and reload around the callsite.
            if (ins->isCall()) {
                for (AnyRegisterIterator iter(allRegisters_.asLiveSet()); iter.more(); iter++) {
                    bool found = false;
                    for (size_t i = 0; i < ins->numDefs(); i++) {
                        if (ins->getDef(i)->isFixed() &&
                            ins->getDef(i)->output()->aliases(LAllocation(*iter))) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        if (!addInitialFixedRange(*iter, outputOf(*ins), outputOf(*ins).next()))
                            return false;
                    }
                }
                if (!callRanges->addRange(alloc(), 0, outputOf(*ins), outputOf(*ins).next()))
                    return false;
            }
            DebugOnly<bool> hasDoubleDef = false;
            DebugOnly<bool> hasFloat32Def = false;
            for (size_t i = 0; i < ins->numDefs(); i++) {
                LDefinition* def = ins->getDef(i);
                if (def->isBogusTemp())
                    continue;
#ifdef DEBUG
                if (def->type() == LDefinition::DOUBLE)
                    hasDoubleDef = true;
                if (def->type() == LDefinition::FLOAT32)
                    hasFloat32Def = true;
#endif
                CodePosition from = outputOf(*ins);

                if (def->policy() == LDefinition::MUST_REUSE_INPUT) {
                    // MUST_REUSE_INPUT is implemented by allocating an output
                    // register and moving the input to it. Register hints are
                    // used to avoid unnecessary moves. We give the input an
                    // LUse::ANY policy to avoid allocating a register for the
                    // input.
                    LUse* inputUse = ins->getOperand(def->getReusedInput())->toUse();
                    MOZ_ASSERT(inputUse->policy() == LUse::REGISTER);
                    MOZ_ASSERT(inputUse->usedAtStart());
                    *inputUse = LUse(inputUse->virtualRegister(), LUse::ANY, /* usedAtStart = */ true);
                }

                if (!vreg(def).addInitialRange(alloc(), from, from.next()))
                    return false;
                vreg(def).setInitialDefinition(from);
                live.remove(def->virtualRegister());
            }

            for (size_t i = 0; i < ins->numTemps(); i++) {
                LDefinition* temp = ins->getTemp(i);
                if (temp->isBogusTemp())
                    continue;

                // Normally temps are considered to cover both the input
                // and output of the associated instruction. In some cases
                // though we want to use a fixed register as both an input
                // and clobbered register in the instruction, so watch for
                // this and shorten the temp to cover only the output.
                CodePosition from = inputOf(*ins);
                if (temp->policy() == LDefinition::FIXED) {
                    AnyRegister reg = temp->output()->toRegister();
                    for (LInstruction::InputIterator alloc(**ins); alloc.more(); alloc.next()) {
                        if (alloc->isUse()) {
                            LUse* use = alloc->toUse();
                            if (use->isFixedRegister()) {
                                if (GetFixedRegister(vreg(use).def(), use) == reg)
                                    from = outputOf(*ins);
                            }
                        }
                    }
                }

                CodePosition to =
                    ins->isCall() ? outputOf(*ins) : outputOf(*ins).next();

                if (!vreg(temp).addInitialRange(alloc(), from, to))
                    return false;
                vreg(temp).setInitialDefinition(from);
            }

            DebugOnly<bool> hasUseRegister = false;
            DebugOnly<bool> hasUseRegisterAtStart = false;

            for (LInstruction::InputIterator inputAlloc(**ins); inputAlloc.more(); inputAlloc.next()) {
                if (inputAlloc->isUse()) {
                    LUse* use = inputAlloc->toUse();

                    // Call uses should always be at-start or fixed, since
                    // calls use all registers.
                    MOZ_ASSERT_IF(ins->isCall() && !inputAlloc.isSnapshotInput(),
                                  use->isFixedRegister() || use->usedAtStart());

#ifdef DEBUG
                    // Don't allow at-start call uses if there are temps of the same kind,
                    // so that we don't assign the same register.
                    if (ins->isCall() && use->usedAtStart()) {
                        for (size_t i = 0; i < ins->numTemps(); i++)
                            MOZ_ASSERT(vreg(ins->getTemp(i)).type() != vreg(use).type());
                    }

                    // If there are both useRegisterAtStart(x) and useRegister(y)
                    // uses, we may assign the same register to both operands
                    // (bug 772830). Don't allow this for now.
                    if (use->policy() == LUse::REGISTER) {
                        if (use->usedAtStart()) {
                            if (!IsInputReused(*ins, use))
                                hasUseRegisterAtStart = true;
                        } else {
                            hasUseRegister = true;
                        }
                    }
                    MOZ_ASSERT(!(hasUseRegister && hasUseRegisterAtStart));
#endif

                    // Don't treat RECOVERED_INPUT uses as keeping the vreg alive.
                    if (use->policy() == LUse::RECOVERED_INPUT)
                        continue;

                    // Fixed uses on calls are specially overridden to happen
                    // at the input position.
                    CodePosition to =
                        (use->usedAtStart() || (ins->isCall() && use->isFixedRegister()))
                        ? inputOf(*ins)
                        : outputOf(*ins);
                    if (use->isFixedRegister()) {
                        LAllocation reg(AnyRegister::FromCode(use->registerCode()));
                        for (size_t i = 0; i < ins->numDefs(); i++) {
                            LDefinition* def = ins->getDef(i);
                            if (def->policy() == LDefinition::FIXED && *def->output() == reg)
                                to = inputOf(*ins);
                        }
                    }

                    if (!vreg(use).addInitialRange(alloc(), entryOf(block), to.next()))
                        return false;
                    UsePosition* usePosition = new(alloc()) UsePosition(use, to);
                    if (!usePosition)
                        return false;
                    vreg(use).addInitialUse(usePosition);
                    live.insert(use->virtualRegister());
                }
            }
        }

        // Phis have simultaneous assignment semantics at block begin, so at
        // the beginning of the block we can be sure that liveIn does not
        // contain any phi outputs.
        for (unsigned int i = 0; i < block->numPhis(); i++) {
            LDefinition* def = block->getPhi(i)->getDef(0);
            if (live.contains(def->virtualRegister())) {
                live.remove(def->virtualRegister());
            } else {
                // This is a dead phi, so add a dummy range over all phis. This
                // can go away if we have an earlier dead code elimination pass.
                CodePosition entryPos = entryOf(block);
                if (!vreg(def).addInitialRange(alloc(), entryPos, entryPos.next()))
                    return false;
            }
        }

        if (mblock->isLoopHeader()) {
            // A divergence from the published algorithm is required here, as
            // our block order does not guarantee that blocks of a loop are
            // contiguous. As a result, a single live range spanning the
            // loop is not possible. Additionally, we require liveIn in a later
            // pass for resolution, so that must also be fixed up here.
            MBasicBlock* loopBlock = mblock->backedge();
            while (true) {
                // Blocks must already have been visited to have a liveIn set.
                MOZ_ASSERT(loopBlock->id() >= mblock->id());

                // Add a range for this entire loop block
                CodePosition from = entryOf(loopBlock->lir());
                CodePosition to = exitOf(loopBlock->lir()).next();

                for (BitSet::Iterator liveRegId(live); liveRegId; ++liveRegId) {
                    if (!vregs[*liveRegId].addInitialRange(alloc(), from, to))
                        return false;
                }

                // Fix up the liveIn set.
                liveIn[loopBlock->id()].insertAll(live);

                // Make sure we don't visit this node again
                loopDone.insert(loopBlock->id());

                // If this is the loop header, any predecessors are either the
                // backedge or out of the loop, so skip any predecessors of
                // this block
                if (loopBlock != mblock) {
                    for (size_t i = 0; i < loopBlock->numPredecessors(); i++) {
                        MBasicBlock* pred = loopBlock->getPredecessor(i);
                        if (loopDone.contains(pred->id()))
                            continue;
                        if (!loopWorkList.append(pred))
                            return false;
                    }
                }

                // Terminate loop if out of work.
                if (loopWorkList.empty())
                    break;

                // Grab the next block off the work list, skipping any OSR block.
                MBasicBlock* osrBlock = graph.mir().osrBlock();
                while (!loopWorkList.empty()) {
                    loopBlock = loopWorkList.popCopy();
                    if (loopBlock != osrBlock)
                        break;
                }

                // If end is reached without finding a non-OSR block, then no more work items were found.
                if (loopBlock == osrBlock) {
                    MOZ_ASSERT(loopWorkList.empty());
                    break;
                }
            }

            // Clear the done set for other loops
            loopDone.clear();
        }

        MOZ_ASSERT_IF(!mblock->numPredecessors(), live.empty());
    }

    JitSpew(JitSpew_RegAlloc, "Liveness analysis complete");

    if (JitSpewEnabled(JitSpew_RegAlloc))
        dumpInstructions();

    return true;
}

bool
BacktrackingAllocator::go()
{
    JitSpew(JitSpew_RegAlloc, "Beginning register allocation");

    if (!init())
        return false;

    if (!buildLivenessInfo())
        return false;

    if (JitSpewEnabled(JitSpew_RegAlloc))
        dumpFixedRanges();

    if (!allocationQueue.reserve(graph.numVirtualRegisters() * 3 / 2))
        return false;

    JitSpew(JitSpew_RegAlloc, "Beginning grouping and queueing registers");
    if (!mergeAndQueueRegisters())
        return false;

    if (JitSpewEnabled(JitSpew_RegAlloc))
        dumpVregs();

    JitSpew(JitSpew_RegAlloc, "Beginning main allocation loop");

    // Allocate, spill and split bundles until finished.
    while (!allocationQueue.empty()) {
        if (mir->shouldCancel("Backtracking Allocation"))
            return false;

        QueueItem item = allocationQueue.removeHighest();
        if (!processBundle(item.bundle))
            return false;
    }
    JitSpew(JitSpew_RegAlloc, "Main allocation loop complete");

    if (!pickStackSlots())
        return false;

    if (JitSpewEnabled(JitSpew_RegAlloc))
        dumpAllocations();

    if (!resolveControlFlow())
        return false;

    if (!reifyAllocations())
        return false;

    if (!populateSafepoints())
        return false;

    if (!annotateMoveGroups())
        return false;

    return true;
}

static bool
IsArgumentSlotDefinition(LDefinition* def)
{
    return def->policy() == LDefinition::FIXED && def->output()->isArgument();
}

static bool
IsThisSlotDefinition(LDefinition* def)
{
    return IsArgumentSlotDefinition(def) &&
        def->output()->toArgument()->index() < THIS_FRAME_ARGSLOT + sizeof(Value);
}

bool
BacktrackingAllocator::tryMergeBundles(LiveBundle* bundle0, LiveBundle* bundle1)
{
    // See if bundle0 and bundle1 can be merged together.
    if (bundle0 == bundle1)
        return true;

    // Get a representative virtual register from each bundle.
    VirtualRegister& reg0 = vregs[bundle0->firstRange()->vreg()];
    VirtualRegister& reg1 = vregs[bundle1->firstRange()->vreg()];

    if (!reg0.isCompatible(reg1))
        return true;

    // Registers which might spill to the frame's |this| slot can only be
    // grouped with other such registers. The frame's |this| slot must always
    // hold the |this| value, as required by JitFrame tracing and by the Ion
    // constructor calling convention.
    if (IsThisSlotDefinition(reg0.def()) || IsThisSlotDefinition(reg1.def())) {
        if (*reg0.def()->output() != *reg1.def()->output())
            return true;
    }

    // Registers which might spill to the frame's argument slots can only be
    // grouped with other such registers if the frame might access those
    // arguments through a lazy arguments object or rest parameter.
    if (IsArgumentSlotDefinition(reg0.def()) || IsArgumentSlotDefinition(reg1.def())) {
        if (graph.mir().entryBlock()->info().mayReadFrameArgsDirectly()) {
            if (*reg0.def()->output() != *reg1.def()->output())
                return true;
        }
    }

    // Limit the number of times we compare ranges if there are many ranges in
    // one of the bundles, to avoid quadratic behavior.
    static const size_t MAX_RANGES = 200;

    // Make sure that ranges in the bundles do not overlap.
    LiveRange::BundleLinkIterator iter0 = bundle0->rangesBegin(), iter1 = bundle1->rangesBegin();
    size_t count = 0;
    while (iter0 && iter1) {
        if (++count >= MAX_RANGES)
            return true;

        LiveRange* range0 = LiveRange::get(*iter0);
        LiveRange* range1 = LiveRange::get(*iter1);

        if (range0->from() >= range1->to())
            iter1++;
        else if (range1->from() >= range0->to())
            iter0++;
        else
            return true;
    }

    // Move all ranges from bundle1 into bundle0.
    while (LiveRange* range = bundle1->popFirstRange())
        bundle0->addRange(range);

    return true;
}

static inline LDefinition*
FindReusingDefinition(LNode* ins, LAllocation* alloc)
{
    for (size_t i = 0; i < ins->numDefs(); i++) {
        LDefinition* def = ins->getDef(i);
        if (def->policy() == LDefinition::MUST_REUSE_INPUT &&
            ins->getOperand(def->getReusedInput()) == alloc)
            return def;
    }
    for (size_t i = 0; i < ins->numTemps(); i++) {
        LDefinition* def = ins->getTemp(i);
        if (def->policy() == LDefinition::MUST_REUSE_INPUT &&
            ins->getOperand(def->getReusedInput()) == alloc)
            return def;
    }
    return nullptr;
}

bool
BacktrackingAllocator::tryMergeReusedRegister(VirtualRegister& def, VirtualRegister& input)
{
    // def is a vreg which reuses input for its output physical register. Try
    // to merge ranges for def with those of input if possible, as avoiding
    // copies before def's instruction is crucial for generated code quality
    // (MUST_REUSE_INPUT is used for all arithmetic on x86/x64).

    if (def.rangeFor(inputOf(def.ins()))) {
        MOZ_ASSERT(def.isTemp());
        def.setMustCopyInput();
        return true;
    }

    LiveRange* inputRange = input.rangeFor(outputOf(def.ins()));
    if (!inputRange) {
        // The input is not live after the instruction, either in a safepoint
        // for the instruction or in subsequent code. The input and output
        // can thus be in the same group.
        return tryMergeBundles(def.firstBundle(), input.firstBundle());
    }

    // The input is live afterwards, either in future instructions or in a
    // safepoint for the reusing instruction. This is impossible to satisfy
    // without copying the input.
    //
    // It may or may not be better to split the input into two bundles at the
    // point of the definition, which may permit merging. One case where it is
    // definitely better to split is if the input never has any register uses
    // after the instruction. Handle this splitting eagerly.

    LBlock* block = def.ins()->block();

    // The input's lifetime must end within the same block as the definition,
    // otherwise it could live on in phis elsewhere.
    if (inputRange != input.lastRange() || inputRange->to() > exitOf(block)) {
        def.setMustCopyInput();
        return true;
    }

    // If we already split the input for some other register, don't make a
    // third bundle.
    if (inputRange->bundle() != input.firstRange()->bundle()) {
        def.setMustCopyInput();
        return true;
    }

    // If the input will start out in memory then adding a separate bundle for
    // memory uses after the def won't help.
    if (input.def()->isFixed() && !input.def()->output()->isRegister()) {
        def.setMustCopyInput();
        return true;
    }

    // The input cannot have register or reused uses after the definition.
    for (UsePositionIterator iter = inputRange->usesBegin(); iter; iter++) {
        if (iter->pos <= inputOf(def.ins()))
            continue;

        LUse* use = iter->use;
        if (FindReusingDefinition(insData[iter->pos], use)) {
            def.setMustCopyInput();
            return true;
        }
        if (use->policy() != LUse::ANY && use->policy() != LUse::KEEPALIVE) {
            def.setMustCopyInput();
            return true;
        }
    }

    LiveRange* preRange = LiveRange::New(alloc(), input.vreg(),
                                         inputRange->from(), outputOf(def.ins()));
    if (!preRange)
        return false;

    // The new range starts at reg's input position, which means it overlaps
    // with the old range at one position. This is what we want, because we
    // need to copy the input before the instruction.
    LiveRange* postRange = LiveRange::New(alloc(), input.vreg(),
                                          inputOf(def.ins()), inputRange->to());
    if (!postRange)
        return false;

    inputRange->distributeUses(preRange);
    inputRange->distributeUses(postRange);
    MOZ_ASSERT(!inputRange->hasUses());

    JitSpew(JitSpew_RegAlloc, "  splitting reused input at %u to try to help grouping",
            inputOf(def.ins()));

    LiveBundle* firstBundle = inputRange->bundle();
    input.removeRange(inputRange);
    input.addRange(preRange);
    input.addRange(postRange);

    firstBundle->removeRange(inputRange);
    firstBundle->addRange(preRange);

    // The new range goes in a separate bundle, where it will be spilled during
    // allocation.
    LiveBundle* secondBundle = LiveBundle::New(alloc(), nullptr, nullptr);
    if (!secondBundle)
        return false;
    secondBundle->addRange(postRange);

    return tryMergeBundles(def.firstBundle(), input.firstBundle());
}

bool
BacktrackingAllocator::mergeAndQueueRegisters()
{
    MOZ_ASSERT(!vregs[0u].hasRanges());

    // Create a bundle for each register containing all its ranges.
    for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
        VirtualRegister& reg = vregs[i];
        if (!reg.hasRanges())
            continue;

        LiveBundle* bundle = LiveBundle::New(alloc(), nullptr, nullptr);
        if (!bundle)
            return false;
        for (LiveRange::RegisterLinkIterator iter = reg.rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);
            bundle->addRange(range);
        }
    }

    // If there is an OSR block, merge parameters in that block with the
    // corresponding parameters in the initial block.
    if (MBasicBlock* osr = graph.mir().osrBlock()) {
        size_t original = 1;
        for (LInstructionIterator iter = osr->lir()->begin(); iter != osr->lir()->end(); iter++) {
            if (iter->isParameter()) {
                for (size_t i = 0; i < iter->numDefs(); i++) {
                    DebugOnly<bool> found = false;
                    VirtualRegister &paramVreg = vreg(iter->getDef(i));
                    for (; original < paramVreg.vreg(); original++) {
                        VirtualRegister &originalVreg = vregs[original];
                        if (*originalVreg.def()->output() == *iter->getDef(i)->output()) {
                            MOZ_ASSERT(originalVreg.ins()->isParameter());
                            if (!tryMergeBundles(originalVreg.firstBundle(), paramVreg.firstBundle()))
                                return false;
                            found = true;
                            break;
                        }
                    }
                    MOZ_ASSERT(found);
                }
            }
        }
    }

    // Try to merge registers with their reused inputs.
    for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
        VirtualRegister& reg = vregs[i];
        if (!reg.hasRanges())
            continue;

        if (reg.def()->policy() == LDefinition::MUST_REUSE_INPUT) {
            LUse* use = reg.ins()->getOperand(reg.def()->getReusedInput())->toUse();
            if (!tryMergeReusedRegister(reg, vreg(use)))
                return false;
        }
    }

    // Try to merge phis with their inputs.
    for (size_t i = 0; i < graph.numBlocks(); i++) {
        LBlock* block = graph.getBlock(i);
        for (size_t j = 0; j < block->numPhis(); j++) {
            LPhi* phi = block->getPhi(j);
            VirtualRegister &outputVreg = vreg(phi->getDef(0));
            for (size_t k = 0, kend = phi->numOperands(); k < kend; k++) {
                VirtualRegister& inputVreg = vreg(phi->getOperand(k)->toUse());
                if (!tryMergeBundles(inputVreg.firstBundle(), outputVreg.firstBundle()))
                    return false;
            }
        }
    }

    // Add all bundles to the allocation queue, and create spill sets for them.
    for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
        VirtualRegister& reg = vregs[i];
        for (LiveRange::RegisterLinkIterator iter = reg.rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);
            LiveBundle* bundle = range->bundle();
            if (range == bundle->firstRange()) {
                SpillSet* spill = SpillSet::New(alloc());
                if (!spill)
                    return false;
                bundle->setSpillSet(spill);

                size_t priority = computePriority(bundle);
                if (!allocationQueue.insert(QueueItem(bundle, priority)))
                    return false;
            }
        }
    }

    return true;
}

static const size_t MAX_ATTEMPTS = 2;

bool
BacktrackingAllocator::tryAllocateFixed(LiveBundle* bundle, Requirement requirement,
                                        bool* success, bool* pfixed,
                                        LiveBundleVector& conflicting)
{
    // Spill bundles which are required to be in a certain stack slot.
    if (!requirement.allocation().isRegister()) {
        JitSpew(JitSpew_RegAlloc, "  stack allocation requirement");
        bundle->setAllocation(requirement.allocation());
        *success = true;
        return true;
    }

    AnyRegister reg = requirement.allocation().toRegister();
    return tryAllocateRegister(registers[reg.code()], bundle, success, pfixed, conflicting);
}

bool
BacktrackingAllocator::tryAllocateNonFixed(LiveBundle* bundle,
                                           Requirement requirement, Requirement hint,
                                           bool* success, bool* pfixed,
                                           LiveBundleVector& conflicting)
{
    // If we want, but do not require a bundle to be in a specific register,
    // only look at that register for allocating and evict or spill if it is
    // not available. Picking a separate register may be even worse than
    // spilling, as it will still necessitate moves and will tie up more
    // registers than if we spilled.
    if (hint.kind() == Requirement::FIXED) {
        AnyRegister reg = hint.allocation().toRegister();
        if (!tryAllocateRegister(registers[reg.code()], bundle, success, pfixed, conflicting))
            return false;
        if (*success)
            return true;
    }

    // Spill bundles which have no hint or register requirement.
    if (requirement.kind() == Requirement::NONE && hint.kind() != Requirement::REGISTER) {
        if (!spill(bundle))
            return false;
        *success = true;
        return true;
    }

    if (conflicting.empty() || minimalBundle(bundle)) {
        // Search for any available register which the bundle can be
        // allocated to.
        for (size_t i = 0; i < AnyRegister::Total; i++) {
            if (!tryAllocateRegister(registers[i], bundle, success, pfixed, conflicting))
                return false;
            if (*success)
                return true;
        }
    }

    // Spill bundles which have no register requirement if they didn't get
    // allocated.
    if (requirement.kind() == Requirement::NONE) {
        if (!spill(bundle))
            return false;
        *success = true;
        return true;
    }

    // We failed to allocate this bundle.
    MOZ_ASSERT(!*success);
    return true;
}

bool
BacktrackingAllocator::processBundle(LiveBundle* bundle)
{
    if (JitSpewEnabled(JitSpew_RegAlloc)) {
        JitSpew(JitSpew_RegAlloc, "Allocating %s [priority %lu] [weight %lu]",
                bundle->toString(), computePriority(bundle), computeSpillWeight(bundle));
    }

    // A bundle can be processed by doing any of the following:
    //
    // - Assigning the bundle a register. The bundle cannot overlap any other
    //   bundle allocated for that physical register.
    //
    // - Spilling the bundle, provided it has no register uses.
    //
    // - Splitting the bundle into two or more bundles which cover the original
    //   one. The new bundles are placed back onto the priority queue for later
    //   processing.
    //
    // - Evicting one or more existing allocated bundles, and then doing one
    //   of the above operations. Evicted bundles are placed back on the
    //   priority queue. Any evicted bundles must have a lower spill weight
    //   than the bundle being processed.
    //
    // As long as this structure is followed, termination is guaranteed.
    // In general, we want to minimize the amount of bundle splitting (which
    // generally necessitates spills), so allocate longer lived, lower weight
    // bundles first and evict and split them later if they prevent allocation
    // for higher weight bundles.

    Requirement requirement, hint;
    bool canAllocate = computeRequirement(bundle, &requirement, &hint);

    bool fixed;
    LiveBundleVector conflicting;
    for (size_t attempt = 0;; attempt++) {
        if (canAllocate) {
            bool success = false;
            fixed = false;
            conflicting.clear();

            // Ok, let's try allocating for this bundle.
            if (requirement.kind() == Requirement::FIXED) {
                if (!tryAllocateFixed(bundle, requirement, &success, &fixed, conflicting))
                    return false;
            } else {
                if (!tryAllocateNonFixed(bundle, requirement, hint, &success, &fixed, conflicting))
                    return false;
            }

            // If that worked, we're done!
            if (success)
                return true;

            // If that didn't work, but we have one or more non-fixed bundles
            // known to be conflicting, maybe we can evict them and try again.
            if (attempt < MAX_ATTEMPTS &&
                !fixed &&
                !conflicting.empty() &&
                maximumSpillWeight(conflicting) < computeSpillWeight(bundle))
                {
                    for (size_t i = 0; i < conflicting.length(); i++) {
                        if (!evictBundle(conflicting[i]))
                            return false;
                    }
                    continue;
                }
        }

        // A minimal bundle cannot be split any further. If we try to split it
        // it at this point we will just end up with the same bundle and will
        // enter an infinite loop. Weights and the initial live ranges must
        // be constructed so that any minimal bundle is allocatable.
        MOZ_ASSERT(!minimalBundle(bundle));

        LiveBundle* conflict = conflicting.empty() ? nullptr : conflicting[0];
        return chooseBundleSplit(bundle, canAllocate && fixed, conflict);
    }
}

bool
BacktrackingAllocator::computeRequirement(LiveBundle* bundle,
                                          Requirement *requirement, Requirement *hint)
{
    // Set any requirement or hint on bundle according to its definition and
    // uses. Return false if there are conflicting requirements which will
    // require the bundle to be split.

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        VirtualRegister &reg = vregs[range->vreg()];

        if (range->hasDefinition()) {
            // Deal with any definition constraints/hints.
            LDefinition::Policy policy = reg.def()->policy();
            if (policy == LDefinition::FIXED) {
                // Fixed policies get a FIXED requirement.
                JitSpew(JitSpew_RegAlloc, "  Requirement %s, fixed by definition",
                        reg.def()->output()->toString());
                if (!requirement->merge(Requirement(*reg.def()->output())))
                    return false;
            } else if (reg.ins()->isPhi()) {
                // Phis don't have any requirements, but they should prefer their
                // input allocations. This is captured by the group hints above.
            } else {
                // Non-phis get a REGISTER requirement.
                if (!requirement->merge(Requirement(Requirement::REGISTER)))
                    return false;
            }
        }

        // Search uses for requirements.
        for (UsePositionIterator iter = range->usesBegin(); iter; iter++) {
            LUse::Policy policy = iter->use->policy();
            if (policy == LUse::FIXED) {
                AnyRegister required = GetFixedRegister(reg.def(), iter->use);

                JitSpew(JitSpew_RegAlloc, "  Requirement %s, due to use at %u",
                        required.name(), iter->pos.bits());

                // If there are multiple fixed registers which the bundle is
                // required to use, fail. The bundle will need to be split before
                // it can be allocated.
                if (!requirement->merge(Requirement(LAllocation(required))))
                    return false;
            } else if (policy == LUse::REGISTER) {
                if (!requirement->merge(Requirement(Requirement::REGISTER)))
                    return false;
            } else if (policy == LUse::ANY) {
                // ANY differs from KEEPALIVE by actively preferring a register.
                hint->merge(Requirement(Requirement::REGISTER));
            }
        }
    }

    return true;
}

bool
BacktrackingAllocator::tryAllocateRegister(PhysicalRegister& r, LiveBundle* bundle,
                                           bool* success, bool* pfixed, LiveBundleVector& conflicting)
{
    *success = false;

    if (!r.allocatable)
        return true;

    LiveBundleVector aliasedConflicting;

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        VirtualRegister &reg = vregs[range->vreg()];

        if (!reg.isCompatible(r.reg))
            return true;

        for (size_t a = 0; a < r.reg.numAliased(); a++) {
            PhysicalRegister& rAlias = registers[r.reg.aliased(a).code()];
            LiveRange* existing;
            if (!rAlias.allocations.contains(range, &existing))
                continue;
            if (existing->hasVreg()) {
                MOZ_ASSERT(existing->bundle()->allocation().toRegister() == rAlias.reg);
                bool duplicate = false;
                for (size_t i = 0; i < aliasedConflicting.length(); i++) {
                    if (aliasedConflicting[i] == existing->bundle()) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate && !aliasedConflicting.append(existing->bundle()))
                    return false;
            } else {
                JitSpew(JitSpew_RegAlloc, "  %s collides with fixed use %s",
                        rAlias.reg.name(), existing->toString());
                *pfixed = true;
                return true;
            }
        }
    }

    if (!aliasedConflicting.empty()) {
        // One or more aliased registers is allocated to another bundle
        // overlapping this one. Keep track of the conflicting set, and in the
        // case of multiple conflicting sets keep track of the set with the
        // lowest maximum spill weight.

        // The #ifdef guards against "unused variable 'existing'" bustage.
#ifdef JS_JITSPEW
        if (JitSpewEnabled(JitSpew_RegAlloc)) {
            if (aliasedConflicting.length() == 1) {
                LiveBundle* existing = aliasedConflicting[0];
                JitSpew(JitSpew_RegAlloc, "  %s collides with %s [weight %lu]",
                        r.reg.name(), existing->toString(), computeSpillWeight(existing));
            } else {
                JitSpew(JitSpew_RegAlloc, "  %s collides with the following", r.reg.name());
                for (size_t i = 0; i < aliasedConflicting.length(); i++) {
                    LiveBundle* existing = aliasedConflicting[i];
                    JitSpew(JitSpew_RegAlloc, "      %s [weight %lu]",
                            existing->toString(), computeSpillWeight(existing));
                }
            }
        }
#endif

        if (conflicting.empty()) {
            if (!conflicting.appendAll(aliasedConflicting))
                return false;
        } else {
            if (maximumSpillWeight(aliasedConflicting) < maximumSpillWeight(conflicting)) {
                conflicting.clear();
                if (!conflicting.appendAll(aliasedConflicting))
                    return false;
            }
        }
        return true;
    }

    JitSpew(JitSpew_RegAlloc, "  allocated to %s", r.reg.name());

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        if (!r.allocations.insert(range))
            return false;
    }

    bundle->setAllocation(LAllocation(r.reg));
    *success = true;
    return true;
}

bool
BacktrackingAllocator::evictBundle(LiveBundle* bundle)
{
    if (JitSpewEnabled(JitSpew_RegAlloc)) {
        JitSpew(JitSpew_RegAlloc, "  Evicting %s [priority %lu] [weight %lu]",
                bundle->toString(), computePriority(bundle), computeSpillWeight(bundle));
    }

    AnyRegister reg(bundle->allocation().toRegister());
    PhysicalRegister& physical = registers[reg.code()];
    MOZ_ASSERT(physical.reg == reg && physical.allocatable);

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        physical.allocations.remove(range);
    }

    bundle->setAllocation(LAllocation());

    size_t priority = computePriority(bundle);
    return allocationQueue.insert(QueueItem(bundle, priority));
}

bool
BacktrackingAllocator::splitAndRequeueBundles(LiveBundle* bundle,
                                              const LiveBundleVector& newBundles)
{
    if (JitSpewEnabled(JitSpew_RegAlloc)) {
        JitSpew(JitSpew_RegAlloc, "    splitting bundle %s into:", bundle->toString());
        for (size_t i = 0; i < newBundles.length(); i++)
            JitSpew(JitSpew_RegAlloc, "      %s", newBundles[i]->toString());
    }

    // Remove all ranges in the old bundle from their register's list.
    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        vregs[range->vreg()].removeRange(range);
    }

    // Add all ranges in the new bundles to their register's list.
    for (size_t i = 0; i < newBundles.length(); i++) {
        LiveBundle* newBundle = newBundles[i];
        for (LiveRange::BundleLinkIterator iter = newBundle->rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);
            vregs[range->vreg()].addRange(range);
        }
    }

    // Queue the new bundles for register assignment.
    for (size_t i = 0; i < newBundles.length(); i++) {
        LiveBundle* newBundle = newBundles[i];
        size_t priority = computePriority(newBundle);
        if (!allocationQueue.insert(QueueItem(newBundle, priority)))
            return false;
    }

    return true;
}

bool
BacktrackingAllocator::spill(LiveBundle* bundle)
{
    JitSpew(JitSpew_RegAlloc, "  Spilling bundle");
    MOZ_ASSERT(bundle->allocation().isBogus());

    if (LiveBundle* spillParent = bundle->spillParent()) {
        JitSpew(JitSpew_RegAlloc, "    Using existing spill bundle");
        for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);
            LiveRange* parentRange = spillParent->rangeFor(range->from());
            MOZ_ASSERT(parentRange->contains(range));
            MOZ_ASSERT(range->vreg() == parentRange->vreg());
            range->distributeUses(parentRange);
            MOZ_ASSERT(!range->hasUses());
            vregs[range->vreg()].removeRange(range);
        }
        return true;
    }

    return bundle->spillSet()->addSpilledBundle(bundle);
}

bool
BacktrackingAllocator::pickStackSlots()
{
    for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
        VirtualRegister& reg = vregs[i];

        if (mir->shouldCancel("Backtracking Pick Stack Slots"))
            return false;

        for (LiveRange::RegisterLinkIterator iter = reg.rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);
            LiveBundle* bundle = range->bundle();

            if (bundle->allocation().isBogus()) {
                if (!pickStackSlot(bundle->spillSet()))
                    return false;
                MOZ_ASSERT(!bundle->allocation().isBogus());
            }
        }
    }

    return true;
}

bool
BacktrackingAllocator::pickStackSlot(SpillSet* spillSet)
{
    // Look through all ranges that have been spilled in this set for a
    // register definition which is fixed to a stack or argument slot. If we
    // find one, use it for all bundles that have been spilled. tryMergeBundles
    // makes sure this reuse is possible when an initial bundle contains ranges
    // from multiple virtual registers.
    for (size_t i = 0; i < spillSet->numSpilledBundles(); i++) {
        LiveBundle* bundle = spillSet->spilledBundle(i);
        for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);
            if (range->hasDefinition()) {
                LDefinition* def = vregs[range->vreg()].def();
                if (def->policy() == LDefinition::FIXED) {
                    MOZ_ASSERT(!def->output()->isRegister());
                    MOZ_ASSERT(!def->output()->isStackSlot());
                    spillSet->setAllocation(*def->output());
                    return true;
                }
            }
        }
    }

    LDefinition::Type type = vregs[spillSet->spilledBundle(0)->firstRange()->vreg()].type();

    SpillSlotList* slotList;
    switch (StackSlotAllocator::width(type)) {
      case 4:  slotList = &normalSlots; break;
      case 8:  slotList = &doubleSlots; break;
      case 16: slotList = &quadSlots;   break;
      default:
        MOZ_CRASH("Bad width");
    }

    // Maximum number of existing spill slots we will look at before giving up
    // and allocating a new slot.
    static const size_t MAX_SEARCH_COUNT = 10;

    size_t searches = 0;
    SpillSlot* stop = nullptr;
    while (!slotList->empty()) {
        SpillSlot* spillSlot = *slotList->begin();
        if (!stop) {
            stop = spillSlot;
        } else if (stop == spillSlot) {
            // We looked through every slot in the list.
            break;
        }

        bool success = true;
        for (size_t i = 0; i < spillSet->numSpilledBundles(); i++) {
            LiveBundle* bundle = spillSet->spilledBundle(i);
            for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
                LiveRange* range = LiveRange::get(*iter);
                LiveRange* existing;
                if (spillSlot->allocated.contains(range, &existing)) {
                    success = false;
                    break;
                }
            }
            if (!success)
                break;
        }
        if (success) {
            // We can reuse this physical stack slot for the new bundles.
            // Update the allocated ranges for the slot.
            for (size_t i = 0; i < spillSet->numSpilledBundles(); i++) {
                LiveBundle* bundle = spillSet->spilledBundle(i);
                if (!insertAllRanges(spillSlot->allocated, bundle))
                    return false;
            }
            spillSet->setAllocation(spillSlot->alloc);
            return true;
        }

        // On a miss, move the spill to the end of the list. This will cause us
        // to make fewer attempts to allocate from slots with a large and
        // highly contended range.
        slotList->popFront();
        slotList->pushBack(spillSlot);

        if (++searches == MAX_SEARCH_COUNT)
            break;
    }

    // We need a new physical stack slot.
    uint32_t stackSlot = stackSlotAllocator.allocateSlot(type);

    SpillSlot* spillSlot = new(alloc()) SpillSlot(stackSlot, alloc().lifoAlloc());
    if (!spillSlot)
        return false;

    for (size_t i = 0; i < spillSet->numSpilledBundles(); i++) {
        LiveBundle* bundle = spillSet->spilledBundle(i);
        if (!insertAllRanges(spillSlot->allocated, bundle))
            return false;
    }

    spillSet->setAllocation(spillSlot->alloc);

    slotList->pushFront(spillSlot);
    return true;
}

bool
BacktrackingAllocator::insertAllRanges(LiveRangeSet& set, LiveBundle* bundle)
{
    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        if (!set.insert(range))
            return false;
    }
    return true;
}

bool
BacktrackingAllocator::deadRange(LiveRange* range)
{
    // Check for direct uses of this range.
    if (range->hasUses() || range->hasDefinition())
        return false;

    CodePosition start = range->from();
    LNode* ins = insData[start];
    if (start == entryOf(ins->block()))
        return false;

    VirtualRegister& reg = vregs[range->vreg()];

    // Check if there are later ranges for this vreg.
    LiveRange::RegisterLinkIterator iter = reg.rangesBegin(range);
    for (iter++; iter; iter++) {
        LiveRange* laterRange = LiveRange::get(*iter);
        if (laterRange->from() > range->from())
            return false;
    }

    // Check if this range ends at a loop backedge.
    LNode* last = insData[range->to().previous()];
    if (last->isGoto() && last->toGoto()->target()->id() < last->block()->mir()->id())
        return false;

    // Check if there are phis which this vreg flows to.
    if (reg.usedByPhi())
        return false;

    return true;
}

bool
BacktrackingAllocator::resolveControlFlow()
{
    // Add moves to handle changing assignments for vregs over their lifetime.
    JitSpew(JitSpew_RegAlloc, "Resolving control flow (vreg loop)");

    // Look for places where a register's assignment changes in the middle of a
    // basic block.
    MOZ_ASSERT(!vregs[0u].hasRanges());
    for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
        VirtualRegister& reg = vregs[i];

        if (mir->shouldCancel("Backtracking Resolve Control Flow (vreg loop)"))
            return false;

        if (!alloc().ensureBallast())
            return false;

        for (LiveRange::RegisterLinkIterator iter = reg.rangesBegin(); iter; ) {
            LiveRange* range = LiveRange::get(*iter);

            // Remove ranges which will never be used.
            if (deadRange(range)) {
                reg.removeRangeAndIncrement(iter);
                continue;
            }

            // The range which defines the register does not have a predecessor
            // to add moves from.
            if (range->hasDefinition()) {
                iter++;
                continue;
            }

            // Ignore ranges that start at block boundaries. We will handle
            // these in the next phase.
            CodePosition start = range->from();
            LNode* ins = insData[start];
            if (start == entryOf(ins->block())) {
                iter++;
                continue;
            }

            // If we already saw a range which covers the start of this range
            // and has the same allocation, we don't need an explicit move at
            // the start of this range.
            bool skip = false;
            for (LiveRange::RegisterLinkIterator prevIter = reg.rangesBegin();
                 prevIter != iter;
                 prevIter++)
            {
                LiveRange* prevRange = LiveRange::get(*prevIter);
                if (prevRange->covers(start) &&
                    prevRange->bundle()->allocation() == range->bundle()->allocation())
                {
                    skip = true;
                    break;
                }
            }
            if (skip) {
                iter++;
                continue;
            }

            LiveRange* predecessorRange = reg.rangeFor(start.previous(), /* preferRegister = */ true);
            if (start.subpos() == CodePosition::INPUT) {
                if (!moveInput(ins->toInstruction(), predecessorRange, range, reg.type()))
                    return false;
            } else {
                if (!moveAfter(ins->toInstruction(), predecessorRange, range, reg.type()))
                    return false;
            }

            iter++;
        }
    }

    JitSpew(JitSpew_RegAlloc, "Resolving control flow (block loop)");

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        if (mir->shouldCancel("Backtracking Resolve Control Flow (block loop)"))
            return false;

        LBlock* successor = graph.getBlock(i);
        MBasicBlock* mSuccessor = successor->mir();
        if (mSuccessor->numPredecessors() < 1)
            continue;

        // Resolve phis to moves.
        for (size_t j = 0; j < successor->numPhis(); j++) {
            LPhi* phi = successor->getPhi(j);
            MOZ_ASSERT(phi->numDefs() == 1);
            LDefinition* def = phi->getDef(0);
            VirtualRegister& reg = vreg(def);
            LiveRange* to = reg.rangeFor(entryOf(successor));
            MOZ_ASSERT(to);

            for (size_t k = 0; k < mSuccessor->numPredecessors(); k++) {
                LBlock* predecessor = mSuccessor->getPredecessor(k)->lir();
                MOZ_ASSERT(predecessor->mir()->numSuccessors() == 1);

                LAllocation* input = phi->getOperand(k);
                LiveRange* from = vreg(input).rangeFor(exitOf(predecessor), /* preferRegister = */ true);
                MOZ_ASSERT(from);

                if (!moveAtExit(predecessor, from, to, def->type()))
                    return false;
            }
        }

        // Add moves to resolve graph edges with different allocations at their
        // source and target.
        BitSet& live = liveIn[mSuccessor->id()];

        for (BitSet::Iterator liveRegId(live); liveRegId; ++liveRegId) {
            VirtualRegister& reg = vregs[*liveRegId];

            for (size_t j = 0; j < mSuccessor->numPredecessors(); j++) {
                LBlock* predecessor = mSuccessor->getPredecessor(j)->lir();

                for (LiveRange::RegisterLinkIterator iter = reg.rangesBegin(); iter; iter++) {
                    LiveRange* to = LiveRange::get(*iter);
                    if (!to->covers(entryOf(successor)))
                        continue;
                    if (to->covers(exitOf(predecessor)))
                        continue;

                    LiveRange* from = reg.rangeFor(exitOf(predecessor), /* preferRegister = */ true);

                    if (mSuccessor->numPredecessors() > 1) {
                        MOZ_ASSERT(predecessor->mir()->numSuccessors() == 1);
                        if (!moveAtExit(predecessor, from, to, reg.type()))
                            return false;
                    } else {
                        if (!moveAtEntry(successor, from, to, reg.type()))
                            return false;
                    }
                }
            }
        }
    }

    return true;
}

bool
BacktrackingAllocator::isReusedInput(LUse* use, LNode* ins, bool considerCopy)
{
    if (LDefinition* def = FindReusingDefinition(ins, use))
        return considerCopy || !vregs[def->virtualRegister()].mustCopyInput();
    return false;
}

bool
BacktrackingAllocator::isRegisterUse(LUse* use, LNode* ins, bool considerCopy)
{
    switch (use->policy()) {
      case LUse::ANY:
        return isReusedInput(use, ins, considerCopy);

      case LUse::REGISTER:
      case LUse::FIXED:
        return true;

      default:
        return false;
    }
}

bool
BacktrackingAllocator::isRegisterDefinition(LiveRange* range)
{
    if (!range->hasDefinition())
        return false;

    VirtualRegister& reg = vregs[range->vreg()];
    if (reg.ins()->isPhi())
        return false;

    if (reg.def()->policy() == LDefinition::FIXED && !reg.def()->output()->isRegister())
        return false;

    return true;
}

bool
BacktrackingAllocator::reifyAllocations()
{
    JitSpew(JitSpew_RegAlloc, "Reifying Allocations");

    MOZ_ASSERT(!vregs[0u].hasRanges());
    for (size_t i = 1; i < graph.numVirtualRegisters(); i++) {
        VirtualRegister& reg = vregs[i];

        if (mir->shouldCancel("Backtracking Reify Allocations (main loop)"))
            return false;

        for (LiveRange::RegisterLinkIterator iter = reg.rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);

            if (range->hasDefinition()) {
                reg.def()->setOutput(range->bundle()->allocation());
                if (reg.ins()->recoversInput()) {
                    LSnapshot* snapshot = reg.ins()->toInstruction()->snapshot();
                    for (size_t i = 0; i < snapshot->numEntries(); i++) {
                        LAllocation* entry = snapshot->getEntry(i);
                        if (entry->isUse() && entry->toUse()->policy() == LUse::RECOVERED_INPUT)
                            *entry = *reg.def()->output();
                    }
                }
            }

            for (UsePositionIterator iter(range->usesBegin()); iter; iter++) {
                LAllocation* alloc = iter->use;
                *alloc = range->bundle()->allocation();

                // For any uses which feed into MUST_REUSE_INPUT definitions,
                // add copies if the use and def have different allocations.
                LNode* ins = insData[iter->pos];
                if (LDefinition* def = FindReusingDefinition(ins, alloc)) {
                    LiveRange* outputRange = vreg(def).rangeFor(outputOf(ins));
                    LAllocation res = outputRange->bundle()->allocation();
                    LAllocation sourceAlloc = range->bundle()->allocation();

                    if (res != *alloc) {
                        LMoveGroup* group = getInputMoveGroup(ins->toInstruction());
                        if (!group->addAfter(sourceAlloc, res, reg.type()))
                            return false;
                        *alloc = res;
                    }
                }
            }

            addLiveRegistersForRange(reg, range);
        }
    }

    graph.setLocalSlotCount(stackSlotAllocator.stackHeight());
    return true;
}

size_t
BacktrackingAllocator::findFirstNonCallSafepoint(CodePosition from)
{
    size_t i = 0;
    for (; i < graph.numNonCallSafepoints(); i++) {
        const LInstruction* ins = graph.getNonCallSafepoint(i);
        if (from <= inputOf(ins))
            break;
    }
    return i;
}

void
BacktrackingAllocator::addLiveRegistersForRange(VirtualRegister& reg, LiveRange* range)
{
    // Fill in the live register sets for all non-call safepoints.
    LAllocation a = range->bundle()->allocation();
    if (!a.isRegister())
        return;

    // Don't add output registers to the safepoint.
    CodePosition start = range->from();
    if (range->hasDefinition() && !reg.isTemp()) {
#ifdef CHECK_OSIPOINT_REGISTERS
        // We don't add the output register to the safepoint,
        // but it still might get added as one of the inputs.
        // So eagerly add this reg to the safepoint clobbered registers.
        if (reg.ins()->isInstruction()) {
            if (LSafepoint* safepoint = reg.ins()->toInstruction()->safepoint())
                safepoint->addClobberedRegister(a.toRegister());
        }
#endif
        start = start.next();
    }

    size_t i = findFirstNonCallSafepoint(start);
    for (; i < graph.numNonCallSafepoints(); i++) {
        LInstruction* ins = graph.getNonCallSafepoint(i);
        CodePosition pos = inputOf(ins);

        // Safepoints are sorted, so we can shortcut out of this loop
        // if we go out of range.
        if (range->to() <= pos)
            break;

        MOZ_ASSERT(range->covers(pos));

        LSafepoint* safepoint = ins->safepoint();
        safepoint->addLiveRegister(a.toRegister());

#ifdef CHECK_OSIPOINT_REGISTERS
        if (reg.isTemp())
            safepoint->addClobberedRegister(a.toRegister());
#endif
    }
}

static inline bool
IsNunbox(VirtualRegister& reg)
{
#ifdef JS_NUNBOX32
    return reg.type() == LDefinition::TYPE ||
           reg.type() == LDefinition::PAYLOAD;
#else
    return false;
#endif
}

static inline bool
IsSlotsOrElements(VirtualRegister& reg)
{
    return reg.type() == LDefinition::SLOTS;
}

static inline bool
IsTraceable(VirtualRegister& reg)
{
    if (reg.type() == LDefinition::OBJECT)
        return true;
#ifdef JS_PUNBOX64
    if (reg.type() == LDefinition::BOX)
        return true;
#endif
    return false;
}

size_t
BacktrackingAllocator::findFirstSafepoint(CodePosition pos, size_t startFrom)
{
    size_t i = startFrom;
    for (; i < graph.numSafepoints(); i++) {
        LInstruction* ins = graph.getSafepoint(i);
        if (pos <= inputOf(ins))
            break;
    }
    return i;
}

bool
BacktrackingAllocator::populateSafepoints()
{
    JitSpew(JitSpew_RegAlloc, "Populating Safepoints");

    size_t firstSafepoint = 0;

    MOZ_ASSERT(!vregs[0u].def());
    for (uint32_t i = 1; i < graph.numVirtualRegisters(); i++) {
        VirtualRegister& reg = vregs[i];

        if (!reg.def() || (!IsTraceable(reg) && !IsSlotsOrElements(reg) && !IsNunbox(reg)))
            continue;

        firstSafepoint = findFirstSafepoint(inputOf(reg.ins()), firstSafepoint);
        if (firstSafepoint >= graph.numSafepoints())
            break;

        for (LiveRange::RegisterLinkIterator iter = reg.rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);

            for (size_t j = firstSafepoint; j < graph.numSafepoints(); j++) {
                LInstruction* ins = graph.getSafepoint(j);

                if (!range->covers(inputOf(ins))) {
                    if (inputOf(ins) >= range->to())
                        break;
                    continue;
                }

                // Include temps but not instruction outputs. Also make sure
                // MUST_REUSE_INPUT is not used with gcthings or nunboxes, or
                // we would have to add the input reg to this safepoint.
                if (ins == reg.ins() && !reg.isTemp()) {
                    DebugOnly<LDefinition*> def = reg.def();
                    MOZ_ASSERT_IF(def->policy() == LDefinition::MUST_REUSE_INPUT,
                                  def->type() == LDefinition::GENERAL ||
                                  def->type() == LDefinition::INT32 ||
                                  def->type() == LDefinition::FLOAT32 ||
                                  def->type() == LDefinition::DOUBLE);
                    continue;
                }

                LSafepoint* safepoint = ins->safepoint();

                LAllocation a = range->bundle()->allocation();
                if (a.isGeneralReg() && ins->isCall())
                    continue;

                switch (reg.type()) {
                  case LDefinition::OBJECT:
                    if (!safepoint->addGcPointer(a))
                        return false;
                    break;
                  case LDefinition::SLOTS:
                    if (!safepoint->addSlotsOrElementsPointer(a))
                        return false;
                    break;
#ifdef JS_NUNBOX32
                  case LDefinition::TYPE:
                    if (!safepoint->addNunboxType(i, a))
                        return false;
                    break;
                  case LDefinition::PAYLOAD:
                    if (!safepoint->addNunboxPayload(i, a))
                        return false;
                    break;
#else
                  case LDefinition::BOX:
                    if (!safepoint->addBoxedValue(a))
                        return false;
                    break;
#endif
                  default:
                    MOZ_CRASH("Bad register type");
                }
            }
        }
    }

    return true;
}

bool
BacktrackingAllocator::annotateMoveGroups()
{
    // Annotate move groups in the LIR graph with any register that is not
    // allocated at that point and can be used as a scratch register. This is
    // only required for x86, as other platforms always have scratch registers
    // available for use.
#ifdef JS_CODEGEN_X86
    LiveRange* range = LiveRange::New(alloc(), 0, CodePosition(), CodePosition().next());
    if (!range)
        return false;

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        if (mir->shouldCancel("Backtracking Annotate Move Groups"))
            return false;

        LBlock* block = graph.getBlock(i);
        LInstruction* last = nullptr;
        for (LInstructionIterator iter = block->begin(); iter != block->end(); ++iter) {
            if (iter->isMoveGroup()) {
                CodePosition from = last ? outputOf(last) : entryOf(block);
                range->setTo(from.next());
                range->setFrom(from);

                for (size_t i = 0; i < AnyRegister::Total; i++) {
                    PhysicalRegister& reg = registers[i];
                    if (reg.reg.isFloat() || !reg.allocatable)
                        continue;

                    // This register is unavailable for use if (a) it is in use
                    // by some live range immediately before the move group,
                    // or (b) it is an operand in one of the group's moves. The
                    // latter case handles live ranges which end immediately
                    // before the move group or start immediately after.
                    // For (b) we need to consider move groups immediately
                    // preceding or following this one.

                    if (iter->toMoveGroup()->uses(reg.reg.gpr()))
                        continue;
                    bool found = false;
                    LInstructionIterator niter(iter);
                    for (niter++; niter != block->end(); niter++) {
                        if (niter->isMoveGroup()) {
                            if (niter->toMoveGroup()->uses(reg.reg.gpr())) {
                                found = true;
                                break;
                            }
                        } else {
                            break;
                        }
                    }
                    if (iter != block->begin()) {
                        LInstructionIterator riter(iter);
                        do {
                            riter--;
                            if (riter->isMoveGroup()) {
                                if (riter->toMoveGroup()->uses(reg.reg.gpr())) {
                                    found = true;
                                    break;
                                }
                            } else {
                                break;
                            }
                        } while (riter != block->begin());
                    }

                    LiveRange* existing;
                    if (found || reg.allocations.contains(range, &existing))
                        continue;

                    iter->toMoveGroup()->setScratchRegister(reg.reg.gpr());
                    break;
                }
            } else {
                last = *iter;
            }
        }
    }
#endif

    return true;
}

/////////////////////////////////////////////////////////////////////
// Debugging methods
/////////////////////////////////////////////////////////////////////

#ifdef DEBUG

const char*
LiveRange::toString() const
{
    // Not reentrant!
    static char buf[2000];

    char* cursor = buf;
    char* end = cursor + sizeof(buf);

    int n = JS_snprintf(cursor, end - cursor, "v%u [%u,%u)",
                        hasVreg() ? vreg() : 0, from().bits(), to().bits());
    if (n < 0) MOZ_CRASH();
    cursor += n;

    if (bundle() && !bundle()->allocation().isBogus()) {
        n = JS_snprintf(cursor, end - cursor, " %s", bundle()->allocation().toString());
        if (n < 0) MOZ_CRASH();
        cursor += n;
    }

    if (hasDefinition()) {
        n = JS_snprintf(cursor, end - cursor, " (def)");
        if (n < 0) MOZ_CRASH();
        cursor += n;
    }

    for (UsePositionIterator iter = usesBegin(); iter; iter++) {
        n = JS_snprintf(cursor, end - cursor, " %s@%u", iter->use->toString(), iter->pos.bits());
        if (n < 0) MOZ_CRASH();
        cursor += n;
    }

    return buf;
}

const char*
LiveBundle::toString() const
{
    // Not reentrant!
    static char buf[2000];

    char* cursor = buf;
    char* end = cursor + sizeof(buf);

    for (LiveRange::BundleLinkIterator iter = rangesBegin(); iter; iter++) {
        int n = JS_snprintf(cursor, end - cursor, "%s %s",
                            (iter == rangesBegin()) ? "" : " ##",
                            LiveRange::get(*iter)->toString());
        if (n < 0) MOZ_CRASH();
        cursor += n;
    }

    return buf;
}

#endif // DEBUG

void
BacktrackingAllocator::dumpVregs()
{
#ifdef DEBUG
    MOZ_ASSERT(!vregs[0u].hasRanges());

    fprintf(stderr, "Live ranges by virtual register:\n");

    for (uint32_t i = 1; i < graph.numVirtualRegisters(); i++) {
        fprintf(stderr, "  ");
        VirtualRegister& reg = vregs[i];
        for (LiveRange::RegisterLinkIterator iter = reg.rangesBegin(); iter; iter++) {
            if (iter != reg.rangesBegin())
                fprintf(stderr, " ## ");
            fprintf(stderr, "%s", LiveRange::get(*iter)->toString());
        }
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "\nLive ranges by bundle:\n");

    for (uint32_t i = 1; i < graph.numVirtualRegisters(); i++) {
        VirtualRegister& reg = vregs[i];
        for (LiveRange::RegisterLinkIterator baseIter = reg.rangesBegin(); baseIter; baseIter++) {
            LiveRange* range = LiveRange::get(*baseIter);
            LiveBundle* bundle = range->bundle();
            if (range == bundle->firstRange()) {
                fprintf(stderr, "  ");
                for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
                    if (iter != bundle->rangesBegin())
                        fprintf(stderr, " ## ");
                    fprintf(stderr, "%s", LiveRange::get(*iter)->toString());
                }
                fprintf(stderr, "\n");
            }
        }
    }
#endif
}

void
BacktrackingAllocator::dumpFixedRanges()
{
#ifdef DEBUG
    fprintf(stderr, "Live ranges by physical register: %s\n", callRanges->toString());
#endif // DEBUG
}

#ifdef DEBUG
struct BacktrackingAllocator::PrintLiveRange
{
    bool& first_;

    explicit PrintLiveRange(bool& first) : first_(first) {}

    void operator()(const LiveRange* range)
    {
        if (first_)
            first_ = false;
        else
            fprintf(stderr, " /");
        fprintf(stderr, " %s", range->toString());
    }
};
#endif

void
BacktrackingAllocator::dumpAllocations()
{
#ifdef DEBUG
    fprintf(stderr, "Allocations:\n");

    dumpVregs();

    fprintf(stderr, "Allocations by physical register:\n");

    for (size_t i = 0; i < AnyRegister::Total; i++) {
        if (registers[i].allocatable && !registers[i].allocations.empty()) {
            fprintf(stderr, "  %s:", AnyRegister::FromCode(i).name());
            bool first = true;
            registers[i].allocations.forEach(PrintLiveRange(first));
            fprintf(stderr, "\n");
        }
    }

    fprintf(stderr, "\n");
#endif // DEBUG
}

///////////////////////////////////////////////////////////////////////////////
// Heuristic Methods
///////////////////////////////////////////////////////////////////////////////

size_t
BacktrackingAllocator::computePriority(LiveBundle* bundle)
{
    // The priority of a bundle is its total length, so that longer lived
    // bundles will be processed before shorter ones (even if the longer ones
    // have a low spill weight). See processBundle().
    size_t lifetimeTotal = 0;

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        lifetimeTotal += range->to() - range->from();
    }

    return lifetimeTotal;
}

bool
BacktrackingAllocator::minimalDef(LiveRange* range, LNode* ins)
{
    // Whether this is a minimal range capturing a definition at ins.
    return (range->to() <= minimalDefEnd(ins).next()) &&
           ((!ins->isPhi() && range->from() == inputOf(ins)) || range->from() == outputOf(ins));
}

bool
BacktrackingAllocator::minimalUse(LiveRange* range, UsePosition* use)
{
    // Whether this is a minimal range capturing |use|.
    LNode* ins = insData[use->pos];
    return (range->from() == inputOf(ins)) &&
           (range->to() == (use->use->usedAtStart() ? outputOf(ins) : outputOf(ins).next()));
}

bool
BacktrackingAllocator::minimalBundle(LiveBundle* bundle, bool* pfixed)
{
    LiveRange::BundleLinkIterator iter = bundle->rangesBegin();
    LiveRange* range = LiveRange::get(*iter);

    if (!range->hasVreg()) {
        *pfixed = true;
        return true;
    }

    // If a bundle contains multiple ranges, splitAtAllRegisterUses will split
    // each range into a separate bundle.
    if (++iter)
        return false;

    if (range->hasDefinition()) {
        VirtualRegister& reg = vregs[range->vreg()];
        if (pfixed)
            *pfixed = reg.def()->policy() == LDefinition::FIXED && reg.def()->output()->isRegister();
        return minimalDef(range, reg.ins());
    }

    bool fixed = false, minimal = false, multiple = false;

    for (UsePositionIterator iter = range->usesBegin(); iter; iter++) {
        if (iter != range->usesBegin())
            multiple = true;
        LUse* use = iter->use;

        switch (use->policy()) {
          case LUse::FIXED:
            if (fixed)
                return false;
            fixed = true;
            if (minimalUse(range, *iter))
                minimal = true;
            break;

          case LUse::REGISTER:
            if (minimalUse(range, *iter))
                minimal = true;
            break;

          default:
            break;
        }
    }

    // If a range contains a fixed use and at least one other use,
    // splitAtAllRegisterUses will split each use into a different bundle.
    if (multiple && fixed)
        minimal = false;

    if (pfixed)
        *pfixed = fixed;
    return minimal;
}

size_t
BacktrackingAllocator::computeSpillWeight(LiveBundle* bundle)
{
    // Minimal bundles have an extremely high spill weight, to ensure they
    // can evict any other bundles and be allocated to a register.
    bool fixed;
    if (minimalBundle(bundle, &fixed))
        return fixed ? 2000000 : 1000000;

    size_t usesTotal = 0;
    fixed = false;

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);

        if (range->hasDefinition()) {
            VirtualRegister& reg = vregs[range->vreg()];
            if (reg.def()->policy() == LDefinition::FIXED && reg.def()->output()->isRegister()) {
                usesTotal += 2000;
                fixed = true;
            } else if (!reg.ins()->isPhi()) {
                usesTotal += 2000;
            }
        }

        for (UsePositionIterator iter = range->usesBegin(); iter; iter++) {
            LUse* use = iter->use;

            switch (use->policy()) {
              case LUse::ANY:
                usesTotal += 1000;
                break;

              case LUse::FIXED:
                fixed = true;
              case LUse::REGISTER:
                usesTotal += 2000;
                break;

              case LUse::KEEPALIVE:
                break;

              default:
                // Note: RECOVERED_INPUT will not appear in UsePositionIterator.
                MOZ_CRASH("Bad use");
            }
        }
    }

    // Bundles with fixed uses are given a higher spill weight, since they must
    // be allocated to a specific register.
    if (testbed && fixed)
        usesTotal *= 2;

    // Compute spill weight as a use density, lowering the weight for long
    // lived bundles with relatively few uses.
    size_t lifetimeTotal = computePriority(bundle);
    return lifetimeTotal ? usesTotal / lifetimeTotal : 0;
}

size_t
BacktrackingAllocator::maximumSpillWeight(const LiveBundleVector& bundles)
{
    size_t maxWeight = 0;
    for (size_t i = 0; i < bundles.length(); i++)
        maxWeight = Max(maxWeight, computeSpillWeight(bundles[i]));
    return maxWeight;
}

bool
BacktrackingAllocator::trySplitAcrossHotcode(LiveBundle* bundle, bool* success)
{
    // If this bundle has portions that are hot and portions that are cold,
    // split it at the boundaries between hot and cold code.

    LiveRange* hotRange = nullptr;

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        if (hotcode.contains(range, &hotRange))
            break;
    }

    // Don't split if there is no hot code in the bundle.
    if (!hotRange) {
        JitSpew(JitSpew_RegAlloc, "  bundle does not contain hot code");
        return true;
    }

    // Don't split if there is no cold code in the bundle.
    bool coldCode = false;
    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        if (!hotRange->contains(range)) {
            coldCode = true;
            break;
        }
    }
    if (!coldCode) {
        JitSpew(JitSpew_RegAlloc, "  bundle does not contain cold code");
        return true;
    }

    JitSpew(JitSpew_RegAlloc, "  split across hot range %s", hotRange->toString());

    // Tweak the splitting method when compiling asm.js code to look at actual
    // uses within the hot/cold code. This heuristic is in place as the below
    // mechanism regresses several asm.js tests. Hopefully this will be fixed
    // soon and this special case removed. See bug 948838.
    if (compilingAsmJS()) {
        SplitPositionVector splitPositions;
        if (!splitPositions.append(hotRange->from()) || !splitPositions.append(hotRange->to()))
            return false;
        *success = true;
        return splitAt(bundle, splitPositions);
    }

    LiveBundle* hotBundle = LiveBundle::New(alloc(), bundle->spillSet(), bundle->spillParent());
    if (!hotBundle)
        return false;
    LiveBundle* preBundle = nullptr;
    LiveBundle* postBundle = nullptr;
    LiveBundle* coldBundle = nullptr;

    if (testbed) {
        coldBundle = LiveBundle::New(alloc(), bundle->spillSet(), bundle->spillParent());
        if (!coldBundle)
            return false;
    }

    // Accumulate the ranges of hot and cold code in the bundle. Note that
    // we are only comparing with the single hot range found, so the cold code
    // may contain separate hot ranges.
    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);
        LiveRange::Range hot, coldPre, coldPost;
        range->intersect(hotRange, &coldPre, &hot, &coldPost);

        if (!hot.empty()) {
            if (!hotBundle->addRangeAndDistributeUses(alloc(), range, hot.from, hot.to))
                return false;
        }

        if (!coldPre.empty()) {
            if (testbed) {
                if (!coldBundle->addRangeAndDistributeUses(alloc(), range, coldPre.from, coldPre.to))
                    return false;
            } else {
                if (!preBundle) {
                    preBundle = LiveBundle::New(alloc(), bundle->spillSet(), bundle->spillParent());
                    if (!preBundle)
                        return false;
                }
                if (!preBundle->addRangeAndDistributeUses(alloc(), range, coldPre.from, coldPre.to))
                    return false;
            }
        }

        if (!coldPost.empty()) {
            if (testbed) {
                if (!coldBundle->addRangeAndDistributeUses(alloc(), range, coldPost.from, coldPost.to))
                    return false;
            } else {
                if (!postBundle) {
                    postBundle = LiveBundle::New(alloc(), bundle->spillSet(), bundle->spillParent());
                    if (!postBundle)
                        return false;
                }
                if (!postBundle->addRangeAndDistributeUses(alloc(), range, coldPost.from, coldPost.to))
                    return false;
            }
        }
    }

    MOZ_ASSERT(hotBundle->numRanges() != 0);

    LiveBundleVector newBundles;
    if (!newBundles.append(hotBundle))
        return false;

    if (testbed) {
        MOZ_ASSERT(coldBundle->numRanges() != 0);
        if (!newBundles.append(coldBundle))
            return false;
    } else {
        MOZ_ASSERT(preBundle || postBundle);
        if (preBundle && !newBundles.append(preBundle))
            return false;
        if (postBundle && !newBundles.append(postBundle))
            return false;
    }

    *success = true;
    return splitAndRequeueBundles(bundle, newBundles);
}

bool
BacktrackingAllocator::trySplitAfterLastRegisterUse(LiveBundle* bundle, LiveBundle* conflict,
                                                    bool* success)
{
    // If this bundle's later uses do not require it to be in a register,
    // split it after the last use which does require a register. If conflict
    // is specified, only consider register uses before the conflict starts.

    CodePosition lastRegisterFrom, lastRegisterTo, lastUse;

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);

        // If the range defines a register, consider that a register use for
        // our purposes here.
        if (isRegisterDefinition(range)) {
            CodePosition spillStart = minimalDefEnd(insData[range->from()]).next();
            if (!conflict || spillStart < conflict->firstRange()->from()) {
                lastUse = lastRegisterFrom = range->from();
                lastRegisterTo = spillStart;
            }
        }

        for (UsePositionIterator iter(range->usesBegin()); iter; iter++) {
            LUse* use = iter->use;
            LNode* ins = insData[iter->pos];

            // Uses in the bundle should be sorted.
            MOZ_ASSERT(iter->pos >= lastUse);
            lastUse = inputOf(ins);

            if (!conflict || outputOf(ins) < conflict->firstRange()->from()) {
                if (isRegisterUse(use, ins, /* considerCopy = */ true)) {
                    lastRegisterFrom = inputOf(ins);
                    lastRegisterTo = iter->pos.next();
                }
            }
        }
    }

    // Can't trim non-register uses off the end by splitting.
    if (!lastRegisterFrom.bits()) {
        JitSpew(JitSpew_RegAlloc, "  bundle has no register uses");
        return true;
    }
    if (lastUse < lastRegisterTo) {
        JitSpew(JitSpew_RegAlloc, "  bundle's last use is a register use");
        return true;
    }

    JitSpew(JitSpew_RegAlloc, "  split after last register use at %u",
            lastRegisterTo.bits());

    SplitPositionVector splitPositions;
    if (!splitPositions.append(lastRegisterTo))
        return false;
    *success = true;
    return splitAt(bundle, splitPositions);
}

bool
BacktrackingAllocator::trySplitBeforeFirstRegisterUse(LiveBundle* bundle, LiveBundle* conflict, bool* success)
{
    // If this bundle's earlier uses do not require it to be in a register,
    // split it before the first use which does require a register. If conflict
    // is specified, only consider register uses after the conflict ends.

    if (isRegisterDefinition(bundle->firstRange())) {
        JitSpew(JitSpew_RegAlloc, "  bundle is defined by a register");
        return true;
    }
    if (!bundle->firstRange()->hasDefinition()) {
        JitSpew(JitSpew_RegAlloc, "  bundle does not have definition");
        return true;
    }

    CodePosition firstRegisterFrom;

    CodePosition conflictEnd;
    if (conflict) {
        for (LiveRange::BundleLinkIterator iter = conflict->rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);
            if (range->to() > conflictEnd)
                conflictEnd = range->to();
        }
    }

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);

        for (UsePositionIterator iter(range->usesBegin()); iter; iter++) {
            LUse* use = iter->use;
            LNode* ins = insData[iter->pos];

            if (!conflict || outputOf(ins) >= conflictEnd) {
                if (isRegisterUse(use, ins, /* considerCopy = */ true)) {
                    firstRegisterFrom = inputOf(ins);
                    break;
                }
            }
        }
    }

    if (!firstRegisterFrom.bits()) {
        // Can't trim non-register uses off the beginning by splitting.
        JitSpew(JitSpew_RegAlloc, "  bundle has no register uses");
        return true;
    }

    JitSpew(JitSpew_RegAlloc, "  split before first register use at %u",
            firstRegisterFrom.bits());

    SplitPositionVector splitPositions;
    if (!splitPositions.append(firstRegisterFrom))
        return false;
    *success = true;
    return splitAt(bundle, splitPositions);
}

// When splitting a bundle according to a list of split positions, return
// whether a use or range at |pos| should use a different bundle than the last
// position this was called for. 
static bool
UseNewBundle(const SplitPositionVector& splitPositions, CodePosition pos,
             size_t* activeSplitPosition)
{
    if (splitPositions.empty()) {
        // When the split positions are empty we are splitting at all uses.
        return true;
    }

    if (*activeSplitPosition == splitPositions.length()) {
        // We've advanced past all split positions.
        return false;
    }

    if (splitPositions[*activeSplitPosition] > pos) {
        // We haven't gotten to the next split position yet.
        return false;
    }

    // We've advanced past the next split position, find the next one which we
    // should split at.
    while (*activeSplitPosition < splitPositions.length() &&
           splitPositions[*activeSplitPosition] <= pos)
    {
        (*activeSplitPosition)++;
    }
    return true;
}

static bool
HasPrecedingRangeSharingVreg(LiveBundle* bundle, LiveRange* range)
{
    MOZ_ASSERT(range->bundle() == bundle);

    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* prevRange = LiveRange::get(*iter);
        if (prevRange == range)
            return false;
        if (prevRange->vreg() == range->vreg())
            return true;
    }

    MOZ_CRASH();
}

static bool
HasFollowingRangeSharingVreg(LiveBundle* bundle, LiveRange* range)
{
    MOZ_ASSERT(range->bundle() == bundle);

    bool foundRange = false;
    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* prevRange = LiveRange::get(*iter);
        if (foundRange && prevRange->vreg() == range->vreg())
            return true;
        if (prevRange == range)
            foundRange = true;
    }

    MOZ_ASSERT(foundRange);
    return false;
}

bool
BacktrackingAllocator::splitAt(LiveBundle* bundle, const SplitPositionVector& splitPositions)
{
    // Split the bundle at the given split points. Register uses which have no
    // intervening split points are consolidated into the same bundle. If the
    // list of split points is empty, then all register uses are placed in
    // minimal bundles.

    // splitPositions should be sorted.
    for (size_t i = 1; i < splitPositions.length(); ++i)
        MOZ_ASSERT(splitPositions[i-1] < splitPositions[i]);

    // We don't need to create a new spill bundle if there already is one.
    bool spillBundleIsNew = false;
    LiveBundle* spillBundle = bundle->spillParent();
    if (!spillBundle) {
        spillBundle = LiveBundle::New(alloc(), bundle->spillSet(), nullptr);
        if (!spillBundle)
            return false;
        spillBundleIsNew = true;

        for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
            LiveRange* range = LiveRange::get(*iter);

            CodePosition from = range->from();
            if (isRegisterDefinition(range))
                from = minimalDefEnd(insData[from]).next();

            if (from < range->to()) {
                if (!spillBundle->addRange(alloc(), range->vreg(), from, range->to()))
                    return false;

                if (range->hasDefinition() && !isRegisterDefinition(range))
                    spillBundle->lastRange()->setHasDefinition();
            }
        }
    }

    LiveBundleVector newBundles;

    // The bundle which ranges are currently being added to.
    LiveBundle* activeBundle = LiveBundle::New(alloc(), bundle->spillSet(), spillBundle);
    if (!newBundles.append(activeBundle))
        return false;

    // State for use by UseNewBundle.
    size_t activeSplitPosition = 0;

    // Make new bundles according to the split positions, and distribute ranges
    // and uses to them.
    for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; iter++) {
        LiveRange* range = LiveRange::get(*iter);

        if (UseNewBundle(splitPositions, range->from(), &activeSplitPosition)) {
            activeBundle = LiveBundle::New(alloc(), bundle->spillSet(), spillBundle);
            if (!newBundles.append(activeBundle))
                return false;
        }

        LiveRange* activeRange = LiveRange::New(alloc(), range->vreg(), range->from(), range->to());
        if (!activeRange)
            return false;
        activeBundle->addRange(activeRange);

        if (isRegisterDefinition(range))
            activeRange->setHasDefinition();

        while (range->hasUses()) {
            UsePosition* use = range->popUse();
            LNode* ins = insData[use->pos];

            // Any uses of a register that appear before its definition has
            // finished must be associated with the range for that definition.
            if (isRegisterDefinition(range) && use->pos <= minimalDefEnd(insData[range->from()])) {
                activeRange->addUse(use);
            } else if (isRegisterUse(use->use, ins)) {
                // Place this register use into a different bundle from the
                // last one if there are any split points between the two uses.
                // UseNewBundle always returns true if we are splitting at all
                // register uses, but we can still reuse the last range and
                // bundle if they have uses at the same position, except when
                // either use is fixed (the two uses might require incompatible
                // registers.)
                if (UseNewBundle(splitPositions, use->pos, &activeSplitPosition) &&
                    (!activeRange->hasUses() ||
                     activeRange->usesBegin()->pos != use->pos ||
                     activeRange->usesBegin()->use->policy() == LUse::FIXED ||
                     use->use->policy() == LUse::FIXED))
                {
                    activeBundle = LiveBundle::New(alloc(), bundle->spillSet(), spillBundle);
                    if (!newBundles.append(activeBundle))
                        return false;
                    activeRange = LiveRange::New(alloc(), range->vreg(), range->from(), range->to());
                    if (!activeRange)
                        return false;
                    activeBundle->addRange(activeRange);
                }

                activeRange->addUse(use);
            } else {
                MOZ_ASSERT(spillBundleIsNew);
                spillBundle->rangeFor(use->pos)->addUse(use);
            }
        }
    }

    LiveBundleVector filteredBundles;

    // Trim the ends of ranges in each new bundle when there are no other
    // earlier or later ranges in the same bundle with the same vreg.
    for (size_t i = 0; i < newBundles.length(); i++) {
        LiveBundle* bundle = newBundles[i];

        for (LiveRange::BundleLinkIterator iter = bundle->rangesBegin(); iter; ) {
            LiveRange* range = LiveRange::get(*iter);

            if (!range->hasDefinition()) {
                if (!HasPrecedingRangeSharingVreg(bundle, range)) {
                    if (range->hasUses()) {
                        UsePosition* use = *range->usesBegin();
                        range->setFrom(inputOf(insData[use->pos]));
                    } else {
                        bundle->removeRangeAndIncrementIterator(iter);
                        continue;
                    }
                }
            }

            if (!HasFollowingRangeSharingVreg(bundle, range)) {
                if (range->hasUses()) {
                    UsePosition* use = range->lastUse();
                    range->setTo(use->pos.next());
                } else if (range->hasDefinition()) {
                    range->setTo(minimalDefEnd(insData[range->from()]).next());
                } else {
                    bundle->removeRangeAndIncrementIterator(iter);
                    continue;
                }
            }

            iter++;
        }

        if (bundle->hasRanges() && !filteredBundles.append(bundle))
            return false;
    }

    if (spillBundleIsNew && !filteredBundles.append(spillBundle))
        return false;

    return splitAndRequeueBundles(bundle, filteredBundles);
}

bool
BacktrackingAllocator::splitAcrossCalls(LiveBundle* bundle)
{
    // Split the bundle to separate register uses and non-register uses and
    // allow the vreg to be spilled across its range.

    // Find the locations of all calls in the bundle's range.
    SplitPositionVector callPositions;
    for (LiveRange::BundleLinkIterator iter = callRanges->rangesBegin(); iter; iter++) {
        LiveRange* callRange = LiveRange::get(*iter);
        if (bundle->rangeFor(callRange->from()) && bundle->rangeFor(callRange->from().previous())) {
            if (!callPositions.append(callRange->from()))
                return false;
        }
    }
    MOZ_ASSERT(callPositions.length());

#ifdef JS_JITSPEW
    JitSpewStart(JitSpew_RegAlloc, "  split across calls at ");
    for (size_t i = 0; i < callPositions.length(); ++i)
        JitSpewCont(JitSpew_RegAlloc, "%s%u", i != 0 ? ", " : "", callPositions[i].bits());
    JitSpewFin(JitSpew_RegAlloc);
#endif

    return splitAt(bundle, callPositions);
}

bool
BacktrackingAllocator::chooseBundleSplit(LiveBundle* bundle, bool fixed, LiveBundle* conflict)
{
    bool success = false;

    if (!trySplitAcrossHotcode(bundle, &success))
        return false;
    if (success)
        return true;

    if (fixed)
        return splitAcrossCalls(bundle);

    if (!trySplitBeforeFirstRegisterUse(bundle, conflict, &success))
        return false;
    if (success)
        return true;

    if (!trySplitAfterLastRegisterUse(bundle, conflict, &success))
        return false;
    if (success)
        return true;

    // Split at all register uses.
    SplitPositionVector emptyPositions;
    return splitAt(bundle, emptyPositions);
}

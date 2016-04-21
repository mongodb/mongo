/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ValueNumbering.h"

#include "jit/AliasAnalysis.h"
#include "jit/IonAnalysis.h"
#include "jit/JitSpewer.h"
#include "jit/MIRGenerator.h"

using namespace js;
using namespace js::jit;

/*
 * Some notes on the main algorithm here:
 *  - The SSA identifier id() is the value number. We do replaceAllUsesWith as
 *    we go, so there's always at most one visible value with a given number.
 *
 *  - Consequently, the GVN algorithm is effectively pessimistic. This means it
 *    is not as powerful as an optimistic GVN would be, but it is simpler and
 *    faster.
 *
 *  - We iterate in RPO, so that when visiting a block, we've already optimized
 *    and hashed all values in dominating blocks. With occasional exceptions,
 *    this allows us to do everything in a single pass.
 *
 *  - When we do use multiple passes, we just re-run the algorithm on the whole
 *    graph instead of doing sparse propagation. This is a tradeoff to keep the
 *    algorithm simpler and lighter on inputs that don't have a lot of
 *    interesting unreachable blocks or degenerate loop induction variables, at
 *    the expense of being slower on inputs that do. The loop for this always
 *    terminates, because it only iterates when code is or will be removed, so
 *    eventually it must stop iterating.
 *
 *  - Values are not immediately removed from the hash set when they go out of
 *    scope. Instead, we check for dominance after a lookup. If the dominance
 *    check fails, the value is removed.
 */

HashNumber
ValueNumberer::VisibleValues::ValueHasher::hash(Lookup ins)
{
    return ins->valueHash();
}

// Test whether two MDefinitions are congruent.
bool
ValueNumberer::VisibleValues::ValueHasher::match(Key k, Lookup l)
{
    // If one of the instructions depends on a store, and the other instruction
    // does not depend on the same store, the instructions are not congruent.
    if (k->dependency() != l->dependency())
        return false;

    bool congruent = k->congruentTo(l); // Ask the values themselves what they think.
#ifdef JS_JITSPEW
    if (congruent != l->congruentTo(k)) {
       JitSpew(JitSpew_GVN, "      congruentTo relation is not symmetric between %s%u and %s%u!!",
               k->opName(), k->id(),
               l->opName(), l->id());
    }
#endif
    return congruent;
}

void
ValueNumberer::VisibleValues::ValueHasher::rekey(Key& k, Key newKey)
{
    k = newKey;
}

ValueNumberer::VisibleValues::VisibleValues(TempAllocator& alloc)
  : set_(alloc)
{}

// Initialize the set.
bool
ValueNumberer::VisibleValues::init()
{
    return set_.init();
}

// Look up the first entry for |def|.
ValueNumberer::VisibleValues::Ptr
ValueNumberer::VisibleValues::findLeader(const MDefinition* def) const
{
    return set_.lookup(def);
}

// Look up the first entry for |def|.
ValueNumberer::VisibleValues::AddPtr
ValueNumberer::VisibleValues::findLeaderForAdd(MDefinition* def)
{
    return set_.lookupForAdd(def);
}

// Insert a value into the set.
bool
ValueNumberer::VisibleValues::add(AddPtr p, MDefinition* def)
{
    return set_.add(p, def);
}

// Insert a value onto the set overwriting any existing entry.
void
ValueNumberer::VisibleValues::overwrite(AddPtr p, MDefinition* def)
{
    set_.rekeyInPlace(p, def);
}

// |def| will be discarded, so remove it from any sets.
void
ValueNumberer::VisibleValues::forget(const MDefinition* def)
{
    Ptr p = set_.lookup(def);
    if (p && *p == def)
        set_.remove(p);
}

// Clear all state.
void
ValueNumberer::VisibleValues::clear()
{
    set_.clear();
}

#ifdef DEBUG
// Test whether |def| is in the set.
bool
ValueNumberer::VisibleValues::has(const MDefinition* def) const
{
    Ptr p = set_.lookup(def);
    return p && *p == def;
}
#endif

// Call MDefinition::justReplaceAllUsesWith, and add some GVN-specific asserts.
static void
ReplaceAllUsesWith(MDefinition* from, MDefinition* to)
{
    MOZ_ASSERT(from != to, "GVN shouldn't try to replace a value with itself");
    MOZ_ASSERT(from->type() == to->type(), "Def replacement has different type");
    MOZ_ASSERT(!to->isDiscarded(), "GVN replaces an instruction by a removed instruction");

    // We don't need the extra setting of UseRemoved flags that the regular
    // replaceAllUsesWith does because we do it ourselves.
    from->justReplaceAllUsesWith(to);
}

// Test whether |succ| is a successor of |block|.
static bool
HasSuccessor(const MControlInstruction* block, const MBasicBlock* succ)
{
    for (size_t i = 0, e = block->numSuccessors(); i != e; ++i) {
        if (block->getSuccessor(i) == succ)
            return true;
    }
    return false;
}

// Given a block which has had predecessors removed but is still reachable, test
// whether the block's new dominator will be closer than its old one and whether
// it will expose potential optimization opportunities.
static MBasicBlock*
ComputeNewDominator(MBasicBlock* block, MBasicBlock* old)
{
    MBasicBlock* now = block->getPredecessor(0);
    for (size_t i = 1, e = block->numPredecessors(); i < e; ++i) {
        MBasicBlock* pred = block->getPredecessor(i);
        // Note that dominators haven't been recomputed yet, so we have to check
        // whether now dominates pred, not block.
        while (!now->dominates(pred)) {
            MBasicBlock* next = now->immediateDominator();
            if (next == old)
                return old;
            if (next == now) {
                MOZ_ASSERT(block == old, "Non-self-dominating block became self-dominating");
                return block;
            }
            now = next;
        }
    }
    MOZ_ASSERT(old != block || old != now, "Missed self-dominating block staying self-dominating");
    return now;
}

// Test for any defs which look potentially interesting to GVN.
static bool
BlockHasInterestingDefs(MBasicBlock* block)
{
    return !block->phisEmpty() || *block->begin() != block->lastIns();
}

// Walk up the dominator tree from |block| to the root and test for any defs
// which look potentially interesting to GVN.
static bool
ScanDominatorsForDefs(MBasicBlock* block)
{
    for (MBasicBlock* i = block;;) {
        if (BlockHasInterestingDefs(block))
            return true;

        MBasicBlock* immediateDominator = i->immediateDominator();
        if (immediateDominator == i)
            break;
        i = immediateDominator;
    }
    return false;
}

// Walk up the dominator tree from |now| to |old| and test for any defs which
// look potentially interesting to GVN.
static bool
ScanDominatorsForDefs(MBasicBlock* now, MBasicBlock* old)
{
    MOZ_ASSERT(old->dominates(now), "Refined dominator not dominated by old dominator");

    for (MBasicBlock* i = now; i != old; i = i->immediateDominator()) {
        if (BlockHasInterestingDefs(i))
            return true;
    }
    return false;
}

// Given a block which has had predecessors removed but is still reachable, test
// whether the block's new dominator will be closer than its old one and whether
// it will expose potential optimization opportunities.
static bool
IsDominatorRefined(MBasicBlock* block)
{
    MBasicBlock* old = block->immediateDominator();
    MBasicBlock* now = ComputeNewDominator(block, old);

    // If this block is just a goto and it doesn't dominate its destination,
    // removing its predecessors won't refine the dominators of anything
    // interesting.
    MControlInstruction* control = block->lastIns();
    if (*block->begin() == control && block->phisEmpty() && control->isGoto() &&
        !block->dominates(control->toGoto()->target()))
    {
        return false;
    }

    // We've computed block's new dominator. Test whether there are any
    // newly-dominating definitions which look interesting.
    if (block == old)
        return block != now && ScanDominatorsForDefs(now);
    MOZ_ASSERT(block != now, "Non-self-dominating block became self-dominating");
    return ScanDominatorsForDefs(now, old);
}

// |def| has just had one of its users release it. If it's now dead, enqueue it
// for discarding, otherwise just make note of it.
bool
ValueNumberer::handleUseReleased(MDefinition* def, UseRemovedOption useRemovedOption)
{
    if (IsDiscardable(def)) {
        values_.forget(def);
        if (!deadDefs_.append(def))
            return false;
    } else {
        if (useRemovedOption == SetUseRemoved)
            def->setUseRemovedUnchecked();
    }
    return true;
}

// Discard |def| and anything in its use-def subtree which is no longer needed.
bool
ValueNumberer::discardDefsRecursively(MDefinition* def)
{
    MOZ_ASSERT(deadDefs_.empty(), "deadDefs_ not cleared");

    return discardDef(def) && processDeadDefs();
}

// Assuming |resume| is unreachable, release its operands.
// It might be nice to integrate this code with prepareForDiscard, however GVN
// needs it to call handleUseReleased so that it can observe when a definition
// becomes unused, so it isn't trivial to do.
bool
ValueNumberer::releaseResumePointOperands(MResumePoint* resume)
{
    for (size_t i = 0, e = resume->numOperands(); i < e; ++i) {
        if (!resume->hasOperand(i))
            continue;
        MDefinition* op = resume->getOperand(i);
        resume->releaseOperand(i);

        // We set the UseRemoved flag when removing resume point operands,
        // because even though we may think we're certain that a particular
        // branch might not be taken, the type information might be incomplete.
        if (!handleUseReleased(op, SetUseRemoved))
            return false;
    }
    return true;
}

// Assuming |phi| is dead, release and remove its operands. If an operand
// becomes dead, push it to the discard worklist.
bool
ValueNumberer::releaseAndRemovePhiOperands(MPhi* phi)
{
    // MPhi saves operands in a vector so we iterate in reverse.
    for (int o = phi->numOperands() - 1; o >= 0; --o) {
        MDefinition* op = phi->getOperand(o);
        phi->removeOperand(o);
        if (!handleUseReleased(op, DontSetUseRemoved))
            return false;
    }
    return true;
}

// Assuming |def| is dead, release its operands. If an operand becomes dead,
// push it to the discard worklist.
bool
ValueNumberer::releaseOperands(MDefinition* def)
{
    for (size_t o = 0, e = def->numOperands(); o < e; ++o) {
        MDefinition* op = def->getOperand(o);
        def->releaseOperand(o);
        if (!handleUseReleased(op, DontSetUseRemoved))
            return false;
    }
    return true;
}

// Discard |def| and mine its operands for any subsequently dead defs.
bool
ValueNumberer::discardDef(MDefinition* def)
{
#ifdef JS_JITSPEW
    JitSpew(JitSpew_GVN, "      Discarding %s %s%u",
            def->block()->isMarked() ? "unreachable" : "dead",
            def->opName(), def->id());
#endif
#ifdef DEBUG
    MOZ_ASSERT(def != nextDef_, "Invalidating the MDefinition iterator");
    if (def->block()->isMarked()) {
        MOZ_ASSERT(!def->hasUses(), "Discarding def that still has uses");
    } else {
        MOZ_ASSERT(IsDiscardable(def), "Discarding non-discardable definition");
        MOZ_ASSERT(!values_.has(def), "Discarding a definition still in the set");
    }
#endif

    MBasicBlock* block = def->block();
    if (def->isPhi()) {
        MPhi* phi = def->toPhi();
        if (!releaseAndRemovePhiOperands(phi))
             return false;
        block->discardPhi(phi);
    } else {
        MInstruction* ins = def->toInstruction();
        if (MResumePoint* resume = ins->resumePoint()) {
            if (!releaseResumePointOperands(resume))
                return false;
        }
        if (!releaseOperands(ins))
             return false;
        block->discardIgnoreOperands(ins);
    }

    // If that was the last definition in the block, it can be safely removed
    // from the graph.
    if (block->phisEmpty() && block->begin() == block->end()) {
        MOZ_ASSERT(block->isMarked(), "Reachable block lacks at least a control instruction");

        // As a special case, don't remove a block which is a dominator tree
        // root so that we don't invalidate the iterator in visitGraph. We'll
        // check for this and remove it later.
        if (block->immediateDominator() != block) {
            JitSpew(JitSpew_GVN, "      Block block%u is now empty; discarding", block->id());
            graph_.removeBlock(block);
            blocksRemoved_ = true;
        } else {
            JitSpew(JitSpew_GVN, "      Dominator root block%u is now empty; will discard later",
                    block->id());
        }
    }

    return true;
}

// Recursively discard all the defs on the deadDefs_ worklist.
bool
ValueNumberer::processDeadDefs()
{
    MDefinition* nextDef = nextDef_;
    while (!deadDefs_.empty()) {
        MDefinition* def = deadDefs_.popCopy();

        // Don't invalidate the MDefinition iterator. This is what we're going
        // to visit next, so we won't miss anything.
        if (def == nextDef)
            continue;

        if (!discardDef(def))
            return false;
    }
    return true;
}

// Test whether |block|, which is a loop header, has any predecessors other than
// |loopPred|, the loop predecessor, which it doesn't dominate.
static bool
hasNonDominatingPredecessor(MBasicBlock* block, MBasicBlock* loopPred)
{
    MOZ_ASSERT(block->isLoopHeader());
    MOZ_ASSERT(block->loopPredecessor() == loopPred);

    for (uint32_t i = 0, e = block->numPredecessors(); i < e; ++i) {
        MBasicBlock* pred = block->getPredecessor(i);
        if (pred != loopPred && !block->dominates(pred))
            return true;
    }
    return false;
}

// A loop is about to be made reachable only through an OSR entry into one of
// its nested loops. Fix everything up.
bool
ValueNumberer::fixupOSROnlyLoop(MBasicBlock* block, MBasicBlock* backedge)
{
    // Create an empty and unreachable(!) block which jumps to |block|. This
    // allows |block| to remain marked as a loop header, so we don't have to
    // worry about moving a different block into place as the new loop header,
    // which is hard, especially if the OSR is into a nested loop. Doing all
    // that would produce slightly more optimal code, but this is so
    // extraordinarily rare that it isn't worth the complexity.
    MBasicBlock* fake = MBasicBlock::NewAsmJS(graph_, block->info(),
                                              nullptr, MBasicBlock::NORMAL);
    if (fake == nullptr)
        return false;

    graph_.insertBlockBefore(block, fake);
    fake->setImmediateDominator(fake);
    fake->addNumDominated(1);
    fake->setDomIndex(fake->id());

    // Create zero-input phis to use as inputs for any phis in |block|.
    // Again, this is a little odd, but it's the least-odd thing we can do
    // without significant complexity.
    for (MPhiIterator iter(block->phisBegin()), end(block->phisEnd()); iter != end; ++iter) {
        MPhi* phi = *iter;
        MPhi* fakePhi = MPhi::New(graph_.alloc(), phi->type());
        fake->addPhi(fakePhi);
        if (!phi->addInputSlow(fakePhi))
            return false;
    }

    fake->end(MGoto::New(graph_.alloc(), block));

    if (!block->addPredecessorWithoutPhis(fake))
        return false;

    // Restore |backedge| as |block|'s loop backedge.
    block->clearLoopHeader();
    block->setLoopHeader(backedge);

    JitSpew(JitSpew_GVN, "        Created fake block%u", fake->id());
    return true;
}

// Remove the CFG edge between |pred| and |block|, after releasing the phi
// operands on that edge and discarding any definitions consequently made dead.
bool
ValueNumberer::removePredecessorAndDoDCE(MBasicBlock* block, MBasicBlock* pred, size_t predIndex)
{
    MOZ_ASSERT(!block->isMarked(),
               "Block marked unreachable should have predecessors removed already");

    // Before removing the predecessor edge, scan the phi operands for that edge
    // for dead code before they get removed.
    MOZ_ASSERT(nextDef_ == nullptr);
    for (MPhiIterator iter(block->phisBegin()), end(block->phisEnd()); iter != end; ) {
        MPhi* phi = *iter++;
        MOZ_ASSERT(!values_.has(phi), "Visited phi in block having predecessor removed");

        MDefinition* op = phi->getOperand(predIndex);
        phi->removeOperand(predIndex);

        nextDef_ = iter != end ? *iter : nullptr;
        if (!handleUseReleased(op, DontSetUseRemoved) || !processDeadDefs())
            return false;

        // If |nextDef_| became dead while we had it pinned, advance the iterator
        // and discard it now.
        while (nextDef_ && !nextDef_->hasUses()) {
            phi = nextDef_->toPhi();
            iter++;
            nextDef_ = iter != end ? *iter : nullptr;
            discardDefsRecursively(phi);
        }
    }
    nextDef_ = nullptr;

    block->removePredecessorWithoutPhiOperands(pred, predIndex);
    return true;
}

// Remove the CFG edge between |pred| and |block|, and if this makes |block|
// unreachable, mark it so, and remove the rest of its incoming edges too. And
// discard any instructions made dead by the entailed release of any phi
// operands.
bool
ValueNumberer::removePredecessorAndCleanUp(MBasicBlock* block, MBasicBlock* pred)
{
    MOZ_ASSERT(!block->isMarked(), "Removing predecessor on block already marked unreachable");

    // We'll be removing a predecessor, so anything we know about phis in this
    // block will be wrong.
    for (MPhiIterator iter(block->phisBegin()), end(block->phisEnd()); iter != end; ++iter)
        values_.forget(*iter);

    // If this is a loop header, test whether it will become an unreachable
    // loop, or whether it needs special OSR-related fixups.
    bool isUnreachableLoop = false;
    MBasicBlock* origBackedgeForOSRFixup = nullptr;
    if (block->isLoopHeader()) {
        if (block->loopPredecessor() == pred) {
            if (MOZ_UNLIKELY(hasNonDominatingPredecessor(block, pred))) {
                JitSpew(JitSpew_GVN, "      "
                        "Loop with header block%u is now only reachable through an "
                        "OSR entry into the middle of the loop!!", block->id());
                origBackedgeForOSRFixup = block->backedge();
            } else {
                // Deleting the entry into the loop makes the loop unreachable.
                isUnreachableLoop = true;
                JitSpew(JitSpew_GVN, "      "
                        "Loop with header block%u is no longer reachable",
                        block->id());
            }
#ifdef JS_JITSPEW
        } else if (block->hasUniqueBackedge() && block->backedge() == pred) {
            JitSpew(JitSpew_GVN, "      Loop with header block%u is no longer a loop",
                    block->id());
#endif
        }
    }

    // Actually remove the CFG edge.
    if (!removePredecessorAndDoDCE(block, pred, block->getPredecessorIndex(pred)))
        return false;

    // We've now edited the CFG; check to see if |block| became unreachable.
    if (block->numPredecessors() == 0 || isUnreachableLoop) {
        JitSpew(JitSpew_GVN, "      Disconnecting block%u", block->id());

        // Remove |block| from its dominator parent's subtree. This is the only
        // immediately-dominated-block information we need to update, because
        // everything dominated by this block is about to be swept away.
        MBasicBlock* parent = block->immediateDominator();
        if (parent != block)
            parent->removeImmediatelyDominatedBlock(block);

        // Completely disconnect it from the CFG. We do this now rather than
        // just doing it later when we arrive there in visitUnreachableBlock
        // so that we don't leave a partially broken loop sitting around. This
        // also lets visitUnreachableBlock assert that numPredecessors() == 0,
        // which is a nice invariant.
        if (block->isLoopHeader())
            block->clearLoopHeader();
        for (size_t i = 0, e = block->numPredecessors(); i < e; ++i) {
            if (!removePredecessorAndDoDCE(block, block->getPredecessor(i), i))
                return false;
        }

        // Clear out the resume point operands, as they can hold things that
        // don't appear to dominate them live.
        if (MResumePoint* resume = block->entryResumePoint()) {
            if (!releaseResumePointOperands(resume) || !processDeadDefs())
                return false;
            if (MResumePoint* outer = block->outerResumePoint()) {
                if (!releaseResumePointOperands(outer) || !processDeadDefs())
                    return false;
            }
            MOZ_ASSERT(nextDef_ == nullptr);
            for (MInstructionIterator iter(block->begin()), end(block->end()); iter != end; ) {
                MInstruction* ins = *iter++;
                nextDef_ = *iter;
                if (MResumePoint* resume = ins->resumePoint()) {
                    if (!releaseResumePointOperands(resume) || !processDeadDefs())
                        return false;
                }
            }
            nextDef_ = nullptr;
        } else {
#ifdef DEBUG
            MOZ_ASSERT(block->outerResumePoint() == nullptr,
                       "Outer resume point in block without an entry resume point");
            for (MInstructionIterator iter(block->begin()), end(block->end());
                 iter != end;
                 ++iter)
            {
                MOZ_ASSERT(iter->resumePoint() == nullptr,
                           "Instruction with resume point in block without entry resume point");
            }
#endif
        }

        // Use the mark to note that we've already removed all its predecessors,
        // and we know it's unreachable.
        block->mark();
    } else if (MOZ_UNLIKELY(origBackedgeForOSRFixup != nullptr)) {
        // The loop is no only reachable through OSR into the middle. Fix it
        // up so that the CFG can remain valid.
        if (!fixupOSROnlyLoop(block, origBackedgeForOSRFixup))
            return false;
    }

    return true;
}

// Return a simplified form of |def|, if we can.
MDefinition*
ValueNumberer::simplified(MDefinition* def) const
{
    return def->foldsTo(graph_.alloc());
}

// If an equivalent and dominating value already exists in the set, return it.
// Otherwise insert |def| into the set and return it.
MDefinition*
ValueNumberer::leader(MDefinition* def)
{
    // If the value isn't suitable for eliminating, don't bother hashing it. The
    // convention is that congruentTo returns false for node kinds that wish to
    // opt out of redundance elimination.
    // TODO: It'd be nice to clean up that convention (bug 1031406).
    if (!def->isEffectful() && def->congruentTo(def)) {
        // Look for a match.
        VisibleValues::AddPtr p = values_.findLeaderForAdd(def);
        if (p) {
            MDefinition* rep = *p;
            if (!rep->isDiscarded() && rep->block()->dominates(def->block())) {
                // We found a dominating congruent value.
                return rep;
            }

            // The congruent value doesn't dominate. It never will again in this
            // dominator tree, so overwrite it.
            values_.overwrite(p, def);
        } else {
            // No match. Add a new entry.
            if (!values_.add(p, def))
                return nullptr;
        }

#ifdef JS_JITSPEW
        JitSpew(JitSpew_GVN, "      Recording %s%u", def->opName(), def->id());
#endif
    }

    return def;
}

// Test whether |phi| is dominated by a congruent phi.
bool
ValueNumberer::hasLeader(const MPhi* phi, const MBasicBlock* phiBlock) const
{
    if (VisibleValues::Ptr p = values_.findLeader(phi)) {
        const MDefinition* rep = *p;
        return rep != phi && rep->block()->dominates(phiBlock);
    }
    return false;
}

// Test whether there are any phis in |header| which are newly optimizable, as a
// result of optimizations done inside the loop. This is not a sparse approach,
// but restarting is rare enough in practice. Termination is ensured by
// discarding the phi triggering the iteration.
bool
ValueNumberer::loopHasOptimizablePhi(MBasicBlock* header) const
{
    // If the header is unreachable, don't bother re-optimizing it.
    if (header->isMarked())
        return false;

    // Rescan the phis for any that can be simplified, since they may be reading
    // values from backedges.
    for (MPhiIterator iter(header->phisBegin()), end(header->phisEnd()); iter != end; ++iter) {
        MPhi* phi = *iter;
        MOZ_ASSERT_IF(!phi->hasUses(), !DeadIfUnused(phi));

        if (phi->operandIfRedundant() || hasLeader(phi, header))
            return true; // Phi can be simplified.
    }
    return false;
}

// Visit |def|.
bool
ValueNumberer::visitDefinition(MDefinition* def)
{
    // Nop does not fit in any of the previous optimization, as its only purpose
    // is to reduce the register pressure by keeping additional resume
    // point. Still, there is no need consecutive list of MNop instructions, and
    // this will slow down every other iteration on the Graph.
    if (def->isNop()) {
        MNop* nop = def->toNop();
        MBasicBlock* block = nop->block();

        // We look backward to know if we can remove the previous Nop, we do not
        // look forward as we would not benefit from the folding made by GVN.
        MInstructionReverseIterator iter = ++block->rbegin(nop);

        // This nop is at the beginning of the basic block, just replace the
        // resume point of the basic block by the one from the resume point.
        if (iter == block->rend()) {
            JitSpew(JitSpew_GVN, "      Removing Nop%u", nop->id());
            nop->moveResumePointAsEntry();
            block->discard(nop);
            return true;
        }

        // The previous instruction is also a Nop, no need to keep it anymore.
        MInstruction* prev = *iter;
        if (prev->isNop()) {
            JitSpew(JitSpew_GVN, "      Removing Nop%u", prev->id());
            block->discard(prev);
            return true;
        }

        return true;
    }

    // Skip optimizations on instructions which are recovered on bailout, to
    // avoid mixing instructions which are recovered on bailouts with
    // instructions which are not.
    if (def->isRecoveredOnBailout())
        return true;

    // If this instruction has a dependency() into an unreachable block, we'll
    // need to update AliasAnalysis.
    MInstruction* dep = def->dependency();
    if (dep != nullptr && (dep->isDiscarded() || dep->block()->isDead())) {
        JitSpew(JitSpew_GVN, "      AliasAnalysis invalidated");
        if (updateAliasAnalysis_ && !dependenciesBroken_) {
            // TODO: Recomputing alias-analysis could theoretically expose more
            // GVN opportunities.
            JitSpew(JitSpew_GVN, "        Will recompute!");
            dependenciesBroken_ = true;
        }
        // Temporarily clear its dependency, to protect foldsTo, which may
        // wish to use the dependency to do store-to-load forwarding.
        def->setDependency(def->toInstruction());
    } else {
        dep = nullptr;
    }

    // Look for a simplified form of |def|.
    MDefinition* sim = simplified(def);
    if (sim != def) {
        if (sim == nullptr)
            return false;

        bool isNewInstruction = sim->block() == nullptr;

        // If |sim| doesn't belong to a block, insert it next to |def|.
        if (isNewInstruction)
            def->block()->insertAfter(def->toInstruction(), sim->toInstruction());

#ifdef JS_JITSPEW
        JitSpew(JitSpew_GVN, "      Folded %s%u to %s%u",
                def->opName(), def->id(), sim->opName(), sim->id());
#endif
        MOZ_ASSERT(!sim->isDiscarded());
        ReplaceAllUsesWith(def, sim);

        // The node's foldsTo said |def| can be replaced by |rep|. If |def| is a
        // guard, then either |rep| is also a guard, or a guard isn't actually
        // needed, so we can clear |def|'s guard flag and let it be discarded.
        def->setNotGuardUnchecked();

        if (DeadIfUnused(def)) {
            if (!discardDefsRecursively(def))
                return false;

            // If that ended up discarding |sim|, then we're done here.
            if (sim->isDiscarded())
                return true;
        }

        // Otherwise, procede to optimize with |sim| in place of |def|.
        def = sim;

        // If the simplified instruction was already part of the graph, then we
        // probably already visited and optimized this instruction.
        if (!isNewInstruction)
            return true;
    }

    // Now that foldsTo is done, re-enable the original dependency. Even though
    // it may be pointing into a discarded block, it's still valid for the
    // purposes of detecting congruent loads.
    if (dep != nullptr)
        def->setDependency(dep);

    // Look for a dominating def which makes |def| redundant.
    MDefinition* rep = leader(def);
    if (rep != def) {
        if (rep == nullptr)
            return false;
        if (rep->updateForReplacement(def)) {
#ifdef JS_JITSPEW
            JitSpew(JitSpew_GVN,
                    "      Replacing %s%u with %s%u",
                    def->opName(), def->id(), rep->opName(), rep->id());
#endif
            ReplaceAllUsesWith(def, rep);

            // The node's congruentTo said |def| is congruent to |rep|, and it's
            // dominated by |rep|. If |def| is a guard, it's covered by |rep|,
            // so we can clear |def|'s guard flag and let it be discarded.
            def->setNotGuardUnchecked();

            if (DeadIfUnused(def)) {
                // discardDef should not add anything to the deadDefs, as the
                // redundant operation should have the same input operands.
                mozilla::DebugOnly<bool> r = discardDef(def);
                MOZ_ASSERT(r, "discardDef shouldn't have tried to add anything to the worklist, "
                              "so it shouldn't have failed");
                MOZ_ASSERT(deadDefs_.empty(),
                           "discardDef shouldn't have added anything to the worklist");
            }
            def = rep;
        }
    }

    return true;
}

// Visit the control instruction at the end of |block|.
bool
ValueNumberer::visitControlInstruction(MBasicBlock* block, const MBasicBlock* dominatorRoot)
{
    // Look for a simplified form of the control instruction.
    MControlInstruction* control = block->lastIns();
    MDefinition* rep = simplified(control);
    if (rep == control)
        return true;

    if (rep == nullptr)
        return false;

    MControlInstruction* newControl = rep->toControlInstruction();
    MOZ_ASSERT(!newControl->block(),
               "Control instruction replacement shouldn't already be in a block");
#ifdef JS_JITSPEW
    JitSpew(JitSpew_GVN, "      Folded control instruction %s%u to %s%u",
            control->opName(), control->id(), newControl->opName(), graph_.getNumInstructionIds());
#endif

    // If the simplification removes any CFG edges, update the CFG and remove
    // any blocks that become dead.
    size_t oldNumSuccs = control->numSuccessors();
    size_t newNumSuccs = newControl->numSuccessors();
    if (newNumSuccs != oldNumSuccs) {
        MOZ_ASSERT(newNumSuccs < oldNumSuccs, "New control instruction has too many successors");
        for (size_t i = 0; i != oldNumSuccs; ++i) {
            MBasicBlock* succ = control->getSuccessor(i);
            if (HasSuccessor(newControl, succ))
                continue;
            if (succ->isMarked())
                continue;
            if (!removePredecessorAndCleanUp(succ, block))
                return false;
            if (succ->isMarked())
                continue;
            if (!rerun_) {
                if (!remainingBlocks_.append(succ))
                    return false;
            }
        }
    }

    if (!releaseOperands(control))
        return false;
    block->discardIgnoreOperands(control);
    block->end(newControl);
    if (block->entryResumePoint() && newNumSuccs != oldNumSuccs)
        block->flagOperandsOfPrunedBranches(newControl);
    return processDeadDefs();
}

// |block| is unreachable. Mine it for opportunities to delete more dead
// code, and then discard it.
bool
ValueNumberer::visitUnreachableBlock(MBasicBlock* block)
{
    JitSpew(JitSpew_GVN, "    Visiting unreachable block%u%s%s%s", block->id(),
            block->isLoopHeader() ? " (loop header)" : "",
            block->isSplitEdge() ? " (split edge)" : "",
            block->immediateDominator() == block ? " (dominator root)" : "");

    MOZ_ASSERT(block->isMarked(), "Visiting unmarked (and therefore reachable?) block");
    MOZ_ASSERT(block->numPredecessors() == 0, "Block marked unreachable still has predecessors");
    MOZ_ASSERT(block != graph_.entryBlock(), "Removing normal entry block");
    MOZ_ASSERT(block != graph_.osrBlock(), "Removing OSR entry block");
    MOZ_ASSERT(deadDefs_.empty(), "deadDefs_ not cleared");

    // Disconnect all outgoing CFG edges.
    for (size_t i = 0, e = block->numSuccessors(); i < e; ++i) {
        MBasicBlock* succ = block->getSuccessor(i);
        if (succ->isDead() || succ->isMarked())
            continue;
        if (!removePredecessorAndCleanUp(succ, block))
            return false;
        if (succ->isMarked())
            continue;
        // |succ| is still reachable. Make a note of it so that we can scan
        // it for interesting dominator tree changes later.
        if (!rerun_) {
            if (!remainingBlocks_.append(succ))
                return false;
        }
    }

    // Discard any instructions with no uses. The remaining instructions will be
    // discarded when their last use is discarded.
    MOZ_ASSERT(nextDef_ == nullptr);
    for (MDefinitionIterator iter(block); iter; ) {
        MDefinition* def = *iter++;
        if (def->hasUses())
            continue;
        nextDef_ = *iter;
        if (!discardDefsRecursively(def))
            return false;
    }

    nextDef_ = nullptr;
    MControlInstruction* control = block->lastIns();
    return discardDefsRecursively(control);
}

// Visit all the phis and instructions |block|.
bool
ValueNumberer::visitBlock(MBasicBlock* block, const MBasicBlock* dominatorRoot)
{
    MOZ_ASSERT(!block->isMarked(), "Blocks marked unreachable during GVN");
    MOZ_ASSERT(!block->isDead(), "Block to visit is already dead");

    JitSpew(JitSpew_GVN, "    Visiting block%u", block->id());

    // Visit the definitions in the block top-down.
    MOZ_ASSERT(nextDef_ == nullptr);
    for (MDefinitionIterator iter(block); iter; ) {
        MDefinition* def = *iter++;

        // Remember where our iterator is so that we don't invalidate it.
        nextDef_ = *iter;

        // If the definition is dead, discard it.
        if (IsDiscardable(def)) {
            if (!discardDefsRecursively(def))
                return false;
            continue;
        }

        if (!visitDefinition(def))
            return false;
    }
    nextDef_ = nullptr;

    return visitControlInstruction(block, dominatorRoot);
}

// Visit all the blocks dominated by dominatorRoot.
bool
ValueNumberer::visitDominatorTree(MBasicBlock* dominatorRoot)
{
    JitSpew(JitSpew_GVN, "  Visiting dominator tree (with %llu blocks) rooted at block%u%s",
            uint64_t(dominatorRoot->numDominated()), dominatorRoot->id(),
            dominatorRoot == graph_.entryBlock() ? " (normal entry block)" :
            dominatorRoot == graph_.osrBlock() ? " (OSR entry block)" :
            dominatorRoot->numPredecessors() == 0 ? " (odd unreachable block)" :
            " (merge point from normal entry and OSR entry)");
    MOZ_ASSERT(dominatorRoot->immediateDominator() == dominatorRoot,
            "root is not a dominator tree root");

    // Visit all blocks dominated by dominatorRoot, in RPO. This has the nice
    // property that we'll always visit a block before any block it dominates,
    // so we can make a single pass through the list and see every full
    // redundance.
    size_t numVisited = 0;
    size_t numDiscarded = 0;
    for (ReversePostorderIterator iter(graph_.rpoBegin(dominatorRoot)); ; ) {
        MOZ_ASSERT(iter != graph_.rpoEnd(), "Inconsistent dominator information");
        MBasicBlock* block = *iter++;
        // We're only visiting blocks in dominatorRoot's tree right now.
        if (!dominatorRoot->dominates(block))
            continue;

        // If this is a loop backedge, remember the header, as we may not be able
        // to find it after we simplify the block.
        MBasicBlock* header = block->isLoopBackedge() ? block->loopHeaderOfBackedge() : nullptr;

        if (block->isMarked()) {
            // This block has become unreachable; handle it specially.
            if (!visitUnreachableBlock(block))
                return false;
            ++numDiscarded;
        } else {
            // Visit the block!
            if (!visitBlock(block, dominatorRoot))
                return false;
            ++numVisited;
        }

        // If the block is/was a loop backedge, check to see if the block that
        // is/was its header has optimizable phis, which would want a re-run.
        if (!rerun_ && header && loopHasOptimizablePhi(header)) {
            JitSpew(JitSpew_GVN, "    Loop phi in block%u can now be optimized; will re-run GVN!",
                    header->id());
            rerun_ = true;
            remainingBlocks_.clear();
        }

        MOZ_ASSERT(numVisited <= dominatorRoot->numDominated() - numDiscarded,
                   "Visited blocks too many times");
        if (numVisited >= dominatorRoot->numDominated() - numDiscarded)
            break;
    }

    totalNumVisited_ += numVisited;
    values_.clear();
    return true;
}

// Visit all the blocks in the graph.
bool
ValueNumberer::visitGraph()
{
    // Due to OSR blocks, the set of blocks dominated by a blocks may not be
    // contiguous in the RPO. Do a separate traversal for each dominator tree
    // root. There's always the main entry, and sometimes there's an OSR entry,
    // and then there are the roots formed where the OSR paths merge with the
    // main entry paths.
    for (ReversePostorderIterator iter(graph_.rpoBegin()); ; ) {
        MOZ_ASSERT(iter != graph_.rpoEnd(), "Inconsistent dominator information");
        MBasicBlock* block = *iter;
        if (block->immediateDominator() == block) {
            if (!visitDominatorTree(block))
                return false;

            // Normally unreachable blocks would be removed by now, but if this
            // block is a dominator tree root, it has been special-cased and left
            // in place in order to avoid invalidating our iterator. Now that
            // we've finished the tree, increment the iterator, and then if it's
            // marked for removal, remove it.
            ++iter;
            if (block->isMarked()) {
                JitSpew(JitSpew_GVN, "      Discarding dominator root block%u",
                        block->id());
                MOZ_ASSERT(block->begin() == block->end(),
                           "Unreachable dominator tree root has instructions after tree walk");
                MOZ_ASSERT(block->phisEmpty(),
                           "Unreachable dominator tree root has phis after tree walk");
                graph_.removeBlock(block);
                blocksRemoved_ = true;
            }

            MOZ_ASSERT(totalNumVisited_ <= graph_.numBlocks(), "Visited blocks too many times");
            if (totalNumVisited_ >= graph_.numBlocks())
                break;
        } else {
            // This block a dominator tree root. Proceed to the next one.
            ++iter;
        }
    }
    totalNumVisited_ = 0;
    return true;
}

ValueNumberer::ValueNumberer(MIRGenerator* mir, MIRGraph& graph)
  : mir_(mir), graph_(graph),
    values_(graph.alloc()),
    deadDefs_(graph.alloc()),
    remainingBlocks_(graph.alloc()),
    nextDef_(nullptr),
    totalNumVisited_(0),
    rerun_(false),
    blocksRemoved_(false),
    updateAliasAnalysis_(false),
    dependenciesBroken_(false)
{}

bool
ValueNumberer::init()
{
    // Initialize the value set. It's tempting to pass in a size here of some
    // function of graph_.getNumInstructionIds(), however if we start out with a
    // large capacity, it will be far larger than the actual element count for
    // most of the pass, so when we remove elements, it would often think it
    // needs to compact itself. Empirically, just letting the HashTable grow as
    // needed on its own seems to work pretty well.
    return values_.init();
}

bool
ValueNumberer::run(UpdateAliasAnalysisFlag updateAliasAnalysis)
{
    updateAliasAnalysis_ = updateAliasAnalysis == UpdateAliasAnalysis;

    JitSpew(JitSpew_GVN, "Running GVN on graph (with %llu blocks)",
            uint64_t(graph_.numBlocks()));

    // Top level non-sparse iteration loop. If an iteration performs a
    // significant change, such as discarding a block which changes the
    // dominator tree and may enable more optimization, this loop takes another
    // iteration.
    int runs = 0;
    for (;;) {
        if (!visitGraph())
            return false;

        // Test whether any block which was not removed but which had at least
        // one predecessor removed will have a new dominator parent.
        while (!remainingBlocks_.empty()) {
            MBasicBlock* block = remainingBlocks_.popCopy();
            if (!block->isDead() && IsDominatorRefined(block)) {
                JitSpew(JitSpew_GVN, "  Dominator for block%u can now be refined; will re-run GVN!",
                        block->id());
                rerun_ = true;
                remainingBlocks_.clear();
                break;
            }
        }

        if (blocksRemoved_) {
            if (!AccountForCFGChanges(mir_, graph_, dependenciesBroken_))
                return false;

            blocksRemoved_ = false;
            dependenciesBroken_ = false;
        }

        if (mir_->shouldCancel("GVN (outer loop)"))
            return false;

        // If no further opportunities have been discovered, we're done.
        if (!rerun_)
            break;

        rerun_ = false;

        // Enforce an arbitrary iteration limit. This is rarely reached, and
        // isn't even strictly necessary, as the algorithm is guaranteed to
        // terminate on its own in a finite amount of time (since every time we
        // re-run we discard the construct which triggered the re-run), but it
        // does help avoid slow compile times on pathological code.
        ++runs;
        if (runs == 6) {
            JitSpew(JitSpew_GVN, "Re-run cutoff of %d reached. Terminating GVN!", runs);
            break;
        }

        JitSpew(JitSpew_GVN, "Re-running GVN on graph (run %d, now with %llu blocks)",
                runs, uint64_t(graph_.numBlocks()));
    }

    return true;
}

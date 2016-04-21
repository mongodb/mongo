/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/LoopUnroller.h"

#include "jit/MIRGraph.h"

using namespace js;
using namespace js::jit;

using mozilla::ArrayLength;

namespace {

struct LoopUnroller
{
    typedef HashMap<MDefinition*, MDefinition*,
                    PointerHasher<MDefinition*, 2>, SystemAllocPolicy> DefinitionMap;

    explicit LoopUnroller(MIRGraph& graph)
      : graph(graph), alloc(graph.alloc())
    {}

    MIRGraph& graph;
    TempAllocator& alloc;

    // Header and body of the original loop.
    MBasicBlock* header;
    MBasicBlock* backedge;

    // Header and body of the unrolled loop.
    MBasicBlock* unrolledHeader;
    MBasicBlock* unrolledBackedge;

    // Old and new preheaders. The old preheader starts out associated with the
    // original loop, but becomes the preheader of the new loop. The new
    // preheader will be given to the original loop.
    MBasicBlock* oldPreheader;
    MBasicBlock* newPreheader;

    // Map terms in the original loop to terms in the current unrolled iteration.
    DefinitionMap unrolledDefinitions;

    MDefinition* getReplacementDefinition(MDefinition* def);
    MResumePoint* makeReplacementResumePoint(MBasicBlock* block, MResumePoint* rp);
    bool makeReplacementInstruction(MInstruction* ins);

    void go(LoopIterationBound* bound);
};

} // namespace

MDefinition*
LoopUnroller::getReplacementDefinition(MDefinition* def)
{
    if (def->block()->id() < header->id()) {
        // The definition is loop invariant.
        return def;
    }

    DefinitionMap::Ptr p = unrolledDefinitions.lookup(def);
    if (!p) {
        // After phi analysis (TypeAnalyzer::replaceRedundantPhi) the resume
        // point at the start of a block can contain definitions from within
        // the block itself.
        MOZ_ASSERT(def->isConstant());

        MConstant* constant = MConstant::New(alloc, def->toConstant()->value());
        oldPreheader->insertBefore(*oldPreheader->begin(), constant);
        return constant;
    }

    return p->value();
}

bool
LoopUnroller::makeReplacementInstruction(MInstruction* ins)
{
    MDefinitionVector inputs(alloc);
    for (size_t i = 0; i < ins->numOperands(); i++) {
        MDefinition* old = ins->getOperand(i);
        MDefinition* replacement = getReplacementDefinition(old);
        if (!inputs.append(replacement))
            return false;
    }

    MInstruction* clone = ins->clone(alloc, inputs);

    unrolledBackedge->add(clone);

    if (!unrolledDefinitions.putNew(ins, clone))
        return false;

    if (MResumePoint* old = ins->resumePoint()) {
        MResumePoint* rp = makeReplacementResumePoint(unrolledBackedge, old);
        clone->setResumePoint(rp);
    }

    return true;
}

MResumePoint*
LoopUnroller::makeReplacementResumePoint(MBasicBlock* block, MResumePoint* rp)
{
    MDefinitionVector inputs(alloc);
    for (size_t i = 0; i < rp->numOperands(); i++) {
        MDefinition* old = rp->getOperand(i);
        MDefinition* replacement = old->isUnused() ? old : getReplacementDefinition(old);
        if (!inputs.append(replacement))
            return nullptr;
    }

    MResumePoint* clone = MResumePoint::New(alloc, block, rp, inputs);
    if (!clone)
        return nullptr;

    return clone;
}

void
LoopUnroller::go(LoopIterationBound* bound)
{
    // For now we always unroll loops the same number of times.
    static const size_t UnrollCount = 10;

    JitSpew(JitSpew_Unrolling, "Attempting to unroll loop");

    header = bound->header;

    // UCE might have determined this isn't actually a loop.
    if (!header->isLoopHeader())
        return;

    backedge = header->backedge();
    oldPreheader = header->loopPredecessor();

    MOZ_ASSERT(oldPreheader->numSuccessors() == 1);

    // Only unroll loops with two blocks: an initial one ending with the
    // bound's test, and the body ending with the backedge.
    MTest* test = bound->test;
    if (header->lastIns() != test)
        return;
    if (test->ifTrue() == backedge) {
        if (test->ifFalse()->id() <= backedge->id())
            return;
    } else if (test->ifFalse() == backedge) {
        if (test->ifTrue()->id() <= backedge->id())
            return;
    } else {
        return;
    }
    if (backedge->numPredecessors() != 1 || backedge->numSuccessors() != 1)
        return;
    MOZ_ASSERT(backedge->phisEmpty());

    MBasicBlock* bodyBlocks[] = { header, backedge };

    // All instructions in the header and body must be clonable.
    for (size_t i = 0; i < ArrayLength(bodyBlocks); i++) {
        MBasicBlock* block = bodyBlocks[i];
        for (MInstructionIterator iter(block->begin()); iter != block->end(); iter++) {
            MInstruction* ins = *iter;
            if (ins->canClone())
                continue;
            if (ins->isTest() || ins->isGoto() || ins->isInterruptCheck())
                continue;
#ifdef JS_JITSPEW
            JitSpew(JitSpew_Unrolling, "Aborting: can't clone instruction %s", ins->opName());
#endif
            return;
        }
    }

    // Compute the linear inequality we will use for exiting the unrolled loop:
    //
    // iterationBound - iterationCount - UnrollCount >= 0
    //
    LinearSum remainingIterationsInequality(bound->boundSum);
    if (!remainingIterationsInequality.add(bound->currentSum, -1))
        return;
    if (!remainingIterationsInequality.add(-int32_t(UnrollCount)))
        return;

    // Terms in the inequality need to be either loop invariant or phis from
    // the original header.
    for (size_t i = 0; i < remainingIterationsInequality.numTerms(); i++) {
        MDefinition* def = remainingIterationsInequality.term(i).term;
        if (def->isDiscarded())
            return;
        if (def->block()->id() < header->id())
            continue;
        if (def->block() == header && def->isPhi())
            continue;
        return;
    }

    // OK, we've checked everything, now unroll the loop.

    JitSpew(JitSpew_Unrolling, "Unrolling loop");

    // The old preheader will go before the unrolled loop, and the old loop
    // will need a new empty preheader.
    const CompileInfo& info = oldPreheader->info();
    if (header->trackedPc()) {
        unrolledHeader =
            MBasicBlock::New(graph, nullptr, info,
                             oldPreheader, header->trackedSite(), MBasicBlock::LOOP_HEADER);
        unrolledBackedge =
            MBasicBlock::New(graph, nullptr, info,
                             unrolledHeader, backedge->trackedSite(), MBasicBlock::NORMAL);
        newPreheader =
            MBasicBlock::New(graph, nullptr, info,
                             unrolledHeader, oldPreheader->trackedSite(), MBasicBlock::NORMAL);
    } else {
        unrolledHeader = MBasicBlock::NewAsmJS(graph, info, oldPreheader, MBasicBlock::LOOP_HEADER);
        unrolledBackedge = MBasicBlock::NewAsmJS(graph, info, unrolledHeader, MBasicBlock::NORMAL);
        newPreheader = MBasicBlock::NewAsmJS(graph, info, unrolledHeader, MBasicBlock::NORMAL);
    }

    unrolledHeader->discardAllResumePoints();
    unrolledBackedge->discardAllResumePoints();
    newPreheader->discardAllResumePoints();

    // Insert new blocks at their RPO position, and update block ids.
    graph.insertBlockAfter(oldPreheader, unrolledHeader);
    graph.insertBlockAfter(unrolledHeader, unrolledBackedge);
    graph.insertBlockAfter(unrolledBackedge, newPreheader);
    graph.renumberBlocksAfter(oldPreheader);

    // We don't tolerate allocation failure after this point.
    // TODO: This is a bit drastic, is it possible to improve this?
    AutoEnterOOMUnsafeRegion oomUnsafe;

    if (!unrolledDefinitions.init())
        oomUnsafe.crash("LoopUnroller::go");

    // Add phis to the unrolled loop header which correspond to the phis in the
    // original loop header.
    MOZ_ASSERT(header->getPredecessor(0) == oldPreheader);
    for (MPhiIterator iter(header->phisBegin()); iter != header->phisEnd(); iter++) {
        MPhi* old = *iter;
        MOZ_ASSERT(old->numOperands() == 2);
        MPhi* phi = MPhi::New(alloc);
        phi->setResultType(old->type());
        phi->setResultTypeSet(old->resultTypeSet());
        phi->setRange(old->range());

        unrolledHeader->addPhi(phi);

        if (!phi->reserveLength(2))
            oomUnsafe.crash("LoopUnroller::go");

        // Set the first input for the phi for now. We'll set the second after
        // finishing the unroll.
        phi->addInput(old->getOperand(0));

        // The old phi will now take the value produced by the unrolled loop.
        old->replaceOperand(0, phi);

        if (!unrolledDefinitions.putNew(old, phi))
            oomUnsafe.crash("LoopUnroller::go");
    }

    // The loop condition can bail out on e.g. integer overflow, so make a
    // resume point based on the initial resume point of the original header.
    MResumePoint* headerResumePoint = header->entryResumePoint();
    if (headerResumePoint) {
        MResumePoint* rp = makeReplacementResumePoint(unrolledHeader, headerResumePoint);
        if (!rp)
            oomUnsafe.crash("LoopUnroller::makeReplacementResumePoint");
        unrolledHeader->setEntryResumePoint(rp);

        // Perform an interrupt check at the start of the unrolled loop.
        unrolledHeader->add(MInterruptCheck::New(alloc));
    }

    // Generate code for the test in the unrolled loop.
    for (size_t i = 0; i < remainingIterationsInequality.numTerms(); i++) {
        MDefinition* def = remainingIterationsInequality.term(i).term;
        MDefinition* replacement = getReplacementDefinition(def);
        remainingIterationsInequality.replaceTerm(i, replacement);
    }
    MCompare* compare = ConvertLinearInequality(alloc, unrolledHeader, remainingIterationsInequality);
    MTest* unrolledTest = MTest::New(alloc, compare, unrolledBackedge, newPreheader);
    unrolledHeader->end(unrolledTest);

    // Make an entry resume point for the unrolled body. The unrolled header
    // does not have side effects on stack values, even if the original loop
    // header does, so use the same resume point as for the unrolled header.
    if (headerResumePoint) {
        MResumePoint* rp = makeReplacementResumePoint(unrolledBackedge, headerResumePoint);
        if (!rp)
            oomUnsafe.crash("LoopUnroller::makeReplacementResumePoint");
        unrolledBackedge->setEntryResumePoint(rp);
    }

    // Make an entry resume point for the new preheader. There are no
    // instructions which use this but some other stuff wants one to be here.
    if (headerResumePoint) {
        MResumePoint* rp = makeReplacementResumePoint(newPreheader, headerResumePoint);
        if (!rp)
            oomUnsafe.crash("LoopUnroller::makeReplacementResumePoint");
        newPreheader->setEntryResumePoint(rp);
    }

    // Generate the unrolled code.
    MOZ_ASSERT(UnrollCount > 1);
    size_t unrollIndex = 0;
    while (true) {
        // Clone the contents of the original loop into the unrolled loop body.
        for (size_t i = 0; i < ArrayLength(bodyBlocks); i++) {
            MBasicBlock* block = bodyBlocks[i];
            for (MInstructionIterator iter(block->begin()); iter != block->end(); iter++) {
                MInstruction* ins = *iter;
                if (ins->canClone()) {
                    if (!makeReplacementInstruction(*iter))
                        oomUnsafe.crash("LoopUnroller::makeReplacementDefinition");
                } else {
                    // Control instructions are handled separately.
                    MOZ_ASSERT(ins->isTest() || ins->isGoto() || ins->isInterruptCheck());
                }
            }
        }

        // Compute the value of each loop header phi after the execution of
        // this unrolled iteration.
        MDefinitionVector phiValues(alloc);
        MOZ_ASSERT(header->getPredecessor(1) == backedge);
        for (MPhiIterator iter(header->phisBegin()); iter != header->phisEnd(); iter++) {
            MPhi* old = *iter;
            MDefinition* oldInput = old->getOperand(1);
            if (!phiValues.append(getReplacementDefinition(oldInput)))
                oomUnsafe.crash("LoopUnroller::go");
        }

        unrolledDefinitions.clear();

        if (unrollIndex == UnrollCount - 1) {
            // We're at the end of the last unrolled iteration, set the
            // backedge input for the unrolled loop phis.
            size_t phiIndex = 0;
            for (MPhiIterator iter(unrolledHeader->phisBegin()); iter != unrolledHeader->phisEnd(); iter++) {
                MPhi* phi = *iter;
                phi->addInput(phiValues[phiIndex++]);
            }
            MOZ_ASSERT(phiIndex == phiValues.length());
            break;
        }

        // Update the map for the phis in the next iteration.
        size_t phiIndex = 0;
        for (MPhiIterator iter(header->phisBegin()); iter != header->phisEnd(); iter++) {
            MPhi* old = *iter;
            if (!unrolledDefinitions.putNew(old, phiValues[phiIndex++]))
                oomUnsafe.crash("LoopUnroller::go");
        }
        MOZ_ASSERT(phiIndex == phiValues.length());

        unrollIndex++;
    }

    MGoto* backedgeJump = MGoto::New(alloc, unrolledHeader);
    unrolledBackedge->end(backedgeJump);

    // Place the old preheader before the unrolled loop.
    MOZ_ASSERT(oldPreheader->lastIns()->isGoto());
    oldPreheader->discardLastIns();
    oldPreheader->end(MGoto::New(alloc, unrolledHeader));

    // Place the new preheader before the original loop.
    newPreheader->end(MGoto::New(alloc, header));

    // Cleanup the MIR graph.
    if (!unrolledHeader->addPredecessorWithoutPhis(unrolledBackedge))
        oomUnsafe.crash("LoopUnroller::go");
    header->replacePredecessor(oldPreheader, newPreheader);
    oldPreheader->setSuccessorWithPhis(unrolledHeader, 0);
    newPreheader->setSuccessorWithPhis(header, 0);
    unrolledBackedge->setSuccessorWithPhis(unrolledHeader, 1);
}

bool
jit::UnrollLoops(MIRGraph& graph, const LoopIterationBoundVector& bounds)
{
    if (bounds.empty())
        return true;

    for (size_t i = 0; i < bounds.length(); i++) {
        LoopUnroller unroller(graph);
        unroller.go(bounds[i]);
    }

    // The MIR graph is valid, but now has several new blocks. Update or
    // recompute previous analysis information for the remaining optimization
    // passes.
    ClearDominatorTree(graph);
    if (!BuildDominatorTree(graph))
        return false;

    return true;
}

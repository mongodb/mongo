/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonAnalysis.h"

#include "jit/AliasAnalysis.h"
#include "jit/BaselineInspector.h"
#include "jit/BaselineJIT.h"
#include "jit/FlowAliasAnalysis.h"
#include "jit/Ion.h"
#include "jit/IonBuilder.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/LIR.h"
#include "jit/Lowering.h"
#include "jit/MIRGraph.h"
#include "vm/RegExpObject.h"
#include "vm/SelfHosting.h"

#include "jit/shared/Lowering-shared-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

typedef Vector<MPhi*, 16, SystemAllocPolicy> MPhiVector;

static bool
FlagPhiInputsAsHavingRemovedUses(MIRGenerator* mir, MBasicBlock* block, MBasicBlock* succ,
                                 MPhiVector& worklist)
{
    // When removing an edge between 2 blocks, we might remove the ability of
    // later phases to figure out that the uses of a Phi should be considered as
    // a use of all its inputs. Thus we need to mark the Phi inputs as having
    // removed uses iff the phi has any uses.
    //
    //
    //        +--------------------+         +---------------------+
    //        |12 MFoo 6           |         |32 MBar 5            |
    //        |                    |         |                     |
    //        |   ...              |         |   ...               |
    //        |                    |         |                     |
    //        |25 MGoto Block 4    |         |43 MGoto Block 4     |
    //        +--------------------+         +---------------------+
    //                   |                              |
    //             |     |                              |
    //             |     |                              |
    //             |     +-----X------------------------+
    //             |         Edge       |
    //             |        Removed     |
    //             |                    |
    //             |       +------------v-----------+
    //             |       |50 MPhi 12 32           |
    //             |       |                        |
    //             |       |   ...                  |
    //             |       |                        |
    //             |       |70 MReturn 50           |
    //             |       +------------------------+
    //             |
    //   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    //             |
    //             v
    //
    //    ^   +--------------------+         +---------------------+
    //   /!\  |12 MConst opt-out   |         |32 MBar 5            |
    //  '---' |                    |         |                     |
    //        |   ...              |         |   ...               |
    //        |78 MBail            |         |                     |
    //        |80 MUnreachable     |         |43 MGoto Block 4     |
    //        +--------------------+         +---------------------+
    //                                                  |
    //                                                  |
    //                                                  |
    //                                  +---------------+
    //                                  |
    //                                  |
    //                                  |
    //                     +------------v-----------+
    //                     |50 MPhi 32              |
    //                     |                        |
    //                     |   ...                  |
    //                     |                        |
    //                     |70 MReturn 50           |
    //                     +------------------------+
    //
    //
    // If the inputs of the Phi are not flagged as having removed uses, then
    // later compilation phase might optimize them out. The problem is that a
    // bailout will use this value and give it back to baseline, which will then
    // use the OptimizedOut magic value in a computation.

    // Conservative upper limit for the number of Phi instructions which are
    // visited while looking for uses.
    const size_t conservativeUsesLimit = 128;

    MOZ_ASSERT(worklist.empty());
    size_t predIndex = succ->getPredecessorIndex(block);
    MPhiIterator end = succ->phisEnd();
    MPhiIterator it = succ->phisBegin();
    for (; it != end; it++) {
        MPhi* phi = *it;

        if (mir->shouldCancel("FlagPhiInputsAsHavingRemovedUses outer loop"))
            return false;

        // We are looking to mark the Phi inputs which are used across the edge
        // between the |block| and its successor |succ|.
        MDefinition* def = phi->getOperand(predIndex);
        if (def->isUseRemoved())
            continue;

        phi->setInWorklist();
        if (!worklist.append(phi))
            return false;

        // Fill the work list with all the Phi nodes uses until we reach either:
        //  - A resume point which uses the Phi as an observable operand.
        //  - An explicit use of the Phi instruction.
        //  - An implicit use of the Phi instruction.
        bool isUsed = false;
        for (size_t idx = 0; !isUsed && idx < worklist.length(); idx++) {
            phi = worklist[idx];

            if (mir->shouldCancel("FlagPhiInputsAsHavingRemovedUses inner loop 1"))
                return false;

            if (phi->isUseRemoved() || phi->isImplicitlyUsed()) {
                // The phi is implicitly used.
                isUsed = true;
                break;
            }

            MUseIterator usesEnd(phi->usesEnd());
            for (MUseIterator use(phi->usesBegin()); use != usesEnd; use++) {
                MNode* consumer = (*use)->consumer();

                if (mir->shouldCancel("FlagPhiInputsAsHavingRemovedUses inner loop 2"))
                    return false;

                if (consumer->isResumePoint()) {
                    MResumePoint* rp = consumer->toResumePoint();
                    if (rp->isObservableOperand(*use)) {
                        // The phi is observable via a resume point operand.
                        isUsed = true;
                        break;
                    }
                    continue;
                }

                MDefinition* cdef = consumer->toDefinition();
                if (!cdef->isPhi()) {
                    // The phi is explicitly used.
                    isUsed = true;
                    break;
                }

                phi = cdef->toPhi();
                if (phi->isInWorklist())
                    continue;

                phi->setInWorklist();
                if (!worklist.append(phi))
                    return false;
            }

            // Use a conservative upper bound to avoid iterating too many times
            // on very large graphs.
            if (idx >= conservativeUsesLimit) {
                isUsed = true;
                break;
            }
        }

        if (isUsed)
            def->setUseRemoved();

        // Remove all the InWorklist flags.
        while (!worklist.empty()) {
            phi = worklist.popCopy();
            phi->setNotInWorklist();
        }
    }

    return true;
}

static bool
FlagAllOperandsAsHavingRemovedUses(MIRGenerator* mir, MBasicBlock* block)
{
    const CompileInfo& info = block->info();

    // Flag all instructions operands as having removed uses.
    MInstructionIterator end = block->end();
    for (MInstructionIterator it = block->begin(); it != end; it++) {
        if (mir->shouldCancel("FlagAllOperandsAsHavingRemovedUses loop 1"))
            return false;

        MInstruction* ins = *it;
        for (size_t i = 0, e = ins->numOperands(); i < e; i++)
            ins->getOperand(i)->setUseRemovedUnchecked();

        // Flag observable resume point operands as having removed uses.
        if (MResumePoint* rp = ins->resumePoint()) {
            // Note: no need to iterate over the caller's of the resume point as
            // this is the same as the entry resume point.
            MOZ_ASSERT(&rp->block()->info() == &info);
            for (size_t i = 0, e = rp->numOperands(); i < e; i++) {
                if (info.isObservableSlot(i))
                    rp->getOperand(i)->setUseRemovedUnchecked();
            }
        }
    }

    // Flag observable operands of the entry resume point as having removed uses.
    MResumePoint* rp = block->entryResumePoint();
    while (rp) {
        if (mir->shouldCancel("FlagAllOperandsAsHavingRemovedUses loop 2"))
            return false;

        const CompileInfo& info = rp->block()->info();
        for (size_t i = 0, e = rp->numOperands(); i < e; i++) {
            if (info.isObservableSlot(i))
                rp->getOperand(i)->setUseRemovedUnchecked();
        }

        rp = rp->caller();
    }

    // Flag Phi inputs of the successors has having removed uses.
    MPhiVector worklist;
    for (size_t i = 0, e = block->numSuccessors(); i < e; i++) {
        if (mir->shouldCancel("FlagAllOperandsAsHavingRemovedUses loop 3"))
            return false;

        if (!FlagPhiInputsAsHavingRemovedUses(mir, block, block->getSuccessor(i), worklist))
            return false;
    }

    return true;
}

static void
RemoveFromSuccessors(MBasicBlock* block)
{
    // Remove this block from its successors.
    size_t numSucc = block->numSuccessors();
    while (numSucc--) {
        MBasicBlock* succ = block->getSuccessor(numSucc);
        if (succ->isDead())
            continue;
        JitSpew(JitSpew_Prune, "Remove block edge %d -> %d.", block->id(), succ->id());
        succ->removePredecessor(block);
    }
}

static void
ConvertToBailingBlock(TempAllocator& alloc, MBasicBlock* block)
{
    // Add a bailout instruction.
    MBail* bail = MBail::New(alloc, Bailout_FirstExecution);
    MInstruction* bailPoint = block->safeInsertTop();
    block->insertBefore(block->safeInsertTop(), bail);

    // Discard all remaining instructions.
    MInstructionIterator clearStart = block->begin(bailPoint);
    block->discardAllInstructionsStartingAt(clearStart);
    if (block->outerResumePoint())
        block->clearOuterResumePoint();

    // And replace the last instruction by the unreachable control instruction.
    block->end(MUnreachable::New(alloc));
}

bool
jit::PruneUnusedBranches(MIRGenerator* mir, MIRGraph& graph)
{
    MOZ_ASSERT(!mir->compilingWasm(), "wasm compilation has no code coverage support.");

    // We do a reverse-post-order traversal, marking basic blocks when the block
    // have to be converted into bailing blocks, and flagging block as
    // unreachable if all predecessors are flagged as bailing or unreachable.
    bool someUnreachable = false;
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {
        if (mir->shouldCancel("Prune unused branches (main loop)"))
            return false;

        JitSpew(JitSpew_Prune, "Investigate Block %d:", block->id());
        JitSpewIndent indent(JitSpew_Prune);

        // Do not touch entry basic blocks.
        if (*block == graph.osrBlock() || *block == graph.entryBlock()) {
            JitSpew(JitSpew_Prune, "Block %d is an entry point.", block->id());
            continue;
        }

        // Compute if all the predecessors of this block are either bailling out
        // or are already flagged as unreachable.
        bool isUnreachable = true;
        bool isLoopHeader = block->isLoopHeader();
        size_t numPred = block->numPredecessors();
        size_t i = 0;
        for (; i < numPred; i++) {
            if (mir->shouldCancel("Prune unused branches (inner loop 1)"))
                return false;

            MBasicBlock* pred = block->getPredecessor(i);

            // The backedge is visited after the loop header, but if the loop
            // header is unreachable, then we can assume that the backedge would
            // be unreachable too.
            if (isLoopHeader && pred == block->backedge())
                continue;

            // Break if any of the predecessor can continue in this block.
            if (!pred->isMarked() && !pred->unreachable()) {
                isUnreachable = false;
                break;
            }
        }

        // Compute if the block should bailout, based on the trivial heuristic
        // which is that if the block never got visited before, then it is
        // likely to not be visited after.
        bool shouldBailout =
            block->getHitState() == MBasicBlock::HitState::Count &&
            block->getHitCount() == 0;

        // Check if the predecessors got accessed a large number of times in
        // comparisons of the current block, in order to know if our attempt at
        // removing this block is not premature.
        if (!isUnreachable && shouldBailout) {
            size_t p = numPred;
            size_t predCount = 0;
            size_t numSuccessorsOfPreds = 1;
            bool isLoopExit = false;
            while (p--) {
                if (mir->shouldCancel("Prune unused branches (inner loop 2)"))
                    return false;

                MBasicBlock* pred = block->getPredecessor(p);
                if (pred->getHitState() == MBasicBlock::HitState::Count)
                    predCount += pred->getHitCount();
                isLoopExit |= pred->isLoopHeader() && pred->backedge() != *block;
                numSuccessorsOfPreds += pred->numSuccessors() - 1;
            }

            // Iterate over the approximated set of dominated blocks and count
            // the number of instructions which are dominated.  Note that this
            // approximation has issues with OSR blocks, but this should not be
            // a big deal.
            size_t numDominatedInst = 0;
            size_t numEffectfulInst = 0;
            int numInOutEdges = block->numPredecessors();
            size_t branchSpan = 0;
            ReversePostorderIterator it(block);
            do {
                if (mir->shouldCancel("Prune unused branches (inner loop 3)"))
                    return false;

                // Iterate over dominated blocks, and visit exit blocks as well.
                numInOutEdges -= it->numPredecessors();
                if (numInOutEdges < 0)
                    break;
                numInOutEdges += it->numSuccessors();

                // Collect information about the instructions within the block.
                for (MDefinitionIterator def(*it); def; def++) {
                    numDominatedInst++;
                    if (def->isEffectful())
                        numEffectfulInst++;
                }

                it++;
                branchSpan++;
            } while(numInOutEdges > 0 && it != graph.rpoEnd());

            // The goal of branch pruning is to remove branches which are
            // preventing other optimization, while keeping branches which would
            // be costly if we were to bailout. The following heuristics are
            // made to prevent bailouts in branches when we estimate that the
            // confidence is not enough to compensate for the cost of a bailout.
            //
            //   1. Confidence for removal varies with the number of hit counts
            //      of the predecessor. The reason being that the likelyhood of
            //      taking this branch is decreasing with the number of hit
            //      counts of the predecessor.
            //
            //   2. Confidence for removal varies with the number of dominated
            //      instructions. The reason being that the complexity of the
            //      branch increases with the number of instructions, thus
            //      working against other optimizations.
            //
            //   3. Confidence for removal varies with the span of the
            //      branch. The reason being that a branch that spans over a
            //      large set of blocks is likely to remove optimization
            //      opportunity as it prevents instructions from the other
            //      branches to dominate the blocks which are after.
            //
            //   4. Confidence for removal varies with the number of effectful
            //      instructions. The reason being that an effectful instruction
            //      can remove optimization opportunities based on Scalar
            //      Replacement, and based on Alias Analysis.
            //
            // The following converts various units in some form of arbitrary
            // score, such that we can compare it to a threshold.
            size_t score = 0;
            MOZ_ASSERT(numSuccessorsOfPreds >= 1);
            score += predCount * JitOptions.branchPruningHitCountFactor / numSuccessorsOfPreds;
            score += numDominatedInst * JitOptions.branchPruningInstFactor;
            score += branchSpan * JitOptions.branchPruningBlockSpanFactor;
            score += numEffectfulInst * JitOptions.branchPruningEffectfulInstFactor;
            if (score < JitOptions.branchPruningThreshold)
                shouldBailout = false;

            // If the predecessors do not have enough hit counts, keep the
            // branch, until we recompile this function later, with more
            // information.
            if (predCount / numSuccessorsOfPreds < 50)
                shouldBailout = false;

            // There is only a single successors to the predecessors, thus the
            // decision should be taken as part of the previous block
            // investigation, and this block should be unreachable.
            if (numSuccessorsOfPreds == 1)
                shouldBailout = false;

            // If this is the exit block of a loop, then keep this basic
            // block. This heuristic is useful as a bailout is often much more
            // costly than a simple exit sequence.
            if (isLoopExit)
                shouldBailout = false;

            // Interpreters are often implemented as a table switch within a for
            // loop. What might happen is that the interpreter heats up in a
            // subset of instructions, but might need other instructions for the
            // rest of the evaluation.
            if (numSuccessorsOfPreds > 8)
                shouldBailout = false;

            JitSpew(JitSpew_Prune, "info: block %d,"
                    " predCount: %zu, domInst: %zu"
                    ", span: %zu, effectful: %zu, "
                    " isLoopExit: %s, numSuccessorsOfPred: %zu."
                    " (score: %zu, shouldBailout: %s)",
                    block->id(), predCount, numDominatedInst, branchSpan, numEffectfulInst,
                    isLoopExit ? "true" : "false", numSuccessorsOfPreds,
                    score, shouldBailout ? "true" : "false");
        }

        // Continue to the next basic block if the current basic block can
        // remain unchanged.
        if (!isUnreachable && !shouldBailout)
            continue;

        someUnreachable = true;
        if (isUnreachable) {
            JitSpew(JitSpew_Prune, "Mark block %d as unreachable.", block->id());
            block->setUnreachable();
            // If the block is unreachable, then there is no need to convert it
            // to a bailing block.
        } else if (shouldBailout) {
            JitSpew(JitSpew_Prune, "Mark block %d as bailing block.", block->id());
            block->markUnchecked();
        }

        // When removing a loop header, we should ensure that its backedge is
        // removed first, otherwise this triggers an assertion in
        // removePredecessorsWithoutPhiOperands.
        if (block->isLoopHeader()) {
            JitSpew(JitSpew_Prune, "Mark block %d as bailing block. (loop backedge)", block->backedge()->id());
            block->backedge()->markUnchecked();
        }
    }

    // Returns early if nothing changed.
    if (!someUnreachable)
        return true;

    JitSpew(JitSpew_Prune, "Convert basic block to bailing blocks, and remove unreachable blocks:");
    JitSpewIndent indent(JitSpew_Prune);

    // As we are going to remove edges and basic block, we have to mark
    // instructions which would be needed by baseline if we were to bailout.
    for (PostorderIterator it(graph.poBegin()); it != graph.poEnd();) {
        if (mir->shouldCancel("Prune unused branches (marking loop)"))
            return false;

        MBasicBlock* block = *it++;
        if (!block->isMarked() && !block->unreachable())
            continue;

        FlagAllOperandsAsHavingRemovedUses(mir, block);
    }

    // Remove the blocks in post-order such that consumers are visited before
    // the predecessors, the only exception being the Phi nodes of loop headers.
    for (PostorderIterator it(graph.poBegin()); it != graph.poEnd();) {
        if (mir->shouldCancel("Prune unused branches (removal loop)"))
            return false;

        MBasicBlock* block = *it++;
        if (!block->isMarked() && !block->unreachable())
            continue;

        JitSpew(JitSpew_Prune, "Remove / Replace block %d.", block->id());
        JitSpewIndent indent(JitSpew_Prune);

        // As we are going to replace/remove the last instruction, we first have
        // to remove this block from the predecessor list of its successors.
        RemoveFromSuccessors(block);

        // Convert the current basic block to a bailing block which ends with an
        // Unreachable control instruction.
        if (block->isMarked()) {
            JitSpew(JitSpew_Prune, "Convert Block %d to a bailing block.", block->id());
            if (!graph.alloc().ensureBallast())
                return false;
            ConvertToBailingBlock(graph.alloc(), block);
            block->unmark();
        }

        // Remove all instructions.
        if (block->unreachable()) {
            JitSpew(JitSpew_Prune, "Remove Block %d.", block->id());
            JitSpewIndent indent(JitSpew_Prune);
            graph.removeBlock(block);
        }
    }

    return true;
}

static bool
SplitCriticalEdgesForBlock(MIRGraph& graph, MBasicBlock* block)
{
    if (block->numSuccessors() < 2)
        return true;
    for (size_t i = 0; i < block->numSuccessors(); i++) {
        MBasicBlock* target = block->getSuccessor(i);
        if (target->numPredecessors() < 2)
            continue;

        // Create a simple new block which contains a goto and which split the
        // edge between block and target.
        MBasicBlock* split = MBasicBlock::NewSplitEdge(graph, block, i, target);
        if (!split)
            return false;
    }
    return true;
}

// A critical edge is an edge which is neither its successor's only predecessor
// nor its predecessor's only successor. Critical edges must be split to
// prevent copy-insertion and code motion from affecting other edges.
bool
jit::SplitCriticalEdges(MIRGraph& graph)
{
    for (MBasicBlockIterator iter(graph.begin()); iter != graph.end(); iter++) {
        MBasicBlock* block = *iter;
        if (!SplitCriticalEdgesForBlock(graph, block))
            return false;
    }
    return true;
}

bool
jit::IsUint32Type(const MDefinition* def)
{
    if (def->isBeta())
        def = def->getOperand(0);

    if (def->type() != MIRType::Int32)
        return false;

    return def->isUrsh() && def->getOperand(1)->isConstant() &&
        def->getOperand(1)->toConstant()->type() == MIRType::Int32 &&
        def->getOperand(1)->toConstant()->toInt32() == 0;
}

// Return whether a block simply computes the specified constant value.
static bool
BlockComputesConstant(MBasicBlock* block, MDefinition* value, bool* constBool)
{
    // Look for values with no uses. This is used to eliminate constant
    // computing blocks in condition statements, and the phi which used to
    // consume the constant has already been removed.
    if (value->hasUses())
        return false;

    if (!value->isConstant() || value->block() != block)
        return false;
    if (!block->phisEmpty())
        return false;
    for (MInstructionIterator iter = block->begin(); iter != block->end(); ++iter) {
        if (*iter != value || !iter->isGoto())
            return false;
    }
    return value->toConstant()->valueToBoolean(constBool);
}

// Find phis that are redudant:
//
// 1) phi(a, a)
//     can get replaced by a
//
// 2) phi(filtertypeset(a, type1), filtertypeset(a, type1))
//     equals filtertypeset(a, type1)
//
// 3) phi(a, filtertypeset(a, type1))
//     equals filtertypeset(a, type1 union type(a))
//     equals filtertypeset(a, type(a))
//     equals a
//
// 4) phi(filtertypeset(a, type1), filtertypeset(a, type2))
//    equals filtertypeset(a, type1 union type2)
//
//    This is the special case. We can only replace this with 'a' iif
//    type(a) == type1 union type2. Since optimizations could have
//    happened based on a more specific phi type.
static bool
IsPhiRedudantFilter(MPhi* phi)
{
    // Handle (1) and (2)
    if (phi->operandIfRedundant())
        return true;

    // Handle (3)
    bool onlyFilters = false;
    MDefinition* a = phi->getOperand(0);
    if (a->isFilterTypeSet()) {
        a = a->toFilterTypeSet()->input();
        onlyFilters = true;
    }

    for (size_t i = 1; i < phi->numOperands(); i++) {
        MDefinition* operand = phi->getOperand(i);
        if (operand == a) {
            onlyFilters = false;
            continue;
        }
        if (operand->isFilterTypeSet() && operand->toFilterTypeSet()->input() == a)
            continue;
        return false;
    }
    if (!onlyFilters)
        return true;

    // Handle (4)
    MOZ_ASSERT(onlyFilters);
    return EqualTypes(a->type(), a->resultTypeSet(),
                      phi->type(), phi->resultTypeSet());
}

// Determine whether phiBlock/testBlock simply compute a phi and perform a
// test on it.
static bool
BlockIsSingleTest(MBasicBlock* phiBlock, MBasicBlock* testBlock, MPhi** pphi, MTest** ptest)
{
    *pphi = nullptr;
    *ptest = nullptr;

    if (phiBlock != testBlock) {
        MOZ_ASSERT(phiBlock->numSuccessors() == 1 && phiBlock->getSuccessor(0) == testBlock);
        if (!phiBlock->begin()->isGoto())
            return false;
    }

    MInstruction* ins = *testBlock->begin();
    if (!ins->isTest())
        return false;
    MTest* test = ins->toTest();
    if (!test->input()->isPhi())
        return false;
    MPhi* phi = test->input()->toPhi();
    if (phi->block() != phiBlock)
        return false;

    for (MUseIterator iter = phi->usesBegin(); iter != phi->usesEnd(); ++iter) {
        MUse* use = *iter;
        if (use->consumer() == test)
            continue;
        if (use->consumer()->isResumePoint()) {
            MBasicBlock* useBlock = use->consumer()->block();
            if (useBlock == phiBlock || useBlock == testBlock)
                continue;
        }
        return false;
    }

    for (MPhiIterator iter = phiBlock->phisBegin(); iter != phiBlock->phisEnd(); ++iter) {
        if (*iter == phi)
            continue;

        if (IsPhiRedudantFilter(*iter))
            continue;

        return false;
    }

    if (phiBlock != testBlock && !testBlock->phisEmpty())
        return false;

    *pphi = phi;
    *ptest = test;

    return true;
}

// Change block so that it ends in a goto to the specific target block.
// existingPred is an existing predecessor of the block.
static MOZ_MUST_USE bool
UpdateGotoSuccessor(TempAllocator& alloc, MBasicBlock* block, MBasicBlock* target,
                     MBasicBlock* existingPred)
{
    MInstruction* ins = block->lastIns();
    MOZ_ASSERT(ins->isGoto());
    ins->toGoto()->target()->removePredecessor(block);
    block->discardLastIns();

    MGoto* newGoto = MGoto::New(alloc, target);
    block->end(newGoto);

    return target->addPredecessorSameInputsAs(block, existingPred);
}

// Change block so that it ends in a test of the specified value, going to
// either ifTrue or ifFalse. existingPred is an existing predecessor of ifTrue
// or ifFalse with the same values incoming to ifTrue/ifFalse as block.
// existingPred is not required to be a predecessor of ifTrue/ifFalse if block
// already ends in a test going to that block on a true/false result.
static MOZ_MUST_USE bool
UpdateTestSuccessors(TempAllocator& alloc, MBasicBlock* block,
                     MDefinition* value, MBasicBlock* ifTrue, MBasicBlock* ifFalse,
                     MBasicBlock* existingPred)
{
    MInstruction* ins = block->lastIns();
    if (ins->isTest()) {
        MTest* test = ins->toTest();
        MOZ_ASSERT(test->input() == value);

        if (ifTrue != test->ifTrue()) {
            test->ifTrue()->removePredecessor(block);
            if (!ifTrue->addPredecessorSameInputsAs(block, existingPred))
                return false;
            MOZ_ASSERT(test->ifTrue() == test->getSuccessor(0));
            test->replaceSuccessor(0, ifTrue);
        }

        if (ifFalse != test->ifFalse()) {
            test->ifFalse()->removePredecessor(block);
            if (!ifFalse->addPredecessorSameInputsAs(block, existingPred))
                return false;
            MOZ_ASSERT(test->ifFalse() == test->getSuccessor(1));
            test->replaceSuccessor(1, ifFalse);
        }

        return true;
    }

    MOZ_ASSERT(ins->isGoto());
    ins->toGoto()->target()->removePredecessor(block);
    block->discardLastIns();

    MTest* test = MTest::New(alloc, value, ifTrue, ifFalse);
    block->end(test);

    if (!ifTrue->addPredecessorSameInputsAs(block, existingPred))
        return false;
    if (!ifFalse->addPredecessorSameInputsAs(block, existingPred))
        return false;
    return true;
}

static bool
MaybeFoldConditionBlock(MIRGraph& graph, MBasicBlock* initialBlock)
{
    // Optimize the MIR graph to improve the code generated for conditional
    // operations. A test like 'if (a ? b : c)' normally requires four blocks,
    // with a phi for the intermediate value. This can be improved to use three
    // blocks with no phi value, and if either b or c is constant,
    // e.g. 'if (a ? b : 0)', then the block associated with that constant
    // can be eliminated.

    /*
     * Look for a diamond pattern:
     *
     *        initialBlock
     *          /     \
     *  trueBranch  falseBranch
     *          \     /
     *          phiBlock
     *             |
     *         testBlock
     *
     * Where phiBlock contains a single phi combining values pushed onto the
     * stack by trueBranch and falseBranch, and testBlock contains a test on
     * that phi. phiBlock and testBlock may be the same block; generated code
     * will use different blocks if the (?:) op is in an inlined function.
     */

    MInstruction* ins = initialBlock->lastIns();
    if (!ins->isTest())
        return true;
    MTest* initialTest = ins->toTest();

    MBasicBlock* trueBranch = initialTest->ifTrue();
    if (trueBranch->numPredecessors() != 1 || trueBranch->numSuccessors() != 1)
        return true;
    MBasicBlock* falseBranch = initialTest->ifFalse();
    if (falseBranch->numPredecessors() != 1 || falseBranch->numSuccessors() != 1)
        return true;
    MBasicBlock* phiBlock = trueBranch->getSuccessor(0);
    if (phiBlock != falseBranch->getSuccessor(0))
        return true;
    if (phiBlock->numPredecessors() != 2)
        return true;

    if (initialBlock->isLoopBackedge() || trueBranch->isLoopBackedge() || falseBranch->isLoopBackedge())
        return true;

    MBasicBlock* testBlock = phiBlock;
    if (testBlock->numSuccessors() == 1) {
        if (testBlock->isLoopBackedge())
            return true;
        testBlock = testBlock->getSuccessor(0);
        if (testBlock->numPredecessors() != 1)
            return true;
    }

    // Make sure the test block does not have any outgoing loop backedges.
    if (!SplitCriticalEdgesForBlock(graph, testBlock))
        return false;

    MPhi* phi;
    MTest* finalTest;
    if (!BlockIsSingleTest(phiBlock, testBlock, &phi, &finalTest))
        return true;

    MDefinition* trueResult = phi->getOperand(phiBlock->indexForPredecessor(trueBranch));
    MDefinition* falseResult = phi->getOperand(phiBlock->indexForPredecessor(falseBranch));

    // OK, we found the desired pattern, now transform the graph.

    // Patch up phis that filter their input.
    for (MPhiIterator iter = phiBlock->phisBegin(); iter != phiBlock->phisEnd(); ++iter) {
        if (*iter == phi)
            continue;

        MOZ_ASSERT(IsPhiRedudantFilter(*iter));
        MDefinition* redundant = (*iter)->operandIfRedundant();

        if (!redundant) {
            redundant = (*iter)->getOperand(0);
            if (redundant->isFilterTypeSet())
                redundant = redundant->toFilterTypeSet()->input();
        }

        (*iter)->replaceAllUsesWith(redundant);
    }

    // Remove the phi from phiBlock.
    phiBlock->discardPhi(*phiBlock->phisBegin());

    // If either trueBranch or falseBranch just computes a constant for the
    // test, determine the block that branch will end up jumping to and eliminate
    // the branch. Otherwise, change the end of the block to a test that jumps
    // directly to successors of testBlock, rather than to testBlock itself.

    MBasicBlock* trueTarget = trueBranch;
    bool constBool;
    if (BlockComputesConstant(trueBranch, trueResult, &constBool)) {
        trueTarget = constBool ? finalTest->ifTrue() : finalTest->ifFalse();
        phiBlock->removePredecessor(trueBranch);
        graph.removeBlock(trueBranch);
    } else if (initialTest->input() == trueResult) {
        if (!UpdateGotoSuccessor(graph.alloc(), trueBranch, finalTest->ifTrue(), testBlock))
            return false;
    } else {
        if (!UpdateTestSuccessors(graph.alloc(), trueBranch, trueResult,
                                  finalTest->ifTrue(), finalTest->ifFalse(), testBlock))
        {
            return false;
        }
    }

    MBasicBlock* falseTarget = falseBranch;
    if (BlockComputesConstant(falseBranch, falseResult, &constBool)) {
        falseTarget = constBool ? finalTest->ifTrue() : finalTest->ifFalse();
        phiBlock->removePredecessor(falseBranch);
        graph.removeBlock(falseBranch);
    } else if (initialTest->input() == falseResult) {
        if (!UpdateGotoSuccessor(graph.alloc(), falseBranch, finalTest->ifFalse(), testBlock))
            return false;
    } else {
        if (!UpdateTestSuccessors(graph.alloc(), falseBranch, falseResult,
                                  finalTest->ifTrue(), finalTest->ifFalse(), testBlock))
        {
            return false;
        }
    }

    // Short circuit the initial test to skip any constant branch eliminated above.
    if (!UpdateTestSuccessors(graph.alloc(), initialBlock, initialTest->input(),
                              trueTarget, falseTarget, testBlock))
    {
        return false;
    }

    // Remove phiBlock, if different from testBlock.
    if (phiBlock != testBlock) {
        testBlock->removePredecessor(phiBlock);
        graph.removeBlock(phiBlock);
    }

    // Remove testBlock itself.
    finalTest->ifTrue()->removePredecessor(testBlock);
    finalTest->ifFalse()->removePredecessor(testBlock);
    graph.removeBlock(testBlock);

    return true;
}

bool
jit::FoldTests(MIRGraph& graph)
{
    for (MBasicBlockIterator block(graph.begin()); block != graph.end(); block++) {
        if (!MaybeFoldConditionBlock(graph, *block))
            return false;
    }
    return true;
}

bool
jit::FoldEmptyBlocks(MIRGraph& graph)
{
    for (MBasicBlockIterator iter(graph.begin()); iter != graph.end(); ) {
        MBasicBlock* block = *iter;
        iter++;

        if (block->numPredecessors() != 1 || block->numSuccessors() != 1)
            continue;

        if (!block->phisEmpty())
            continue;

        if (block->outerResumePoint())
            continue;

        if (*block->begin() != *block->rbegin())
            continue;

        MBasicBlock* succ = block->getSuccessor(0);
        MBasicBlock* pred = block->getPredecessor(0);

        if (succ->numPredecessors() != 1)
            continue;

        size_t pos = pred->getSuccessorIndex(block);
        pred->lastIns()->replaceSuccessor(pos, succ);

        graph.removeBlock(block);

        if (!succ->addPredecessorSameInputsAs(pred, block))
            return false;
        succ->removePredecessor(block);
    }
    return true;
}

static void
EliminateTriviallyDeadResumePointOperands(MIRGraph& graph, MResumePoint* rp)
{
    // If we will pop the top of the stack immediately after resuming,
    // then don't preserve the top value in the resume point.
    if (rp->mode() != MResumePoint::ResumeAt || *rp->pc() != JSOP_POP)
        return;

    size_t top = rp->stackDepth() - 1;
    MOZ_ASSERT(!rp->isObservableOperand(top));

    MDefinition* def = rp->getOperand(top);
    if (def->isConstant())
        return;

    MConstant* constant = rp->block()->optimizedOutConstant(graph.alloc());
    rp->replaceOperand(top, constant);
}

// Operands to a resume point which are dead at the point of the resume can be
// replaced with a magic value. This analysis supports limited detection of
// dead operands, pruning those which are defined in the resume point's basic
// block and have no uses outside the block or at points later than the resume
// point.
//
// This is intended to ensure that extra resume points within a basic block
// will not artificially extend the lifetimes of any SSA values. This could
// otherwise occur if the new resume point captured a value which is created
// between the old and new resume point and is dead at the new resume point.
bool
jit::EliminateDeadResumePointOperands(MIRGenerator* mir, MIRGraph& graph)
{
    // If we are compiling try blocks, locals and arguments may be observable
    // from catch or finally blocks (which Ion does not compile). For now just
    // disable the pass in this case.
    if (graph.hasTryBlock())
        return true;

    for (PostorderIterator block = graph.poBegin(); block != graph.poEnd(); block++) {
        if (mir->shouldCancel("Eliminate Dead Resume Point Operands (main loop)"))
            return false;

        if (MResumePoint* rp = block->entryResumePoint()) {
            if (!graph.alloc().ensureBallast())
                return false;
            EliminateTriviallyDeadResumePointOperands(graph, rp);
        }

        // The logic below can get confused on infinite loops.
        if (block->isLoopHeader() && block->backedge() == *block)
            continue;

        for (MInstructionIterator ins = block->begin(); ins != block->end(); ins++) {
            if (MResumePoint* rp = ins->resumePoint()) {
                if (!graph.alloc().ensureBallast())
                    return false;
                EliminateTriviallyDeadResumePointOperands(graph, rp);
            }

            // No benefit to replacing constant operands with other constants.
            if (ins->isConstant())
                continue;

            // Scanning uses does not give us sufficient information to tell
            // where instructions that are involved in box/unbox operations or
            // parameter passing might be live. Rewriting uses of these terms
            // in resume points may affect the interpreter's behavior. Rather
            // than doing a more sophisticated analysis, just ignore these.
            if (ins->isUnbox() || ins->isParameter() || ins->isTypeBarrier() ||
                ins->isComputeThis() || ins->isFilterTypeSet())
            {
                continue;
            }

            // Early intermediate values captured by resume points, such as
            // TypedObject, ArrayState and its allocation, may be legitimately
            // dead in Ion code, but are still needed if we bail out. They can
            // recover on bailout.
            if (ins->isNewDerivedTypedObject() || ins->isRecoveredOnBailout()) {
                MOZ_ASSERT(ins->canRecoverOnBailout());
                continue;
            }

            // If the instruction's behavior has been constant folded into a
            // separate instruction, we can't determine precisely where the
            // instruction becomes dead and can't eliminate its uses.
            if (ins->isImplicitlyUsed() || ins->isUseRemoved())
                continue;

            // Check if this instruction's result is only used within the
            // current block, and keep track of its last use in a definition
            // (not resume point). This requires the instructions in the block
            // to be numbered, ensured by running this immediately after alias
            // analysis.
            uint32_t maxDefinition = 0;
            for (MUseIterator uses(ins->usesBegin()); uses != ins->usesEnd(); uses++) {
                MNode* consumer = uses->consumer();
                if (consumer->isResumePoint()) {
                    // If the instruction's is captured by one of the resume point, then
                    // it might be observed indirectly while the frame is live on the
                    // stack, so it has to be computed.
                    MResumePoint* resume = consumer->toResumePoint();
                    if (resume->isObservableOperand(*uses)) {
                        maxDefinition = UINT32_MAX;
                        break;
                    }
                    continue;
                }

                MDefinition* def = consumer->toDefinition();
                if (def->block() != *block || def->isBox() || def->isPhi()) {
                    maxDefinition = UINT32_MAX;
                    break;
                }
                maxDefinition = Max(maxDefinition, def->id());
            }
            if (maxDefinition == UINT32_MAX)
                continue;

            // Walk the uses a second time, removing any in resume points after
            // the last use in a definition.
            for (MUseIterator uses(ins->usesBegin()); uses != ins->usesEnd(); ) {
                MUse* use = *uses++;
                if (use->consumer()->isDefinition())
                    continue;
                MResumePoint* mrp = use->consumer()->toResumePoint();
                if (mrp->block() != *block ||
                    !mrp->instruction() ||
                    mrp->instruction() == *ins ||
                    mrp->instruction()->id() <= maxDefinition)
                {
                    continue;
                }

                if (!graph.alloc().ensureBallast())
                    return false;

                // Store an optimized out magic value in place of all dead
                // resume point operands. Making any such substitution can in
                // general alter the interpreter's behavior, even though the
                // code is dead, as the interpreter will still execute opcodes
                // whose effects cannot be observed. If the magic value value
                // were to flow to, say, a dead property access the
                // interpreter could throw an exception; we avoid this problem
                // by removing dead operands before removing dead code.
                MConstant* constant = MConstant::New(graph.alloc(), MagicValue(JS_OPTIMIZED_OUT));
                block->insertBefore(*(block->begin()), constant);
                use->replaceProducer(constant);
            }
        }
    }

    return true;
}

// Test whether |def| would be needed if it had no uses.
bool
js::jit::DeadIfUnused(const MDefinition* def)
{
    return !def->isEffectful() &&
           (!def->isGuard() || def->block() == def->block()->graph().osrBlock()) &&
           !def->isGuardRangeBailouts() &&
           !def->isControlInstruction() &&
           (!def->isInstruction() || !def->toInstruction()->resumePoint());
}

// Test whether |def| may be safely discarded, due to being dead or due to being
// located in a basic block which has itself been marked for discarding.
bool
js::jit::IsDiscardable(const MDefinition* def)
{
    return !def->hasUses() && (DeadIfUnused(def) || def->block()->isMarked());
}

// Instructions are useless if they are unused and have no side effects.
// This pass eliminates useless instructions.
// The graph itself is unchanged.
bool
jit::EliminateDeadCode(MIRGenerator* mir, MIRGraph& graph)
{
    // Traverse in postorder so that we hit uses before definitions.
    // Traverse instruction list backwards for the same reason.
    for (PostorderIterator block = graph.poBegin(); block != graph.poEnd(); block++) {
        if (mir->shouldCancel("Eliminate Dead Code (main loop)"))
            return false;

        // Remove unused instructions.
        for (MInstructionReverseIterator iter = block->rbegin(); iter != block->rend(); ) {
            MInstruction* inst = *iter++;
            if (js::jit::IsDiscardable(inst))
            {
                block->discard(inst);
            }
        }
    }

    return true;
}

static inline bool
IsPhiObservable(MPhi* phi, Observability observe)
{
    // If the phi has uses which are not reflected in SSA, then behavior in the
    // interpreter may be affected by removing the phi.
    if (phi->isImplicitlyUsed() || phi->isUseRemoved())
        return true;

    // Check for uses of this phi node outside of other phi nodes.
    // Note that, initially, we skip reading resume points, which we
    // don't count as actual uses. If the only uses are resume points,
    // then the SSA name is never consumed by the program.  However,
    // after optimizations have been performed, it's possible that the
    // actual uses in the program have been (incorrectly) optimized
    // away, so we must be more conservative and consider resume
    // points as well.
    for (MUseIterator iter(phi->usesBegin()); iter != phi->usesEnd(); iter++) {
        MNode* consumer = iter->consumer();
        if (consumer->isResumePoint()) {
            MResumePoint* resume = consumer->toResumePoint();
            if (observe == ConservativeObservability)
                return true;
            if (resume->isObservableOperand(*iter))
                return true;
        } else {
            MDefinition* def = consumer->toDefinition();
            if (!def->isPhi())
                return true;
        }
    }

    return false;
}

// Handles cases like:
//    x is phi(a, x) --> a
//    x is phi(a, a) --> a
static inline MDefinition*
IsPhiRedundant(MPhi* phi)
{
    MDefinition* first = phi->operandIfRedundant();
    if (first == nullptr)
        return nullptr;

    // Propagate the ImplicitlyUsed flag if |phi| is replaced with another phi.
    if (phi->isImplicitlyUsed())
        first->setImplicitlyUsedUnchecked();

    return first;
}

bool
jit::EliminatePhis(MIRGenerator* mir, MIRGraph& graph,
                   Observability observe)
{
    // Eliminates redundant or unobservable phis from the graph.  A
    // redundant phi is something like b = phi(a, a) or b = phi(a, b),
    // both of which can be replaced with a.  An unobservable phi is
    // one that whose value is never used in the program.
    //
    // Note that we must be careful not to eliminate phis representing
    // values that the interpreter will require later.  When the graph
    // is first constructed, we can be more aggressive, because there
    // is a greater correspondence between the CFG and the bytecode.
    // After optimizations such as GVN have been performed, however,
    // the bytecode and CFG may not correspond as closely to one
    // another.  In that case, we must be more conservative.  The flag
    // |conservativeObservability| is used to indicate that eliminate
    // phis is being run after some optimizations have been performed,
    // and thus we should use more conservative rules about
    // observability.  The particular danger is that we can optimize
    // away uses of a phi because we think they are not executable,
    // but the foundation for that assumption is false TI information
    // that will eventually be invalidated.  Therefore, if
    // |conservativeObservability| is set, we will consider any use
    // from a resume point to be observable.  Otherwise, we demand a
    // use from an actual instruction.

    Vector<MPhi*, 16, SystemAllocPolicy> worklist;

    // Add all observable phis to a worklist. We use the "in worklist" bit to
    // mean "this phi is live".
    for (PostorderIterator block = graph.poBegin(); block != graph.poEnd(); block++) {
        MPhiIterator iter = block->phisBegin();
        while (iter != block->phisEnd()) {
            MPhi* phi = *iter++;

            if (mir->shouldCancel("Eliminate Phis (populate loop)"))
                return false;

            // Flag all as unused, only observable phis would be marked as used
            // when processed by the work list.
            phi->setUnused();

            // If the phi is redundant, remove it here.
            if (MDefinition* redundant = IsPhiRedundant(phi)) {
                phi->justReplaceAllUsesWith(redundant);
                block->discardPhi(phi);
                continue;
            }

            // Enqueue observable Phis.
            if (IsPhiObservable(phi, observe)) {
                phi->setInWorklist();
                if (!worklist.append(phi))
                    return false;
            }
        }
    }

    // Iteratively mark all phis reachable from live phis.
    while (!worklist.empty()) {
        if (mir->shouldCancel("Eliminate Phis (worklist)"))
            return false;

        MPhi* phi = worklist.popCopy();
        MOZ_ASSERT(phi->isUnused());
        phi->setNotInWorklist();

        // The removal of Phis can produce newly redundant phis.
        if (MDefinition* redundant = IsPhiRedundant(phi)) {
            // Add to the worklist the used phis which are impacted.
            for (MUseDefIterator it(phi); it; it++) {
                if (it.def()->isPhi()) {
                    MPhi* use = it.def()->toPhi();
                    if (!use->isUnused()) {
                        use->setUnusedUnchecked();
                        use->setInWorklist();
                        if (!worklist.append(use))
                            return false;
                    }
                }
            }
            phi->justReplaceAllUsesWith(redundant);
        } else {
            // Otherwise flag them as used.
            phi->setNotUnused();
        }

        // The current phi is/was used, so all its operands are used.
        for (size_t i = 0, e = phi->numOperands(); i < e; i++) {
            MDefinition* in = phi->getOperand(i);
            if (!in->isPhi() || !in->isUnused() || in->isInWorklist())
                continue;
            in->setInWorklist();
            if (!worklist.append(in->toPhi()))
                return false;
        }
    }

    // Sweep dead phis.
    for (PostorderIterator block = graph.poBegin(); block != graph.poEnd(); block++) {
        MPhiIterator iter = block->phisBegin();
        while (iter != block->phisEnd()) {
            MPhi* phi = *iter++;
            if (phi->isUnused()) {
                if (!phi->optimizeOutAllUses(graph.alloc()))
                    return false;
                block->discardPhi(phi);
            }
        }
    }

    return true;
}

namespace {

// The type analysis algorithm inserts conversions and box/unbox instructions
// to make the IR graph well-typed for future passes.
//
// Phi adjustment: If a phi's inputs are all the same type, the phi is
// specialized to return that type.
//
// Input adjustment: Each input is asked to apply conversion operations to its
// inputs. This may include Box, Unbox, or other instruction-specific type
// conversion operations.
//
class TypeAnalyzer
{
    MIRGenerator* mir;
    MIRGraph& graph;
    Vector<MPhi*, 0, SystemAllocPolicy> phiWorklist_;

    TempAllocator& alloc() const {
        return graph.alloc();
    }

    bool addPhiToWorklist(MPhi* phi) {
        if (phi->isInWorklist())
            return true;
        if (!phiWorklist_.append(phi))
            return false;
        phi->setInWorklist();
        return true;
    }
    MPhi* popPhi() {
        MPhi* phi = phiWorklist_.popCopy();
        phi->setNotInWorklist();
        return phi;
    }

    bool respecialize(MPhi* phi, MIRType type);
    bool propagateSpecialization(MPhi* phi);
    bool specializePhis();
    void replaceRedundantPhi(MPhi* phi);
    bool adjustPhiInputs(MPhi* phi);
    bool adjustInputs(MDefinition* def);
    bool insertConversions();

    bool checkFloatCoherency();
    bool graphContainsFloat32();
    bool markPhiConsumers();
    bool markPhiProducers();
    bool specializeValidFloatOps();
    bool tryEmitFloatOperations();

  public:
    TypeAnalyzer(MIRGenerator* mir, MIRGraph& graph)
      : mir(mir), graph(graph)
    { }

    bool analyze();
};

} /* anonymous namespace */

// Try to specialize this phi based on its non-cyclic inputs.
static MIRType
GuessPhiType(MPhi* phi, bool* hasInputsWithEmptyTypes)
{
#ifdef DEBUG
    // Check that different magic constants aren't flowing together. Ignore
    // JS_OPTIMIZED_OUT, since an operand could be legitimately optimized
    // away.
    MIRType magicType = MIRType::None;
    for (size_t i = 0; i < phi->numOperands(); i++) {
        MDefinition* in = phi->getOperand(i);
        if (in->type() == MIRType::MagicOptimizedArguments ||
            in->type() == MIRType::MagicHole ||
            in->type() == MIRType::MagicIsConstructing)
        {
            if (magicType == MIRType::None)
                magicType = in->type();
            MOZ_ASSERT(magicType == in->type());
        }
    }
#endif

    *hasInputsWithEmptyTypes = false;

    MIRType type = MIRType::None;
    bool convertibleToFloat32 = false;
    bool hasPhiInputs = false;
    for (size_t i = 0, e = phi->numOperands(); i < e; i++) {
        MDefinition* in = phi->getOperand(i);
        if (in->isPhi()) {
            hasPhiInputs = true;
            if (!in->toPhi()->triedToSpecialize())
                continue;
            if (in->type() == MIRType::None) {
                // The operand is a phi we tried to specialize, but we were
                // unable to guess its type. propagateSpecialization will
                // propagate the type to this phi when it becomes known.
                continue;
            }
        }

        // Ignore operands which we've never observed.
        if (in->resultTypeSet() && in->resultTypeSet()->empty()) {
            *hasInputsWithEmptyTypes = true;
            continue;
        }

        if (type == MIRType::None) {
            type = in->type();
            if (in->canProduceFloat32())
                convertibleToFloat32 = true;
            continue;
        }
        if (type != in->type()) {
            if (convertibleToFloat32 && in->type() == MIRType::Float32) {
                // If we only saw definitions that can be converted into Float32 before and
                // encounter a Float32 value, promote previous values to Float32
                type = MIRType::Float32;
            } else if (IsTypeRepresentableAsDouble(type) &&
                       IsTypeRepresentableAsDouble(in->type()))
            {
                // Specialize phis with int32 and double operands as double.
                type = MIRType::Double;
                convertibleToFloat32 &= in->canProduceFloat32();
            } else {
                return MIRType::Value;
            }
        }
    }

    if (type == MIRType::None && !hasPhiInputs) {
        // All inputs are non-phis with empty typesets. Use MIRType::Value
        // in this case, as it's impossible to get better type information.
        MOZ_ASSERT(*hasInputsWithEmptyTypes);
        type = MIRType::Value;
    }

    return type;
}

bool
TypeAnalyzer::respecialize(MPhi* phi, MIRType type)
{
    if (phi->type() == type)
        return true;
    phi->specialize(type);
    return addPhiToWorklist(phi);
}

bool
TypeAnalyzer::propagateSpecialization(MPhi* phi)
{
    MOZ_ASSERT(phi->type() != MIRType::None);

    // Verify that this specialization matches any phis depending on it.
    for (MUseDefIterator iter(phi); iter; iter++) {
        if (!iter.def()->isPhi())
            continue;
        MPhi* use = iter.def()->toPhi();
        if (!use->triedToSpecialize())
            continue;
        if (use->type() == MIRType::None) {
            // We tried to specialize this phi, but were unable to guess its
            // type. Now that we know the type of one of its operands, we can
            // specialize it.
            if (!respecialize(use, phi->type()))
                return false;
            continue;
        }
        if (use->type() != phi->type()) {
            // Specialize phis with int32 that can be converted to float and float operands as floats.
            if ((use->type() == MIRType::Int32 && use->canProduceFloat32() && phi->type() == MIRType::Float32) ||
                (phi->type() == MIRType::Int32 && phi->canProduceFloat32() && use->type() == MIRType::Float32))
            {
                if (!respecialize(use, MIRType::Float32))
                    return false;
                continue;
            }

            // Specialize phis with int32 and double operands as double.
            if (IsTypeRepresentableAsDouble(use->type()) &&
                IsTypeRepresentableAsDouble(phi->type()))
            {
                if (!respecialize(use, MIRType::Double))
                    return false;
                continue;
            }

            // This phi in our use chain can now no longer be specialized.
            if (!respecialize(use, MIRType::Value))
                return false;
        }
    }

    return true;
}

bool
TypeAnalyzer::specializePhis()
{
    Vector<MPhi*, 0, SystemAllocPolicy> phisWithEmptyInputTypes;

    for (PostorderIterator block(graph.poBegin()); block != graph.poEnd(); block++) {
        if (mir->shouldCancel("Specialize Phis (main loop)"))
            return false;

        for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++) {
            if (mir->shouldCancel("Specialize Phis (inner loop)"))
                return false;

            bool hasInputsWithEmptyTypes;
            MIRType type = GuessPhiType(*phi, &hasInputsWithEmptyTypes);
            phi->specialize(type);
            if (type == MIRType::None) {
                // We tried to guess the type but failed because all operands are
                // phis we still have to visit. Set the triedToSpecialize flag but
                // don't propagate the type to other phis, propagateSpecialization
                // will do that once we know the type of one of the operands.

                // Edge case: when this phi has a non-phi input with an empty
                // typeset, it's possible for two phis to have a cyclic
                // dependency and they will both have MIRType::None. Specialize
                // such phis to MIRType::Value later on.
                if (hasInputsWithEmptyTypes && !phisWithEmptyInputTypes.append(*phi))
                    return false;
                continue;
            }
            if (!propagateSpecialization(*phi))
                return false;
        }
    }

    do {
        while (!phiWorklist_.empty()) {
            if (mir->shouldCancel("Specialize Phis (worklist)"))
                return false;

            MPhi* phi = popPhi();
            if (!propagateSpecialization(phi))
                return false;
        }

        // When two phis have a cyclic dependency and inputs that have an empty
        // typeset (which are ignored by GuessPhiType), we may still have to
        // specialize these to MIRType::Value.
        while (!phisWithEmptyInputTypes.empty()) {
            if (mir->shouldCancel("Specialize Phis (phisWithEmptyInputTypes)"))
                return false;

            MPhi* phi = phisWithEmptyInputTypes.popCopy();
            if (phi->type() == MIRType::None) {
                phi->specialize(MIRType::Value);
                if (!propagateSpecialization(phi))
                    return false;
            }
        }
    } while (!phiWorklist_.empty());

    return true;
}

bool
TypeAnalyzer::adjustPhiInputs(MPhi* phi)
{
    MIRType phiType = phi->type();
    MOZ_ASSERT(phiType != MIRType::None);

    // If we specialized a type that's not Value, there are 3 cases:
    // 1. Every input is of that type.
    // 2. Every observed input is of that type (i.e., some inputs haven't been executed yet).
    // 3. Inputs were doubles and int32s, and was specialized to double.
    if (phiType != MIRType::Value) {
        for (size_t i = 0, e = phi->numOperands(); i < e; i++) {
            MDefinition* in = phi->getOperand(i);
            if (in->type() == phiType)
                continue;

            if (!alloc().ensureBallast())
                return false;

            if (in->isBox() && in->toBox()->input()->type() == phiType) {
                phi->replaceOperand(i, in->toBox()->input());
            } else {
                MInstruction* replacement;

                if (phiType == MIRType::Double && IsFloatType(in->type())) {
                    // Convert int32 operands to double.
                    replacement = MToDouble::New(alloc(), in);
                } else if (phiType == MIRType::Float32) {
                    if (in->type() == MIRType::Int32 || in->type() == MIRType::Double) {
                        replacement = MToFloat32::New(alloc(), in);
                    } else {
                        // See comment below
                        if (in->type() != MIRType::Value) {
                            MBox* box = MBox::New(alloc(), in);
                            in->block()->insertBefore(in->block()->lastIns(), box);
                            in = box;
                        }

                        MUnbox* unbox = MUnbox::New(alloc(), in, MIRType::Double, MUnbox::Fallible);
                        in->block()->insertBefore(in->block()->lastIns(), unbox);
                        replacement = MToFloat32::New(alloc(), in);
                    }
                } else {
                    // If we know this branch will fail to convert to phiType,
                    // insert a box that'll immediately fail in the fallible unbox
                    // below.
                    if (in->type() != MIRType::Value) {
                        MBox* box = MBox::New(alloc(), in);
                        in->block()->insertBefore(in->block()->lastIns(), box);
                        in = box;
                    }

                    // Be optimistic and insert unboxes when the operand is a
                    // value.
                    replacement = MUnbox::New(alloc(), in, phiType, MUnbox::Fallible);
                }

                in->block()->insertBefore(in->block()->lastIns(), replacement);
                phi->replaceOperand(i, replacement);
            }
        }

        return true;
    }

    // Box every typed input.
    for (size_t i = 0, e = phi->numOperands(); i < e; i++) {
        MDefinition* in = phi->getOperand(i);
        if (in->type() == MIRType::Value)
            continue;

        // The input is being explicitly unboxed, so sneak past and grab
        // the original box.
        if (in->isUnbox() && phi->typeIncludes(in->toUnbox()->input()))
            in = in->toUnbox()->input();

        if (in->type() != MIRType::Value) {
            if (!alloc().ensureBallast())
                return false;

            MBasicBlock* pred = phi->block()->getPredecessor(i);
            in = AlwaysBoxAt(alloc(), pred->lastIns(), in);
        }

        phi->replaceOperand(i, in);
    }

    return true;
}

bool
TypeAnalyzer::adjustInputs(MDefinition* def)
{
    // Definitions such as MPhi have no type policy.
    if (!def->isInstruction())
        return true;

    MInstruction* ins = def->toInstruction();
    TypePolicy* policy = ins->typePolicy();
    if (policy && !policy->adjustInputs(alloc(), ins))
        return false;
    return true;
}

void
TypeAnalyzer::replaceRedundantPhi(MPhi* phi)
{
    MBasicBlock* block = phi->block();
    js::Value v;
    switch (phi->type()) {
      case MIRType::Undefined:
        v = UndefinedValue();
        break;
      case MIRType::Null:
        v = NullValue();
        break;
      case MIRType::MagicOptimizedArguments:
        v = MagicValue(JS_OPTIMIZED_ARGUMENTS);
        break;
      case MIRType::MagicOptimizedOut:
        v = MagicValue(JS_OPTIMIZED_OUT);
        break;
      case MIRType::MagicUninitializedLexical:
        v = MagicValue(JS_UNINITIALIZED_LEXICAL);
        break;
      default:
        MOZ_CRASH("unexpected type");
    }
    MConstant* c = MConstant::New(alloc(), v);
    // The instruction pass will insert the box
    block->insertBefore(*(block->begin()), c);
    phi->justReplaceAllUsesWith(c);
}

bool
TypeAnalyzer::insertConversions()
{
    // Instructions are processed in reverse postorder: all uses are defs are
    // seen before uses. This ensures that output adjustment (which may rewrite
    // inputs of uses) does not conflict with input adjustment.
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {
        if (mir->shouldCancel("Insert Conversions"))
            return false;

        for (MPhiIterator iter(block->phisBegin()), end(block->phisEnd()); iter != end; ) {
            MPhi* phi = *iter++;
            if (phi->type() == MIRType::Undefined ||
                phi->type() == MIRType::Null ||
                phi->type() == MIRType::MagicOptimizedArguments ||
                phi->type() == MIRType::MagicOptimizedOut ||
                phi->type() == MIRType::MagicUninitializedLexical)
            {
                if (!alloc().ensureBallast())
                    return false;

                replaceRedundantPhi(phi);
                block->discardPhi(phi);
            } else {
                if (!adjustPhiInputs(phi))
                    return false;
            }
        }

        // AdjustInputs can add/remove/mutate instructions before and after the
        // current instruction. Only increment the iterator after it is finished.
        for (MInstructionIterator iter(block->begin()); iter != block->end(); iter++) {
            if (!alloc().ensureBallast())
                return false;

            if (!adjustInputs(*iter))
                return false;
        }
    }
    return true;
}

// This function tries to emit Float32 specialized operations whenever it's possible.
// MIR nodes are flagged as:
// - Producers, when they can create Float32 that might need to be coerced into a Double.
//   Loads in Float32 arrays and conversions to Float32 are producers.
// - Consumers, when they can have Float32 as inputs and validate a legal use of a Float32.
//   Stores in Float32 arrays and conversions to Float32 are consumers.
// - Float32 commutative, when using the Float32 instruction instead of the Double instruction
//   does not result in a compound loss of precision. This is the case for +, -, /, * with 2
//   operands, for instance. However, an addition with 3 operands is not commutative anymore,
//   so an intermediate coercion is needed.
// Except for phis, all these flags are known after Ion building, so they cannot change during
// the process.
//
// The idea behind the algorithm is easy: whenever we can prove that a commutative operation
// has only producers as inputs and consumers as uses, we can specialize the operation as a
// float32 operation. Otherwise, we have to convert all float32 inputs to doubles. Even
// if a lot of conversions are produced, GVN will take care of eliminating the redundant ones.
//
// Phis have a special status. Phis need to be flagged as producers or consumers as they can
// be inputs or outputs of commutative instructions. Fortunately, producers and consumers
// properties are such that we can deduce the property using all non phis inputs first (which form
// an initial phi graph) and then propagate all properties from one phi to another using a
// fixed point algorithm. The algorithm is ensured to terminate as each iteration has less or as
// many flagged phis as the previous iteration (so the worst steady state case is all phis being
// flagged as false).
//
// In a nutshell, the algorithm applies three passes:
// 1 - Determine which phis are consumers. Each phi gets an initial value by making a global AND on
// all its non-phi inputs. Then each phi propagates its value to other phis. If after propagation,
// the flag value changed, we have to reapply the algorithm on all phi operands, as a phi is a
// consumer if all of its uses are consumers.
// 2 - Determine which phis are producers. It's the same algorithm, except that we have to reapply
// the algorithm on all phi uses, as a phi is a producer if all of its operands are producers.
// 3 - Go through all commutative operations and ensure their inputs are all producers and their
// uses are all consumers.
bool
TypeAnalyzer::markPhiConsumers()
{
    MOZ_ASSERT(phiWorklist_.empty());

    // Iterate in postorder so worklist is initialized to RPO.
    for (PostorderIterator block(graph.poBegin()); block != graph.poEnd(); ++block) {
        if (mir->shouldCancel("Ensure Float32 commutativity - Consumer Phis - Initial state"))
            return false;

        for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); ++phi) {
            MOZ_ASSERT(!phi->isInWorklist());
            bool canConsumeFloat32 = true;
            for (MUseDefIterator use(*phi); canConsumeFloat32 && use; use++) {
                MDefinition* usedef = use.def();
                canConsumeFloat32 &= usedef->isPhi() || usedef->canConsumeFloat32(use.use());
            }
            phi->setCanConsumeFloat32(canConsumeFloat32);
            if (canConsumeFloat32 && !addPhiToWorklist(*phi))
                return false;
        }
    }

    while (!phiWorklist_.empty()) {
        if (mir->shouldCancel("Ensure Float32 commutativity - Consumer Phis - Fixed point"))
            return false;

        MPhi* phi = popPhi();
        MOZ_ASSERT(phi->canConsumeFloat32(nullptr /* unused */));

        bool validConsumer = true;
        for (MUseDefIterator use(phi); use; use++) {
            MDefinition* def = use.def();
            if (def->isPhi() && !def->canConsumeFloat32(use.use())) {
                validConsumer = false;
                break;
            }
        }

        if (validConsumer)
            continue;

        // Propagate invalidated phis
        phi->setCanConsumeFloat32(false);
        for (size_t i = 0, e = phi->numOperands(); i < e; ++i) {
            MDefinition* input = phi->getOperand(i);
            if (input->isPhi() && !input->isInWorklist() && input->canConsumeFloat32(nullptr /* unused */))
            {
                if (!addPhiToWorklist(input->toPhi()))
                    return false;
            }
        }
    }
    return true;
}

bool
TypeAnalyzer::markPhiProducers()
{
    MOZ_ASSERT(phiWorklist_.empty());

    // Iterate in reverse postorder so worklist is initialized to PO.
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); ++block) {
        if (mir->shouldCancel("Ensure Float32 commutativity - Producer Phis - initial state"))
            return false;

        for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); ++phi) {
            MOZ_ASSERT(!phi->isInWorklist());
            bool canProduceFloat32 = true;
            for (size_t i = 0, e = phi->numOperands(); canProduceFloat32 && i < e; ++i) {
                MDefinition* input = phi->getOperand(i);
                canProduceFloat32 &= input->isPhi() || input->canProduceFloat32();
            }
            phi->setCanProduceFloat32(canProduceFloat32);
            if (canProduceFloat32 && !addPhiToWorklist(*phi))
                return false;
        }
    }

    while (!phiWorklist_.empty()) {
        if (mir->shouldCancel("Ensure Float32 commutativity - Producer Phis - Fixed point"))
            return false;

        MPhi* phi = popPhi();
        MOZ_ASSERT(phi->canProduceFloat32());

        bool validProducer = true;
        for (size_t i = 0, e = phi->numOperands(); i < e; ++i) {
            MDefinition* input = phi->getOperand(i);
            if (input->isPhi() && !input->canProduceFloat32()) {
                validProducer = false;
                break;
            }
        }

        if (validProducer)
            continue;

        // Propagate invalidated phis
        phi->setCanProduceFloat32(false);
        for (MUseDefIterator use(phi); use; use++) {
            MDefinition* def = use.def();
            if (def->isPhi() && !def->isInWorklist() && def->canProduceFloat32())
            {
                if (!addPhiToWorklist(def->toPhi()))
                    return false;
            }
        }
    }
    return true;
}

bool
TypeAnalyzer::specializeValidFloatOps()
{
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); ++block) {
        if (mir->shouldCancel("Ensure Float32 commutativity - Instructions"))
            return false;

        for (MInstructionIterator ins(block->begin()); ins != block->end(); ++ins) {
            if (!ins->isFloat32Commutative())
                continue;

            if (ins->type() == MIRType::Float32)
                continue;

            if (!alloc().ensureBallast())
                return false;

            // This call will try to specialize the instruction iff all uses are consumers and
            // all inputs are producers.
            ins->trySpecializeFloat32(alloc());
        }
    }
    return true;
}

bool
TypeAnalyzer::graphContainsFloat32()
{
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); ++block) {
        for (MDefinitionIterator def(*block); def; def++) {
            if (mir->shouldCancel("Ensure Float32 commutativity - Graph contains Float32"))
                return false;

            if (def->type() == MIRType::Float32)
                return true;
        }
    }
    return false;
}

bool
TypeAnalyzer::tryEmitFloatOperations()
{
    // Asm.js uses the ahead of time type checks to specialize operations, no need to check
    // them again at this point.
    if (mir->compilingWasm())
        return true;

    // Check ahead of time that there is at least one definition typed as Float32, otherwise we
    // don't need this pass.
    if (!graphContainsFloat32())
        return true;

    if (!markPhiConsumers())
       return false;
    if (!markPhiProducers())
       return false;
    if (!specializeValidFloatOps())
       return false;
    return true;
}

bool
TypeAnalyzer::checkFloatCoherency()
{
#ifdef DEBUG
    // Asserts that all Float32 instructions are flowing into Float32 consumers or specialized
    // operations
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); ++block) {
        if (mir->shouldCancel("Check Float32 coherency"))
            return false;

        for (MDefinitionIterator def(*block); def; def++) {
            if (def->type() != MIRType::Float32)
                continue;

            for (MUseDefIterator use(*def); use; use++) {
                MDefinition* consumer = use.def();
                MOZ_ASSERT(consumer->isConsistentFloat32Use(use.use()));
            }
        }
    }
#endif
    return true;
}

bool
TypeAnalyzer::analyze()
{
    if (!tryEmitFloatOperations())
        return false;
    if (!specializePhis())
        return false;
    if (!insertConversions())
        return false;
    if (!checkFloatCoherency())
        return false;
    return true;
}

bool
jit::ApplyTypeInformation(MIRGenerator* mir, MIRGraph& graph)
{
    TypeAnalyzer analyzer(mir, graph);

    if (!analyzer.analyze())
        return false;

    return true;
}

// Check if `def` is only the N-th operand of `useDef`.
static inline size_t
IsExclusiveNthOperand(MDefinition* useDef, size_t n, MDefinition* def)
{
    uint32_t num = useDef->numOperands();
    if (n >= num || useDef->getOperand(n) != def)
        return false;

    for (uint32_t i = 0; i < num; i++) {
        if (i == n)
            continue;
        if (useDef->getOperand(i) == def)
            return false;
    }

    return true;
}

static size_t
IsExclusiveThisArg(MCall* call, MDefinition* def)
{
    return IsExclusiveNthOperand(call, MCall::IndexOfThis(), def);
}

static size_t
IsExclusiveFirstArg(MCall* call, MDefinition* def)
{
    return IsExclusiveNthOperand(call, MCall::IndexOfArgument(0), def);
}

static bool
IsRegExpHoistableCall(CompileRuntime* runtime, MCall* call, MDefinition* def)
{
    if (call->isConstructing())
        return false;

    JSAtom* name;
    if (WrappedFunction* fun = call->getSingleTarget()) {
        if (!fun->isSelfHostedBuiltin())
            return false;
        name = GetSelfHostedFunctionName(fun->rawJSFunction());
    } else {
        MDefinition* funDef = call->getFunction();
        if (funDef->isDebugCheckSelfHosted())
            funDef = funDef->toDebugCheckSelfHosted()->input();
        if (funDef->isTypeBarrier())
            funDef = funDef->toTypeBarrier()->input();

        if (!funDef->isCallGetIntrinsicValue())
            return false;
        name = funDef->toCallGetIntrinsicValue()->name();
    }

    // Hoistable only if the RegExp is the first argument of RegExpBuiltinExec.
    if (name == runtime->names().RegExpBuiltinExec ||
        name == runtime->names().UnwrapAndCallRegExpBuiltinExec ||
        name == runtime->names().RegExpMatcher ||
        name == runtime->names().RegExpTester ||
        name == runtime->names().RegExpSearcher)
    {
        return IsExclusiveFirstArg(call, def);
    }

    if (name == runtime->names().RegExp_prototype_Exec)
        return IsExclusiveThisArg(call, def);

    return false;
}

static bool
CanCompareRegExp(MCompare* compare, MDefinition* def)
{
    MDefinition* value;
    if (compare->lhs() == def) {
        value = compare->rhs();
    } else {
        MOZ_ASSERT(compare->rhs() == def);
        value = compare->lhs();
    }

    // Comparing two regexp that weren't cloned will give different result
    // than if they were cloned.
    if (value->mightBeType(MIRType::Object))
        return false;

    // Make sure @@toPrimitive is not called which could notice
    // the difference between a not cloned/cloned regexp.

    JSOp op = compare->jsop();
    // Strict equality comparison won't invoke @@toPrimitive.
    if (op == JSOP_STRICTEQ || op == JSOP_STRICTNE)
        return true;

    if (op != JSOP_EQ && op != JSOP_NE) {
        // Relational comparison always invoke @@toPrimitive.
        MOZ_ASSERT(op == JSOP_GT || op == JSOP_GE || op == JSOP_LT || op == JSOP_LE);
        return false;
    }

    // Loose equality comparison can invoke @@toPrimitive.
    if (value->mightBeType(MIRType::Boolean) || value->mightBeType(MIRType::String) ||
        value->mightBeType(MIRType::Int32) ||
        value->mightBeType(MIRType::Double) || value->mightBeType(MIRType::Float32) ||
        value->mightBeType(MIRType::Symbol))
    {
        return false;
    }

    return true;
}

static inline void
SetNotInWorklist(MDefinitionVector& worklist)
{
    for (size_t i = 0; i < worklist.length(); i++)
        worklist[i]->setNotInWorklist();
}

static bool
IsRegExpHoistable(MIRGenerator* mir, MDefinition* regexp, MDefinitionVector& worklist,
                  bool* hoistable)
{
    MOZ_ASSERT(worklist.length() == 0);

    if (!worklist.append(regexp))
        return false;
    regexp->setInWorklist();

    for (size_t i = 0; i < worklist.length(); i++) {
        MDefinition* def = worklist[i];
        if (mir->shouldCancel("IsRegExpHoistable outer loop"))
            return false;

        for (MUseIterator use = def->usesBegin(); use != def->usesEnd(); use++) {
            if (mir->shouldCancel("IsRegExpHoistable inner loop"))
                return false;

            // Ignore resume points. At this point all uses are listed.
            // No DCE or GVN or something has happened.
            if (use->consumer()->isResumePoint())
                continue;

            MDefinition* useDef = use->consumer()->toDefinition();

            // Step through a few white-listed ops.
            if (useDef->isPhi() || useDef->isFilterTypeSet() || useDef->isGuardShape()) {
                if (useDef->isInWorklist())
                    continue;

                if (!worklist.append(useDef))
                    return false;
                useDef->setInWorklist();
                continue;
            }

            // Instructions that doesn't invoke unknown code that may modify
            // RegExp instance or pass it to elsewhere.
            if (useDef->isRegExpMatcher() || useDef->isRegExpTester() ||
                useDef->isRegExpSearcher())
            {
                if (IsExclusiveNthOperand(useDef, 0, def))
                    continue;
            } else if (useDef->isLoadFixedSlot() || useDef->isTypeOf()) {
                continue;
            } else if (useDef->isCompare()) {
                if (CanCompareRegExp(useDef->toCompare(), def))
                    continue;
            }
            // Instructions that modifies `lastIndex` property.
            else if (useDef->isStoreFixedSlot()) {
                if (IsExclusiveNthOperand(useDef, 0, def)) {
                    MStoreFixedSlot* store = useDef->toStoreFixedSlot();
                    if (store->slot() == RegExpObject::lastIndexSlot())
                        continue;
                }
            } else if (useDef->isSetPropertyCache()) {
                if (IsExclusiveNthOperand(useDef, 0, def)) {
                    MSetPropertyCache* setProp = useDef->toSetPropertyCache();
                    if (setProp->idval()->isConstant()) {
                        Value propIdVal = setProp->idval()->toConstant()->toJSValue();
                        if (propIdVal.isString()) {
                            CompileRuntime* runtime = mir->runtime;
                            if (propIdVal.toString() == runtime->names().lastIndex)
                                continue;
                        }
                    }
                }
            }
            // MCall is safe only for some known safe functions.
            else if (useDef->isCall()) {
                if (IsRegExpHoistableCall(mir->runtime, useDef->toCall(), def))
                    continue;
            }

            // Everything else is unsafe.
            SetNotInWorklist(worklist);
            worklist.clear();
            *hoistable = false;

            return true;
        }
    }

    SetNotInWorklist(worklist);
    worklist.clear();
    *hoistable = true;
    return true;
}

bool
jit::MakeMRegExpHoistable(MIRGenerator* mir, MIRGraph& graph)
{
    // If we are compiling try blocks, regular expressions may be observable
    // from catch blocks (which Ion does not compile). For now just disable the
    // pass in this case.
    if (graph.hasTryBlock())
        return true;

    MDefinitionVector worklist(graph.alloc());

    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {
        if (mir->shouldCancel("MakeMRegExpHoistable outer loop"))
            return false;

        for (MDefinitionIterator iter(*block); iter; iter++) {
            if (!*iter)
                MOZ_CRASH("confirm bug 1263794.");

            if (mir->shouldCancel("MakeMRegExpHoistable inner loop"))
                return false;

            if (!iter->isRegExp())
                continue;

            MRegExp* regexp = iter->toRegExp();

            bool hoistable = false;
            if (!IsRegExpHoistable(mir, regexp, worklist, &hoistable))
                return false;

            if (!hoistable)
                continue;

            // Make MRegExp hoistable
            regexp->setMovable();
            regexp->setDoNotClone();

            // That would be incorrect for global/sticky, because lastIndex
            // could be wrong.  Therefore setting the lastIndex to 0. That is
            // faster than a not movable regexp.
            RegExpObject* source = regexp->source();
            if (source->sticky() || source->global()) {
                if (!graph.alloc().ensureBallast())
                    return false;
                MConstant* zero = MConstant::New(graph.alloc(), Int32Value(0));
                regexp->block()->insertAfter(regexp, zero);

                MStoreFixedSlot* lastIndex =
                    MStoreFixedSlot::New(graph.alloc(), regexp, RegExpObject::lastIndexSlot(), zero);
                regexp->block()->insertAfter(zero, lastIndex);
            }
        }
    }

    return true;
}

void
jit::RenumberBlocks(MIRGraph& graph)
{
    size_t id = 0;
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++)
        block->setId(id++);
}

// A utility for code which deletes blocks. Renumber the remaining blocks,
// recompute dominators, and optionally recompute AliasAnalysis dependencies.
bool
jit::AccountForCFGChanges(MIRGenerator* mir, MIRGraph& graph, bool updateAliasAnalysis,
                          bool underValueNumberer)
{
    // Renumber the blocks and clear out the old dominator info.
    size_t id = 0;
    for (ReversePostorderIterator i(graph.rpoBegin()), e(graph.rpoEnd()); i != e; ++i) {
        i->clearDominatorInfo();
        i->setId(id++);
    }

    // Recompute dominator info.
    if (!BuildDominatorTree(graph))
        return false;

    // If needed, update alias analysis dependencies.
    if (updateAliasAnalysis) {
        TraceLoggerThread* logger = TraceLoggerForCurrentThread();
        AutoTraceLog log(logger, TraceLogger_AliasAnalysis);

        if (JitOptions.disableFlowAA) {
            if (!AliasAnalysis(mir, graph).analyze())
                return false;
        } else {
            if (!FlowAliasAnalysis(mir, graph).analyze())
                return false;
        }
    }

    AssertExtendedGraphCoherency(graph, underValueNumberer);
    return true;
}

// Remove all blocks not marked with isMarked(). Unmark all remaining blocks.
// Alias analysis dependencies may be invalid after calling this function.
bool
jit::RemoveUnmarkedBlocks(MIRGenerator* mir, MIRGraph& graph, uint32_t numMarkedBlocks)
{
    if (numMarkedBlocks == graph.numBlocks()) {
        // If all blocks are marked, no blocks need removal. Just clear the
        // marks. We'll still need to update the dominator tree below though,
        // since we may have removed edges even if we didn't remove any blocks.
        graph.unmarkBlocks();
    } else {
        // As we are going to remove edges and basic blocks, we have to mark
        // instructions which would be needed by baseline if we were to
        // bailout.
        for (PostorderIterator it(graph.poBegin()); it != graph.poEnd();) {
            MBasicBlock* block = *it++;
            if (!block->isMarked())
                continue;

            FlagAllOperandsAsHavingRemovedUses(mir, block);
        }

        // Find unmarked blocks and remove them.
        for (ReversePostorderIterator iter(graph.rpoBegin()); iter != graph.rpoEnd();) {
            MBasicBlock* block = *iter++;

            if (block->isMarked()) {
                block->unmark();
                continue;
            }

            // The block is unreachable. Clear out the loop header flag, as
            // we're doing the sweep of a mark-and-sweep here, so we no longer
            // need to worry about whether an unmarked block is a loop or not.
            if (block->isLoopHeader())
                block->clearLoopHeader();

            for (size_t i = 0, e = block->numSuccessors(); i != e; ++i)
                block->getSuccessor(i)->removePredecessor(block);
            graph.removeBlockIncludingPhis(block);
        }
    }

    // Renumber the blocks and update the dominator tree.
    return AccountForCFGChanges(mir, graph, /*updateAliasAnalysis=*/false);
}

// A Simple, Fast Dominance Algorithm by Cooper et al.
// Modified to support empty intersections for OSR, and in RPO.
static MBasicBlock*
IntersectDominators(MBasicBlock* block1, MBasicBlock* block2)
{
    MBasicBlock* finger1 = block1;
    MBasicBlock* finger2 = block2;

    MOZ_ASSERT(finger1);
    MOZ_ASSERT(finger2);

    // In the original paper, the block ID comparisons are on the postorder index.
    // This implementation iterates in RPO, so the comparisons are reversed.

    // For this function to be called, the block must have multiple predecessors.
    // If a finger is then found to be self-dominating, it must therefore be
    // reachable from multiple roots through non-intersecting control flow.
    // nullptr is returned in this case, to denote an empty intersection.

    while (finger1->id() != finger2->id()) {
        while (finger1->id() > finger2->id()) {
            MBasicBlock* idom = finger1->immediateDominator();
            if (idom == finger1)
                return nullptr; // Empty intersection.
            finger1 = idom;
        }

        while (finger2->id() > finger1->id()) {
            MBasicBlock* idom = finger2->immediateDominator();
            if (idom == finger2)
                return nullptr; // Empty intersection.
            finger2 = idom;
        }
    }
    return finger1;
}

void
jit::ClearDominatorTree(MIRGraph& graph)
{
    for (MBasicBlockIterator iter = graph.begin(); iter != graph.end(); iter++)
        iter->clearDominatorInfo();
}

static void
ComputeImmediateDominators(MIRGraph& graph)
{
    // The default start block is a root and therefore only self-dominates.
    MBasicBlock* startBlock = graph.entryBlock();
    startBlock->setImmediateDominator(startBlock);

    // Any OSR block is a root and therefore only self-dominates.
    MBasicBlock* osrBlock = graph.osrBlock();
    if (osrBlock)
        osrBlock->setImmediateDominator(osrBlock);

    bool changed = true;

    while (changed) {
        changed = false;

        ReversePostorderIterator block = graph.rpoBegin();

        // For each block in RPO, intersect all dominators.
        for (; block != graph.rpoEnd(); block++) {
            // If a node has once been found to have no exclusive dominator,
            // it will never have an exclusive dominator, so it may be skipped.
            if (block->immediateDominator() == *block)
                continue;

            // A block with no predecessors is not reachable from any entry, so
            // it self-dominates.
            if (MOZ_UNLIKELY(block->numPredecessors() == 0)) {
                block->setImmediateDominator(*block);
                continue;
            }

            MBasicBlock* newIdom = block->getPredecessor(0);

            // Find the first common dominator.
            for (size_t i = 1; i < block->numPredecessors(); i++) {
                MBasicBlock* pred = block->getPredecessor(i);
                if (pred->immediateDominator() == nullptr)
                    continue;

                newIdom = IntersectDominators(pred, newIdom);

                // If there is no common dominator, the block self-dominates.
                if (newIdom == nullptr) {
                    block->setImmediateDominator(*block);
                    changed = true;
                    break;
                }
            }

            if (newIdom && block->immediateDominator() != newIdom) {
                block->setImmediateDominator(newIdom);
                changed = true;
            }
        }
    }

#ifdef DEBUG
    // Assert that all blocks have dominator information.
    for (MBasicBlockIterator block(graph.begin()); block != graph.end(); block++) {
        MOZ_ASSERT(block->immediateDominator() != nullptr);
    }
#endif
}

bool
jit::BuildDominatorTree(MIRGraph& graph)
{
    ComputeImmediateDominators(graph);

    Vector<MBasicBlock*, 4, JitAllocPolicy> worklist(graph.alloc());

    // Traversing through the graph in post-order means that every non-phi use
    // of a definition is visited before the def itself. Since a def
    // dominates its uses, by the time we reach a particular
    // block, we have processed all of its dominated children, so
    // block->numDominated() is accurate.
    for (PostorderIterator i(graph.poBegin()); i != graph.poEnd(); i++) {
        MBasicBlock* child = *i;
        MBasicBlock* parent = child->immediateDominator();

        // Dominance is defined such that blocks always dominate themselves.
        child->addNumDominated(1);

        // If the block only self-dominates, it has no definite parent.
        // Add it to the worklist as a root for pre-order traversal.
        // This includes all roots. Order does not matter.
        if (child == parent) {
            if (!worklist.append(child))
                return false;
            continue;
        }

        if (!parent->addImmediatelyDominatedBlock(child))
            return false;

        parent->addNumDominated(child->numDominated());
    }

#ifdef DEBUG
    // If compiling with OSR, many blocks will self-dominate.
    // Without OSR, there is only one root block which dominates all.
    if (!graph.osrBlock())
        MOZ_ASSERT(graph.entryBlock()->numDominated() == graph.numBlocks());
#endif
    // Now, iterate through the dominator tree in pre-order and annotate every
    // block with its index in the traversal.
    size_t index = 0;
    while (!worklist.empty()) {
        MBasicBlock* block = worklist.popCopy();
        block->setDomIndex(index);

        if (!worklist.append(block->immediatelyDominatedBlocksBegin(),
                             block->immediatelyDominatedBlocksEnd())) {
            return false;
        }
        index++;
    }

    return true;
}

bool
jit::BuildPhiReverseMapping(MIRGraph& graph)
{
    // Build a mapping such that given a basic block, whose successor has one or
    // more phis, we can find our specific input to that phi. To make this fast
    // mapping work we rely on a specific property of our structured control
    // flow graph: For a block with phis, its predecessors each have only one
    // successor with phis. Consider each case:
    //   * Blocks with less than two predecessors cannot have phis.
    //   * Breaks. A break always has exactly one successor, and the break
    //             catch block has exactly one predecessor for each break, as
    //             well as a final predecessor for the actual loop exit.
    //   * Continues. A continue always has exactly one successor, and the
    //             continue catch block has exactly one predecessor for each
    //             continue, as well as a final predecessor for the actual
    //             loop continuation. The continue itself has exactly one
    //             successor.
    //   * An if. Each branch as exactly one predecessor.
    //   * A switch. Each branch has exactly one predecessor.
    //   * Loop tail. A new block is always created for the exit, and if a
    //             break statement is present, the exit block will forward
    //             directly to the break block.
    for (MBasicBlockIterator block(graph.begin()); block != graph.end(); block++) {
        if (block->phisEmpty())
            continue;

        // Assert on the above.
        for (size_t j = 0; j < block->numPredecessors(); j++) {
            MBasicBlock* pred = block->getPredecessor(j);

#ifdef DEBUG
            size_t numSuccessorsWithPhis = 0;
            for (size_t k = 0; k < pred->numSuccessors(); k++) {
                MBasicBlock* successor = pred->getSuccessor(k);
                if (!successor->phisEmpty())
                    numSuccessorsWithPhis++;
            }
            MOZ_ASSERT(numSuccessorsWithPhis <= 1);
#endif

            pred->setSuccessorWithPhis(*block, j);
        }
    }

    return true;
}

#ifdef DEBUG
static bool
CheckSuccessorImpliesPredecessor(MBasicBlock* A, MBasicBlock* B)
{
    // Assuming B = succ(A), verify A = pred(B).
    for (size_t i = 0; i < B->numPredecessors(); i++) {
        if (A == B->getPredecessor(i))
            return true;
    }
    return false;
}

static bool
CheckPredecessorImpliesSuccessor(MBasicBlock* A, MBasicBlock* B)
{
    // Assuming B = pred(A), verify A = succ(B).
    for (size_t i = 0; i < B->numSuccessors(); i++) {
        if (A == B->getSuccessor(i))
            return true;
    }
    return false;
}

// If you have issues with the usesBalance assertions, then define the macro
// _DEBUG_CHECK_OPERANDS_USES_BALANCE to spew information on the error output.
// This output can then be processed with the following awk script to filter and
// highlight which checks are missing or if there is an unexpected operand /
// use.
//
// define _DEBUG_CHECK_OPERANDS_USES_BALANCE 1
/*

$ ./js 2>stderr.log
$ gawk '
    /^==Check/ { context = ""; state = $2; }
    /^[a-z]/ { context = context "\n\t" $0; }
    /^==End/ {
      if (state == "Operand") {
        list[context] = list[context] - 1;
      } else if (state == "Use") {
        list[context] = list[context] + 1;
      }
    }
    END {
      for (ctx in list) {
        if (list[ctx] > 0) {
          print "Missing operand check", ctx, "\n"
        }
        if (list[ctx] < 0) {
          print "Missing use check", ctx, "\n"
        }
      };
    }'  < stderr.log

*/

static void
CheckOperand(const MNode* consumer, const MUse* use, int32_t* usesBalance)
{
    MOZ_ASSERT(use->hasProducer());
    MDefinition* producer = use->producer();
    MOZ_ASSERT(!producer->isDiscarded());
    MOZ_ASSERT(producer->block() != nullptr);
    MOZ_ASSERT(use->consumer() == consumer);
#ifdef _DEBUG_CHECK_OPERANDS_USES_BALANCE
    Fprinter print(stderr);
    print.printf("==Check Operand\n");
    use->producer()->dump(print);
    print.printf("  index: %zu\n", use->consumer()->indexOf(use));
    use->consumer()->dump(print);
    print.printf("==End\n");
#endif
    --*usesBalance;
}

static void
CheckUse(const MDefinition* producer, const MUse* use, int32_t* usesBalance)
{
    MOZ_ASSERT(!use->consumer()->block()->isDead());
    MOZ_ASSERT_IF(use->consumer()->isDefinition(),
                  !use->consumer()->toDefinition()->isDiscarded());
    MOZ_ASSERT(use->consumer()->block() != nullptr);
    MOZ_ASSERT(use->consumer()->getOperand(use->index()) == producer);
#ifdef _DEBUG_CHECK_OPERANDS_USES_BALANCE
    Fprinter print(stderr);
    print.printf("==Check Use\n");
    use->producer()->dump(print);
    print.printf("  index: %zu\n", use->consumer()->indexOf(use));
    use->consumer()->dump(print);
    print.printf("==End\n");
#endif
    ++*usesBalance;
}

// To properly encode entry resume points, we have to ensure that all the
// operands of the entry resume point are located before the safeInsertTop
// location.
static void
AssertOperandsBeforeSafeInsertTop(MResumePoint* resume)
{
    MBasicBlock* block = resume->block();
    if (block == block->graph().osrBlock())
        return;
    MInstruction* stop = block->safeInsertTop();
    for (size_t i = 0, e = resume->numOperands(); i < e; ++i) {
        MDefinition* def = resume->getOperand(i);
        if (def->block() != block)
            continue;
        if (def->isPhi())
            continue;

        for (MInstructionIterator ins = block->begin(); true; ins++) {
            if (*ins == def)
                break;
            MOZ_ASSERT(*ins != stop,
                       "Resume point operand located after the safeInsertTop location");
        }
    }
}
#endif // DEBUG

void
jit::AssertBasicGraphCoherency(MIRGraph& graph, bool force)
{
#ifdef DEBUG
    if (!JitOptions.fullDebugChecks && !force)
        return;

    MOZ_ASSERT(graph.entryBlock()->numPredecessors() == 0);
    MOZ_ASSERT(graph.entryBlock()->phisEmpty());
    MOZ_ASSERT(!graph.entryBlock()->unreachable());

    if (MBasicBlock* osrBlock = graph.osrBlock()) {
        MOZ_ASSERT(osrBlock->numPredecessors() == 0);
        MOZ_ASSERT(osrBlock->phisEmpty());
        MOZ_ASSERT(osrBlock != graph.entryBlock());
        MOZ_ASSERT(!osrBlock->unreachable());
    }

    if (MResumePoint* resumePoint = graph.entryResumePoint())
        MOZ_ASSERT(resumePoint->block() == graph.entryBlock());

    // Assert successor and predecessor list coherency.
    uint32_t count = 0;
    int32_t usesBalance = 0;
    for (MBasicBlockIterator block(graph.begin()); block != graph.end(); block++) {
        count++;

        MOZ_ASSERT(&block->graph() == &graph);
        MOZ_ASSERT(!block->isDead());
        MOZ_ASSERT_IF(block->outerResumePoint() != nullptr,
                      block->entryResumePoint() != nullptr);

        for (size_t i = 0; i < block->numSuccessors(); i++)
            MOZ_ASSERT(CheckSuccessorImpliesPredecessor(*block, block->getSuccessor(i)));

        for (size_t i = 0; i < block->numPredecessors(); i++)
            MOZ_ASSERT(CheckPredecessorImpliesSuccessor(*block, block->getPredecessor(i)));

        if (MResumePoint* resume = block->entryResumePoint()) {
            MOZ_ASSERT(!resume->instruction());
            MOZ_ASSERT(resume->block() == *block);
            AssertOperandsBeforeSafeInsertTop(resume);
        }
        if (MResumePoint* resume = block->outerResumePoint()) {
            MOZ_ASSERT(!resume->instruction());
            MOZ_ASSERT(resume->block() == *block);
        }
        for (MResumePointIterator iter(block->resumePointsBegin()); iter != block->resumePointsEnd(); iter++) {
            // We cannot yet assert that is there is no instruction then this is
            // the entry resume point because we are still storing resume points
            // in the InlinePropertyTable.
            MOZ_ASSERT_IF(iter->instruction(), iter->instruction()->block() == *block);
            for (uint32_t i = 0, e = iter->numOperands(); i < e; i++)
                CheckOperand(*iter, iter->getUseFor(i), &usesBalance);
        }
        for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++) {
            MOZ_ASSERT(phi->numOperands() == block->numPredecessors());
            MOZ_ASSERT(!phi->isRecoveredOnBailout());
            MOZ_ASSERT(phi->type() != MIRType::None);
            MOZ_ASSERT(phi->dependency() == nullptr);
        }
        for (MDefinitionIterator iter(*block); iter; iter++) {
            MOZ_ASSERT(iter->block() == *block);
            MOZ_ASSERT_IF(iter->hasUses(), iter->type() != MIRType::None);
            MOZ_ASSERT(!iter->isDiscarded());
            MOZ_ASSERT_IF(iter->isStart(),
                          *block == graph.entryBlock() || *block == graph.osrBlock());
            MOZ_ASSERT_IF(iter->isParameter(),
                          *block == graph.entryBlock() || *block == graph.osrBlock());
            MOZ_ASSERT_IF(iter->isOsrEntry(), *block == graph.osrBlock());
            MOZ_ASSERT_IF(iter->isOsrValue(), *block == graph.osrBlock());

            // Assert that use chains are valid for this instruction.
            for (uint32_t i = 0, end = iter->numOperands(); i < end; i++)
                CheckOperand(*iter, iter->getUseFor(i), &usesBalance);
            for (MUseIterator use(iter->usesBegin()); use != iter->usesEnd(); use++)
                CheckUse(*iter, *use, &usesBalance);

            if (iter->isInstruction()) {
                if (MResumePoint* resume = iter->toInstruction()->resumePoint()) {
                    MOZ_ASSERT(resume->instruction() == *iter);
                    MOZ_ASSERT(resume->block() == *block);
                    MOZ_ASSERT(resume->block()->entryResumePoint() != nullptr);
                }
            }

            if (iter->isRecoveredOnBailout())
                MOZ_ASSERT(!iter->hasLiveDefUses());
        }

        // The control instruction is not visited by the MDefinitionIterator.
        MControlInstruction* control = block->lastIns();
        MOZ_ASSERT(control->block() == *block);
        MOZ_ASSERT(!control->hasUses());
        MOZ_ASSERT(control->type() == MIRType::None);
        MOZ_ASSERT(!control->isDiscarded());
        MOZ_ASSERT(!control->isRecoveredOnBailout());
        MOZ_ASSERT(control->resumePoint() == nullptr);
        for (uint32_t i = 0, end = control->numOperands(); i < end; i++)
            CheckOperand(control, control->getUseFor(i), &usesBalance);
        for (size_t i = 0; i < control->numSuccessors(); i++)
            MOZ_ASSERT(control->getSuccessor(i));
    }

    // In case issues, see the _DEBUG_CHECK_OPERANDS_USES_BALANCE macro above.
    MOZ_ASSERT(usesBalance <= 0, "More use checks than operand checks");
    MOZ_ASSERT(usesBalance >= 0, "More operand checks than use checks");
    MOZ_ASSERT(graph.numBlocks() == count);
#endif
}

#ifdef DEBUG
static void
AssertReversePostorder(MIRGraph& graph)
{
    // Check that every block is visited after all its predecessors (except backedges).
    for (ReversePostorderIterator iter(graph.rpoBegin()); iter != graph.rpoEnd(); ++iter) {
        MBasicBlock* block = *iter;
        MOZ_ASSERT(!block->isMarked());

        for (size_t i = 0; i < block->numPredecessors(); i++) {
            MBasicBlock* pred = block->getPredecessor(i);
            if (!pred->isMarked()) {
                MOZ_ASSERT(pred->isLoopBackedge());
                MOZ_ASSERT(block->backedge() == pred);
            }
        }

        block->mark();
    }

    graph.unmarkBlocks();
}
#endif

#ifdef DEBUG
static void
AssertDominatorTree(MIRGraph& graph)
{
    // Check dominators.

    MOZ_ASSERT(graph.entryBlock()->immediateDominator() == graph.entryBlock());
    if (MBasicBlock* osrBlock = graph.osrBlock())
        MOZ_ASSERT(osrBlock->immediateDominator() == osrBlock);
    else
        MOZ_ASSERT(graph.entryBlock()->numDominated() == graph.numBlocks());

    size_t i = graph.numBlocks();
    size_t totalNumDominated = 0;
    for (MBasicBlockIterator block(graph.begin()); block != graph.end(); block++) {
        MOZ_ASSERT(block->dominates(*block));

        MBasicBlock* idom = block->immediateDominator();
        MOZ_ASSERT(idom->dominates(*block));
        MOZ_ASSERT(idom == *block || idom->id() < block->id());

        if (idom == *block) {
            totalNumDominated += block->numDominated();
        } else {
            bool foundInParent = false;
            for (size_t j = 0; j < idom->numImmediatelyDominatedBlocks(); j++) {
                if (idom->getImmediatelyDominatedBlock(j) == *block) {
                    foundInParent = true;
                    break;
                }
            }
            MOZ_ASSERT(foundInParent);
        }

        size_t numDominated = 1;
        for (size_t j = 0; j < block->numImmediatelyDominatedBlocks(); j++) {
            MBasicBlock* dom = block->getImmediatelyDominatedBlock(j);
            MOZ_ASSERT(block->dominates(dom));
            MOZ_ASSERT(dom->id() > block->id());
            MOZ_ASSERT(dom->immediateDominator() == *block);

            numDominated += dom->numDominated();
        }
        MOZ_ASSERT(block->numDominated() == numDominated);
        MOZ_ASSERT(block->numDominated() <= i);
        MOZ_ASSERT(block->numSuccessors() != 0 || block->numDominated() == 1);
        i--;
    }
    MOZ_ASSERT(i == 0);
    MOZ_ASSERT(totalNumDominated == graph.numBlocks());
}
#endif

void
jit::AssertGraphCoherency(MIRGraph& graph, bool force)
{
#ifdef DEBUG
    if (!JitOptions.checkGraphConsistency)
        return;
    if (!JitOptions.fullDebugChecks && !force)
        return;
    AssertBasicGraphCoherency(graph, force);
    AssertReversePostorder(graph);
#endif
}

#ifdef DEBUG
static bool
IsResumableMIRType(MIRType type)
{
    // see CodeGeneratorShared::encodeAllocation
    switch (type) {
      case MIRType::Undefined:
      case MIRType::Null:
      case MIRType::Boolean:
      case MIRType::Int32:
      case MIRType::Double:
      case MIRType::Float32:
      case MIRType::String:
      case MIRType::Symbol:
      case MIRType::Object:
      case MIRType::MagicOptimizedArguments:
      case MIRType::MagicOptimizedOut:
      case MIRType::MagicUninitializedLexical:
      case MIRType::MagicIsConstructing:
      case MIRType::Value:
      case MIRType::Int32x4:
      case MIRType::Int16x8:
      case MIRType::Int8x16:
      case MIRType::Float32x4:
      case MIRType::Bool32x4:
      case MIRType::Bool16x8:
      case MIRType::Bool8x16:
        return true;

      case MIRType::MagicHole:
      case MIRType::ObjectOrNull:
      case MIRType::None:
      case MIRType::Slots:
      case MIRType::Elements:
      case MIRType::Pointer:
      case MIRType::Shape:
      case MIRType::ObjectGroup:
      case MIRType::Doublex2: // NYI, see also RSimdBox::recover
      case MIRType::SinCosDouble:
      case MIRType::Int64:
        return false;
    }
    MOZ_CRASH("Unknown MIRType.");
}

static void
AssertResumableOperands(MNode* node)
{
    for (size_t i = 0, e = node->numOperands(); i < e; ++i) {
        MDefinition* op = node->getOperand(i);
        if (op->isRecoveredOnBailout())
            continue;
        MOZ_ASSERT(IsResumableMIRType(op->type()),
                   "Resume point cannot encode its operands");
    }
}

static void
AssertIfResumableInstruction(MDefinition* def)
{
    if (!def->isRecoveredOnBailout())
        return;
    AssertResumableOperands(def);
}

static void
AssertResumePointDominatedByOperands(MResumePoint* resume)
{
    for (size_t i = 0, e = resume->numOperands(); i < e; ++i) {
        MDefinition* op = resume->getOperand(i);
        if (op->type() == MIRType::MagicOptimizedArguments)
            continue;
        MOZ_ASSERT(op->block()->dominates(resume->block()),
                   "Resume point is not dominated by its operands");
    }
}
#endif // DEBUG

void
jit::AssertExtendedGraphCoherency(MIRGraph& graph, bool underValueNumberer, bool force)
{
    // Checks the basic GraphCoherency but also other conditions that
    // do not hold immediately (such as the fact that critical edges
    // are split)

#ifdef DEBUG
    if (!JitOptions.checkGraphConsistency)
        return;
    if (!JitOptions.fullDebugChecks && !force)
        return;

    AssertGraphCoherency(graph, force);

    AssertDominatorTree(graph);

    DebugOnly<uint32_t> idx = 0;
    for (MBasicBlockIterator block(graph.begin()); block != graph.end(); block++) {
        MOZ_ASSERT(block->id() == idx);
        ++idx;

        // No critical edges:
        if (block->numSuccessors() > 1)
            for (size_t i = 0; i < block->numSuccessors(); i++)
                MOZ_ASSERT(block->getSuccessor(i)->numPredecessors() == 1);

        if (block->isLoopHeader()) {
            if (underValueNumberer && block->numPredecessors() == 3) {
                // Fixup block.
                MOZ_ASSERT(block->getPredecessor(1)->numPredecessors() == 0);
                MOZ_ASSERT(graph.osrBlock(),
                           "Fixup blocks should only exists if we have an osr block.");
            } else {
                MOZ_ASSERT(block->numPredecessors() == 2);
            }
            MBasicBlock* backedge = block->backedge();
            MOZ_ASSERT(backedge->id() >= block->id());
            MOZ_ASSERT(backedge->numSuccessors() == 1);
            MOZ_ASSERT(backedge->getSuccessor(0) == *block);
        }

        if (!block->phisEmpty()) {
            for (size_t i = 0; i < block->numPredecessors(); i++) {
                MBasicBlock* pred = block->getPredecessor(i);
                MOZ_ASSERT(pred->successorWithPhis() == *block);
                MOZ_ASSERT(pred->positionInPhiSuccessor() == i);
            }
        }

        uint32_t successorWithPhis = 0;
        for (size_t i = 0; i < block->numSuccessors(); i++)
            if (!block->getSuccessor(i)->phisEmpty())
                successorWithPhis++;

        MOZ_ASSERT(successorWithPhis <= 1);
        MOZ_ASSERT((successorWithPhis != 0) == (block->successorWithPhis() != nullptr));

        // Verify that phi operands dominate the corresponding CFG predecessor
        // edges.
        for (MPhiIterator iter(block->phisBegin()), end(block->phisEnd()); iter != end; ++iter) {
            MPhi* phi = *iter;
            for (size_t i = 0, e = phi->numOperands(); i < e; ++i) {
                MOZ_ASSERT(phi->getOperand(i)->block()->dominates(block->getPredecessor(i)),
                           "Phi input is not dominated by its operand");
            }
        }

        // Verify that instructions are dominated by their operands.
        for (MInstructionIterator iter(block->begin()), end(block->end()); iter != end; ++iter) {
            MInstruction* ins = *iter;
            for (size_t i = 0, e = ins->numOperands(); i < e; ++i) {
                MDefinition* op = ins->getOperand(i);
                MBasicBlock* opBlock = op->block();
                MOZ_ASSERT(opBlock->dominates(*block),
                           "Instruction is not dominated by its operands");

                // If the operand is an instruction in the same block, check
                // that it comes first.
                if (opBlock == *block && !op->isPhi()) {
                    MInstructionIterator opIter = block->begin(op->toInstruction());
                    do {
                        ++opIter;
                        MOZ_ASSERT(opIter != block->end(),
                                   "Operand in same block as instruction does not precede");
                    } while (*opIter != ins);
                }
            }
            AssertIfResumableInstruction(ins);
            if (MResumePoint* resume = ins->resumePoint()) {
                AssertResumePointDominatedByOperands(resume);
                AssertResumableOperands(resume);
            }
        }

        // Verify that the block resume points are dominated by their operands.
        if (MResumePoint* resume = block->entryResumePoint()) {
            AssertResumePointDominatedByOperands(resume);
            AssertResumableOperands(resume);
        }
        if (MResumePoint* resume = block->outerResumePoint()) {
            AssertResumePointDominatedByOperands(resume);
            AssertResumableOperands(resume);
        }
    }
#endif
}


struct BoundsCheckInfo
{
    MBoundsCheck* check;
    uint32_t validEnd;
};

typedef HashMap<uint32_t,
                BoundsCheckInfo,
                DefaultHasher<uint32_t>,
                JitAllocPolicy> BoundsCheckMap;

// Compute a hash for bounds checks which ignores constant offsets in the index.
static HashNumber
BoundsCheckHashIgnoreOffset(MBoundsCheck* check)
{
    SimpleLinearSum indexSum = ExtractLinearSum(check->index());
    uintptr_t index = indexSum.term ? uintptr_t(indexSum.term) : 0;
    uintptr_t length = uintptr_t(check->length());
    return index ^ length;
}

static MBoundsCheck*
FindDominatingBoundsCheck(BoundsCheckMap& checks, MBoundsCheck* check, size_t index)
{
    // Since we are traversing the dominator tree in pre-order, when we
    // are looking at the |index|-th block, the next numDominated() blocks
    // we traverse are precisely the set of blocks that are dominated.
    //
    // So, this value is visible in all blocks if:
    // index <= index + ins->block->numDominated()
    // and becomes invalid after that.
    HashNumber hash = BoundsCheckHashIgnoreOffset(check);
    BoundsCheckMap::Ptr p = checks.lookup(hash);
    if (!p || index >= p->value().validEnd) {
        // We didn't find a dominating bounds check.
        BoundsCheckInfo info;
        info.check = check;
        info.validEnd = index + check->block()->numDominated();

        if(!checks.put(hash, info))
            return nullptr;

        return check;
    }

    return p->value().check;
}

static MathSpace
ExtractMathSpace(MDefinition* ins)
{
    MOZ_ASSERT(ins->isAdd() || ins->isSub());
    MBinaryArithInstruction* arith = nullptr;
    if (ins->isAdd())
        arith = ins->toAdd();
    else
        arith = ins->toSub();
    switch (arith->truncateKind()) {
      case MDefinition::NoTruncate:
      case MDefinition::TruncateAfterBailouts:
        // TruncateAfterBailouts is considered as infinite space because the
        // LinearSum will effectively remove the bailout check.
        return MathSpace::Infinite;
      case MDefinition::IndirectTruncate:
      case MDefinition::Truncate:
        return MathSpace::Modulo;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Unknown TruncateKind");
}

// Extract a linear sum from ins, if possible (otherwise giving the sum 'ins + 0').
SimpleLinearSum
jit::ExtractLinearSum(MDefinition* ins, MathSpace space)
{
    if (ins->isBeta())
        ins = ins->getOperand(0);

    if (ins->type() != MIRType::Int32)
        return SimpleLinearSum(ins, 0);

    if (ins->isConstant())
        return SimpleLinearSum(nullptr, ins->toConstant()->toInt32());

    if (!ins->isAdd() && !ins->isSub())
        return SimpleLinearSum(ins, 0);

    // Only allow math which are in the same space.
    MathSpace insSpace = ExtractMathSpace(ins);
    if (space == MathSpace::Unknown)
        space = insSpace;
    else if (space != insSpace)
        return SimpleLinearSum(ins, 0);
    MOZ_ASSERT(space == MathSpace::Modulo || space == MathSpace::Infinite);

    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);
    if (lhs->type() != MIRType::Int32 || rhs->type() != MIRType::Int32)
        return SimpleLinearSum(ins, 0);

    // Extract linear sums of each operand.
    SimpleLinearSum lsum = ExtractLinearSum(lhs, space);
    SimpleLinearSum rsum = ExtractLinearSum(rhs, space);

    // LinearSum only considers a single term operand, if both sides have
    // terms, then ignore extracted linear sums.
    if (lsum.term && rsum.term)
        return SimpleLinearSum(ins, 0);

    // Check if this is of the form <SUM> + n or n + <SUM>.
    if (ins->isAdd()) {
        int32_t constant;
        if (space == MathSpace::Modulo)
            constant = uint32_t(lsum.constant) + uint32_t(rsum.constant);
        else if (!SafeAdd(lsum.constant, rsum.constant, &constant))
            return SimpleLinearSum(ins, 0);
        return SimpleLinearSum(lsum.term ? lsum.term : rsum.term, constant);
    }

    MOZ_ASSERT(ins->isSub());
    // Check if this is of the form <SUM> - n.
    if (lsum.term) {
        int32_t constant;
        if (space == MathSpace::Modulo)
            constant = uint32_t(lsum.constant) - uint32_t(rsum.constant);
        else if (!SafeSub(lsum.constant, rsum.constant, &constant))
            return SimpleLinearSum(ins, 0);
        return SimpleLinearSum(lsum.term, constant);
    }

    // Ignore any of the form n - <SUM>.
    return SimpleLinearSum(ins, 0);
}

// Extract a linear inequality holding when a boolean test goes in the
// specified direction, of the form 'lhs + lhsN <= rhs' (or >=).
bool
jit::ExtractLinearInequality(MTest* test, BranchDirection direction,
                             SimpleLinearSum* plhs, MDefinition** prhs, bool* plessEqual)
{
    if (!test->getOperand(0)->isCompare())
        return false;

    MCompare* compare = test->getOperand(0)->toCompare();

    MDefinition* lhs = compare->getOperand(0);
    MDefinition* rhs = compare->getOperand(1);

    // TODO: optimize Compare_UInt32
    if (!compare->isInt32Comparison())
        return false;

    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    MOZ_ASSERT(rhs->type() == MIRType::Int32);

    JSOp jsop = compare->jsop();
    if (direction == FALSE_BRANCH)
        jsop = NegateCompareOp(jsop);

    SimpleLinearSum lsum = ExtractLinearSum(lhs);
    SimpleLinearSum rsum = ExtractLinearSum(rhs);

    if (!SafeSub(lsum.constant, rsum.constant, &lsum.constant))
        return false;

    // Normalize operations to use <= or >=.
    switch (jsop) {
      case JSOP_LE:
        *plessEqual = true;
        break;
      case JSOP_LT:
        /* x < y ==> x + 1 <= y */
        if (!SafeAdd(lsum.constant, 1, &lsum.constant))
            return false;
        *plessEqual = true;
        break;
      case JSOP_GE:
        *plessEqual = false;
        break;
      case JSOP_GT:
        /* x > y ==> x - 1 >= y */
        if (!SafeSub(lsum.constant, 1, &lsum.constant))
            return false;
        *plessEqual = false;
        break;
      default:
        return false;
    }

    *plhs = lsum;
    *prhs = rsum.term;

    return true;
}

static bool
TryEliminateBoundsCheck(BoundsCheckMap& checks, size_t blockIndex, MBoundsCheck* dominated, bool* eliminated)
{
    MOZ_ASSERT(!*eliminated);

    // Replace all uses of the bounds check with the actual index.
    // This is (a) necessary, because we can coalesce two different
    // bounds checks and would otherwise use the wrong index and
    // (b) helps register allocation. Note that this is safe since
    // no other pass after bounds check elimination moves instructions.
    dominated->replaceAllUsesWith(dominated->index());

    if (!dominated->isMovable())
        return true;

    if (!dominated->fallible())
        return true;

    MBoundsCheck* dominating = FindDominatingBoundsCheck(checks, dominated, blockIndex);
    if (!dominating)
        return false;

    if (dominating == dominated) {
        // We didn't find a dominating bounds check.
        return true;
    }

    // We found two bounds checks with the same hash number, but we still have
    // to make sure the lengths and index terms are equal.
    if (dominating->length() != dominated->length())
        return true;

    SimpleLinearSum sumA = ExtractLinearSum(dominating->index());
    SimpleLinearSum sumB = ExtractLinearSum(dominated->index());

    // Both terms should be nullptr or the same definition.
    if (sumA.term != sumB.term)
        return true;

    // This bounds check is redundant.
    *eliminated = true;

    // Normalize the ranges according to the constant offsets in the two indexes.
    int32_t minimumA, maximumA, minimumB, maximumB;
    if (!SafeAdd(sumA.constant, dominating->minimum(), &minimumA) ||
        !SafeAdd(sumA.constant, dominating->maximum(), &maximumA) ||
        !SafeAdd(sumB.constant, dominated->minimum(), &minimumB) ||
        !SafeAdd(sumB.constant, dominated->maximum(), &maximumB))
    {
        return false;
    }

    // Update the dominating check to cover both ranges, denormalizing the
    // result per the constant offset in the index.
    int32_t newMinimum, newMaximum;
    if (!SafeSub(Min(minimumA, minimumB), sumA.constant, &newMinimum) ||
        !SafeSub(Max(maximumA, maximumB), sumA.constant, &newMaximum))
    {
        return false;
    }

    dominating->setMinimum(newMinimum);
    dominating->setMaximum(newMaximum);
    return true;
}

static void
TryEliminateTypeBarrierFromTest(MTypeBarrier* barrier, bool filtersNull, bool filtersUndefined,
                                MTest* test, BranchDirection direction, bool* eliminated)
{
    MOZ_ASSERT(filtersNull || filtersUndefined);

    // Watch for code patterns similar to 'if (x.f) { ... = x.f }'.  If x.f
    // is either an object or null/undefined, there will be a type barrier on
    // the latter read as the null/undefined value is never realized there.
    // The type barrier can be eliminated, however, by looking at tests
    // performed on the result of the first operation that filter out all
    // types that have been seen in the first access but not the second.

    // A test 'if (x.f)' filters both null and undefined.

    // Disregard the possible unbox added before the Typebarrier for checking.
    MDefinition* input = barrier->input();
    MUnbox* inputUnbox = nullptr;
    if (input->isUnbox() && input->toUnbox()->mode() != MUnbox::Fallible) {
        inputUnbox = input->toUnbox();
        input = inputUnbox->input();
    }

    MDefinition* subject = nullptr;
    bool removeUndefined;
    bool removeNull;
    test->filtersUndefinedOrNull(direction == TRUE_BRANCH, &subject, &removeUndefined, &removeNull);

    // The Test doesn't filter undefined nor null.
    if (!subject)
        return;

    // Make sure the subject equals the input to the TypeBarrier.
    if (subject != input)
        return;

    // When the TypeBarrier filters undefined, the test must at least also do,
    // this, before the TypeBarrier can get removed.
    if (!removeUndefined && filtersUndefined)
        return;

    // When the TypeBarrier filters null, the test must at least also do,
    // this, before the TypeBarrier can get removed.
    if (!removeNull && filtersNull)
        return;

    // Eliminate the TypeBarrier. The possible TypeBarrier unboxing is kept,
    // but made infallible.
    *eliminated = true;
    if (inputUnbox)
        inputUnbox->makeInfallible();
    barrier->replaceAllUsesWith(barrier->input());
}

static bool
TryEliminateTypeBarrier(MTypeBarrier* barrier, bool* eliminated)
{
    MOZ_ASSERT(!*eliminated);

    const TemporaryTypeSet* barrierTypes = barrier->resultTypeSet();
    const TemporaryTypeSet* inputTypes = barrier->input()->resultTypeSet();

    // Disregard the possible unbox added before the Typebarrier.
    if (barrier->input()->isUnbox() && barrier->input()->toUnbox()->mode() != MUnbox::Fallible)
        inputTypes = barrier->input()->toUnbox()->input()->resultTypeSet();

    if (!barrierTypes || !inputTypes)
        return true;

    bool filtersNull = barrierTypes->filtersType(inputTypes, TypeSet::NullType());
    bool filtersUndefined = barrierTypes->filtersType(inputTypes, TypeSet::UndefinedType());

    if (!filtersNull && !filtersUndefined)
        return true;

    MBasicBlock* block = barrier->block();
    while (true) {
        BranchDirection direction;
        MTest* test = block->immediateDominatorBranch(&direction);

        if (test) {
            TryEliminateTypeBarrierFromTest(barrier, filtersNull, filtersUndefined,
                                            test, direction, eliminated);
        }

        MBasicBlock* previous = block->immediateDominator();
        if (previous == block)
            break;
        block = previous;
    }

    return true;
}

static bool
TryOptimizeLoadObjectOrNull(MDefinition* def, MDefinitionVector* peliminateList)
{
    if (def->type() != MIRType::Value)
        return true;

    // Check if this definition can only produce object or null values.
    TemporaryTypeSet* types = def->resultTypeSet();
    if (!types)
        return true;
    if (types->baseFlags() & ~(TYPE_FLAG_NULL | TYPE_FLAG_ANYOBJECT))
        return true;

    MDefinitionVector eliminateList(def->block()->graph().alloc());

    for (MUseDefIterator iter(def); iter; ++iter) {
        MDefinition* ndef = iter.def();
        switch (ndef->op()) {
          case MDefinition::Opcode::Compare:
            if (ndef->toCompare()->compareType() != MCompare::Compare_Null)
                return true;
            break;
          case MDefinition::Opcode::Test:
            break;
          case MDefinition::Opcode::PostWriteBarrier:
            break;
          case MDefinition::Opcode::StoreFixedSlot:
            break;
          case MDefinition::Opcode::StoreSlot:
            break;
          case MDefinition::Opcode::ToObjectOrNull:
            if (!eliminateList.append(ndef->toToObjectOrNull()))
                return false;
            break;
          case MDefinition::Opcode::Unbox:
            if (ndef->type() != MIRType::Object)
                return true;
            break;
          case MDefinition::Opcode::TypeBarrier:
            // For now, only handle type barriers which are not consumed
            // anywhere and only test that the value is null.
            if (ndef->hasUses() || ndef->resultTypeSet()->getKnownMIRType() != MIRType::Null)
                return true;
            break;
          default:
            return true;
        }
    }

    // On punboxing systems we are better off leaving the value boxed if it
    // is only stored back to the heap.
#ifdef JS_PUNBOX64
    bool foundUse = false;
    for (MUseDefIterator iter(def); iter; ++iter) {
        MDefinition* ndef = iter.def();
        if (!ndef->isStoreFixedSlot() && !ndef->isStoreSlot()) {
            foundUse = true;
            break;
        }
    }
    if (!foundUse)
        return true;
#endif // JS_PUNBOX64

    def->setResultType(MIRType::ObjectOrNull);

    // Fixup the result type of MTypeBarrier uses.
    for (MUseDefIterator iter(def); iter; ++iter) {
        MDefinition* ndef = iter.def();
        if (ndef->isTypeBarrier())
            ndef->setResultType(MIRType::ObjectOrNull);
    }

    // Eliminate MToObjectOrNull instruction uses.
    for (size_t i = 0; i < eliminateList.length(); i++) {
        MDefinition* ndef = eliminateList[i];
        ndef->replaceAllUsesWith(def);
        if (!peliminateList->append(ndef))
            return false;
    }

    return true;
}

static inline MDefinition*
PassthroughOperand(MDefinition* def)
{
    if (def->isConvertElementsToDoubles())
        return def->toConvertElementsToDoubles()->elements();
    if (def->isMaybeCopyElementsForWrite())
        return def->toMaybeCopyElementsForWrite()->object();
    if (!JitOptions.spectreObjectMitigationsMisc) {
        // If Spectre mitigations are enabled, LConvertUnboxedObjectToNative
        // needs to have its own def.
        if (def->isConvertUnboxedObjectToNative())
            return def->toConvertUnboxedObjectToNative()->object();
    }
    return nullptr;
}

// Eliminate checks which are redundant given each other or other instructions.
//
// A type barrier is considered redundant if all missing types have been tested
// for by earlier control instructions.
//
// A bounds check is considered redundant if it's dominated by another bounds
// check with the same length and the indexes differ by only a constant amount.
// In this case we eliminate the redundant bounds check and update the other one
// to cover the ranges of both checks.
//
// Bounds checks are added to a hash map and since the hash function ignores
// differences in constant offset, this offers a fast way to find redundant
// checks.
bool
jit::EliminateRedundantChecks(MIRGraph& graph)
{
    BoundsCheckMap checks(graph.alloc());

    if (!checks.init())
        return false;

    // Stack for pre-order CFG traversal.
    Vector<MBasicBlock*, 1, JitAllocPolicy> worklist(graph.alloc());

    // The index of the current block in the CFG traversal.
    size_t index = 0;

    // Add all self-dominating blocks to the worklist.
    // This includes all roots. Order does not matter.
    for (MBasicBlockIterator i(graph.begin()); i != graph.end(); i++) {
        MBasicBlock* block = *i;
        if (block->immediateDominator() == block) {
            if (!worklist.append(block))
                return false;
        }
    }

    MDefinitionVector eliminateList(graph.alloc());

    // Starting from each self-dominating block, traverse the CFG in pre-order.
    while (!worklist.empty()) {
        MBasicBlock* block = worklist.popCopy();

        // Add all immediate dominators to the front of the worklist.
        if (!worklist.append(block->immediatelyDominatedBlocksBegin(),
                             block->immediatelyDominatedBlocksEnd())) {
            return false;
        }

        for (MDefinitionIterator iter(block); iter; ) {
            MDefinition* def = *iter++;

            bool eliminated = false;

            switch (def->op()) {
              case MDefinition::Opcode::BoundsCheck:
                if (!TryEliminateBoundsCheck(checks, index, def->toBoundsCheck(), &eliminated))
                    return false;
                break;
              case MDefinition::Opcode::TypeBarrier:
                if (!TryEliminateTypeBarrier(def->toTypeBarrier(), &eliminated))
                    return false;
                break;
              case MDefinition::Opcode::LoadFixedSlot:
              case MDefinition::Opcode::LoadSlot:
              case MDefinition::Opcode::LoadUnboxedObjectOrNull:
                if (!TryOptimizeLoadObjectOrNull(def, &eliminateList))
                    return false;
                break;
              default:
                // Now that code motion passes have finished, replace
                // instructions which pass through one of their operands
                // (and perform additional checks) with that operand.
                if (MDefinition* passthrough = PassthroughOperand(def))
                    def->replaceAllUsesWith(passthrough);
                break;
            }

            if (eliminated)
                block->discardDef(def);
        }
        index++;
    }

    MOZ_ASSERT(index == graph.numBlocks());

    for (size_t i = 0; i < eliminateList.length(); i++) {
        MDefinition* def = eliminateList[i];
        def->block()->discardDef(def);
    }

    return true;
}

static bool
NeedsKeepAlive(MInstruction* slotsOrElements, MInstruction* use)
{
    MOZ_ASSERT(slotsOrElements->type() == MIRType::Elements ||
               slotsOrElements->type() == MIRType::Slots);

    if (slotsOrElements->block() != use->block())
        return true;

    MBasicBlock* block = use->block();
    MInstructionIterator iter(block->begin(slotsOrElements));
    MOZ_ASSERT(*iter == slotsOrElements);
    ++iter;

    while (true) {
        if (*iter == use)
            return false;

        switch (iter->op()) {
          case MDefinition::Opcode::Nop:
          case MDefinition::Opcode::Constant:
          case MDefinition::Opcode::KeepAliveObject:
          case MDefinition::Opcode::Unbox:
          case MDefinition::Opcode::LoadSlot:
          case MDefinition::Opcode::StoreSlot:
          case MDefinition::Opcode::LoadFixedSlot:
          case MDefinition::Opcode::StoreFixedSlot:
          case MDefinition::Opcode::LoadElement:
          case MDefinition::Opcode::StoreElement:
          case MDefinition::Opcode::InitializedLength:
          case MDefinition::Opcode::ArrayLength:
          case MDefinition::Opcode::BoundsCheck:
            iter++;
            break;
          default:
            return true;
        }
    }

    MOZ_CRASH("Unreachable");
}

bool
jit::AddKeepAliveInstructions(MIRGraph& graph)
{
    for (MBasicBlockIterator i(graph.begin()); i != graph.end(); i++) {
        MBasicBlock* block = *i;

        for (MInstructionIterator insIter(block->begin()); insIter != block->end(); insIter++) {
            MInstruction* ins = *insIter;
            if (ins->type() != MIRType::Elements && ins->type() != MIRType::Slots)
                continue;

            MDefinition* ownerObject;
            switch (ins->op()) {
              case MDefinition::Opcode::ConstantElements:
                continue;
              case MDefinition::Opcode::ConvertElementsToDoubles:
                // EliminateRedundantChecks should have replaced all uses.
                MOZ_ASSERT(!ins->hasUses());
                continue;
              case MDefinition::Opcode::Elements:
              case MDefinition::Opcode::TypedArrayElements:
              case MDefinition::Opcode::TypedObjectElements:
                MOZ_ASSERT(ins->numOperands() == 1);
                ownerObject = ins->getOperand(0);
                break;
              case MDefinition::Opcode::Slots:
                ownerObject = ins->toSlots()->object();
                break;
              default:
                MOZ_CRASH("Unexpected op");
            }

            MOZ_ASSERT(ownerObject->type() == MIRType::Object);

            if (ownerObject->isConstant()) {
                // Constants are kept alive by other pointers, for instance
                // ImmGCPtr in JIT code.
                continue;
            }

            for (MUseDefIterator uses(ins); uses; uses++) {
                MInstruction* use = uses.def()->toInstruction();

                if (use->isStoreElementHole()) {
                    // StoreElementHole has an explicit object operand. If GVN
                    // is disabled, we can get different unbox instructions with
                    // the same object as input, so we check for that case.
                    MOZ_ASSERT_IF(!use->toStoreElementHole()->object()->isUnbox() && !ownerObject->isUnbox(),
                                  use->toStoreElementHole()->object() == ownerObject);
                    continue;
                }

                if (use->isFallibleStoreElement()) {
                    // See StoreElementHole case above.
                    MOZ_ASSERT_IF(!use->toFallibleStoreElement()->object()->isUnbox() && !ownerObject->isUnbox(),
                                  use->toFallibleStoreElement()->object() == ownerObject);
                    continue;
                }

                if (use->isInArray()) {
                    // See StoreElementHole case above.
                    MOZ_ASSERT_IF(!use->toInArray()->object()->isUnbox() && !ownerObject->isUnbox(),
                                  use->toInArray()->object() == ownerObject);
                    continue;
                }

                if (!NeedsKeepAlive(ins, use))
                    continue;

                if (!graph.alloc().ensureBallast())
                    return false;
                MKeepAliveObject* keepAlive = MKeepAliveObject::New(graph.alloc(), ownerObject);
                use->block()->insertAfter(use, keepAlive);
            }
        }
    }

    return true;
}

bool
LinearSum::multiply(int32_t scale)
{
    for (size_t i = 0; i < terms_.length(); i++) {
        if (!SafeMul(scale, terms_[i].scale, &terms_[i].scale))
            return false;
    }
    return SafeMul(scale, constant_, &constant_);
}

bool
LinearSum::divide(uint32_t scale)
{
    MOZ_ASSERT(scale > 0);

    for (size_t i = 0; i < terms_.length(); i++) {
        if (terms_[i].scale % scale != 0)
            return false;
    }
    if (constant_ % scale != 0)
        return false;

    for (size_t i = 0; i < terms_.length(); i++)
        terms_[i].scale /= scale;
    constant_ /= scale;

    return true;
}

bool
LinearSum::add(const LinearSum& other, int32_t scale /* = 1 */)
{
    for (size_t i = 0; i < other.terms_.length(); i++) {
        int32_t newScale = scale;
        if (!SafeMul(scale, other.terms_[i].scale, &newScale))
            return false;
        if (!add(other.terms_[i].term, newScale))
            return false;
    }
    int32_t newConstant = scale;
    if (!SafeMul(scale, other.constant_, &newConstant))
        return false;
    return add(newConstant);
}

bool
LinearSum::add(SimpleLinearSum other, int32_t scale)
{
    if (other.term && !add(other.term, scale))
        return false;

    int32_t constant;
    if (!SafeMul(other.constant, scale, &constant))
        return false;

    return add(constant);
}

bool
LinearSum::add(MDefinition* term, int32_t scale)
{
    MOZ_ASSERT(term);

    if (scale == 0)
        return true;

    if (MConstant* termConst = term->maybeConstantValue()) {
        int32_t constant = termConst->toInt32();
        if (!SafeMul(constant, scale, &constant))
            return false;
        return add(constant);
    }

    for (size_t i = 0; i < terms_.length(); i++) {
        if (term == terms_[i].term) {
            if (!SafeAdd(scale, terms_[i].scale, &terms_[i].scale))
                return false;
            if (terms_[i].scale == 0) {
                terms_[i] = terms_.back();
                terms_.popBack();
            }
            return true;
        }
    }

    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!terms_.append(LinearTerm(term, scale)))
        oomUnsafe.crash("LinearSum::add");

    return true;
}

bool
LinearSum::add(int32_t constant)
{
    return SafeAdd(constant, constant_, &constant_);
}

void
LinearSum::dump(GenericPrinter& out) const
{
    for (size_t i = 0; i < terms_.length(); i++) {
        int32_t scale = terms_[i].scale;
        int32_t id = terms_[i].term->id();
        MOZ_ASSERT(scale);
        if (scale > 0) {
            if (i)
                out.printf("+");
            if (scale == 1)
                out.printf("#%d", id);
            else
                out.printf("%d*#%d", scale, id);
        } else if (scale == -1) {
            out.printf("-#%d", id);
        } else {
            out.printf("%d*#%d", scale, id);
        }
    }
    if (constant_ > 0)
        out.printf("+%d", constant_);
    else if (constant_ < 0)
        out.printf("%d", constant_);
}

void
LinearSum::dump() const
{
    Fprinter out(stderr);
    dump(out);
    out.finish();
}

MDefinition*
jit::ConvertLinearSum(TempAllocator& alloc, MBasicBlock* block, const LinearSum& sum, bool convertConstant)
{
    MDefinition* def = nullptr;

    for (size_t i = 0; i < sum.numTerms(); i++) {
        LinearTerm term = sum.term(i);
        MOZ_ASSERT(!term.term->isConstant());
        if (term.scale == 1) {
            if (def) {
                def = MAdd::New(alloc, def, term.term);
                def->toAdd()->setInt32Specialization();
                block->insertAtEnd(def->toInstruction());
                def->computeRange(alloc);
            } else {
                def = term.term;
            }
        } else if (term.scale == -1) {
            if (!def) {
                def = MConstant::New(alloc, Int32Value(0));
                block->insertAtEnd(def->toInstruction());
                def->computeRange(alloc);
            }
            def = MSub::New(alloc, def, term.term);
            def->toSub()->setInt32Specialization();
            block->insertAtEnd(def->toInstruction());
            def->computeRange(alloc);
        } else {
            MOZ_ASSERT(term.scale != 0);
            MConstant* factor = MConstant::New(alloc, Int32Value(term.scale));
            block->insertAtEnd(factor);
            MMul* mul = MMul::New(alloc, term.term, factor);
            mul->setInt32Specialization();
            block->insertAtEnd(mul);
            mul->computeRange(alloc);
            if (def) {
                def = MAdd::New(alloc, def, mul);
                def->toAdd()->setInt32Specialization();
                block->insertAtEnd(def->toInstruction());
                def->computeRange(alloc);
            } else {
                def = mul;
            }
        }
    }

    if (convertConstant && sum.constant()) {
        MConstant* constant = MConstant::New(alloc, Int32Value(sum.constant()));
        block->insertAtEnd(constant);
        constant->computeRange(alloc);
        if (def) {
            def = MAdd::New(alloc, def, constant);
            def->toAdd()->setInt32Specialization();
            block->insertAtEnd(def->toInstruction());
            def->computeRange(alloc);
        } else {
            def = constant;
        }
    }

    if (!def) {
        def = MConstant::New(alloc, Int32Value(0));
        block->insertAtEnd(def->toInstruction());
        def->computeRange(alloc);
    }

    return def;
}

MCompare*
jit::ConvertLinearInequality(TempAllocator& alloc, MBasicBlock* block, const LinearSum& sum)
{
    LinearSum lhs(sum);

    // Look for a term with a -1 scale which we can use for the rhs.
    MDefinition* rhsDef = nullptr;
    for (size_t i = 0; i < lhs.numTerms(); i++) {
        if (lhs.term(i).scale == -1) {
            AutoEnterOOMUnsafeRegion oomUnsafe;
            rhsDef = lhs.term(i).term;
            if (!lhs.add(rhsDef, 1))
               oomUnsafe.crash("ConvertLinearInequality");
            break;
        }
    }

    MDefinition* lhsDef = nullptr;
    JSOp op = JSOP_GE;

    do {
        if (!lhs.numTerms()) {
            lhsDef = MConstant::New(alloc, Int32Value(lhs.constant()));
            block->insertAtEnd(lhsDef->toInstruction());
            lhsDef->computeRange(alloc);
            break;
        }

        lhsDef = ConvertLinearSum(alloc, block, lhs);
        if (lhs.constant() == 0)
            break;

        if (lhs.constant() == -1) {
            op = JSOP_GT;
            break;
        }

        if (!rhsDef) {
            int32_t constant = lhs.constant();
            if (SafeMul(constant, -1, &constant)) {
                rhsDef = MConstant::New(alloc, Int32Value(constant));
                block->insertAtEnd(rhsDef->toInstruction());
                rhsDef->computeRange(alloc);
                break;
            }
        }

        MDefinition* constant = MConstant::New(alloc, Int32Value(lhs.constant()));
        block->insertAtEnd(constant->toInstruction());
        constant->computeRange(alloc);
        lhsDef = MAdd::New(alloc, lhsDef, constant);
        lhsDef->toAdd()->setInt32Specialization();
        block->insertAtEnd(lhsDef->toInstruction());
        lhsDef->computeRange(alloc);
    } while (false);

    if (!rhsDef) {
        rhsDef = MConstant::New(alloc, Int32Value(0));
        block->insertAtEnd(rhsDef->toInstruction());
        rhsDef->computeRange(alloc);
    }

    MCompare* compare = MCompare::New(alloc, lhsDef, rhsDef, op);
    block->insertAtEnd(compare);
    compare->setCompareType(MCompare::Compare_Int32);

    return compare;
}

static bool
AnalyzePoppedThis(JSContext* cx, ObjectGroup* group,
                  MDefinition* thisValue, MInstruction* ins, bool definitelyExecuted,
                  HandlePlainObject baseobj,
                  Vector<TypeNewScript::Initializer>* initializerList,
                  Vector<PropertyName*>* accessedProperties,
                  bool* phandled)
{
    // Determine the effect that a use of the |this| value when calling |new|
    // on a script has on the properties definitely held by the new object.

    if (ins->isCallSetProperty()) {
        MCallSetProperty* setprop = ins->toCallSetProperty();

        if (setprop->object() != thisValue)
            return true;

        if (setprop->name() == cx->names().prototype ||
            setprop->name() == cx->names().proto ||
            setprop->name() == cx->names().constructor)
        {
            return true;
        }

        // Ignore assignments to properties that were already written to.
        if (baseobj->lookup(cx, NameToId(setprop->name()))) {
            *phandled = true;
            return true;
        }

        // Don't add definite properties for properties that were already
        // read in the constructor.
        for (size_t i = 0; i < accessedProperties->length(); i++) {
            if ((*accessedProperties)[i] == setprop->name())
                return true;
        }

        // Assignments to new properties must always execute.
        if (!definitelyExecuted)
            return true;

        RootedId id(cx, NameToId(setprop->name()));
        if (!AddClearDefiniteGetterSetterForPrototypeChain(cx, group, id)) {
            // The prototype chain already contains a getter/setter for this
            // property, or type information is too imprecise.
            return true;
        }

        // Add the property to the object, being careful not to update type information.
        DebugOnly<unsigned> slotSpan = baseobj->slotSpan();
        MOZ_ASSERT(!baseobj->containsPure(id));
        if (!NativeObject::addDataProperty(cx, baseobj, id, SHAPE_INVALID_SLOT, JSPROP_ENUMERATE))
            return false;
        MOZ_ASSERT(baseobj->slotSpan() != slotSpan);
        MOZ_ASSERT(!baseobj->inDictionaryMode());

        Vector<MResumePoint*> callerResumePoints(cx);
        for (MResumePoint* rp = ins->block()->callerResumePoint();
             rp;
             rp = rp->block()->callerResumePoint())
        {
            if (!callerResumePoints.append(rp))
                return false;
        }

        for (int i = callerResumePoints.length() - 1; i >= 0; i--) {
            MResumePoint* rp = callerResumePoints[i];
            JSScript* script = rp->block()->info().script();
            TypeNewScript::Initializer entry(TypeNewScript::Initializer::SETPROP_FRAME,
                                             script->pcToOffset(rp->pc()));
            if (!initializerList->append(entry))
                return false;
        }

        JSScript* script = ins->block()->info().script();
        TypeNewScript::Initializer entry(TypeNewScript::Initializer::SETPROP,
                                         script->pcToOffset(setprop->resumePoint()->pc()));
        if (!initializerList->append(entry))
            return false;

        *phandled = true;
        return true;
    }

    if (ins->isCallGetProperty()) {
        MCallGetProperty* get = ins->toCallGetProperty();

        /*
         * Properties can be read from the 'this' object if the following hold:
         *
         * - The read is not on a getter along the prototype chain, which
         *   could cause 'this' to escape.
         *
         * - The accessed property is either already a definite property or
         *   is not later added as one. Since the definite properties are
         *   added to the object at the point of its creation, reading a
         *   definite property before it is assigned could incorrectly hit.
         */
        RootedId id(cx, NameToId(get->name()));
        if (!baseobj->lookup(cx, id) && !accessedProperties->append(get->name()))
            return false;

        if (!AddClearDefiniteGetterSetterForPrototypeChain(cx, group, id)) {
            // The |this| value can escape if any property reads it does go
            // through a getter.
            return true;
        }

        *phandled = true;
        return true;
    }

    if (ins->isPostWriteBarrier()) {
        *phandled = true;
        return true;
    }

    return true;
}

static int
CmpInstructions(const void* a, const void* b)
{
    return (*static_cast<MInstruction * const*>(a))->id() -
           (*static_cast<MInstruction * const*>(b))->id();
}

bool
jit::AnalyzeNewScriptDefiniteProperties(JSContext* cx, HandleFunction fun,
                                        ObjectGroup* group, HandlePlainObject baseobj,
                                        Vector<TypeNewScript::Initializer>* initializerList)
{
    MOZ_ASSERT(cx->zone()->types.activeAnalysis);

    // When invoking 'new' on the specified script, try to find some properties
    // which will definitely be added to the created object before it has a
    // chance to escape and be accessed elsewhere.

    RootedScript script(cx, JSFunction::getOrCreateScript(cx, fun));
    if (!script)
        return false;

    if (!jit::IsIonEnabled(cx) || !jit::IsBaselineEnabled(cx) || !script->canBaselineCompile())
        return true;

    static const uint32_t MAX_SCRIPT_SIZE = 2000;
    if (script->length() > MAX_SCRIPT_SIZE)
        return true;

    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
    TraceLoggerEvent event(TraceLogger_AnnotateScripts, script);
    AutoTraceLog logScript(logger, event);
    AutoTraceLog logCompile(logger, TraceLogger_IonAnalysis);

    Vector<PropertyName*> accessedProperties(cx);

    LifoAlloc alloc(TempAllocator::PreferredLifoChunkSize);
    TempAllocator temp(&alloc);
    JitContext jctx(cx, &temp);

    if (!jit::CanLikelyAllocateMoreExecutableMemory())
        return true;

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return false;

    if (!script->hasBaselineScript()) {
        MethodStatus status = BaselineCompile(cx, script);
        if (status == Method_Error)
            return false;
        if (status != Method_Compiled)
            return true;
    }

    TypeScript::SetThis(cx, script, TypeSet::ObjectType(group));

    MIRGraph graph(&temp);
    InlineScriptTree* inlineScriptTree = InlineScriptTree::New(&temp, nullptr, nullptr, script);
    if (!inlineScriptTree)
        return false;

    CompileInfo info(CompileRuntime::get(cx->runtime()), script, fun,
                     /* osrPc = */ nullptr,
                     Analysis_DefiniteProperties,
                     script->needsArgsObj(),
                     inlineScriptTree);

    const OptimizationInfo* optimizationInfo = IonOptimizations.get(OptimizationLevel::Normal);

    CompilerConstraintList* constraints = NewCompilerConstraintList(temp);
    if (!constraints) {
        ReportOutOfMemory(cx);
        return false;
    }

    BaselineInspector inspector(script);
    const JitCompileOptions options(cx);

    IonBuilder builder(cx, CompileCompartment::get(cx->compartment()), options, &temp, &graph, constraints,
                       &inspector, &info, optimizationInfo, /* baselineFrame = */ nullptr);

    AbortReasonOr<Ok> buildResult = builder.build();
    if (buildResult.isErr()) {
        AbortReason reason = buildResult.unwrapErr();
        if (cx->isThrowingOverRecursed() || cx->isThrowingOutOfMemory())
            return false;
        if (reason == AbortReason::Alloc) {
            ReportOutOfMemory(cx);
            return false;
        }
        MOZ_ASSERT(!cx->isExceptionPending());
        return true;
    }

    FinishDefinitePropertiesAnalysis(cx, constraints);

    if (!SplitCriticalEdges(graph)) {
        ReportOutOfMemory(cx);
        return false;
    }

    RenumberBlocks(graph);

    if (!BuildDominatorTree(graph)) {
        ReportOutOfMemory(cx);
        return false;
    }

    if (!EliminatePhis(&builder, graph, AggressiveObservability)) {
        ReportOutOfMemory(cx);
        return false;
    }

    MDefinition* thisValue = graph.entryBlock()->getSlot(info.thisSlot());

    // Get a list of instructions using the |this| value in the order they
    // appear in the graph.
    Vector<MInstruction*, 4> instructions(cx);

    for (MUseDefIterator uses(thisValue); uses; uses++) {
        MDefinition* use = uses.def();

        // Don't track |this| through assignments to phis.
        if (!use->isInstruction())
            return true;

        if (!instructions.append(use->toInstruction()))
            return false;
    }

    // Sort the instructions to visit in increasing order.
    qsort(instructions.begin(), instructions.length(),
          sizeof(MInstruction*), CmpInstructions);

    // Find all exit blocks in the graph.
    Vector<MBasicBlock*> exitBlocks(cx);
    for (MBasicBlockIterator block(graph.begin()); block != graph.end(); block++) {
        if (!block->numSuccessors() && !exitBlocks.append(*block))
            return false;
    }

    // id of the last block which added a new property.
    size_t lastAddedBlock = 0;

    for (size_t i = 0; i < instructions.length(); i++) {
        MInstruction* ins = instructions[i];

        // Track whether the use of |this| is in unconditional code, i.e.
        // the block dominates all graph exits.
        bool definitelyExecuted = true;
        for (size_t i = 0; i < exitBlocks.length(); i++) {
            for (MBasicBlock* exit = exitBlocks[i];
                 exit != ins->block();
                 exit = exit->immediateDominator())
            {
                if (exit == exit->immediateDominator()) {
                    definitelyExecuted = false;
                    break;
                }
            }
        }

        // Also check to see if the instruction is inside a loop body. Even if
        // an access will always execute in the script, if it executes multiple
        // times then we can get confused when rolling back objects while
        // clearing the new script information.
        if (ins->block()->loopDepth() != 0)
            definitelyExecuted = false;

        bool handled = false;
        size_t slotSpan = baseobj->slotSpan();
        if (!AnalyzePoppedThis(cx, group, thisValue, ins, definitelyExecuted,
                               baseobj, initializerList, &accessedProperties, &handled))
        {
            return false;
        }
        if (!handled)
            break;

        if (slotSpan != baseobj->slotSpan()) {
            MOZ_ASSERT(ins->block()->id() >= lastAddedBlock);
            lastAddedBlock = ins->block()->id();
        }
    }

    if (baseobj->slotSpan() != 0) {
        // We found some definite properties, but their correctness is still
        // contingent on the correct frames being inlined. Add constraints to
        // invalidate the definite properties if additional functions could be
        // called at the inline frame sites.
        Vector<MBasicBlock*> exitBlocks(cx);
        for (MBasicBlockIterator block(graph.begin()); block != graph.end(); block++) {
            // Inlining decisions made after the last new property was added to
            // the object don't need to be frozen.
            if (block->id() > lastAddedBlock)
                break;
            if (MResumePoint* rp = block->callerResumePoint()) {
                if (block->numPredecessors() == 1 && block->getPredecessor(0) == rp->block()) {
                    JSScript* script = rp->block()->info().script();
                    if (!AddClearDefiniteFunctionUsesInScript(cx, group, script, block->info().script()))
                        return false;
                }
            }
        }
    }

    return true;
}

static bool
ArgumentsUseCanBeLazy(JSContext* cx, JSScript* script, MInstruction* ins, size_t index,
                      bool* argumentsContentsObserved)
{
    // We can read the frame's arguments directly for f.apply(x, arguments).
    if (ins->isCall()) {
        if (*ins->toCall()->resumePoint()->pc() == JSOP_FUNAPPLY &&
            ins->toCall()->numActualArgs() == 2 &&
            index == MCall::IndexOfArgument(1))
        {
            *argumentsContentsObserved = true;
            return true;
        }
    }

    // arguments[i] can read fp->canonicalActualArg(i) directly.
    if (ins->isCallGetElement() && index == 0) {
        *argumentsContentsObserved = true;
        return true;
    }

    // MGetArgumentsObjectArg needs to be considered as a use that allows laziness.
    if (ins->isGetArgumentsObjectArg() && index == 0)
        return true;

    // arguments.length length can read fp->numActualArgs() directly.
    // arguments.callee can read fp->callee() directly if the arguments object
    // is mapped.
    if (ins->isCallGetProperty() && index == 0 &&
        (ins->toCallGetProperty()->name() == cx->names().length ||
         (script->hasMappedArgsObj() && ins->toCallGetProperty()->name() == cx->names().callee)))
    {
        return true;
    }

    return false;
}

bool
jit::AnalyzeArgumentsUsage(JSContext* cx, JSScript* scriptArg)
{
    RootedScript script(cx, scriptArg);
    AutoEnterAnalysis enter(cx);

    MOZ_ASSERT(!script->analyzedArgsUsage());

    // Treat the script as needing an arguments object until we determine it
    // does not need one. This both allows us to easily see where the arguments
    // object can escape through assignments to the function's named arguments,
    // and also simplifies handling of early returns.
    script->setNeedsArgsObj(true);

    // Always construct arguments objects when in debug mode, for generator
    // scripts (generators can be suspended when speculation fails) or when
    // direct eval is present.
    //
    // FIXME: Don't build arguments for ES6 generator expressions.
    if (scriptArg->isDebuggee() ||
        script->isGenerator() ||
        script->isAsync() ||
        script->bindingsAccessedDynamically())
    {
        return true;
    }

    if (!jit::IsIonEnabled(cx))
        return true;

    static const uint32_t MAX_SCRIPT_SIZE = 10000;
    if (script->length() > MAX_SCRIPT_SIZE)
        return true;

    AutoKeepTypeScripts keepTypes(cx);
    if (!script->ensureHasTypes(cx, keepTypes))
        return false;

    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
    TraceLoggerEvent event(TraceLogger_AnnotateScripts, script);
    AutoTraceLog logScript(logger, event);
    AutoTraceLog logCompile(logger, TraceLogger_IonAnalysis);

    LifoAlloc alloc(TempAllocator::PreferredLifoChunkSize);
    TempAllocator temp(&alloc);
    JitContext jctx(cx, &temp);

    if (!jit::CanLikelyAllocateMoreExecutableMemory())
        return true;

    if (!cx->compartment()->ensureJitCompartmentExists(cx))
        return false;

    MIRGraph graph(&temp);
    InlineScriptTree* inlineScriptTree = InlineScriptTree::New(&temp, nullptr, nullptr, script);
    if (!inlineScriptTree) {
        ReportOutOfMemory(cx);
        return false;
    }

    CompileInfo info(CompileRuntime::get(cx->runtime()), script, script->functionNonDelazifying(),
                     /* osrPc = */ nullptr,
                     Analysis_ArgumentsUsage,
                     /* needsArgsObj = */ true,
                     inlineScriptTree);

    const OptimizationInfo* optimizationInfo = IonOptimizations.get(OptimizationLevel::Normal);

    CompilerConstraintList* constraints = NewCompilerConstraintList(temp);
    if (!constraints) {
        ReportOutOfMemory(cx);
        return false;
    }

    BaselineInspector inspector(script);
    const JitCompileOptions options(cx);

    IonBuilder builder(nullptr, CompileCompartment::get(cx->compartment()), options, &temp, &graph, constraints,
                       &inspector, &info, optimizationInfo, /* baselineFrame = */ nullptr);

    AbortReasonOr<Ok> buildResult = builder.build();
    if (buildResult.isErr()) {
        AbortReason reason = buildResult.unwrapErr();
        if (cx->isThrowingOverRecursed() || cx->isThrowingOutOfMemory())
            return false;
        if (reason == AbortReason::Alloc) {
            ReportOutOfMemory(cx);
            return false;
        }
        MOZ_ASSERT(!cx->isExceptionPending());
        return true;
    }

    if (!SplitCriticalEdges(graph)) {
        ReportOutOfMemory(cx);
        return false;
    }

    RenumberBlocks(graph);

    if (!BuildDominatorTree(graph)) {
        ReportOutOfMemory(cx);
        return false;
    }

    if (!EliminatePhis(&builder, graph, AggressiveObservability)) {
        ReportOutOfMemory(cx);
        return false;
    }

    MDefinition* argumentsValue = graph.entryBlock()->getSlot(info.argsObjSlot());

    bool argumentsContentsObserved = false;

    for (MUseDefIterator uses(argumentsValue); uses; uses++) {
        MDefinition* use = uses.def();

        // Don't track |arguments| through assignments to phis.
        if (!use->isInstruction())
            return true;

        if (!ArgumentsUseCanBeLazy(cx, script, use->toInstruction(), use->indexOf(uses.use()),
                                   &argumentsContentsObserved))
        {
            return true;
        }
    }

    // If a script explicitly accesses the contents of 'arguments', and has
    // formals which may be stored as part of a call object, don't use lazy
    // arguments. The compiler can then assume that accesses through
    // arguments[i] will be on unaliased variables.
    if (script->funHasAnyAliasedFormal() && argumentsContentsObserved)
        return true;

    script->setNeedsArgsObj(false);
    return true;
}

// Mark all the blocks that are in the loop with the given header.
// Returns the number of blocks marked. Set *canOsr to true if the loop is
// reachable from both the normal entry and the OSR entry.
size_t
jit::MarkLoopBlocks(MIRGraph& graph, MBasicBlock* header, bool* canOsr)
{
#ifdef DEBUG
    for (ReversePostorderIterator i = graph.rpoBegin(), e = graph.rpoEnd(); i != e; ++i)
        MOZ_ASSERT(!i->isMarked(), "Some blocks already marked");
#endif

    MBasicBlock* osrBlock = graph.osrBlock();
    *canOsr = false;

    // The blocks are in RPO; start at the loop backedge, which marks the bottom
    // of the loop, and walk up until we get to the header. Loops may be
    // discontiguous, so we trace predecessors to determine which blocks are
    // actually part of the loop. The backedge is always part of the loop, and
    // so are its predecessors, transitively, up to the loop header or an OSR
    // entry.
    MBasicBlock* backedge = header->backedge();
    backedge->mark();
    size_t numMarked = 1;
    for (PostorderIterator i = graph.poBegin(backedge); ; ++i) {
        MOZ_ASSERT(i != graph.poEnd(),
                   "Reached the end of the graph while searching for the loop header");
        MBasicBlock* block = *i;
        // If we've reached the loop header, we're done.
        if (block == header)
            break;
        // A block not marked by the time we reach it is not in the loop.
        if (!block->isMarked())
            continue;

        // This block is in the loop; trace to its predecessors.
        for (size_t p = 0, e = block->numPredecessors(); p != e; ++p) {
            MBasicBlock* pred = block->getPredecessor(p);
            if (pred->isMarked())
                continue;

            // Blocks dominated by the OSR entry are not part of the loop
            // (unless they aren't reachable from the normal entry).
            if (osrBlock && pred != header &&
                osrBlock->dominates(pred) && !osrBlock->dominates(header))
            {
                *canOsr = true;
                continue;
            }

            MOZ_ASSERT(pred->id() >= header->id() && pred->id() <= backedge->id(),
                       "Loop block not between loop header and loop backedge");

            pred->mark();
            ++numMarked;

            // A nested loop may not exit back to the enclosing loop at its
            // bottom. If we just marked its header, then the whole nested loop
            // is part of the enclosing loop.
            if (pred->isLoopHeader()) {
                MBasicBlock* innerBackedge = pred->backedge();
                if (!innerBackedge->isMarked()) {
                    // Mark its backedge so that we add all of its blocks to the
                    // outer loop as we walk upwards.
                    innerBackedge->mark();
                    ++numMarked;

                    // If the nested loop is not contiguous, we may have already
                    // passed its backedge. If this happens, back up.
                    if (innerBackedge->id() > block->id()) {
                        i = graph.poBegin(innerBackedge);
                        --i;
                    }
                }
            }
        }
    }

    // If there's no path connecting the header to the backedge, then this isn't
    // actually a loop. This can happen when the code starts with a loop but GVN
    // folds some branches away.
    if (!header->isMarked()) {
        jit::UnmarkLoopBlocks(graph, header);
        return 0;
    }

    return numMarked;
}

// Unmark all the blocks that are in the loop with the given header.
void
jit::UnmarkLoopBlocks(MIRGraph& graph, MBasicBlock* header)
{
    MBasicBlock* backedge = header->backedge();
    for (ReversePostorderIterator i = graph.rpoBegin(header); ; ++i) {
        MOZ_ASSERT(i != graph.rpoEnd(),
                   "Reached the end of the graph while searching for the backedge");
        MBasicBlock* block = *i;
        if (block->isMarked()) {
            block->unmark();
            if (block == backedge)
                break;
        }
    }

#ifdef DEBUG
    for (ReversePostorderIterator i = graph.rpoBegin(), e = graph.rpoEnd(); i != e; ++i)
        MOZ_ASSERT(!i->isMarked(), "Not all blocks got unmarked");
#endif
}

// Reorder the blocks in the loop starting at the given header to be contiguous.
static void
MakeLoopContiguous(MIRGraph& graph, MBasicBlock* header, size_t numMarked)
{
    MBasicBlock* backedge = header->backedge();

    MOZ_ASSERT(header->isMarked(), "Loop header is not part of loop");
    MOZ_ASSERT(backedge->isMarked(), "Loop backedge is not part of loop");

    // If there are any blocks between the loop header and the loop backedge
    // that are not part of the loop, prepare to move them to the end. We keep
    // them in order, which preserves RPO.
    ReversePostorderIterator insertIter = graph.rpoBegin(backedge);
    insertIter++;
    MBasicBlock* insertPt = *insertIter;

    // Visit all the blocks from the loop header to the loop backedge.
    size_t headerId = header->id();
    size_t inLoopId = headerId;
    size_t notInLoopId = inLoopId + numMarked;
    ReversePostorderIterator i = graph.rpoBegin(header);
    for (;;) {
        MBasicBlock* block = *i++;
        MOZ_ASSERT(block->id() >= header->id() && block->id() <= backedge->id(),
                   "Loop backedge should be last block in loop");

        if (block->isMarked()) {
            // This block is in the loop.
            block->unmark();
            block->setId(inLoopId++);
            // If we've reached the loop backedge, we're done!
            if (block == backedge)
                break;
        } else {
            // This block is not in the loop. Move it to the end.
            graph.moveBlockBefore(insertPt, block);
            block->setId(notInLoopId++);
        }
    }
    MOZ_ASSERT(header->id() == headerId, "Loop header id changed");
    MOZ_ASSERT(inLoopId == headerId + numMarked, "Wrong number of blocks kept in loop");
    MOZ_ASSERT(notInLoopId == (insertIter != graph.rpoEnd() ? insertPt->id() : graph.numBlocks()),
               "Wrong number of blocks moved out of loop");
}

// Reorder the blocks in the graph so that loops are contiguous.
bool
jit::MakeLoopsContiguous(MIRGraph& graph)
{
    // Visit all loop headers (in any order).
    for (MBasicBlockIterator i(graph.begin()); i != graph.end(); i++) {
        MBasicBlock* header = *i;
        if (!header->isLoopHeader())
            continue;

        // Mark all blocks that are actually part of the loop.
        bool canOsr;
        size_t numMarked = MarkLoopBlocks(graph, header, &canOsr);

        // If the loop isn't a loop, don't try to optimize it.
        if (numMarked == 0)
            continue;

        // If there's an OSR block entering the loop in the middle, it's tricky,
        // so don't try to handle it, for now.
        if (canOsr) {
            UnmarkLoopBlocks(graph, header);
            continue;
        }

        // Move all blocks between header and backedge that aren't marked to
        // the end of the loop, making the loop itself contiguous.
        MakeLoopContiguous(graph, header, numMarked);
    }

    return true;
}

MRootList::MRootList(TempAllocator& alloc)
{
#define INIT_VECTOR(name, _0, _1) \
    roots_[JS::RootKind::name].emplace(alloc);
JS_FOR_EACH_TRACEKIND(INIT_VECTOR)
#undef INIT_VECTOR
}

template <typename T>
static void
TraceVector(JSTracer* trc, const MRootList::RootVector& vector, const char* name)
{
    for (auto ptr : vector) {
        T ptrT = static_cast<T>(ptr);
        TraceManuallyBarrieredEdge(trc, &ptrT, name);
        MOZ_ASSERT(ptr == ptrT, "Shouldn't move without updating MIR pointers");
    }
}

void
MRootList::trace(JSTracer* trc)
{
#define TRACE_ROOTS(name, type, _) \
    TraceVector<type*>(trc, *roots_[JS::RootKind::name], "mir-root-" #name);
JS_FOR_EACH_TRACEKIND(TRACE_ROOTS)
#undef TRACE_ROOTS
}

MOZ_MUST_USE bool
jit::CreateMIRRootList(IonBuilder& builder)
{
    MOZ_ASSERT(!builder.info().isAnalysis());

    TempAllocator& alloc = builder.alloc();
    MIRGraph& graph = builder.graph();

    MRootList* roots = new(alloc.fallible()) MRootList(alloc);
    if (!roots)
        return false;

    JSScript* prevScript = nullptr;

    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {

        JSScript* script = block->info().script();
        if (script != prevScript) {
            if (!roots->append(script))
                return false;
            prevScript = script;
        }

        for (MInstructionIterator iter(block->begin()), end(block->end()); iter != end; iter++) {
            if (!iter->appendRoots(*roots))
                return false;
        }
    }

    builder.setRootList(*roots);
    return true;
}

static void
DumpDefinition(GenericPrinter& out, MDefinition* def, size_t depth)
{
    MDefinition::PrintOpcodeName(out, def->op());

    if (depth == 0)
        return;

    for (size_t i = 0; i < def->numOperands(); i++) {
        out.printf(" (");
        DumpDefinition(out, def->getOperand(i), depth - 1);
        out.printf(")");
    }
}

void
jit::DumpMIRExpressions(MIRGraph& graph)
{
    if (!JitSpewEnabled(JitSpew_MIRExpressions))
        return;

    size_t depth = 2;

    Fprinter& out = JitSpewPrinter();
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {
        for (MInstructionIterator iter(block->begin()), end(block->end()); iter != end; iter++) {
            DumpDefinition(out, *iter, depth);
            out.printf("\n");
        }
    }
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonAnalysis.h"

#include <algorithm>
#include <utility>  // for ::std::pair

#include "jit/AliasAnalysis.h"
#include "jit/BaselineJIT.h"
#include "jit/CompileInfo.h"
#include "jit/InlineScriptTree.h"
#include "jit/Ion.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/LIR.h"
#include "jit/Lowering.h"
#include "jit/MIRGraph.h"
#include "jit/WarpBuilder.h"
#include "jit/WarpOracle.h"
#include "util/CheckedArithmetic.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/RegExpObject.h"
#include "vm/SelfHosting.h"

#include "jit/InlineScriptTree-inl.h"
#include "jit/JitScript-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

// Stack used by FlagPhiInputsAsImplicitlyUsed. It stores the Phi instruction
// pointer and the MUseIterator which should be visited next.
using MPhiUseIteratorStack =
    Vector<std::pair<MPhi*, MUseIterator>, 16, SystemAllocPolicy>;

// Look for Phi uses with a depth-first search. If any uses are found the stack
// of MPhi instructions is returned in the |worklist| argument.
static bool DepthFirstSearchUse(MIRGenerator* mir,
                                MPhiUseIteratorStack& worklist, MPhi* phi) {
  // Push a Phi and the next use to iterate over in the worklist.
  auto push = [&worklist](MPhi* phi, MUseIterator use) -> bool {
    phi->setInWorklist();
    return worklist.append(std::make_pair(phi, use));
  };

#ifdef DEBUG
  // Used to assert that when we have no uses, we at least visited all the
  // transitive uses.
  size_t refUseCount = phi->useCount();
  size_t useCount = 0;
#endif
  MOZ_ASSERT(worklist.empty());
  if (!push(phi, phi->usesBegin())) {
    return false;
  }

  while (!worklist.empty()) {
    // Resume iterating over the last phi-use pair added by the next loop.
    auto pair = worklist.popCopy();
    MPhi* producer = pair.first;
    MUseIterator use = pair.second;
    MUseIterator end(producer->usesEnd());
    producer->setNotInWorklist();

    // Keep going down the tree of uses, skipping (continue)
    // non-observable/unused cases and Phi which are already listed in the
    // worklist. Stop (return) as soon as one use is found.
    while (use != end) {
      MNode* consumer = (*use)->consumer();
      MUseIterator it = use;
      use++;
#ifdef DEBUG
      useCount++;
#endif
      if (mir->shouldCancel("FlagPhiInputsAsImplicitlyUsed inner loop")) {
        return false;
      }

      if (consumer->isResumePoint()) {
        MResumePoint* rp = consumer->toResumePoint();
        // Observable operands are similar to potential uses.
        if (rp->isObservableOperand(*it)) {
          return push(producer, use);
        }
        continue;
      }

      MDefinition* cdef = consumer->toDefinition();
      if (!cdef->isPhi()) {
        // The producer is explicitly used by a definition.
        return push(producer, use);
      }

      MPhi* cphi = cdef->toPhi();
      if (cphi->getUsageAnalysis() == PhiUsage::Used ||
          cphi->isImplicitlyUsed()) {
        // The information got cached on the Phi the last time it
        // got visited, or when flagging operands of implicitly used
        // instructions.
        return push(producer, use);
      }

      if (cphi->isInWorklist() || cphi == producer) {
        // We are already iterating over the uses of this Phi instruction which
        // are part of a loop, instead of trying to handle loops, conservatively
        // mark them as used.
        return push(producer, use);
      }

      if (cphi->getUsageAnalysis() == PhiUsage::Unused) {
        // The instruction already got visited and is known to have
        // no uses. Skip it.
        continue;
      }

      // We found another Phi instruction, move the use iterator to
      // the next use push it to the worklist stack. Then, continue
      // with a depth search.
      if (!push(producer, use)) {
        return false;
      }
      producer = cphi;
      use = producer->usesBegin();
      end = producer->usesEnd();
#ifdef DEBUG
      refUseCount += producer->useCount();
#endif
    }

    // When unused, we cannot bubble up this information without iterating
    // over the rest of the previous Phi instruction consumers.
    MOZ_ASSERT(use == end);
    producer->setUsageAnalysis(PhiUsage::Unused);
  }

  MOZ_ASSERT(useCount == refUseCount);
  return true;
}

static bool FlagPhiInputsAsImplicitlyUsed(MIRGenerator* mir, MBasicBlock* block,
                                          MBasicBlock* succ,
                                          MPhiUseIteratorStack& worklist) {
  // When removing an edge between 2 blocks, we might remove the ability of
  // later phases to figure out that the uses of a Phi should be considered as
  // a use of all its inputs. Thus we need to mark the Phi inputs as being
  // implicitly used iff the phi has any uses.
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
  // If the inputs of the Phi are not flagged as implicitly used, then
  // later compilation phase might optimize them out. The problem is that a
  // bailout will use this value and give it back to baseline, which will then
  // use the OptimizedOut magic value in a computation.
  //
  // Unfortunately, we cannot be too conservative about flagging Phi inputs as
  // having implicit uses, as this would prevent many optimizations from being
  // used. Thus, the following code is in charge of flagging Phi instructions
  // as Unused or Used, and setting ImplicitlyUsed accordingly.
  size_t predIndex = succ->getPredecessorIndex(block);
  MPhiIterator end = succ->phisEnd();
  MPhiIterator it = succ->phisBegin();
  for (; it != end; it++) {
    MPhi* phi = *it;

    if (mir->shouldCancel("FlagPhiInputsAsImplicitlyUsed outer loop")) {
      return false;
    }

    // We are looking to mark the Phi inputs which are used across the edge
    // between the |block| and its successor |succ|.
    MDefinition* def = phi->getOperand(predIndex);
    if (def->isImplicitlyUsed()) {
      continue;
    }

    // If the Phi is either Used or Unused, set the ImplicitlyUsed flag
    // accordingly.
    if (phi->getUsageAnalysis() == PhiUsage::Used || phi->isImplicitlyUsed()) {
      def->setImplicitlyUsedUnchecked();
      continue;
    } else if (phi->getUsageAnalysis() == PhiUsage::Unused) {
      continue;
    }

    // We do not know if the Phi was Used or Unused, iterate over all uses
    // with a depth-search of uses. Returns the matching stack in the
    // worklist as soon as one use is found.
    MOZ_ASSERT(worklist.empty());
    if (!DepthFirstSearchUse(mir, worklist, phi)) {
      return false;
    }

    MOZ_ASSERT_IF(worklist.empty(),
                  phi->getUsageAnalysis() == PhiUsage::Unused);
    if (!worklist.empty()) {
      // One of the Phis is used, set Used flags on all the Phis which are
      // in the use chain.
      def->setImplicitlyUsedUnchecked();
      do {
        auto pair = worklist.popCopy();
        MPhi* producer = pair.first;
        producer->setUsageAnalysis(PhiUsage::Used);
        producer->setNotInWorklist();
      } while (!worklist.empty());
    }
    MOZ_ASSERT(phi->getUsageAnalysis() != PhiUsage::Unknown);
  }

  return true;
}

static MInstructionIterator FindFirstInstructionAfterBail(MBasicBlock* block) {
  MOZ_ASSERT(block->alwaysBails());
  for (MInstructionIterator it = block->begin(); it != block->end(); it++) {
    MInstruction* ins = *it;
    if (ins->isBail()) {
      it++;
      return it;
    }
  }
  MOZ_CRASH("Expected MBail in alwaysBails block");
}

// Given an iterator pointing to the first removed instruction, mark
// the operands of each removed instruction as having implicit uses.
static bool FlagOperandsAsImplicitlyUsedAfter(
    MIRGenerator* mir, MBasicBlock* block, MInstructionIterator firstRemoved) {
  MOZ_ASSERT(firstRemoved->block() == block);

  const CompileInfo& info = block->info();

  // Flag operands of removed instructions as having implicit uses.
  MInstructionIterator end = block->end();
  for (MInstructionIterator it = firstRemoved; it != end; it++) {
    if (mir->shouldCancel("FlagOperandsAsImplicitlyUsedAfter (loop 1)")) {
      return false;
    }

    MInstruction* ins = *it;
    for (size_t i = 0, e = ins->numOperands(); i < e; i++) {
      ins->getOperand(i)->setImplicitlyUsedUnchecked();
    }

    // Flag observable resume point operands as having implicit uses.
    if (MResumePoint* rp = ins->resumePoint()) {
      // Note: no need to iterate over the caller's of the resume point as
      // this is the same as the entry resume point.
      MOZ_ASSERT(&rp->block()->info() == &info);
      for (size_t i = 0, e = rp->numOperands(); i < e; i++) {
        if (info.isObservableSlot(i)) {
          rp->getOperand(i)->setImplicitlyUsedUnchecked();
        }
      }
    }
  }

  // Flag Phi inputs of the successors as having implicit uses.
  MPhiUseIteratorStack worklist;
  for (size_t i = 0, e = block->numSuccessors(); i < e; i++) {
    if (mir->shouldCancel("FlagOperandsAsImplicitlyUsedAfter (loop 2)")) {
      return false;
    }

    if (!FlagPhiInputsAsImplicitlyUsed(mir, block, block->getSuccessor(i),
                                       worklist)) {
      return false;
    }
  }

  return true;
}

static bool FlagEntryResumePointOperands(MIRGenerator* mir,
                                         MBasicBlock* block) {
  // Flag observable operands of the entry resume point as having implicit uses.
  MResumePoint* rp = block->entryResumePoint();
  while (rp) {
    if (mir->shouldCancel("FlagEntryResumePointOperands")) {
      return false;
    }

    const CompileInfo& info = rp->block()->info();
    for (size_t i = 0, e = rp->numOperands(); i < e; i++) {
      if (info.isObservableSlot(i)) {
        rp->getOperand(i)->setImplicitlyUsedUnchecked();
      }
    }

    rp = rp->caller();
  }

  return true;
}

static bool FlagAllOperandsAsImplicitlyUsed(MIRGenerator* mir,
                                            MBasicBlock* block) {
  return FlagEntryResumePointOperands(mir, block) &&
         FlagOperandsAsImplicitlyUsedAfter(mir, block, block->begin());
}

// WarpBuilder sets the alwaysBails flag on blocks that contain an
// unconditional bailout. We trim any instructions in those blocks
// after the first unconditional bailout, and remove any blocks that
// are only reachable through bailing blocks.
bool jit::PruneUnusedBranches(MIRGenerator* mir, MIRGraph& graph) {
  JitSpew(JitSpew_Prune, "Begin");

  // Pruning is guided by unconditional bailouts. Wasm does not have bailouts.
  MOZ_ASSERT(!mir->compilingWasm());

  Vector<MBasicBlock*, 16, SystemAllocPolicy> worklist;
  uint32_t numMarked = 0;
  bool needsTrim = false;

  auto markReachable = [&](MBasicBlock* block) -> bool {
    block->mark();
    numMarked++;
    if (block->alwaysBails()) {
      needsTrim = true;
    }
    return worklist.append(block);
  };

  // The entry block is always reachable.
  if (!markReachable(graph.entryBlock())) {
    return false;
  }

  // The OSR entry block is always reachable if it exists.
  if (graph.osrBlock() && !markReachable(graph.osrBlock())) {
    return false;
  }

  // Iteratively mark all reachable blocks.
  while (!worklist.empty()) {
    if (mir->shouldCancel("Prune unused branches (marking reachable)")) {
      return false;
    }
    MBasicBlock* block = worklist.popCopy();

    JitSpew(JitSpew_Prune, "Visit block %u:", block->id());
    JitSpewIndent indent(JitSpew_Prune);

    // If this block always bails, then it does not reach its successors.
    if (block->alwaysBails()) {
      continue;
    }

    for (size_t i = 0; i < block->numSuccessors(); i++) {
      MBasicBlock* succ = block->getSuccessor(i);
      if (succ->isMarked()) {
        continue;
      }
      JitSpew(JitSpew_Prune, "Reaches block %u", succ->id());
      if (!markReachable(succ)) {
        return false;
      }
    }
  }

  if (!needsTrim && numMarked == graph.numBlocks()) {
    // There is nothing to prune.
    graph.unmarkBlocks();
    return true;
  }

  JitSpew(JitSpew_Prune, "Remove unreachable instructions and blocks:");
  JitSpewIndent indent(JitSpew_Prune);

  // The operands of removed instructions may be needed in baseline
  // after bailing out.
  for (PostorderIterator it(graph.poBegin()); it != graph.poEnd();) {
    if (mir->shouldCancel("Prune unused branches (marking operands)")) {
      return false;
    }

    MBasicBlock* block = *it++;
    if (!block->isMarked()) {
      // If we are removing the block entirely, mark the operands of every
      // instruction as being implicitly used.
      FlagAllOperandsAsImplicitlyUsed(mir, block);
    } else if (block->alwaysBails()) {
      // If we are only trimming instructions after a bail, only mark operands
      // of removed instructions.
      MInstructionIterator firstRemoved = FindFirstInstructionAfterBail(block);
      FlagOperandsAsImplicitlyUsedAfter(mir, block, firstRemoved);
    }
  }

  // Remove the blocks in post-order such that consumers are visited before
  // the predecessors, the only exception being the Phi nodes of loop headers.
  for (PostorderIterator it(graph.poBegin()); it != graph.poEnd();) {
    if (mir->shouldCancel("Prune unused branches (removal loop)")) {
      return false;
    }
    if (!graph.alloc().ensureBallast()) {
      return false;
    }

    MBasicBlock* block = *it++;
    if (block->isMarked() && !block->alwaysBails()) {
      continue;
    }

    // As we are going to replace/remove the last instruction, we first have
    // to remove this block from the predecessor list of its successors.
    size_t numSucc = block->numSuccessors();
    for (uint32_t i = 0; i < numSucc; i++) {
      MBasicBlock* succ = block->getSuccessor(i);
      if (succ->isDead()) {
        continue;
      }

      // Our dominators code expects all loop headers to have two predecessors.
      // If we are removing the normal entry to a loop, but can still reach
      // the loop header via OSR, we create a fake unreachable predecessor.
      if (succ->isLoopHeader() && block != succ->backedge()) {
        MOZ_ASSERT(graph.osrBlock());
        if (!graph.alloc().ensureBallast()) {
          return false;
        }

        MBasicBlock* fake = MBasicBlock::NewFakeLoopPredecessor(graph, succ);
        if (!fake) {
          return false;
        }
        // Mark the block to avoid removing it as unreachable.
        fake->mark();

        JitSpew(JitSpew_Prune,
                "Header %u only reachable by OSR. Add fake predecessor %u",
                succ->id(), fake->id());
      }

      JitSpew(JitSpew_Prune, "Remove block edge %u -> %u.", block->id(),
              succ->id());
      succ->removePredecessor(block);
    }

    if (!block->isMarked()) {
      // Remove unreachable blocks from the CFG.
      JitSpew(JitSpew_Prune, "Remove block %u.", block->id());
      graph.removeBlock(block);
    } else {
      // Remove unreachable instructions after unconditional bailouts.
      JitSpew(JitSpew_Prune, "Trim block %u.", block->id());

      // Discard all instructions after the first MBail.
      MInstructionIterator firstRemoved = FindFirstInstructionAfterBail(block);
      block->discardAllInstructionsStartingAt(firstRemoved);

      if (block->outerResumePoint()) {
        block->clearOuterResumePoint();
      }

      block->end(MUnreachable::New(graph.alloc()));
    }
  }
  graph.unmarkBlocks();

  return true;
}

static bool SplitCriticalEdgesForBlock(MIRGraph& graph, MBasicBlock* block) {
  if (block->numSuccessors() < 2) {
    return true;
  }
  for (size_t i = 0; i < block->numSuccessors(); i++) {
    MBasicBlock* target = block->getSuccessor(i);
    if (target->numPredecessors() < 2) {
      continue;
    }

    // Create a simple new block which contains a goto and which split the
    // edge between block and target.
    MBasicBlock* split = MBasicBlock::NewSplitEdge(graph, block, i, target);
    if (!split) {
      return false;
    }
  }
  return true;
}

// A critical edge is an edge which is neither its successor's only predecessor
// nor its predecessor's only successor. Critical edges must be split to
// prevent copy-insertion and code motion from affecting other edges.
bool jit::SplitCriticalEdges(MIRGraph& graph) {
  for (MBasicBlockIterator iter(graph.begin()); iter != graph.end(); iter++) {
    MBasicBlock* block = *iter;
    if (!SplitCriticalEdgesForBlock(graph, block)) {
      return false;
    }
  }
  return true;
}

bool jit::IsUint32Type(const MDefinition* def) {
  if (def->isBeta()) {
    def = def->getOperand(0);
  }

  if (def->type() != MIRType::Int32) {
    return false;
  }

  return def->isUrsh() && def->getOperand(1)->isConstant() &&
         def->getOperand(1)->toConstant()->type() == MIRType::Int32 &&
         def->getOperand(1)->toConstant()->toInt32() == 0;
}

// Return whether a block simply computes the specified constant value.
static bool BlockComputesConstant(MBasicBlock* block, MDefinition* value,
                                  bool* constBool) {
  // Look for values with no uses. This is used to eliminate constant
  // computing blocks in condition statements, and the phi which used to
  // consume the constant has already been removed.
  if (value->hasUses()) {
    return false;
  }

  if (!value->isConstant() || value->block() != block) {
    return false;
  }
  if (!block->phisEmpty()) {
    return false;
  }
  for (MInstructionIterator iter = block->begin(); iter != block->end();
       ++iter) {
    if (*iter != value || !iter->isGoto()) {
      return false;
    }
  }
  return value->toConstant()->valueToBoolean(constBool);
}

// Determine whether phiBlock/testBlock simply compute a phi and perform a
// test on it.
static bool BlockIsSingleTest(MBasicBlock* phiBlock, MBasicBlock* testBlock,
                              MPhi** pphi, MTest** ptest) {
  *pphi = nullptr;
  *ptest = nullptr;

  if (phiBlock != testBlock) {
    MOZ_ASSERT(phiBlock->numSuccessors() == 1 &&
               phiBlock->getSuccessor(0) == testBlock);
    if (!phiBlock->begin()->isGoto()) {
      return false;
    }
  }

  MInstruction* ins = *testBlock->begin();
  if (!ins->isTest()) {
    return false;
  }
  MTest* test = ins->toTest();
  if (!test->input()->isPhi()) {
    return false;
  }
  MPhi* phi = test->input()->toPhi();
  if (phi->block() != phiBlock) {
    return false;
  }

  for (MUseIterator iter = phi->usesBegin(); iter != phi->usesEnd(); ++iter) {
    MUse* use = *iter;
    if (use->consumer() == test) {
      continue;
    }
    if (use->consumer()->isResumePoint()) {
      MBasicBlock* useBlock = use->consumer()->block();
      if (useBlock == phiBlock || useBlock == testBlock) {
        continue;
      }
    }
    return false;
  }

  for (MPhiIterator iter = phiBlock->phisBegin(); iter != phiBlock->phisEnd();
       ++iter) {
    if (*iter != phi) {
      return false;
    }
  }

  if (phiBlock != testBlock && !testBlock->phisEmpty()) {
    return false;
  }

  *pphi = phi;
  *ptest = test;

  return true;
}

// Change block so that it ends in a goto to the specific target block.
// existingPred is an existing predecessor of the block.
[[nodiscard]] static bool UpdateGotoSuccessor(TempAllocator& alloc,
                                              MBasicBlock* block,
                                              MBasicBlock* target,
                                              MBasicBlock* existingPred) {
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
[[nodiscard]] static bool UpdateTestSuccessors(
    TempAllocator& alloc, MBasicBlock* block, MDefinition* value,
    MBasicBlock* ifTrue, MBasicBlock* ifFalse, MBasicBlock* existingPred) {
  MInstruction* ins = block->lastIns();
  if (ins->isTest()) {
    MTest* test = ins->toTest();
    MOZ_ASSERT(test->input() == value);

    if (ifTrue != test->ifTrue()) {
      test->ifTrue()->removePredecessor(block);
      if (!ifTrue->addPredecessorSameInputsAs(block, existingPred)) {
        return false;
      }
      MOZ_ASSERT(test->ifTrue() == test->getSuccessor(0));
      test->replaceSuccessor(0, ifTrue);
    }

    if (ifFalse != test->ifFalse()) {
      test->ifFalse()->removePredecessor(block);
      if (!ifFalse->addPredecessorSameInputsAs(block, existingPred)) {
        return false;
      }
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

  if (!ifTrue->addPredecessorSameInputsAs(block, existingPred)) {
    return false;
  }
  if (!ifFalse->addPredecessorSameInputsAs(block, existingPred)) {
    return false;
  }
  return true;
}

static bool MaybeFoldConditionBlock(MIRGraph& graph,
                                    MBasicBlock* initialBlock) {
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
  if (!ins->isTest()) {
    return true;
  }
  MTest* initialTest = ins->toTest();

  MBasicBlock* trueBranch = initialTest->ifTrue();
  if (trueBranch->numPredecessors() != 1 || trueBranch->numSuccessors() != 1) {
    return true;
  }
  MBasicBlock* falseBranch = initialTest->ifFalse();
  if (falseBranch->numPredecessors() != 1 ||
      falseBranch->numSuccessors() != 1) {
    return true;
  }
  MBasicBlock* phiBlock = trueBranch->getSuccessor(0);
  if (phiBlock != falseBranch->getSuccessor(0)) {
    return true;
  }
  if (phiBlock->numPredecessors() != 2) {
    return true;
  }

  if (initialBlock->isLoopBackedge() || trueBranch->isLoopBackedge() ||
      falseBranch->isLoopBackedge()) {
    return true;
  }

  MBasicBlock* testBlock = phiBlock;
  if (testBlock->numSuccessors() == 1) {
    if (testBlock->isLoopBackedge()) {
      return true;
    }
    testBlock = testBlock->getSuccessor(0);
    if (testBlock->numPredecessors() != 1) {
      return true;
    }
  }

  // Make sure the test block does not have any outgoing loop backedges.
  if (!SplitCriticalEdgesForBlock(graph, testBlock)) {
    return false;
  }

  MPhi* phi;
  MTest* finalTest;
  if (!BlockIsSingleTest(phiBlock, testBlock, &phi, &finalTest)) {
    return true;
  }

  MDefinition* trueResult =
      phi->getOperand(phiBlock->indexForPredecessor(trueBranch));
  MDefinition* falseResult =
      phi->getOperand(phiBlock->indexForPredecessor(falseBranch));

  // OK, we found the desired pattern, now transform the graph.

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
    if (!UpdateGotoSuccessor(graph.alloc(), trueBranch, finalTest->ifTrue(),
                             testBlock)) {
      return false;
    }
  } else {
    if (!UpdateTestSuccessors(graph.alloc(), trueBranch, trueResult,
                              finalTest->ifTrue(), finalTest->ifFalse(),
                              testBlock)) {
      return false;
    }
  }

  MBasicBlock* falseTarget = falseBranch;
  if (BlockComputesConstant(falseBranch, falseResult, &constBool)) {
    falseTarget = constBool ? finalTest->ifTrue() : finalTest->ifFalse();
    phiBlock->removePredecessor(falseBranch);
    graph.removeBlock(falseBranch);
  } else if (initialTest->input() == falseResult) {
    if (!UpdateGotoSuccessor(graph.alloc(), falseBranch, finalTest->ifFalse(),
                             testBlock)) {
      return false;
    }
  } else {
    if (!UpdateTestSuccessors(graph.alloc(), falseBranch, falseResult,
                              finalTest->ifTrue(), finalTest->ifFalse(),
                              testBlock)) {
      return false;
    }
  }

  // Short circuit the initial test to skip any constant branch eliminated
  // above.
  if (!UpdateTestSuccessors(graph.alloc(), initialBlock, initialTest->input(),
                            trueTarget, falseTarget, testBlock)) {
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

bool jit::FoldTests(MIRGraph& graph) {
  for (MBasicBlockIterator block(graph.begin()); block != graph.end();
       block++) {
    if (!MaybeFoldConditionBlock(graph, *block)) {
      return false;
    }
  }
  return true;
}

bool jit::FoldEmptyBlocks(MIRGraph& graph) {
  for (MBasicBlockIterator iter(graph.begin()); iter != graph.end();) {
    MBasicBlock* block = *iter;
    iter++;

    if (block->numPredecessors() != 1 || block->numSuccessors() != 1) {
      continue;
    }

    if (!block->phisEmpty()) {
      continue;
    }

    if (block->outerResumePoint()) {
      continue;
    }

    if (*block->begin() != *block->rbegin()) {
      continue;
    }

    MBasicBlock* succ = block->getSuccessor(0);
    MBasicBlock* pred = block->getPredecessor(0);

    if (succ->numPredecessors() != 1) {
      continue;
    }

    size_t pos = pred->getSuccessorIndex(block);
    pred->lastIns()->replaceSuccessor(pos, succ);

    graph.removeBlock(block);

    if (!succ->addPredecessorSameInputsAs(pred, block)) {
      return false;
    }
    succ->removePredecessor(block);
  }
  return true;
}

static void EliminateTriviallyDeadResumePointOperands(MIRGraph& graph,
                                                      MResumePoint* rp) {
  // If we will pop the top of the stack immediately after resuming,
  // then don't preserve the top value in the resume point.
  if (rp->mode() != MResumePoint::ResumeAt || JSOp(*rp->pc()) != JSOp::Pop) {
    return;
  }

  size_t top = rp->stackDepth() - 1;
  MOZ_ASSERT(!rp->isObservableOperand(top));

  MDefinition* def = rp->getOperand(top);
  if (def->isConstant()) {
    return;
  }

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
bool jit::EliminateDeadResumePointOperands(MIRGenerator* mir, MIRGraph& graph) {
  // If we are compiling try blocks, locals and arguments may be observable
  // from catch or finally blocks (which Ion does not compile). For now just
  // disable the pass in this case.
  if (graph.hasTryBlock()) {
    return true;
  }

  for (PostorderIterator block = graph.poBegin(); block != graph.poEnd();
       block++) {
    if (mir->shouldCancel("Eliminate Dead Resume Point Operands (main loop)")) {
      return false;
    }

    if (MResumePoint* rp = block->entryResumePoint()) {
      if (!graph.alloc().ensureBallast()) {
        return false;
      }
      EliminateTriviallyDeadResumePointOperands(graph, rp);
    }

    // The logic below can get confused on infinite loops.
    if (block->isLoopHeader() && block->backedge() == *block) {
      continue;
    }

    for (MInstructionIterator ins = block->begin(); ins != block->end();
         ins++) {
      if (MResumePoint* rp = ins->resumePoint()) {
        if (!graph.alloc().ensureBallast()) {
          return false;
        }
        EliminateTriviallyDeadResumePointOperands(graph, rp);
      }

      // No benefit to replacing constant operands with other constants.
      if (ins->isConstant()) {
        continue;
      }

      // Scanning uses does not give us sufficient information to tell
      // where instructions that are involved in box/unbox operations or
      // parameter passing might be live. Rewriting uses of these terms
      // in resume points may affect the interpreter's behavior. Rather
      // than doing a more sophisticated analysis, just ignore these.
      if (ins->isUnbox() || ins->isParameter() || ins->isBoxNonStrictThis()) {
        continue;
      }

      // Early intermediate values captured by resume points, such as
      // ArrayState and its allocation, may be legitimately dead in Ion code,
      // but are still needed if we bail out. They can recover on bailout.
      if (ins->isRecoveredOnBailout()) {
        MOZ_ASSERT(ins->canRecoverOnBailout());
        continue;
      }

      // If the instruction's behavior has been constant folded into a
      // separate instruction, we can't determine precisely where the
      // instruction becomes dead and can't eliminate its uses.
      if (ins->isImplicitlyUsed()) {
        continue;
      }

      // Check if this instruction's result is only used within the
      // current block, and keep track of its last use in a definition
      // (not resume point). This requires the instructions in the block
      // to be numbered, ensured by running this immediately after alias
      // analysis.
      uint32_t maxDefinition = 0;
      for (MUseIterator uses(ins->usesBegin()); uses != ins->usesEnd();
           uses++) {
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
        maxDefinition = std::max(maxDefinition, def->id());
      }
      if (maxDefinition == UINT32_MAX) {
        continue;
      }

      // Walk the uses a second time, removing any in resume points after
      // the last use in a definition.
      for (MUseIterator uses(ins->usesBegin()); uses != ins->usesEnd();) {
        MUse* use = *uses++;
        if (use->consumer()->isDefinition()) {
          continue;
        }
        MResumePoint* mrp = use->consumer()->toResumePoint();
        if (mrp->block() != *block || !mrp->instruction() ||
            mrp->instruction() == *ins ||
            mrp->instruction()->id() <= maxDefinition) {
          continue;
        }

        if (!graph.alloc().ensureBallast()) {
          return false;
        }

        // Store an optimized out magic value in place of all dead
        // resume point operands. Making any such substitution can in
        // general alter the interpreter's behavior, even though the
        // code is dead, as the interpreter will still execute opcodes
        // whose effects cannot be observed. If the magic value value
        // were to flow to, say, a dead property access the
        // interpreter could throw an exception; we avoid this problem
        // by removing dead operands before removing dead code.
        MConstant* constant =
            MConstant::New(graph.alloc(), MagicValue(JS_OPTIMIZED_OUT));
        block->insertBefore(*(block->begin()), constant);
        use->replaceProducer(constant);
      }
    }
  }

  return true;
}

// Test whether |def| would be needed if it had no uses.
bool js::jit::DeadIfUnused(const MDefinition* def) {
  // Effectful instructions of course cannot be removed.
  if (def->isEffectful()) {
    return false;
  }

  // Never eliminate guard instructions.
  if (def->isGuard()) {
    return false;
  }

  // Required to be preserved, as the type guard related to this instruction
  // is part of the semantics of a transformation.
  if (def->isGuardRangeBailouts()) {
    return false;
  }

  // Control instructions have no uses, but also shouldn't be optimized out
  if (def->isControlInstruction()) {
    return false;
  }

  // Used when lowering to generate the corresponding snapshots and aggregate
  // the list of recover instructions to be repeated.
  if (def->isInstruction() && def->toInstruction()->resumePoint()) {
    return false;
  }

  return true;
}

// Test whether |def| may be safely discarded, due to being dead or due to being
// located in a basic block which has itself been marked for discarding.
bool js::jit::IsDiscardable(const MDefinition* def) {
  return !def->hasUses() && (DeadIfUnused(def) || def->block()->isMarked());
}

// Instructions are useless if they are unused and have no side effects.
// This pass eliminates useless instructions.
// The graph itself is unchanged.
bool jit::EliminateDeadCode(MIRGenerator* mir, MIRGraph& graph) {
  // Traverse in postorder so that we hit uses before definitions.
  // Traverse instruction list backwards for the same reason.
  for (PostorderIterator block = graph.poBegin(); block != graph.poEnd();
       block++) {
    if (mir->shouldCancel("Eliminate Dead Code (main loop)")) {
      return false;
    }

    // Remove unused instructions.
    for (MInstructionReverseIterator iter = block->rbegin();
         iter != block->rend();) {
      MInstruction* inst = *iter++;
      if (js::jit::IsDiscardable(inst)) {
        block->discard(inst);
      }
    }
  }

  return true;
}

static inline bool IsPhiObservable(MPhi* phi, Observability observe) {
  // If the phi has uses which are not reflected in SSA, then behavior in the
  // interpreter may be affected by removing the phi.
  if (phi->isImplicitlyUsed()) {
    return true;
  }

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
      if (observe == ConservativeObservability) {
        return true;
      }
      if (resume->isObservableOperand(*iter)) {
        return true;
      }
    } else {
      MDefinition* def = consumer->toDefinition();
      if (!def->isPhi()) {
        return true;
      }
    }
  }

  return false;
}

// Handles cases like:
//    x is phi(a, x) --> a
//    x is phi(a, a) --> a
static inline MDefinition* IsPhiRedundant(MPhi* phi) {
  MDefinition* first = phi->operandIfRedundant();
  if (first == nullptr) {
    return nullptr;
  }

  // Propagate the ImplicitlyUsed flag if |phi| is replaced with another phi.
  if (phi->isImplicitlyUsed()) {
    first->setImplicitlyUsedUnchecked();
  }

  return first;
}

bool jit::EliminatePhis(MIRGenerator* mir, MIRGraph& graph,
                        Observability observe) {
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
  for (PostorderIterator block = graph.poBegin(); block != graph.poEnd();
       block++) {
    MPhiIterator iter = block->phisBegin();
    while (iter != block->phisEnd()) {
      MPhi* phi = *iter++;

      if (mir->shouldCancel("Eliminate Phis (populate loop)")) {
        return false;
      }

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
        if (!worklist.append(phi)) {
          return false;
        }
      }
    }
  }

  // Iteratively mark all phis reachable from live phis.
  while (!worklist.empty()) {
    if (mir->shouldCancel("Eliminate Phis (worklist)")) {
      return false;
    }

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
            if (!worklist.append(use)) {
              return false;
            }
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
      if (!in->isPhi() || !in->isUnused() || in->isInWorklist()) {
        continue;
      }
      in->setInWorklist();
      if (!worklist.append(in->toPhi())) {
        return false;
      }
    }
  }

  // Sweep dead phis.
  for (PostorderIterator block = graph.poBegin(); block != graph.poEnd();
       block++) {
    MPhiIterator iter = block->phisBegin();
    while (iter != block->phisEnd()) {
      MPhi* phi = *iter++;
      if (phi->isUnused()) {
        if (!phi->optimizeOutAllUses(graph.alloc())) {
          return false;
        }
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
class TypeAnalyzer {
  MIRGenerator* mir;
  MIRGraph& graph;
  Vector<MPhi*, 0, SystemAllocPolicy> phiWorklist_;

  TempAllocator& alloc() const { return graph.alloc(); }

  bool addPhiToWorklist(MPhi* phi) {
    if (phi->isInWorklist()) {
      return true;
    }
    if (!phiWorklist_.append(phi)) {
      return false;
    }
    phi->setInWorklist();
    return true;
  }
  MPhi* popPhi() {
    MPhi* phi = phiWorklist_.popCopy();
    phi->setNotInWorklist();
    return phi;
  }

  [[nodiscard]] bool propagateAllPhiSpecializations();

  bool respecialize(MPhi* phi, MIRType type);
  bool propagateSpecialization(MPhi* phi);
  bool specializePhis();
  bool specializeOsrOnlyPhis();
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

  bool shouldSpecializeOsrPhis() const;
  MIRType guessPhiType(MPhi* phi) const;

 public:
  TypeAnalyzer(MIRGenerator* mir, MIRGraph& graph) : mir(mir), graph(graph) {}

  bool analyze();
};

} /* anonymous namespace */

bool TypeAnalyzer::shouldSpecializeOsrPhis() const {
  // [SMDOC] OSR Phi Type Specialization
  //
  // Without special handling for OSR phis, we end up with unspecialized phis
  // (MIRType::Value) in the loop (pre)header and other blocks, resulting in
  // unnecessary boxing and unboxing in the loop body.
  //
  // To fix this, phi type specialization needs special code to deal with the
  // OSR entry block. Recall that OSR results in the following basic block
  // structure:
  //
  //  +------------------+                 +-----------------+
  //  | Code before loop |                 | OSR entry block |
  //  +------------------+                 +-----------------+
  //          |                                       |
  //          |                                       |
  //          |           +---------------+           |
  //          +---------> | OSR preheader | <---------+
  //                      +---------------+
  //                              |
  //                              V
  //                      +---------------+
  //                      | Loop header   |<-----+
  //                      +---------------+      |
  //                              |              |
  //                             ...             |
  //                              |              |
  //                      +---------------+      |
  //                      | Loop backedge |------+
  //                      +---------------+
  //
  // OSR phi specialization happens in three steps:
  //
  // (1) Specialize phis but ignore MOsrValue phi inputs. In other words,
  //     pretend the OSR entry block doesn't exist. See guessPhiType.
  //
  // (2) Once phi specialization is done, look at the types of loop header phis
  //     and add these types to the corresponding preheader phis. This way, the
  //     types of the preheader phis are based on the code before the loop and
  //     the code in the loop body. These are exactly the types we expect for
  //     the OSR Values. See the last part of TypeAnalyzer::specializePhis.
  //
  // (3) For type-specialized preheader phis, add guard/unbox instructions to
  //     the OSR entry block to guard the incoming Value indeed has this type.
  //     This happens in:
  //
  //     * TypeAnalyzer::adjustPhiInputs: adds a fallible unbox for values that
  //       can be unboxed.
  //
  //     * TypeAnalyzer::replaceRedundantPhi: adds a type guard for values that
  //       can't be unboxed (null/undefined/magic Values).
  if (!mir->graph().osrBlock()) {
    return false;
  }

  return !mir->outerInfo().hadSpeculativePhiBailout();
}

// Try to specialize this phi based on its non-cyclic inputs.
MIRType TypeAnalyzer::guessPhiType(MPhi* phi) const {
#ifdef DEBUG
  // Check that different magic constants aren't flowing together. Ignore
  // JS_OPTIMIZED_OUT, since an operand could be legitimately optimized
  // away.
  MIRType magicType = MIRType::None;
  for (size_t i = 0; i < phi->numOperands(); i++) {
    MDefinition* in = phi->getOperand(i);
    if (in->type() == MIRType::MagicHole ||
        in->type() == MIRType::MagicIsConstructing) {
      if (magicType == MIRType::None) {
        magicType = in->type();
      }
      MOZ_ASSERT(magicType == in->type());
    }
  }
#endif

  MIRType type = MIRType::None;
  bool convertibleToFloat32 = false;
  bool hasOSRValueInput = false;
  DebugOnly<bool> hasSpecializableInput = false;
  for (size_t i = 0, e = phi->numOperands(); i < e; i++) {
    MDefinition* in = phi->getOperand(i);
    if (in->isPhi()) {
      hasSpecializableInput = true;
      if (!in->toPhi()->triedToSpecialize()) {
        continue;
      }
      if (in->type() == MIRType::None) {
        // The operand is a phi we tried to specialize, but we were
        // unable to guess its type. propagateSpecialization will
        // propagate the type to this phi when it becomes known.
        continue;
      }
    }

    // See shouldSpecializeOsrPhis comment. This is the first step mentioned
    // there.
    if (shouldSpecializeOsrPhis() && in->isOsrValue()) {
      hasOSRValueInput = true;
      hasSpecializableInput = true;
      continue;
    }

    if (type == MIRType::None) {
      type = in->type();
      if (in->canProduceFloat32() &&
          !mir->outerInfo().hadSpeculativePhiBailout()) {
        convertibleToFloat32 = true;
      }
      continue;
    }

    if (type == in->type()) {
      convertibleToFloat32 = convertibleToFloat32 && in->canProduceFloat32();
    } else {
      if (convertibleToFloat32 && in->type() == MIRType::Float32) {
        // If we only saw definitions that can be converted into Float32 before
        // and encounter a Float32 value, promote previous values to Float32
        type = MIRType::Float32;
      } else if (IsTypeRepresentableAsDouble(type) &&
                 IsTypeRepresentableAsDouble(in->type())) {
        // Specialize phis with int32 and double operands as double.
        type = MIRType::Double;
        convertibleToFloat32 = convertibleToFloat32 && in->canProduceFloat32();
      } else {
        return MIRType::Value;
      }
    }
  }

  if (hasOSRValueInput && type == MIRType::Float32) {
    // TODO(post-Warp): simplify float32 handling in this function or (better)
    // make the float32 analysis a stand-alone optimization pass instead of
    // complicating type analysis. See bug 1655773.
    type = MIRType::Double;
  }

  MOZ_ASSERT_IF(type == MIRType::None, hasSpecializableInput);
  return type;
}

bool TypeAnalyzer::respecialize(MPhi* phi, MIRType type) {
  if (phi->type() == type) {
    return true;
  }
  phi->specialize(type);
  return addPhiToWorklist(phi);
}

bool TypeAnalyzer::propagateSpecialization(MPhi* phi) {
  MOZ_ASSERT(phi->type() != MIRType::None);

  // Verify that this specialization matches any phis depending on it.
  for (MUseDefIterator iter(phi); iter; iter++) {
    if (!iter.def()->isPhi()) {
      continue;
    }
    MPhi* use = iter.def()->toPhi();
    if (!use->triedToSpecialize()) {
      continue;
    }
    if (use->type() == MIRType::None) {
      // We tried to specialize this phi, but were unable to guess its
      // type. Now that we know the type of one of its operands, we can
      // specialize it.
      if (!respecialize(use, phi->type())) {
        return false;
      }
      continue;
    }
    if (use->type() != phi->type()) {
      // Specialize phis with int32 that can be converted to float and float
      // operands as floats.
      if ((use->type() == MIRType::Int32 && use->canProduceFloat32() &&
           phi->type() == MIRType::Float32) ||
          (phi->type() == MIRType::Int32 && phi->canProduceFloat32() &&
           use->type() == MIRType::Float32)) {
        if (!respecialize(use, MIRType::Float32)) {
          return false;
        }
        continue;
      }

      // Specialize phis with int32 and double operands as double.
      if (IsTypeRepresentableAsDouble(use->type()) &&
          IsTypeRepresentableAsDouble(phi->type())) {
        if (!respecialize(use, MIRType::Double)) {
          return false;
        }
        continue;
      }

      // This phi in our use chain can now no longer be specialized.
      if (!respecialize(use, MIRType::Value)) {
        return false;
      }
    }
  }

  return true;
}

bool TypeAnalyzer::propagateAllPhiSpecializations() {
  while (!phiWorklist_.empty()) {
    if (mir->shouldCancel("Specialize Phis (worklist)")) {
      return false;
    }

    MPhi* phi = popPhi();
    if (!propagateSpecialization(phi)) {
      return false;
    }
  }

  return true;
}

// If branch pruning removes the path from the entry block to the OSR
// preheader, we may have phis (or chains of phis) with no operands
// other than OsrValues. These phis will still have MIRType::None.
// Since we don't have any information about them, we specialize them
// as MIRType::Value.
bool TypeAnalyzer::specializeOsrOnlyPhis() {
  MOZ_ASSERT(graph.osrBlock());
  MOZ_ASSERT(graph.osrPreHeaderBlock()->numPredecessors() == 1);

  for (PostorderIterator block(graph.poBegin()); block != graph.poEnd();
       block++) {
    if (mir->shouldCancel("Specialize osr-only phis (main loop)")) {
      return false;
    }

    for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++) {
      if (mir->shouldCancel("Specialize osr-only phis (inner loop)")) {
        return false;
      }

      if (phi->type() == MIRType::None) {
        phi->specialize(MIRType::Value);
      }
    }
  }
  return true;
}

bool TypeAnalyzer::specializePhis() {
  for (PostorderIterator block(graph.poBegin()); block != graph.poEnd();
       block++) {
    if (mir->shouldCancel("Specialize Phis (main loop)")) {
      return false;
    }

    for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++) {
      if (mir->shouldCancel("Specialize Phis (inner loop)")) {
        return false;
      }

      MIRType type = guessPhiType(*phi);
      phi->specialize(type);
      if (type == MIRType::None) {
        // We tried to guess the type but failed because all operands are
        // phis we still have to visit. Set the triedToSpecialize flag but
        // don't propagate the type to other phis, propagateSpecialization
        // will do that once we know the type of one of the operands.
        continue;
      }
      if (!propagateSpecialization(*phi)) {
        return false;
      }
    }
  }

  if (!propagateAllPhiSpecializations()) {
    return false;
  }

  if (shouldSpecializeOsrPhis()) {
    // See shouldSpecializeOsrPhis comment. This is the second step, propagating
    // loop header phi types to preheader phis.
    MBasicBlock* preHeader = graph.osrPreHeaderBlock();
    MBasicBlock* header = preHeader->getSingleSuccessor();

    if (preHeader->numPredecessors() == 1) {
      MOZ_ASSERT(preHeader->getPredecessor(0) == graph.osrBlock());
      // Branch pruning has removed the path from the entry block
      // to the preheader. Specialize any phis with no non-osr inputs.
      if (!specializeOsrOnlyPhis()) {
        return false;
      }
    } else if (header->isLoopHeader()) {
      for (MPhiIterator phi(header->phisBegin()); phi != header->phisEnd();
           phi++) {
        MPhi* preHeaderPhi = phi->getOperand(0)->toPhi();
        MOZ_ASSERT(preHeaderPhi->block() == preHeader);

        if (preHeaderPhi->type() == MIRType::Value) {
          // Already includes everything.
          continue;
        }

        MIRType loopType = phi->type();
        if (!respecialize(preHeaderPhi, loopType)) {
          return false;
        }
      }
      if (!propagateAllPhiSpecializations()) {
        return false;
      }
    } else {
      // Edge case: there is no backedge in this loop. This can happen
      // if the header is a 'pending' loop header when control flow in
      // the loop body is terminated unconditionally, or if a block
      // that dominates the backedge unconditionally bails out. In
      // this case the header only has the preheader as predecessor
      // and we don't need to do anything.
      MOZ_ASSERT(header->numPredecessors() == 1);
    }
  }

  MOZ_ASSERT(phiWorklist_.empty());
  return true;
}

bool TypeAnalyzer::adjustPhiInputs(MPhi* phi) {
  MIRType phiType = phi->type();
  MOZ_ASSERT(phiType != MIRType::None);

  // If we specialized a type that's not Value, there are 3 cases:
  // 1. Every input is of that type.
  // 2. Every observed input is of that type (i.e., some inputs haven't been
  // executed yet).
  // 3. Inputs were doubles and int32s, and was specialized to double.
  if (phiType != MIRType::Value) {
    for (size_t i = 0, e = phi->numOperands(); i < e; i++) {
      MDefinition* in = phi->getOperand(i);
      if (in->type() == phiType) {
        continue;
      }

      if (!alloc().ensureBallast()) {
        return false;
      }

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

            MUnbox* unbox =
                MUnbox::New(alloc(), in, MIRType::Double, MUnbox::Fallible);
            unbox->setBailoutKind(BailoutKind::SpeculativePhi);
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

        replacement->setBailoutKind(BailoutKind::SpeculativePhi);
        in->block()->insertBefore(in->block()->lastIns(), replacement);
        phi->replaceOperand(i, replacement);
      }
    }

    return true;
  }

  // Box every typed input.
  for (size_t i = 0, e = phi->numOperands(); i < e; i++) {
    MDefinition* in = phi->getOperand(i);
    if (in->type() == MIRType::Value) {
      continue;
    }

    // The input is being explicitly unboxed, so sneak past and grab the
    // original box. Don't bother optimizing if magic values are involved.
    if (in->isUnbox()) {
      MDefinition* unboxInput = in->toUnbox()->input();
      if (!IsMagicType(unboxInput->type()) && phi->typeIncludes(unboxInput)) {
        in = in->toUnbox()->input();
      }
    }

    if (in->type() != MIRType::Value) {
      if (!alloc().ensureBallast()) {
        return false;
      }

      MBasicBlock* pred = phi->block()->getPredecessor(i);
      in = AlwaysBoxAt(alloc(), pred->lastIns(), in);
    }

    phi->replaceOperand(i, in);
  }

  return true;
}

bool TypeAnalyzer::adjustInputs(MDefinition* def) {
  // Definitions such as MPhi have no type policy.
  if (!def->isInstruction()) {
    return true;
  }

  MInstruction* ins = def->toInstruction();
  const TypePolicy* policy = ins->typePolicy();
  if (policy && !policy->adjustInputs(alloc(), ins)) {
    return false;
  }
  return true;
}

void TypeAnalyzer::replaceRedundantPhi(MPhi* phi) {
  MBasicBlock* block = phi->block();
  js::Value v;
  switch (phi->type()) {
    case MIRType::Undefined:
      v = UndefinedValue();
      break;
    case MIRType::Null:
      v = NullValue();
      break;
    case MIRType::MagicOptimizedOut:
      v = MagicValue(JS_OPTIMIZED_OUT);
      break;
    case MIRType::MagicUninitializedLexical:
      v = MagicValue(JS_UNINITIALIZED_LEXICAL);
      break;
    case MIRType::MagicIsConstructing:
      v = MagicValue(JS_IS_CONSTRUCTING);
      break;
    case MIRType::MagicHole:
    default:
      MOZ_CRASH("unexpected type");
  }
  MConstant* c = MConstant::New(alloc(), v);
  // The instruction pass will insert the box
  block->insertBefore(*(block->begin()), c);
  phi->justReplaceAllUsesWith(c);

  if (shouldSpecializeOsrPhis()) {
    // See shouldSpecializeOsrPhis comment. This is part of the third step,
    // guard the incoming MOsrValue is of this type.
    for (uint32_t i = 0; i < phi->numOperands(); i++) {
      MDefinition* def = phi->getOperand(i);
      if (def->type() != phi->type()) {
        MOZ_ASSERT(def->isOsrValue() || def->isPhi());
        MOZ_ASSERT(def->type() == MIRType::Value);
        MGuardValue* guard = MGuardValue::New(alloc(), def, v);
        guard->setBailoutKind(BailoutKind::SpeculativePhi);
        def->block()->insertBefore(def->block()->lastIns(), guard);
      }
    }
  }
}

bool TypeAnalyzer::insertConversions() {
  // Instructions are processed in reverse postorder: all uses are defs are
  // seen before uses. This ensures that output adjustment (which may rewrite
  // inputs of uses) does not conflict with input adjustment.
  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); block++) {
    if (mir->shouldCancel("Insert Conversions")) {
      return false;
    }

    for (MPhiIterator iter(block->phisBegin()), end(block->phisEnd());
         iter != end;) {
      MPhi* phi = *iter++;
      if (IsNullOrUndefined(phi->type()) || IsMagicType(phi->type())) {
        // We can replace this phi with a constant.
        if (!alloc().ensureBallast()) {
          return false;
        }
        replaceRedundantPhi(phi);
        block->discardPhi(phi);
      } else {
        if (!adjustPhiInputs(phi)) {
          return false;
        }
      }
    }

    // AdjustInputs can add/remove/mutate instructions before and after the
    // current instruction. Only increment the iterator after it is finished.
    for (MInstructionIterator iter(block->begin()); iter != block->end();
         iter++) {
      if (!alloc().ensureBallast()) {
        return false;
      }

      if (!adjustInputs(*iter)) {
        return false;
      }
    }
  }
  return true;
}

/* clang-format off */
//
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
//
/* clang-format on */
bool TypeAnalyzer::markPhiConsumers() {
  MOZ_ASSERT(phiWorklist_.empty());

  // Iterate in postorder so worklist is initialized to RPO.
  for (PostorderIterator block(graph.poBegin()); block != graph.poEnd();
       ++block) {
    if (mir->shouldCancel(
            "Ensure Float32 commutativity - Consumer Phis - Initial state")) {
      return false;
    }

    for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); ++phi) {
      MOZ_ASSERT(!phi->isInWorklist());
      bool canConsumeFloat32 = !phi->isImplicitlyUsed();
      for (MUseDefIterator use(*phi); canConsumeFloat32 && use; use++) {
        MDefinition* usedef = use.def();
        canConsumeFloat32 &=
            usedef->isPhi() || usedef->canConsumeFloat32(use.use());
      }
      phi->setCanConsumeFloat32(canConsumeFloat32);
      if (canConsumeFloat32 && !addPhiToWorklist(*phi)) {
        return false;
      }
    }
  }

  while (!phiWorklist_.empty()) {
    if (mir->shouldCancel(
            "Ensure Float32 commutativity - Consumer Phis - Fixed point")) {
      return false;
    }

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

    if (validConsumer) {
      continue;
    }

    // Propagate invalidated phis
    phi->setCanConsumeFloat32(false);
    for (size_t i = 0, e = phi->numOperands(); i < e; ++i) {
      MDefinition* input = phi->getOperand(i);
      if (input->isPhi() && !input->isInWorklist() &&
          input->canConsumeFloat32(nullptr /* unused */)) {
        if (!addPhiToWorklist(input->toPhi())) {
          return false;
        }
      }
    }
  }
  return true;
}

bool TypeAnalyzer::markPhiProducers() {
  MOZ_ASSERT(phiWorklist_.empty());

  // Iterate in reverse postorder so worklist is initialized to PO.
  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); ++block) {
    if (mir->shouldCancel(
            "Ensure Float32 commutativity - Producer Phis - initial state")) {
      return false;
    }

    for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); ++phi) {
      MOZ_ASSERT(!phi->isInWorklist());
      bool canProduceFloat32 = true;
      for (size_t i = 0, e = phi->numOperands(); canProduceFloat32 && i < e;
           ++i) {
        MDefinition* input = phi->getOperand(i);
        canProduceFloat32 &= input->isPhi() || input->canProduceFloat32();
      }
      phi->setCanProduceFloat32(canProduceFloat32);
      if (canProduceFloat32 && !addPhiToWorklist(*phi)) {
        return false;
      }
    }
  }

  while (!phiWorklist_.empty()) {
    if (mir->shouldCancel(
            "Ensure Float32 commutativity - Producer Phis - Fixed point")) {
      return false;
    }

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

    if (validProducer) {
      continue;
    }

    // Propagate invalidated phis
    phi->setCanProduceFloat32(false);
    for (MUseDefIterator use(phi); use; use++) {
      MDefinition* def = use.def();
      if (def->isPhi() && !def->isInWorklist() && def->canProduceFloat32()) {
        if (!addPhiToWorklist(def->toPhi())) {
          return false;
        }
      }
    }
  }
  return true;
}

bool TypeAnalyzer::specializeValidFloatOps() {
  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); ++block) {
    if (mir->shouldCancel("Ensure Float32 commutativity - Instructions")) {
      return false;
    }

    for (MInstructionIterator ins(block->begin()); ins != block->end(); ++ins) {
      if (!ins->isFloat32Commutative()) {
        continue;
      }

      if (ins->type() == MIRType::Float32) {
        continue;
      }

      if (!alloc().ensureBallast()) {
        return false;
      }

      // This call will try to specialize the instruction iff all uses are
      // consumers and all inputs are producers.
      ins->trySpecializeFloat32(alloc());
    }
  }
  return true;
}

bool TypeAnalyzer::graphContainsFloat32() {
  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); ++block) {
    for (MDefinitionIterator def(*block); def; def++) {
      if (mir->shouldCancel(
              "Ensure Float32 commutativity - Graph contains Float32")) {
        return false;
      }

      if (def->type() == MIRType::Float32) {
        return true;
      }
    }
  }
  return false;
}

bool TypeAnalyzer::tryEmitFloatOperations() {
  // Asm.js uses the ahead of time type checks to specialize operations, no need
  // to check them again at this point.
  if (mir->compilingWasm()) {
    return true;
  }

  // Check ahead of time that there is at least one definition typed as Float32,
  // otherwise we don't need this pass.
  if (!graphContainsFloat32()) {
    return true;
  }

  if (!markPhiConsumers()) {
    return false;
  }
  if (!markPhiProducers()) {
    return false;
  }
  if (!specializeValidFloatOps()) {
    return false;
  }
  return true;
}

bool TypeAnalyzer::checkFloatCoherency() {
#ifdef DEBUG
  // Asserts that all Float32 instructions are flowing into Float32 consumers or
  // specialized operations
  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); ++block) {
    if (mir->shouldCancel("Check Float32 coherency")) {
      return false;
    }

    for (MDefinitionIterator def(*block); def; def++) {
      if (def->type() != MIRType::Float32) {
        continue;
      }

      for (MUseDefIterator use(*def); use; use++) {
        MDefinition* consumer = use.def();
        MOZ_ASSERT(consumer->isConsistentFloat32Use(use.use()));
      }
    }
  }
#endif
  return true;
}

bool TypeAnalyzer::analyze() {
  if (!tryEmitFloatOperations()) {
    return false;
  }
  if (!specializePhis()) {
    return false;
  }
  if (!insertConversions()) {
    return false;
  }
  if (!checkFloatCoherency()) {
    return false;
  }
  return true;
}

bool jit::ApplyTypeInformation(MIRGenerator* mir, MIRGraph& graph) {
  TypeAnalyzer analyzer(mir, graph);

  if (!analyzer.analyze()) {
    return false;
  }

  return true;
}

void jit::RenumberBlocks(MIRGraph& graph) {
  size_t id = 0;
  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); block++) {
    block->setId(id++);
  }
}

// A utility for code which deletes blocks. Renumber the remaining blocks,
// recompute dominators, and optionally recompute AliasAnalysis dependencies.
bool jit::AccountForCFGChanges(MIRGenerator* mir, MIRGraph& graph,
                               bool updateAliasAnalysis,
                               bool underValueNumberer) {
  // Renumber the blocks and clear out the old dominator info.
  size_t id = 0;
  for (ReversePostorderIterator i(graph.rpoBegin()), e(graph.rpoEnd()); i != e;
       ++i) {
    i->clearDominatorInfo();
    i->setId(id++);
  }

  // Recompute dominator info.
  if (!BuildDominatorTree(graph)) {
    return false;
  }

  // If needed, update alias analysis dependencies.
  if (updateAliasAnalysis) {
    TraceLoggerThread* logger = TraceLoggerForCurrentThread();
    AutoTraceLog log(logger, TraceLogger_AliasAnalysis);

    if (!AliasAnalysis(mir, graph).analyze()) {
      return false;
    }
  }

  AssertExtendedGraphCoherency(graph, underValueNumberer);
  return true;
}

// Remove all blocks not marked with isMarked(). Unmark all remaining blocks.
// Alias analysis dependencies may be invalid after calling this function.
bool jit::RemoveUnmarkedBlocks(MIRGenerator* mir, MIRGraph& graph,
                               uint32_t numMarkedBlocks) {
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
      if (block->isMarked()) {
        continue;
      }

      FlagAllOperandsAsImplicitlyUsed(mir, block);
    }

    // Find unmarked blocks and remove them.
    for (ReversePostorderIterator iter(graph.rpoBegin());
         iter != graph.rpoEnd();) {
      MBasicBlock* block = *iter++;

      if (block->isMarked()) {
        block->unmark();
        continue;
      }

      // The block is unreachable. Clear out the loop header flag, as
      // we're doing the sweep of a mark-and-sweep here, so we no longer
      // need to worry about whether an unmarked block is a loop or not.
      if (block->isLoopHeader()) {
        block->clearLoopHeader();
      }

      for (size_t i = 0, e = block->numSuccessors(); i != e; ++i) {
        block->getSuccessor(i)->removePredecessor(block);
      }
      graph.removeBlock(block);
    }
  }

  // Renumber the blocks and update the dominator tree.
  return AccountForCFGChanges(mir, graph, /*updateAliasAnalysis=*/false);
}

// A Simple, Fast Dominance Algorithm by Cooper et al.
// Modified to support empty intersections for OSR, and in RPO.
static MBasicBlock* IntersectDominators(MBasicBlock* block1,
                                        MBasicBlock* block2) {
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
      if (idom == finger1) {
        return nullptr;  // Empty intersection.
      }
      finger1 = idom;
    }

    while (finger2->id() > finger1->id()) {
      MBasicBlock* idom = finger2->immediateDominator();
      if (idom == finger2) {
        return nullptr;  // Empty intersection.
      }
      finger2 = idom;
    }
  }
  return finger1;
}

void jit::ClearDominatorTree(MIRGraph& graph) {
  for (MBasicBlockIterator iter = graph.begin(); iter != graph.end(); iter++) {
    iter->clearDominatorInfo();
  }
}

static void ComputeImmediateDominators(MIRGraph& graph) {
  // The default start block is a root and therefore only self-dominates.
  MBasicBlock* startBlock = graph.entryBlock();
  startBlock->setImmediateDominator(startBlock);

  // Any OSR block is a root and therefore only self-dominates.
  MBasicBlock* osrBlock = graph.osrBlock();
  if (osrBlock) {
    osrBlock->setImmediateDominator(osrBlock);
  }

  bool changed = true;

  while (changed) {
    changed = false;

    ReversePostorderIterator block = graph.rpoBegin();

    // For each block in RPO, intersect all dominators.
    for (; block != graph.rpoEnd(); block++) {
      // If a node has once been found to have no exclusive dominator,
      // it will never have an exclusive dominator, so it may be skipped.
      if (block->immediateDominator() == *block) {
        continue;
      }

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
        if (pred->immediateDominator() == nullptr) {
          continue;
        }

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
  for (MBasicBlockIterator block(graph.begin()); block != graph.end();
       block++) {
    MOZ_ASSERT(block->immediateDominator() != nullptr);
  }
#endif
}

bool jit::BuildDominatorTree(MIRGraph& graph) {
  MOZ_ASSERT(graph.canBuildDominators());

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
      if (!worklist.append(child)) {
        return false;
      }
      continue;
    }

    if (!parent->addImmediatelyDominatedBlock(child)) {
      return false;
    }

    parent->addNumDominated(child->numDominated());
  }

#ifdef DEBUG
  // If compiling with OSR, many blocks will self-dominate.
  // Without OSR, there is only one root block which dominates all.
  if (!graph.osrBlock()) {
    MOZ_ASSERT(graph.entryBlock()->numDominated() == graph.numBlocks());
  }
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

bool jit::BuildPhiReverseMapping(MIRGraph& graph) {
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
  for (MBasicBlockIterator block(graph.begin()); block != graph.end();
       block++) {
    if (block->phisEmpty()) {
      continue;
    }

    // Assert on the above.
    for (size_t j = 0; j < block->numPredecessors(); j++) {
      MBasicBlock* pred = block->getPredecessor(j);

#ifdef DEBUG
      size_t numSuccessorsWithPhis = 0;
      for (size_t k = 0; k < pred->numSuccessors(); k++) {
        MBasicBlock* successor = pred->getSuccessor(k);
        if (!successor->phisEmpty()) {
          numSuccessorsWithPhis++;
        }
      }
      MOZ_ASSERT(numSuccessorsWithPhis <= 1);
#endif

      pred->setSuccessorWithPhis(*block, j);
    }
  }

  return true;
}

#ifdef DEBUG
static bool CheckSuccessorImpliesPredecessor(MBasicBlock* A, MBasicBlock* B) {
  // Assuming B = succ(A), verify A = pred(B).
  for (size_t i = 0; i < B->numPredecessors(); i++) {
    if (A == B->getPredecessor(i)) {
      return true;
    }
  }
  return false;
}

static bool CheckPredecessorImpliesSuccessor(MBasicBlock* A, MBasicBlock* B) {
  // Assuming B = pred(A), verify A = succ(B).
  for (size_t i = 0; i < B->numSuccessors(); i++) {
    if (A == B->getSuccessor(i)) {
      return true;
    }
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

static void CheckOperand(const MNode* consumer, const MUse* use,
                         int32_t* usesBalance) {
  MOZ_ASSERT(use->hasProducer());
  MDefinition* producer = use->producer();
  MOZ_ASSERT(!producer->isDiscarded());
  MOZ_ASSERT(producer->block() != nullptr);
  MOZ_ASSERT(use->consumer() == consumer);
#  ifdef _DEBUG_CHECK_OPERANDS_USES_BALANCE
  Fprinter print(stderr);
  print.printf("==Check Operand\n");
  use->producer()->dump(print);
  print.printf("  index: %zu\n", use->consumer()->indexOf(use));
  use->consumer()->dump(print);
  print.printf("==End\n");
#  endif
  --*usesBalance;
}

static void CheckUse(const MDefinition* producer, const MUse* use,
                     int32_t* usesBalance) {
  MOZ_ASSERT(!use->consumer()->block()->isDead());
  MOZ_ASSERT_IF(use->consumer()->isDefinition(),
                !use->consumer()->toDefinition()->isDiscarded());
  MOZ_ASSERT(use->consumer()->block() != nullptr);
  MOZ_ASSERT(use->consumer()->getOperand(use->index()) == producer);
#  ifdef _DEBUG_CHECK_OPERANDS_USES_BALANCE
  Fprinter print(stderr);
  print.printf("==Check Use\n");
  use->producer()->dump(print);
  print.printf("  index: %zu\n", use->consumer()->indexOf(use));
  use->consumer()->dump(print);
  print.printf("==End\n");
#  endif
  ++*usesBalance;
}

// To properly encode entry resume points, we have to ensure that all the
// operands of the entry resume point are located before the safeInsertTop
// location.
static void AssertOperandsBeforeSafeInsertTop(MResumePoint* resume) {
  MBasicBlock* block = resume->block();
  if (block == block->graph().osrBlock()) {
    return;
  }
  MInstruction* stop = block->safeInsertTop();
  for (size_t i = 0, e = resume->numOperands(); i < e; ++i) {
    MDefinition* def = resume->getOperand(i);
    if (def->block() != block) {
      continue;
    }
    if (def->isPhi()) {
      continue;
    }

    for (MInstructionIterator ins = block->begin(); true; ins++) {
      if (*ins == def) {
        break;
      }
      MOZ_ASSERT(
          *ins != stop,
          "Resume point operand located after the safeInsertTop location");
    }
  }
}
#endif  // DEBUG

void jit::AssertBasicGraphCoherency(MIRGraph& graph, bool force) {
#ifdef DEBUG
  if (!JitOptions.fullDebugChecks && !force) {
    return;
  }

  MOZ_ASSERT(graph.entryBlock()->numPredecessors() == 0);
  MOZ_ASSERT(graph.entryBlock()->phisEmpty());
  MOZ_ASSERT(!graph.entryBlock()->unreachable());

  if (MBasicBlock* osrBlock = graph.osrBlock()) {
    MOZ_ASSERT(osrBlock->numPredecessors() == 0);
    MOZ_ASSERT(osrBlock->phisEmpty());
    MOZ_ASSERT(osrBlock != graph.entryBlock());
    MOZ_ASSERT(!osrBlock->unreachable());
  }

  if (MResumePoint* resumePoint = graph.entryResumePoint()) {
    MOZ_ASSERT(resumePoint->block() == graph.entryBlock());
  }

  // Assert successor and predecessor list coherency.
  uint32_t count = 0;
  int32_t usesBalance = 0;
  for (MBasicBlockIterator block(graph.begin()); block != graph.end();
       block++) {
    count++;

    MOZ_ASSERT(&block->graph() == &graph);
    MOZ_ASSERT(!block->isDead());
    MOZ_ASSERT_IF(block->outerResumePoint() != nullptr,
                  block->entryResumePoint() != nullptr);

    for (size_t i = 0; i < block->numSuccessors(); i++) {
      MOZ_ASSERT(
          CheckSuccessorImpliesPredecessor(*block, block->getSuccessor(i)));
    }

    for (size_t i = 0; i < block->numPredecessors(); i++) {
      MOZ_ASSERT(
          CheckPredecessorImpliesSuccessor(*block, block->getPredecessor(i)));
    }

    if (MResumePoint* resume = block->entryResumePoint()) {
      MOZ_ASSERT(!resume->instruction());
      MOZ_ASSERT(resume->block() == *block);
      AssertOperandsBeforeSafeInsertTop(resume);
    }
    if (MResumePoint* resume = block->outerResumePoint()) {
      MOZ_ASSERT(!resume->instruction());
      MOZ_ASSERT(resume->block() == *block);
    }
    for (MResumePointIterator iter(block->resumePointsBegin());
         iter != block->resumePointsEnd(); iter++) {
      // We cannot yet assert that is there is no instruction then this is
      // the entry resume point because we are still storing resume points
      // in the InlinePropertyTable.
      MOZ_ASSERT_IF(iter->instruction(),
                    iter->instruction()->block() == *block);
      for (uint32_t i = 0, e = iter->numOperands(); i < e; i++) {
        CheckOperand(*iter, iter->getUseFor(i), &usesBalance);
      }
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
      for (uint32_t i = 0, end = iter->numOperands(); i < end; i++) {
        CheckOperand(*iter, iter->getUseFor(i), &usesBalance);
      }
      for (MUseIterator use(iter->usesBegin()); use != iter->usesEnd(); use++) {
        CheckUse(*iter, *use, &usesBalance);
      }

      if (iter->isInstruction()) {
        if (MResumePoint* resume = iter->toInstruction()->resumePoint()) {
          MOZ_ASSERT(resume->instruction() == *iter);
          MOZ_ASSERT(resume->block() == *block);
          MOZ_ASSERT(resume->block()->entryResumePoint() != nullptr);
        }
      }

      if (iter->isRecoveredOnBailout()) {
        MOZ_ASSERT(!iter->hasLiveDefUses());
      }
    }

    // The control instruction is not visited by the MDefinitionIterator.
    MControlInstruction* control = block->lastIns();
    MOZ_ASSERT(control->block() == *block);
    MOZ_ASSERT(!control->hasUses());
    MOZ_ASSERT(control->type() == MIRType::None);
    MOZ_ASSERT(!control->isDiscarded());
    MOZ_ASSERT(!control->isRecoveredOnBailout());
    MOZ_ASSERT(control->resumePoint() == nullptr);
    for (uint32_t i = 0, end = control->numOperands(); i < end; i++) {
      CheckOperand(control, control->getUseFor(i), &usesBalance);
    }
    for (size_t i = 0; i < control->numSuccessors(); i++) {
      MOZ_ASSERT(control->getSuccessor(i));
    }
  }

  // In case issues, see the _DEBUG_CHECK_OPERANDS_USES_BALANCE macro above.
  MOZ_ASSERT(usesBalance <= 0, "More use checks than operand checks");
  MOZ_ASSERT(usesBalance >= 0, "More operand checks than use checks");
  MOZ_ASSERT(graph.numBlocks() == count);
#endif
}

#ifdef DEBUG
static void AssertReversePostorder(MIRGraph& graph) {
  // Check that every block is visited after all its predecessors (except
  // backedges).
  for (ReversePostorderIterator iter(graph.rpoBegin()); iter != graph.rpoEnd();
       ++iter) {
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
static void AssertDominatorTree(MIRGraph& graph) {
  // Check dominators.

  MOZ_ASSERT(graph.entryBlock()->immediateDominator() == graph.entryBlock());
  if (MBasicBlock* osrBlock = graph.osrBlock()) {
    MOZ_ASSERT(osrBlock->immediateDominator() == osrBlock);
  } else {
    MOZ_ASSERT(graph.entryBlock()->numDominated() == graph.numBlocks());
  }

  size_t i = graph.numBlocks();
  size_t totalNumDominated = 0;
  for (MBasicBlockIterator block(graph.begin()); block != graph.end();
       block++) {
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

void jit::AssertGraphCoherency(MIRGraph& graph, bool force) {
#ifdef DEBUG
  if (!JitOptions.checkGraphConsistency) {
    return;
  }
  if (!JitOptions.fullDebugChecks && !force) {
    return;
  }
  AssertBasicGraphCoherency(graph, force);
  AssertReversePostorder(graph);
#endif
}

#ifdef DEBUG
static bool IsResumableMIRType(MIRType type) {
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
    case MIRType::BigInt:
    case MIRType::Object:
    case MIRType::Shape:
    case MIRType::MagicOptimizedOut:
    case MIRType::MagicUninitializedLexical:
    case MIRType::MagicIsConstructing:
    case MIRType::Value:
    case MIRType::Simd128:
      return true;

    case MIRType::MagicHole:
    case MIRType::None:
    case MIRType::Slots:
    case MIRType::Elements:
    case MIRType::Pointer:
    case MIRType::Int64:
    case MIRType::RefOrNull:
    case MIRType::StackResults:
    case MIRType::IntPtr:
      return false;
  }
  MOZ_CRASH("Unknown MIRType.");
}

static void AssertResumableOperands(MNode* node) {
  for (size_t i = 0, e = node->numOperands(); i < e; ++i) {
    MDefinition* op = node->getOperand(i);
    if (op->isRecoveredOnBailout()) {
      continue;
    }
    MOZ_ASSERT(IsResumableMIRType(op->type()),
               "Resume point cannot encode its operands");
  }
}

static void AssertIfResumableInstruction(MDefinition* def) {
  if (!def->isRecoveredOnBailout()) {
    return;
  }
  AssertResumableOperands(def);
}

static void AssertResumePointDominatedByOperands(MResumePoint* resume) {
  for (size_t i = 0, e = resume->numOperands(); i < e; ++i) {
    MDefinition* op = resume->getOperand(i);
    MOZ_ASSERT(op->block()->dominates(resume->block()),
               "Resume point is not dominated by its operands");
  }
}
#endif  // DEBUG

void jit::AssertExtendedGraphCoherency(MIRGraph& graph, bool underValueNumberer,
                                       bool force) {
  // Checks the basic GraphCoherency but also other conditions that
  // do not hold immediately (such as the fact that critical edges
  // are split)

#ifdef DEBUG
  if (!JitOptions.checkGraphConsistency) {
    return;
  }
  if (!JitOptions.fullDebugChecks && !force) {
    return;
  }

  AssertGraphCoherency(graph, force);

  AssertDominatorTree(graph);

  DebugOnly<uint32_t> idx = 0;
  for (MBasicBlockIterator block(graph.begin()); block != graph.end();
       block++) {
    MOZ_ASSERT(block->id() == idx);
    ++idx;

    // No critical edges:
    if (block->numSuccessors() > 1) {
      for (size_t i = 0; i < block->numSuccessors(); i++) {
        MOZ_ASSERT(block->getSuccessor(i)->numPredecessors() == 1);
      }
    }

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
    for (size_t i = 0; i < block->numSuccessors(); i++) {
      if (!block->getSuccessor(i)->phisEmpty()) {
        successorWithPhis++;
      }
    }

    MOZ_ASSERT(successorWithPhis <= 1);
    MOZ_ASSERT((successorWithPhis != 0) ==
               (block->successorWithPhis() != nullptr));

    // Verify that phi operands dominate the corresponding CFG predecessor
    // edges.
    for (MPhiIterator iter(block->phisBegin()), end(block->phisEnd());
         iter != end; ++iter) {
      MPhi* phi = *iter;
      for (size_t i = 0, e = phi->numOperands(); i < e; ++i) {
        MOZ_ASSERT(
            phi->getOperand(i)->block()->dominates(block->getPredecessor(i)),
            "Phi input is not dominated by its operand");
      }
    }

    // Verify that instructions are dominated by their operands.
    for (MInstructionIterator iter(block->begin()), end(block->end());
         iter != end; ++iter) {
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

struct BoundsCheckInfo {
  MBoundsCheck* check;
  uint32_t validEnd;
};

typedef HashMap<uint32_t, BoundsCheckInfo, DefaultHasher<uint32_t>,
                JitAllocPolicy>
    BoundsCheckMap;

// Compute a hash for bounds checks which ignores constant offsets in the index.
static HashNumber BoundsCheckHashIgnoreOffset(MBoundsCheck* check) {
  SimpleLinearSum indexSum = ExtractLinearSum(check->index());
  uintptr_t index = indexSum.term ? uintptr_t(indexSum.term) : 0;
  uintptr_t length = uintptr_t(check->length());
  return index ^ length;
}

static MBoundsCheck* FindDominatingBoundsCheck(BoundsCheckMap& checks,
                                               MBoundsCheck* check,
                                               size_t index) {
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

    if (!checks.put(hash, info)) return nullptr;

    return check;
  }

  return p->value().check;
}

static MathSpace ExtractMathSpace(MDefinition* ins) {
  MOZ_ASSERT(ins->isAdd() || ins->isSub());
  MBinaryArithInstruction* arith = nullptr;
  if (ins->isAdd()) {
    arith = ins->toAdd();
  } else {
    arith = ins->toSub();
  }
  switch (arith->truncateKind()) {
    case TruncateKind::NoTruncate:
    case TruncateKind::TruncateAfterBailouts:
      // TruncateAfterBailouts is considered as infinite space because the
      // LinearSum will effectively remove the bailout check.
      return MathSpace::Infinite;
    case TruncateKind::IndirectTruncate:
    case TruncateKind::Truncate:
      return MathSpace::Modulo;
  }
  MOZ_CRASH("Unknown TruncateKind");
}

static bool MonotoneAdd(int32_t lhs, int32_t rhs) {
  return (lhs >= 0 && rhs >= 0) || (lhs <= 0 && rhs <= 0);
}

static bool MonotoneSub(int32_t lhs, int32_t rhs) {
  return (lhs >= 0 && rhs <= 0) || (lhs <= 0 && rhs >= 0);
}

// Extract a linear sum from ins, if possible (otherwise giving the
// sum 'ins + 0').
SimpleLinearSum jit::ExtractLinearSum(MDefinition* ins, MathSpace space,
                                      int32_t recursionDepth) {
  const int32_t SAFE_RECURSION_LIMIT = 100;
  if (recursionDepth > SAFE_RECURSION_LIMIT) {
    return SimpleLinearSum(ins, 0);
  }

  // Unwrap Int32ToIntPtr. This instruction only changes the representation
  // (int32_t to intptr_t) without affecting the value.
  if (ins->isInt32ToIntPtr()) {
    ins = ins->toInt32ToIntPtr()->input();
  }

  if (ins->isBeta()) {
    ins = ins->getOperand(0);
  }

  MOZ_ASSERT(!ins->isInt32ToIntPtr());

  if (ins->type() != MIRType::Int32) {
    return SimpleLinearSum(ins, 0);
  }

  if (ins->isConstant()) {
    return SimpleLinearSum(nullptr, ins->toConstant()->toInt32());
  }

  if (!ins->isAdd() && !ins->isSub()) {
    return SimpleLinearSum(ins, 0);
  }

  // Only allow math which are in the same space.
  MathSpace insSpace = ExtractMathSpace(ins);
  if (space == MathSpace::Unknown) {
    space = insSpace;
  } else if (space != insSpace) {
    return SimpleLinearSum(ins, 0);
  }
  MOZ_ASSERT(space == MathSpace::Modulo || space == MathSpace::Infinite);

  MDefinition* lhs = ins->getOperand(0);
  MDefinition* rhs = ins->getOperand(1);
  if (lhs->type() != MIRType::Int32 || rhs->type() != MIRType::Int32) {
    return SimpleLinearSum(ins, 0);
  }

  // Extract linear sums of each operand.
  SimpleLinearSum lsum = ExtractLinearSum(lhs, space, recursionDepth + 1);
  SimpleLinearSum rsum = ExtractLinearSum(rhs, space, recursionDepth + 1);

  // LinearSum only considers a single term operand, if both sides have
  // terms, then ignore extracted linear sums.
  if (lsum.term && rsum.term) {
    return SimpleLinearSum(ins, 0);
  }

  // Check if this is of the form <SUM> + n or n + <SUM>.
  if (ins->isAdd()) {
    int32_t constant;
    if (space == MathSpace::Modulo) {
      constant = uint32_t(lsum.constant) + uint32_t(rsum.constant);
    } else if (!SafeAdd(lsum.constant, rsum.constant, &constant) ||
               !MonotoneAdd(lsum.constant, rsum.constant)) {
      return SimpleLinearSum(ins, 0);
    }
    return SimpleLinearSum(lsum.term ? lsum.term : rsum.term, constant);
  }

  MOZ_ASSERT(ins->isSub());
  // Check if this is of the form <SUM> - n.
  if (lsum.term) {
    int32_t constant;
    if (space == MathSpace::Modulo) {
      constant = uint32_t(lsum.constant) - uint32_t(rsum.constant);
    } else if (!SafeSub(lsum.constant, rsum.constant, &constant) ||
               !MonotoneSub(lsum.constant, rsum.constant)) {
      return SimpleLinearSum(ins, 0);
    }
    return SimpleLinearSum(lsum.term, constant);
  }

  // Ignore any of the form n - <SUM>.
  return SimpleLinearSum(ins, 0);
}

// Extract a linear inequality holding when a boolean test goes in the
// specified direction, of the form 'lhs + lhsN <= rhs' (or >=).
bool jit::ExtractLinearInequality(MTest* test, BranchDirection direction,
                                  SimpleLinearSum* plhs, MDefinition** prhs,
                                  bool* plessEqual) {
  if (!test->getOperand(0)->isCompare()) {
    return false;
  }

  MCompare* compare = test->getOperand(0)->toCompare();

  MDefinition* lhs = compare->getOperand(0);
  MDefinition* rhs = compare->getOperand(1);

  // TODO: optimize Compare_UInt32
  if (!compare->isInt32Comparison()) {
    return false;
  }

  MOZ_ASSERT(lhs->type() == MIRType::Int32);
  MOZ_ASSERT(rhs->type() == MIRType::Int32);

  JSOp jsop = compare->jsop();
  if (direction == FALSE_BRANCH) {
    jsop = NegateCompareOp(jsop);
  }

  SimpleLinearSum lsum = ExtractLinearSum(lhs);
  SimpleLinearSum rsum = ExtractLinearSum(rhs);

  if (!SafeSub(lsum.constant, rsum.constant, &lsum.constant)) {
    return false;
  }

  // Normalize operations to use <= or >=.
  switch (jsop) {
    case JSOp::Le:
      *plessEqual = true;
      break;
    case JSOp::Lt:
      /* x < y ==> x + 1 <= y */
      if (!SafeAdd(lsum.constant, 1, &lsum.constant)) {
        return false;
      }
      *plessEqual = true;
      break;
    case JSOp::Ge:
      *plessEqual = false;
      break;
    case JSOp::Gt:
      /* x > y ==> x - 1 >= y */
      if (!SafeSub(lsum.constant, 1, &lsum.constant)) {
        return false;
      }
      *plessEqual = false;
      break;
    default:
      return false;
  }

  *plhs = lsum;
  *prhs = rsum.term;

  return true;
}

static bool TryEliminateBoundsCheck(BoundsCheckMap& checks, size_t blockIndex,
                                    MBoundsCheck* dominated, bool* eliminated) {
  MOZ_ASSERT(!*eliminated);

  // Replace all uses of the bounds check with the actual index.
  // This is (a) necessary, because we can coalesce two different
  // bounds checks and would otherwise use the wrong index and
  // (b) helps register allocation. Note that this is safe since
  // no other pass after bounds check elimination moves instructions.
  dominated->replaceAllUsesWith(dominated->index());

  if (!dominated->isMovable()) {
    return true;
  }

  if (!dominated->fallible()) {
    return true;
  }

  MBoundsCheck* dominating =
      FindDominatingBoundsCheck(checks, dominated, blockIndex);
  if (!dominating) {
    return false;
  }

  if (dominating == dominated) {
    // We didn't find a dominating bounds check.
    return true;
  }

  // We found two bounds checks with the same hash number, but we still have
  // to make sure the lengths and index terms are equal.
  if (dominating->length() != dominated->length()) {
    return true;
  }

  SimpleLinearSum sumA = ExtractLinearSum(dominating->index());
  SimpleLinearSum sumB = ExtractLinearSum(dominated->index());

  // Both terms should be nullptr or the same definition.
  if (sumA.term != sumB.term) {
    return true;
  }

  // This bounds check is redundant.
  *eliminated = true;

  // Normalize the ranges according to the constant offsets in the two indexes.
  int32_t minimumA, maximumA, minimumB, maximumB;
  if (!SafeAdd(sumA.constant, dominating->minimum(), &minimumA) ||
      !SafeAdd(sumA.constant, dominating->maximum(), &maximumA) ||
      !SafeAdd(sumB.constant, dominated->minimum(), &minimumB) ||
      !SafeAdd(sumB.constant, dominated->maximum(), &maximumB)) {
    return false;
  }

  // Update the dominating check to cover both ranges, denormalizing the
  // result per the constant offset in the index.
  int32_t newMinimum, newMaximum;
  if (!SafeSub(std::min(minimumA, minimumB), sumA.constant, &newMinimum) ||
      !SafeSub(std::max(maximumA, maximumB), sumA.constant, &newMaximum)) {
    return false;
  }

  dominating->setMinimum(newMinimum);
  dominating->setMaximum(newMaximum);
  dominating->setBailoutKind(BailoutKind::HoistBoundsCheck);

  return true;
}

// Eliminate checks which are redundant given each other or other instructions.
//
// A bounds check is considered redundant if it's dominated by another bounds
// check with the same length and the indexes differ by only a constant amount.
// In this case we eliminate the redundant bounds check and update the other one
// to cover the ranges of both checks.
//
// Bounds checks are added to a hash map and since the hash function ignores
// differences in constant offset, this offers a fast way to find redundant
// checks.
bool jit::EliminateRedundantChecks(MIRGraph& graph) {
  BoundsCheckMap checks(graph.alloc());

  // Stack for pre-order CFG traversal.
  Vector<MBasicBlock*, 1, JitAllocPolicy> worklist(graph.alloc());

  // The index of the current block in the CFG traversal.
  size_t index = 0;

  // Add all self-dominating blocks to the worklist.
  // This includes all roots. Order does not matter.
  for (MBasicBlockIterator i(graph.begin()); i != graph.end(); i++) {
    MBasicBlock* block = *i;
    if (block->immediateDominator() == block) {
      if (!worklist.append(block)) {
        return false;
      }
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

    for (MDefinitionIterator iter(block); iter;) {
      MDefinition* def = *iter++;

      if (!def->isBoundsCheck()) {
        continue;
      }

      bool eliminated = false;
      if (!TryEliminateBoundsCheck(checks, index, def->toBoundsCheck(),
                                   &eliminated)) {
        return false;
      }

      if (eliminated) {
        block->discardDef(def);
      }
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

static bool NeedsKeepAlive(MInstruction* slotsOrElements, MInstruction* use) {
  MOZ_ASSERT(slotsOrElements->type() == MIRType::Elements ||
             slotsOrElements->type() == MIRType::Slots);

  if (slotsOrElements->block() != use->block()) {
    return true;
  }

  MBasicBlock* block = use->block();
  MInstructionIterator iter(block->begin(slotsOrElements));
  MOZ_ASSERT(*iter == slotsOrElements);
  ++iter;

  while (true) {
    if (*iter == use) {
      return false;
    }

    switch (iter->op()) {
      case MDefinition::Opcode::Nop:
      case MDefinition::Opcode::Constant:
      case MDefinition::Opcode::KeepAliveObject:
      case MDefinition::Opcode::Unbox:
      case MDefinition::Opcode::LoadDynamicSlot:
      case MDefinition::Opcode::StoreDynamicSlot:
      case MDefinition::Opcode::LoadFixedSlot:
      case MDefinition::Opcode::StoreFixedSlot:
      case MDefinition::Opcode::LoadElement:
      case MDefinition::Opcode::LoadElementAndUnbox:
      case MDefinition::Opcode::StoreElement:
      case MDefinition::Opcode::StoreHoleValueElement:
      case MDefinition::Opcode::InitializedLength:
      case MDefinition::Opcode::ArrayLength:
      case MDefinition::Opcode::BoundsCheck:
      case MDefinition::Opcode::GuardElementNotHole:
      case MDefinition::Opcode::SpectreMaskIndex:
        iter++;
        break;
      default:
        return true;
    }
  }

  MOZ_CRASH("Unreachable");
}

bool jit::AddKeepAliveInstructions(MIRGraph& graph) {
  for (MBasicBlockIterator i(graph.begin()); i != graph.end(); i++) {
    MBasicBlock* block = *i;

    for (MInstructionIterator insIter(block->begin()); insIter != block->end();
         insIter++) {
      MInstruction* ins = *insIter;
      if (ins->type() != MIRType::Elements && ins->type() != MIRType::Slots) {
        continue;
      }

      MDefinition* ownerObject;
      switch (ins->op()) {
        case MDefinition::Opcode::Elements:
        case MDefinition::Opcode::ArrayBufferViewElements:
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
          MOZ_ASSERT_IF(!use->toStoreElementHole()->object()->isUnbox() &&
                            !ownerObject->isUnbox(),
                        use->toStoreElementHole()->object() == ownerObject);
          continue;
        }

        if (use->isInArray()) {
          // See StoreElementHole case above.
          MOZ_ASSERT_IF(
              !use->toInArray()->object()->isUnbox() && !ownerObject->isUnbox(),
              use->toInArray()->object() == ownerObject);
          continue;
        }

        if (!NeedsKeepAlive(ins, use)) {
          continue;
        }

        if (!graph.alloc().ensureBallast()) {
          return false;
        }
        MKeepAliveObject* keepAlive =
            MKeepAliveObject::New(graph.alloc(), ownerObject);
        use->block()->insertAfter(use, keepAlive);
      }
    }
  }

  return true;
}

bool LinearSum::multiply(int32_t scale) {
  for (size_t i = 0; i < terms_.length(); i++) {
    if (!SafeMul(scale, terms_[i].scale, &terms_[i].scale)) {
      return false;
    }
  }
  return SafeMul(scale, constant_, &constant_);
}

bool LinearSum::divide(uint32_t scale) {
  MOZ_ASSERT(scale > 0);

  for (size_t i = 0; i < terms_.length(); i++) {
    if (terms_[i].scale % scale != 0) {
      return false;
    }
  }
  if (constant_ % scale != 0) {
    return false;
  }

  for (size_t i = 0; i < terms_.length(); i++) {
    terms_[i].scale /= scale;
  }
  constant_ /= scale;

  return true;
}

bool LinearSum::add(const LinearSum& other, int32_t scale /* = 1 */) {
  for (size_t i = 0; i < other.terms_.length(); i++) {
    int32_t newScale = scale;
    if (!SafeMul(scale, other.terms_[i].scale, &newScale)) {
      return false;
    }
    if (!add(other.terms_[i].term, newScale)) {
      return false;
    }
  }
  int32_t newConstant = scale;
  if (!SafeMul(scale, other.constant_, &newConstant)) {
    return false;
  }
  return add(newConstant);
}

bool LinearSum::add(SimpleLinearSum other, int32_t scale) {
  if (other.term && !add(other.term, scale)) {
    return false;
  }

  int32_t constant;
  if (!SafeMul(other.constant, scale, &constant)) {
    return false;
  }

  return add(constant);
}

bool LinearSum::add(MDefinition* term, int32_t scale) {
  MOZ_ASSERT(term);

  if (scale == 0) {
    return true;
  }

  if (MConstant* termConst = term->maybeConstantValue()) {
    int32_t constant = termConst->toInt32();
    if (!SafeMul(constant, scale, &constant)) {
      return false;
    }
    return add(constant);
  }

  for (size_t i = 0; i < terms_.length(); i++) {
    if (term == terms_[i].term) {
      if (!SafeAdd(scale, terms_[i].scale, &terms_[i].scale)) {
        return false;
      }
      if (terms_[i].scale == 0) {
        terms_[i] = terms_.back();
        terms_.popBack();
      }
      return true;
    }
  }

  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!terms_.append(LinearTerm(term, scale))) {
    oomUnsafe.crash("LinearSum::add");
  }

  return true;
}

bool LinearSum::add(int32_t constant) {
  return SafeAdd(constant, constant_, &constant_);
}

void LinearSum::dump(GenericPrinter& out) const {
  for (size_t i = 0; i < terms_.length(); i++) {
    int32_t scale = terms_[i].scale;
    int32_t id = terms_[i].term->id();
    MOZ_ASSERT(scale);
    if (scale > 0) {
      if (i) {
        out.printf("+");
      }
      if (scale == 1) {
        out.printf("#%d", id);
      } else {
        out.printf("%d*#%d", scale, id);
      }
    } else if (scale == -1) {
      out.printf("-#%d", id);
    } else {
      out.printf("%d*#%d", scale, id);
    }
  }
  if (constant_ > 0) {
    out.printf("+%d", constant_);
  } else if (constant_ < 0) {
    out.printf("%d", constant_);
  }
}

void LinearSum::dump() const {
  Fprinter out(stderr);
  dump(out);
  out.finish();
}

MDefinition* jit::ConvertLinearSum(TempAllocator& alloc, MBasicBlock* block,
                                   const LinearSum& sum,
                                   BailoutKind bailoutKind) {
  MDefinition* def = nullptr;

  for (size_t i = 0; i < sum.numTerms(); i++) {
    LinearTerm term = sum.term(i);
    MOZ_ASSERT(!term.term->isConstant());
    if (term.scale == 1) {
      if (def) {
        def = MAdd::New(alloc, def, term.term, MIRType::Int32);
        def->setBailoutKind(bailoutKind);
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
      def = MSub::New(alloc, def, term.term, MIRType::Int32);
      def->setBailoutKind(bailoutKind);
      block->insertAtEnd(def->toInstruction());
      def->computeRange(alloc);
    } else {
      MOZ_ASSERT(term.scale != 0);
      MConstant* factor = MConstant::New(alloc, Int32Value(term.scale));
      block->insertAtEnd(factor);
      MMul* mul = MMul::New(alloc, term.term, factor, MIRType::Int32);
      mul->setBailoutKind(bailoutKind);
      block->insertAtEnd(mul);
      mul->computeRange(alloc);
      if (def) {
        def = MAdd::New(alloc, def, mul, MIRType::Int32);
        def->setBailoutKind(bailoutKind);
        block->insertAtEnd(def->toInstruction());
        def->computeRange(alloc);
      } else {
        def = mul;
      }
    }
  }

  if (!def) {
    def = MConstant::New(alloc, Int32Value(0));
    block->insertAtEnd(def->toInstruction());
    def->computeRange(alloc);
  }

  return def;
}

// Mark all the blocks that are in the loop with the given header.
// Returns the number of blocks marked. Set *canOsr to true if the loop is
// reachable from both the normal entry and the OSR entry.
size_t jit::MarkLoopBlocks(MIRGraph& graph, MBasicBlock* header, bool* canOsr) {
#ifdef DEBUG
  for (ReversePostorderIterator i = graph.rpoBegin(), e = graph.rpoEnd();
       i != e; ++i) {
    MOZ_ASSERT(!i->isMarked(), "Some blocks already marked");
  }
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
  for (PostorderIterator i = graph.poBegin(backedge);; ++i) {
    MOZ_ASSERT(
        i != graph.poEnd(),
        "Reached the end of the graph while searching for the loop header");
    MBasicBlock* block = *i;
    // If we've reached the loop header, we're done.
    if (block == header) {
      break;
    }
    // A block not marked by the time we reach it is not in the loop.
    if (!block->isMarked()) {
      continue;
    }

    // This block is in the loop; trace to its predecessors.
    for (size_t p = 0, e = block->numPredecessors(); p != e; ++p) {
      MBasicBlock* pred = block->getPredecessor(p);
      if (pred->isMarked()) {
        continue;
      }

      // Blocks dominated by the OSR entry are not part of the loop
      // (unless they aren't reachable from the normal entry).
      if (osrBlock && pred != header && osrBlock->dominates(pred) &&
          !osrBlock->dominates(header)) {
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
void jit::UnmarkLoopBlocks(MIRGraph& graph, MBasicBlock* header) {
  MBasicBlock* backedge = header->backedge();
  for (ReversePostorderIterator i = graph.rpoBegin(header);; ++i) {
    MOZ_ASSERT(i != graph.rpoEnd(),
               "Reached the end of the graph while searching for the backedge");
    MBasicBlock* block = *i;
    if (block->isMarked()) {
      block->unmark();
      if (block == backedge) {
        break;
      }
    }
  }

#ifdef DEBUG
  for (ReversePostorderIterator i = graph.rpoBegin(), e = graph.rpoEnd();
       i != e; ++i) {
    MOZ_ASSERT(!i->isMarked(), "Not all blocks got unmarked");
  }
#endif
}

bool jit::FoldLoadsWithUnbox(MIRGenerator* mir, MIRGraph& graph) {
  // This pass folds MLoadFixedSlot, MLoadDynamicSlot, MLoadElement instructions
  // followed by MUnbox into a single instruction. For LoadElement this allows
  // us to fuse the hole check with the type check for the unbox.

  for (MBasicBlockIterator block(graph.begin()); block != graph.end();
       block++) {
    if (mir->shouldCancel("FoldLoadsWithUnbox")) {
      return false;
    }

    for (MInstructionIterator insIter(block->begin());
         insIter != block->end();) {
      MInstruction* ins = *insIter;
      insIter++;

      // We're only interested in loads producing a Value.
      if (!ins->isLoadFixedSlot() && !ins->isLoadDynamicSlot() &&
          !ins->isLoadElement()) {
        continue;
      }
      if (ins->type() != MIRType::Value) {
        continue;
      }

      MInstruction* load = ins;

      // Ensure there's a single def-use (ignoring resume points) and it's an
      // unbox.
      MDefinition* defUse = load->maybeSingleDefUse();
      if (!defUse) {
        continue;
      }
      if (!defUse->isUnbox()) {
        continue;
      }

      // For now require the load and unbox to be in the same block. This isn't
      // strictly necessary but it's the common case and could prevent bailouts
      // when moving the unbox before a loop.
      MUnbox* unbox = defUse->toUnbox();
      if (unbox->block() != *block) {
        continue;
      }

      MOZ_ASSERT(!IsMagicType(unbox->type()));

      // If this is a LoadElement that needs a hole check, we only support
      // folding it with a fallible unbox so that we can eliminate the hole
      // check.
      if (load->isLoadElement() && load->toLoadElement()->needsHoleCheck() &&
          !unbox->fallible()) {
        continue;
      }

      // Combine the load and unbox into a single MIR instruction.
      if (!graph.alloc().ensureBallast()) {
        return false;
      }

      MIRType type = unbox->type();
      MUnbox::Mode mode = unbox->mode();

      MInstruction* replacement;
      switch (load->op()) {
        case MDefinition::Opcode::LoadFixedSlot: {
          auto* loadIns = load->toLoadFixedSlot();
          replacement = MLoadFixedSlotAndUnbox::New(
              graph.alloc(), loadIns->object(), loadIns->slot(), mode, type);
          break;
        }
        case MDefinition::Opcode::LoadDynamicSlot: {
          auto* loadIns = load->toLoadDynamicSlot();
          replacement = MLoadDynamicSlotAndUnbox::New(
              graph.alloc(), loadIns->slots(), loadIns->slot(), mode, type);
          break;
        }
        case MDefinition::Opcode::LoadElement: {
          auto* loadIns = load->toLoadElement();
          MOZ_ASSERT_IF(loadIns->needsHoleCheck(), unbox->fallible());
          replacement = MLoadElementAndUnbox::New(
              graph.alloc(), loadIns->elements(), loadIns->index(), mode, type);
          break;
        }
        default:
          MOZ_CRASH("Unexpected instruction");
      }

      block->insertBefore(load, replacement);
      unbox->replaceAllUsesWith(replacement);
      load->replaceAllUsesWith(replacement);

      if (*insIter == unbox) {
        insIter++;
      }
      block->discard(unbox);
      block->discard(load);
    }
  }

  return true;
}

// Reorder the blocks in the loop starting at the given header to be contiguous.
static void MakeLoopContiguous(MIRGraph& graph, MBasicBlock* header,
                               size_t numMarked) {
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
      if (block == backedge) {
        break;
      }
    } else {
      // This block is not in the loop. Move it to the end.
      graph.moveBlockBefore(insertPt, block);
      block->setId(notInLoopId++);
    }
  }
  MOZ_ASSERT(header->id() == headerId, "Loop header id changed");
  MOZ_ASSERT(inLoopId == headerId + numMarked,
             "Wrong number of blocks kept in loop");
  MOZ_ASSERT(notInLoopId == (insertIter != graph.rpoEnd() ? insertPt->id()
                                                          : graph.numBlocks()),
             "Wrong number of blocks moved out of loop");
}

// Reorder the blocks in the graph so that loops are contiguous.
bool jit::MakeLoopsContiguous(MIRGraph& graph) {
  // Visit all loop headers (in any order).
  for (MBasicBlockIterator i(graph.begin()); i != graph.end(); i++) {
    MBasicBlock* header = *i;
    if (!header->isLoopHeader()) {
      continue;
    }

    // Mark all blocks that are actually part of the loop.
    bool canOsr;
    size_t numMarked = MarkLoopBlocks(graph, header, &canOsr);

    // If the loop isn't a loop, don't try to optimize it.
    if (numMarked == 0) {
      continue;
    }

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

#ifdef JS_JITSPEW
static void DumpDefinition(GenericPrinter& out, MDefinition* def,
                           size_t depth) {
  out.printf("%u:", def->id());
  if (def->isConstant()) {
    def->printOpcode(out);
  } else {
    MDefinition::PrintOpcodeName(out, def->op());
  }

  if (depth == 0) {
    return;
  }

  for (size_t i = 0; i < def->numOperands(); i++) {
    out.printf(" (");
    DumpDefinition(out, def->getOperand(i), depth - 1);
    out.printf(")");
  }
}
#endif

void jit::DumpMIRExpressions(MIRGraph& graph, const CompileInfo& info,
                             const char* phase) {
#ifdef JS_JITSPEW
  if (!JitSpewEnabled(JitSpew_MIRExpressions)) {
    return;
  }

  Fprinter& out = JitSpewPrinter();
  out.printf("===== %s =====\n", phase);

  size_t depth = 2;
  bool isFirstBlock = true;
  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); block++) {
    if (isFirstBlock) {
      isFirstBlock = false;
    } else {
      out.printf("  --\n");
    }
    for (MInstructionIterator iter(block->begin()), end(block->end());
         iter != end; iter++) {
      out.printf("  ");
      DumpDefinition(out, *iter, depth);
      out.printf("\n");
    }
  }

  if (info.compilingWasm()) {
    out.printf("===== end wasm MIR dump =====\n");
  } else {
    out.printf("===== %s:%u =====\n", info.filename(), info.lineno());
  }
#endif
}

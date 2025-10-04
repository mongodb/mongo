/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Sink.h"

#include "jit/IonOptimizationLevels.h"
#include "jit/JitSpewer.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"

namespace js {
namespace jit {

// Given the last found common dominator and a new definition to dominate, the
// CommonDominator function returns the basic block which dominate the last
// common dominator and the definition. If no such block exists, then this
// functions return null.
static MBasicBlock* CommonDominator(MBasicBlock* commonDominator,
                                    MBasicBlock* defBlock) {
  // This is the first instruction visited, record its basic block as being
  // the only interesting one.
  if (!commonDominator) {
    return defBlock;
  }

  // Iterate on immediate dominators of the known common dominator to find a
  // block which dominates all previous uses as well as this instruction.
  while (!commonDominator->dominates(defBlock)) {
    MBasicBlock* nextBlock = commonDominator->immediateDominator();
    // All uses are dominated, so, this cannot happen unless the graph
    // coherency is not respected.
    MOZ_ASSERT(commonDominator != nextBlock);
    commonDominator = nextBlock;
  }

  return commonDominator;
}

bool Sink(MIRGenerator* mir, MIRGraph& graph) {
  JitSpew(JitSpew_Sink, "Begin");
  TempAllocator& alloc = graph.alloc();
  bool sinkEnabled = mir->optimizationInfo().sinkEnabled();

  for (PostorderIterator block = graph.poBegin(); block != graph.poEnd();
       block++) {
    if (mir->shouldCancel("Sink")) {
      return false;
    }

    for (MInstructionReverseIterator iter = block->rbegin();
         iter != block->rend();) {
      MInstruction* ins = *iter++;

      // Only instructions which can be recovered on bailout can be moved
      // into the bailout paths.
      if (ins->isGuard() || ins->isGuardRangeBailouts() ||
          ins->isRecoveredOnBailout() || !ins->canRecoverOnBailout()) {
        continue;
      }

      // Compute a common dominator for all uses of the current
      // instruction.
      bool hasLiveUses = false;
      bool hasUses = false;
      MBasicBlock* usesDominator = nullptr;
      for (MUseIterator i(ins->usesBegin()), e(ins->usesEnd()); i != e; i++) {
        hasUses = true;
        MNode* consumerNode = (*i)->consumer();
        if (consumerNode->isResumePoint()) {
          if (!consumerNode->toResumePoint()->isRecoverableOperand(*i)) {
            hasLiveUses = true;
          }
          continue;
        }

        MDefinition* consumer = consumerNode->toDefinition();
        if (consumer->isRecoveredOnBailout()) {
          continue;
        }

        hasLiveUses = true;

        // If the instruction is a Phi, then we should dominate the
        // predecessor from which the value is coming from.
        MBasicBlock* consumerBlock = consumer->block();
        if (consumer->isPhi()) {
          consumerBlock = consumerBlock->getPredecessor(consumer->indexOf(*i));
        }

        usesDominator = CommonDominator(usesDominator, consumerBlock);
        if (usesDominator == *block) {
          break;
        }
      }

      // Leave this instruction for DCE.
      if (!hasUses) {
        continue;
      }

      // We have no uses, so sink this instruction in all the bailout
      // paths.
      if (!hasLiveUses) {
        MOZ_ASSERT(!usesDominator);
        ins->setRecoveredOnBailout();
        JitSpewDef(JitSpew_Sink,
                   "  No live uses, recover the instruction on bailout\n", ins);
        continue;
      }

      // This guard is temporarly moved here as the above code deals with
      // Dead Code elimination, which got moved into this Sink phase, as
      // the Dead Code elimination used to move instructions with no-live
      // uses to the bailout path.
      if (!sinkEnabled) {
        continue;
      }

      // To move an effectful instruction, we would have to verify that the
      // side-effect is not observed. In the mean time, we just inhibit
      // this optimization on effectful instructions.
      if (ins->isEffectful()) {
        continue;
      }

      // If all the uses are under a loop, we might not want to work
      // against LICM by moving everything back into the loop, but if the
      // loop is it-self inside an if, then we still want to move the
      // computation under this if statement.
      while (block->loopDepth() < usesDominator->loopDepth()) {
        MOZ_ASSERT(usesDominator != usesDominator->immediateDominator());
        usesDominator = usesDominator->immediateDominator();
      }

      // Only move instructions if there is a branch between the dominator
      // of the uses and the original instruction. This prevent moving the
      // computation of the arguments into an inline function if there is
      // no major win.
      MBasicBlock* lastJoin = usesDominator;
      while (*block != lastJoin && lastJoin->numPredecessors() == 1) {
        MOZ_ASSERT(lastJoin != lastJoin->immediateDominator());
        MBasicBlock* next = lastJoin->immediateDominator();
        if (next->numSuccessors() > 1) {
          break;
        }
        lastJoin = next;
      }
      if (*block == lastJoin) {
        continue;
      }

      // Skip to the next instruction if we cannot find a common dominator
      // for all the uses of this instruction, or if the common dominator
      // correspond to the block of the current instruction.
      if (!usesDominator || usesDominator == *block) {
        continue;
      }

      // Only instruction which can be recovered on bailout and which are
      // sinkable can be moved into blocks which are below while filling
      // the resume points with a clone which is recovered on bailout.

      // If the instruction has live uses and if it is clonable, then we
      // can clone the instruction for all non-dominated uses and move the
      // instruction into the block which is dominating all live uses.
      if (!ins->canClone()) {
        continue;
      }

      // If the block is a split-edge block, which is created for folding
      // test conditions, then the block has no resume point and has
      // multiple predecessors.  In such case, we cannot safely move
      // bailing instruction to these blocks as we have no way to bailout.
      if (!usesDominator->entryResumePoint() &&
          usesDominator->numPredecessors() != 1) {
        continue;
      }

      JitSpewDef(JitSpew_Sink, "  Can Clone & Recover, sink instruction\n",
                 ins);
      JitSpew(JitSpew_Sink, "  into Block %u", usesDominator->id());

      // Copy the arguments and clone the instruction.
      MDefinitionVector operands(alloc);
      for (size_t i = 0, end = ins->numOperands(); i < end; i++) {
        if (!operands.append(ins->getOperand(i))) {
          return false;
        }
      }

      MInstruction* clone = ins->clone(alloc, operands);
      if (!clone) {
        return false;
      }
      ins->block()->insertBefore(ins, clone);
      clone->setRecoveredOnBailout();

      // We should not update the producer of the entry resume point, as
      // it cannot refer to any instruction within the basic block excepts
      // for Phi nodes.
      MResumePoint* entry = usesDominator->entryResumePoint();

      // Replace the instruction by its clone in all the resume points /
      // recovered-on-bailout instructions which are not in blocks which
      // are dominated by the usesDominator block.
      for (MUseIterator i(ins->usesBegin()), e(ins->usesEnd()); i != e;) {
        MUse* use = *i++;
        MNode* consumer = use->consumer();

        // If the consumer is a Phi, then we look for the index of the
        // use to find the corresponding predecessor block, which is
        // then used as the consumer block.
        MBasicBlock* consumerBlock = consumer->block();
        if (consumer->isDefinition() && consumer->toDefinition()->isPhi()) {
          consumerBlock = consumerBlock->getPredecessor(
              consumer->toDefinition()->toPhi()->indexOf(use));
        }

        // Keep the current instruction for all dominated uses, except
        // for the entry resume point of the block in which the
        // instruction would be moved into.
        if (usesDominator->dominates(consumerBlock) &&
            (!consumer->isResumePoint() ||
             consumer->toResumePoint() != entry)) {
          continue;
        }

        use->replaceProducer(clone);
      }

      // As we move this instruction in a different block, we should
      // verify that we do not carry over a resume point which would refer
      // to an outdated state of the control flow.
      if (ins->resumePoint()) {
        ins->clearResumePoint();
      }

      // Now, that all uses which are not dominated by usesDominator are
      // using the cloned instruction, we can safely move the instruction
      // into the usesDominator block.
      MInstruction* at =
          usesDominator->safeInsertTop(nullptr, MBasicBlock::IgnoreRecover);
      block->moveBefore(at, ins);
    }
  }

  return true;
}

}  // namespace jit
}  // namespace js

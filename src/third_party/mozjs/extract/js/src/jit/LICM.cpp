/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/LICM.h"

#include "jit/IonAnalysis.h"
#include "jit/JitSpewer.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace js::jit;

// There are two constants which control whether a loop is LICM'd or is left
// unchanged.  For rationale see comment in jit::LICM() below.
//
// A bit of quick profiling with the wasm Embenchen suite on x64 shows that
// the threshold pair (100,25) has either no effect or gives a small net
// reduction in memory traffic, compared to unconstrained LICMing.  Halving
// them to (50,12) gives a small overall increase in memory traffic,
// suggesting it excludes too many loops from LICM.  Doubling them to (200,50)
// gives a win that is even smaller than (100,25), hence (100,25) seems the
// best choice.
//
// If a loop has more than this number of basic blocks in its body, it won't
// be LICM'd.
static constexpr size_t LargestAllowedLoop = 100;

// If a loop contains an MTableSwitch instruction that has more than this many
// successors, it won't be LICM'd.
static constexpr size_t LargestAllowedTableSwitch = 25;

// Test whether any instruction in the loop possiblyCalls().
static bool LoopContainsPossibleCall(MIRGraph& graph, MBasicBlock* header,
                                     MBasicBlock* backedge) {
  for (auto i(graph.rpoBegin(header));; ++i) {
    MOZ_ASSERT(i != graph.rpoEnd(),
               "Reached end of graph searching for blocks in loop");
    MBasicBlock* block = *i;
    if (!block->isMarked()) {
      continue;
    }

    for (auto insIter(block->begin()), insEnd(block->end()); insIter != insEnd;
         ++insIter) {
      MInstruction* ins = *insIter;
      if (ins->possiblyCalls()) {
#ifdef JS_JITSPEW
        JitSpew(JitSpew_LICM, "    Possible call found at %s%u", ins->opName(),
                ins->id());
#endif
        return true;
      }
    }

    if (block == backedge) {
      break;
    }
  }
  return false;
}

// Tests whether any instruction in the loop is a table-switch with more than
// `LargestAllowedTableSwitch` successors.  If it returns true, it also
// returns the actual number of successors of the instruction in question,
// although that is used only for statistics/debug printing.
static bool LoopContainsBigTableSwitch(MIRGraph& graph, MBasicBlock* header,
                                       /*OUT*/ size_t* numSuccessors) {
  MBasicBlock* backedge = header->backedge();

  for (auto i(graph.rpoBegin(header));; ++i) {
    MOZ_ASSERT(i != graph.rpoEnd(),
               "Reached end of graph searching for blocks in loop");
    MBasicBlock* block = *i;
    if (!block->isMarked()) {
      continue;
    }

    for (auto insIter(block->begin()), insEnd(block->end()); insIter != insEnd;
         ++insIter) {
      MInstruction* ins = *insIter;
      if (ins->isTableSwitch() &&
          ins->toTableSwitch()->numSuccessors() > LargestAllowedTableSwitch) {
        *numSuccessors = ins->toTableSwitch()->numSuccessors();
        return true;
      }
    }

    if (block == backedge) {
      break;
    }
  }
  return false;
}

// When a nested loop has no exits back into what would be its parent loop,
// MarkLoopBlocks on the parent loop doesn't mark the blocks of the nested
// loop, since they technically aren't part of the loop. However, AliasAnalysis
// currently does consider such nested loops to be part of their parent
// loops. Consequently, we can't use IsInLoop on dependency() values; we must
// test whether a dependency() is *before* the loop, even if it is not
// technically in the loop.
static bool IsBeforeLoop(MDefinition* ins, MBasicBlock* header) {
  return ins->block()->id() < header->id();
}

// Test whether the given instruction is inside the loop (and thus not
// loop-invariant).
static bool IsInLoop(MDefinition* ins) { return ins->block()->isMarked(); }

// Test whether the given instruction is cheap and not worth hoisting unless
// one of its users will be hoisted as well.
static bool RequiresHoistedUse(const MDefinition* ins, bool hasCalls) {
  if (ins->isBox()) {
    MOZ_ASSERT(!ins->toBox()->input()->isBox(),
               "Box of a box could lead to unbounded recursion");
    return true;
  }

  // Integer constants are usually cheap and aren't worth hoisting on their
  // own, in general. Floating-point constants typically are worth hoisting,
  // unless they'll end up being spilled (eg. due to a call).
  if (ins->isConstant() && (!IsFloatingPointType(ins->type()) || hasCalls)) {
    return true;
  }

  return false;
}

// Test whether the given instruction has any operands defined within the loop.
static bool HasOperandInLoop(MInstruction* ins, bool hasCalls) {
  // An instruction is only loop invariant if it and all of its operands can
  // be safely hoisted into the loop preheader.
  for (size_t i = 0, e = ins->numOperands(); i != e; ++i) {
    MDefinition* op = ins->getOperand(i);

    if (!IsInLoop(op)) {
      continue;
    }

    if (RequiresHoistedUse(op, hasCalls)) {
      // Recursively test for loop invariance. Note that the recursion is
      // bounded because we require RequiresHoistedUse to be set at each
      // level.
      if (!HasOperandInLoop(op->toInstruction(), hasCalls)) {
        continue;
      }
    }

    return true;
  }
  return false;
}

// Test whether the given instruction is hoistable, ignoring memory
// dependencies.
static bool IsHoistableIgnoringDependency(MInstruction* ins, bool hasCalls) {
  return ins->isMovable() && !ins->isEffectful() &&
         !HasOperandInLoop(ins, hasCalls);
}

// Test whether the given instruction has a memory dependency inside the loop.
static bool HasDependencyInLoop(MInstruction* ins, MBasicBlock* header) {
  // Don't hoist if this instruction depends on a store inside the loop.
  if (MDefinition* dep = ins->dependency()) {
    return !IsBeforeLoop(dep, header);
  }
  return false;
}

// Test whether the given instruction is hoistable.
static bool IsHoistable(MInstruction* ins, MBasicBlock* header, bool hasCalls) {
  return IsHoistableIgnoringDependency(ins, hasCalls) &&
         !HasDependencyInLoop(ins, header);
}

// In preparation for hoisting an instruction, hoist any of its operands which
// were too cheap to hoist on their own.
static void MoveDeferredOperands(MInstruction* ins, MInstruction* hoistPoint,
                                 bool hasCalls) {
  // If any of our operands were waiting for a user to be hoisted, make a note
  // to hoist them.
  for (size_t i = 0, e = ins->numOperands(); i != e; ++i) {
    MDefinition* op = ins->getOperand(i);
    if (!IsInLoop(op)) {
      continue;
    }
    MOZ_ASSERT(RequiresHoistedUse(op, hasCalls),
               "Deferred loop-invariant operand is not cheap");
    MInstruction* opIns = op->toInstruction();

    // Recursively move the operands. Note that the recursion is bounded
    // because we require RequiresHoistedUse to be set at each level.
    MoveDeferredOperands(opIns, hoistPoint, hasCalls);

#ifdef JS_JITSPEW
    JitSpew(JitSpew_LICM,
            "      Hoisting %s%u (now that a user will be hoisted)",
            opIns->opName(), opIns->id());
#endif

    opIns->block()->moveBefore(hoistPoint, opIns);
    opIns->setBailoutKind(BailoutKind::LICM);
  }
}

static void VisitLoopBlock(MBasicBlock* block, MBasicBlock* header,
                           MInstruction* hoistPoint, bool hasCalls) {
  for (auto insIter(block->begin()), insEnd(block->end()); insIter != insEnd;) {
    MInstruction* ins = *insIter++;

    if (!IsHoistable(ins, header, hasCalls)) {
#ifdef JS_JITSPEW
      if (IsHoistableIgnoringDependency(ins, hasCalls)) {
        JitSpew(JitSpew_LICM,
                "      %s%u isn't hoistable due to dependency on %s%u",
                ins->opName(), ins->id(), ins->dependency()->opName(),
                ins->dependency()->id());
      }
#endif
      continue;
    }

    // Don't hoist a cheap constant if it doesn't enable us to hoist one of
    // its uses. We want those instructions as close as possible to their
    // use, to minimize register pressure.
    if (RequiresHoistedUse(ins, hasCalls)) {
#ifdef JS_JITSPEW
      JitSpew(JitSpew_LICM, "      %s%u will be hoisted only if its users are",
              ins->opName(), ins->id());
#endif
      continue;
    }

    // Hoist operands which were too cheap to hoist on their own.
    MoveDeferredOperands(ins, hoistPoint, hasCalls);

#ifdef JS_JITSPEW
    JitSpew(JitSpew_LICM, "      Hoisting %s%u", ins->opName(), ins->id());
#endif

    // Move the instruction to the hoistPoint.
    block->moveBefore(hoistPoint, ins);
    ins->setBailoutKind(BailoutKind::LICM);
  }
}

static void VisitLoop(MIRGraph& graph, MBasicBlock* header) {
  MInstruction* hoistPoint = header->loopPredecessor()->lastIns();

#ifdef JS_JITSPEW
  JitSpew(JitSpew_LICM, "  Visiting loop with header block%u, hoisting to %s%u",
          header->id(), hoistPoint->opName(), hoistPoint->id());
#endif

  MBasicBlock* backedge = header->backedge();

  // This indicates whether the loop contains calls or other things which
  // clobber most or all floating-point registers. In such loops,
  // floating-point constants should not be hoisted unless it enables further
  // hoisting.
  bool hasCalls = LoopContainsPossibleCall(graph, header, backedge);

  for (auto i(graph.rpoBegin(header));; ++i) {
    MOZ_ASSERT(i != graph.rpoEnd(),
               "Reached end of graph searching for blocks in loop");
    MBasicBlock* block = *i;
    if (!block->isMarked()) {
      continue;
    }

#ifdef JS_JITSPEW
    JitSpew(JitSpew_LICM, "    Visiting block%u", block->id());
#endif

    VisitLoopBlock(block, header, hoistPoint, hasCalls);

    if (block == backedge) {
      break;
    }
  }
}

bool jit::LICM(const MIRGenerator* mir, MIRGraph& graph) {
  JitSpew(JitSpew_LICM, "Beginning LICM pass");

  // Iterate in RPO to visit outer loops before inner loops. We'd hoist the
  // same things either way, but outer first means we do a little less work.
  for (auto i(graph.rpoBegin()), e(graph.rpoEnd()); i != e; ++i) {
    MBasicBlock* header = *i;
    if (!header->isLoopHeader()) {
      continue;
    }

    bool canOsr;
    size_t numBlocks = MarkLoopBlocks(graph, header, &canOsr);

    if (numBlocks == 0) {
      JitSpew(JitSpew_LICM,
              "  Skipping loop with header block%u -- contains zero blocks",
              header->id());
      continue;
    }

    // There are various reasons why we might choose not to LICM a given loop:
    //
    // (a) Hoisting out of a loop that has an entry from the OSR block in
    //     addition to its normal entry is tricky.  In theory we could clone
    //     the instruction and insert phis.  In practice we don't bother.
    //
    // (b) If the loop contains a large number of blocks, we play safe and
    //     punt, in order to reduce the risk of creating excessive register
    //     pressure by hoisting lots of values out of the loop.  In a larger
    //     loop there's more likely to be duplication of invariant expressions
    //     within the loop body, and that duplication will be GVN'd but only
    //     within the scope of the loop body, so there's less loss from not
    //     lifting them out of the loop entirely.
    //
    // (c) If the loop contains a multiway switch with many successors, there
    //     could be paths with low probabilities, from which LICMing will be a
    //     net loss, especially if a large number of values are hoisted out.
    //     See bug 1708381 for a spectacular example and bug 1712078 for
    //     further discussion.
    //
    // It's preferable to perform test (c) only if (a) and (b) pass since (c)
    // is more expensive to determine -- requiring a visit to all the MIR
    // nodes -- than (a) or (b), which only involve visiting all blocks.

    bool doVisit = true;
    if (canOsr) {
      JitSpew(JitSpew_LICM, "  Skipping loop with header block%u due to OSR",
              header->id());
      doVisit = false;
    } else if (numBlocks > LargestAllowedLoop) {
      JitSpew(JitSpew_LICM,
              "  Skipping loop with header block%u "
              "due to too many blocks (%u > thresh %u)",
              header->id(), (uint32_t)numBlocks, (uint32_t)LargestAllowedLoop);
      doVisit = false;
    } else {
      size_t switchSize = 0;
      if (LoopContainsBigTableSwitch(graph, header, &switchSize)) {
        JitSpew(JitSpew_LICM,
                "  Skipping loop with header block%u "
                "due to oversize tableswitch (%u > thresh %u)",
                header->id(), (uint32_t)switchSize,
                (uint32_t)LargestAllowedTableSwitch);
        doVisit = false;
      }
    }

    if (doVisit) {
      VisitLoop(graph, header);
    }

    UnmarkLoopBlocks(graph, header);

    if (mir->shouldCancel("LICM (main loop)")) {
      return false;
    }
  }

  return true;
}

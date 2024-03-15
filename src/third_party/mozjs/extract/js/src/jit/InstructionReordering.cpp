/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/InstructionReordering.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace js::jit;

static void MoveBefore(MBasicBlock* block, MInstruction* at,
                       MInstruction* ins) {
  if (at == ins) {
    return;
  }

  // Update instruction numbers.
  for (MInstructionIterator iter(block->begin(at)); *iter != ins; iter++) {
    MOZ_ASSERT(iter->id() < ins->id());
    iter->setId(iter->id() + 1);
  }
  ins->setId(at->id() - 1);
  block->moveBefore(at, ins);
}

static bool IsLastUse(MDefinition* ins, MDefinition* input,
                      MBasicBlock* loopHeader) {
  // If we are in a loop, this cannot be the last use of any definitions from
  // outside the loop, as those definitions can be used in future iterations.
  if (loopHeader && input->block()->id() < loopHeader->id()) {
    return false;
  }
  for (MUseDefIterator iter(input); iter; iter++) {
    // Watch for uses defined in blocks which ReorderInstructions hasn't
    // processed yet. These nodes have not had their ids set yet.
    if (iter.def()->block()->id() > ins->block()->id()) {
      return false;
    }
    if (iter.def()->id() > ins->id()) {
      return false;
    }
  }
  return true;
}

static void MoveConstantsToStart(MBasicBlock* block,
                                 MInstruction* insertionPoint) {
  // Move constants with a single use in the current block to the start of the
  // block. Constants won't be reordered by ReorderInstructions, as they have no
  // inputs. Moving them up as high as possible can allow their use to be moved
  // up further, though, and has no cost if the constant is emitted at its use.

  MInstructionIterator iter(block->begin(insertionPoint));
  while (iter != block->end()) {
    MInstruction* ins = *iter;
    iter++;

    if (!ins->isConstant() || !ins->hasOneUse() ||
        ins->usesBegin()->consumer()->block() != block ||
        IsFloatingPointType(ins->type())) {
      continue;
    }

    MOZ_ASSERT(ins->isMovable());
    MOZ_ASSERT(insertionPoint != ins);

    // Note: we don't need to use MoveBefore here because MoveConstantsToStart
    // is called right before we renumber all instructions in this block.
    block->moveBefore(insertionPoint, ins);
  }
}

bool jit::ReorderInstructions(MIRGraph& graph) {
  // Renumber all instructions in the graph as we go.
  size_t nextId = 0;

  // List of the headers of any loops we are in.
  Vector<MBasicBlock*, 4, SystemAllocPolicy> loopHeaders;

  for (ReversePostorderIterator block(graph.rpoBegin());
       block != graph.rpoEnd(); block++) {
    // Don't reorder instructions within entry blocks, which have special
    // requirements.
    bool isEntryBlock =
        *block == graph.entryBlock() || *block == graph.osrBlock();

    MInstruction* insertionPoint = nullptr;
    if (!isEntryBlock) {
      // Move constants to the start of the block before renumbering all
      // instructions.
      insertionPoint = block->safeInsertTop();
      MoveConstantsToStart(*block, insertionPoint);
    }

    // Renumber all definitions inside the basic blocks.
    for (MPhiIterator iter(block->phisBegin()); iter != block->phisEnd();
         iter++) {
      iter->setId(nextId++);
    }

    for (MInstructionIterator iter(block->begin()); iter != block->end();
         iter++) {
      iter->setId(nextId++);
    }

    if (isEntryBlock) {
      continue;
    }

    if (block->isLoopHeader()) {
      if (!loopHeaders.append(*block)) {
        return false;
      }
    }

    MBasicBlock* innerLoop = loopHeaders.empty() ? nullptr : loopHeaders.back();

    MInstructionReverseIterator rtop = ++block->rbegin(insertionPoint);
    for (MInstructionIterator iter(block->begin(insertionPoint));
         iter != block->end();) {
      MInstruction* ins = *iter;

      // Filter out some instructions which are never reordered.
      if (ins->isEffectful() || !ins->isMovable() || ins->resumePoint() ||
          ins == block->lastIns()) {
        iter++;
        continue;
      }

      // Look for inputs where this instruction is the last use of that
      // input. If we move this instruction up, the input's lifetime will
      // be shortened, modulo resume point uses (which don't need to be
      // stored in a register, and can be handled by the register
      // allocator by just spilling at some point with no reload).
      Vector<MDefinition*, 4, SystemAllocPolicy> lastUsedInputs;
      for (size_t i = 0; i < ins->numOperands(); i++) {
        MDefinition* input = ins->getOperand(i);
        if (!input->isConstant() && IsLastUse(ins, input, innerLoop)) {
          if (!lastUsedInputs.append(input)) {
            return false;
          }
        }
      }

      // Don't try to move instructions which aren't the last use of any
      // of their inputs (we really ought to move these down instead).
      if (lastUsedInputs.length() < 2) {
        iter++;
        continue;
      }

      MInstruction* target = ins;
      MInstruction* postCallTarget = nullptr;
      for (MInstructionReverseIterator riter = ++block->rbegin(ins);
           riter != rtop; riter++) {
        MInstruction* prev = *riter;
        if (prev->isInterruptCheck()) {
          break;
        }
        if (prev->isSetInitializedLength()) {
          break;
        }

        // The instruction can't be moved before any of its uses.
        bool isUse = false;
        for (size_t i = 0; i < ins->numOperands(); i++) {
          if (ins->getOperand(i) == prev) {
            isUse = true;
            break;
          }
        }
        if (isUse) {
          break;
        }

        // The instruction can't be moved before an instruction that
        // stores to a location read by the instruction.
        if (prev->isEffectful() &&
            (ins->getAliasSet().flags() & prev->getAliasSet().flags()) &&
            ins->mightAlias(prev) != MDefinition::AliasType::NoAlias) {
          break;
        }

        // Make sure the instruction will still be the last use of one
        // of its inputs when moved up this far.
        for (size_t i = 0; i < lastUsedInputs.length();) {
          bool found = false;
          for (size_t j = 0; j < prev->numOperands(); j++) {
            if (prev->getOperand(j) == lastUsedInputs[i]) {
              found = true;
              break;
            }
          }
          if (found) {
            lastUsedInputs[i] = lastUsedInputs.back();
            lastUsedInputs.popBack();
          } else {
            i++;
          }
        }
        if (lastUsedInputs.length() < 2) {
          break;
        }

        // If we see a captured call result, either move the instruction before
        // the corresponding call or don't move it at all.
        if (prev->isCallResultCapture()) {
          if (!postCallTarget) {
            postCallTarget = target;
          }
        } else if (postCallTarget) {
          MOZ_ASSERT(MWasmCallBase::IsWasmCall(prev) ||
                     prev->isIonToWasmCall());
          postCallTarget = nullptr;
        }

        // We can move the instruction before this one.
        target = prev;
      }

      if (postCallTarget) {
        // We would have plonked this instruction between a call and its
        // captured return value.  Instead put it after the last corresponding
        // return value.
        target = postCallTarget;
      }

      iter++;
      MoveBefore(*block, target, ins);

      // Instruction reordering can move a bailing instruction up past a call
      // that throws an exception, causing spurious bailouts. This should rarely
      // be an issue in practice, so we only update the bailout kind if we don't
      // have anything more specific.
      if (ins->bailoutKind() == BailoutKind::TranspiledCacheIR) {
        ins->setBailoutKind(BailoutKind::InstructionReordering);
      }
    }

    if (block->isLoopBackedge()) {
      loopHeaders.popBack();
    }
  }

  return true;
}

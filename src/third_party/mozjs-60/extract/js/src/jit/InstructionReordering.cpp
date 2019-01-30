/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/InstructionReordering.h"
#include "jit/MIRGraph.h"

using namespace js;
using namespace js::jit;

static void
MoveBefore(MBasicBlock* block, MInstruction* at, MInstruction* ins)
{
    if (at == ins)
        return;

    // Update instruction numbers.
    for (MInstructionIterator iter(block->begin(at)); *iter != ins; iter++) {
        MOZ_ASSERT(iter->id() < ins->id());
        iter->setId(iter->id() + 1);
    }
    ins->setId(at->id() - 1);
    block->moveBefore(at, ins);
}

static bool
IsLastUse(MDefinition* ins, MDefinition* input, MBasicBlock* loopHeader)
{
    // If we are in a loop, this cannot be the last use of any definitions from
    // outside the loop, as those definitions can be used in future iterations.
    if (loopHeader && input->block()->id() < loopHeader->id())
        return false;
    for (MUseDefIterator iter(input); iter; iter++) {
        // Watch for uses defined in blocks which ReorderInstructions hasn't
        // processed yet. These nodes have not had their ids set yet.
        if (iter.def()->block()->id() > ins->block()->id())
            return false;
        if (iter.def()->id() > ins->id())
            return false;
    }
    return true;
}

bool
jit::ReorderInstructions(MIRGraph& graph)
{
    // Renumber all instructions in the graph as we go.
    size_t nextId = 0;

    // List of the headers of any loops we are in.
    Vector<MBasicBlock*, 4, SystemAllocPolicy> loopHeaders;

    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {
        // Renumber all definitions inside the basic blocks.
        for (MPhiIterator iter(block->phisBegin()); iter != block->phisEnd(); iter++)
            iter->setId(nextId++);

        for (MInstructionIterator iter(block->begin()); iter != block->end(); iter++)
            iter->setId(nextId++);

        // Don't reorder instructions within entry blocks, which have special requirements.
        if (*block == graph.entryBlock() || *block == graph.osrBlock())
            continue;

        if (block->isLoopHeader()) {
            if (!loopHeaders.append(*block))
                return false;
        }

        MBasicBlock* innerLoop = loopHeaders.empty() ? nullptr : loopHeaders.back();

        MInstruction* top = block->safeInsertTop();
        MInstructionReverseIterator rtop = ++block->rbegin(top);
        for (MInstructionIterator iter(block->begin(top)); iter != block->end(); ) {
            MInstruction* ins = *iter;

            // Filter out some instructions which are never reordered.
            if (ins->isEffectful() ||
                !ins->isMovable() ||
                ins->resumePoint() ||
                ins == block->lastIns())
            {
                iter++;
                continue;
            }

            // Move constants with a single use in the current block to the
            // start of the block. Constants won't be reordered by the logic
            // below, as they have no inputs. Moving them up as high as
            // possible can allow their use to be moved up further, though,
            // and has no cost if the constant is emitted at its use.
            if (ins->isConstant() &&
                ins->hasOneUse() &&
                ins->usesBegin()->consumer()->block() == *block &&
                !IsFloatingPointType(ins->type()))
            {
                iter++;
                MInstructionIterator targetIter = block->begin();
                while (targetIter->isConstant() || targetIter->isInterruptCheck()) {
                    if (*targetIter == ins)
                        break;
                    targetIter++;
                }
                MoveBefore(*block, *targetIter, ins);
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
                    if (!lastUsedInputs.append(input))
                        return false;
                }
            }

            // Don't try to move instructions which aren't the last use of any
            // of their inputs (we really ought to move these down instead).
            if (lastUsedInputs.length() < 2) {
                iter++;
                continue;
            }

            MInstruction* target = ins;
            for (MInstructionReverseIterator riter = ++block->rbegin(ins); riter != rtop; riter++) {
                MInstruction* prev = *riter;
                if (prev->isInterruptCheck())
                    break;

                // The instruction can't be moved before any of its uses.
                bool isUse = false;
                for (size_t i = 0; i < ins->numOperands(); i++) {
                    if (ins->getOperand(i) == prev) {
                        isUse = true;
                        break;
                    }
                }
                if (isUse)
                    break;

                // The instruction can't be moved before an instruction that
                // stores to a location read by the instruction.
                if (prev->isEffectful() &&
                    (ins->getAliasSet().flags() & prev->getAliasSet().flags()) &&
                    ins->mightAlias(prev) != MDefinition::AliasType::NoAlias)
                {
                    break;
                }

                // Make sure the instruction will still be the last use of one
                // of its inputs when moved up this far.
                for (size_t i = 0; i < lastUsedInputs.length(); ) {
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
                if (lastUsedInputs.length() < 2)
                    break;

                // We can move the instruction before this one.
                target = prev;
            }

            iter++;
            MoveBefore(*block, target, ins);
        }

        if (block->isLoopBackedge())
            loopHeaders.popBack();
    }

    return true;
}

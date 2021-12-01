/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/RegisterAllocator.h"

using namespace js;
using namespace js::jit;

bool
AllocationIntegrityState::record()
{
    // Ignore repeated record() calls.
    if (!instructions.empty())
        return true;

    if (!instructions.appendN(InstructionInfo(), graph.numInstructions()))
        return false;

    if (!virtualRegisters.appendN((LDefinition*)nullptr, graph.numVirtualRegisters()))
        return false;

    if (!blocks.reserve(graph.numBlocks()))
        return false;
    for (size_t i = 0; i < graph.numBlocks(); i++) {
        blocks.infallibleAppend(BlockInfo());
        LBlock* block = graph.getBlock(i);
        MOZ_ASSERT(block->mir()->id() == i);

        BlockInfo& blockInfo = blocks[i];
        if (!blockInfo.phis.reserve(block->numPhis()))
            return false;

        for (size_t j = 0; j < block->numPhis(); j++) {
            blockInfo.phis.infallibleAppend(InstructionInfo());
            InstructionInfo& info = blockInfo.phis[j];
            LPhi* phi = block->getPhi(j);
            MOZ_ASSERT(phi->numDefs() == 1);
            uint32_t vreg = phi->getDef(0)->virtualRegister();
            virtualRegisters[vreg] = phi->getDef(0);
            if (!info.outputs.append(*phi->getDef(0)))
                return false;
            for (size_t k = 0, kend = phi->numOperands(); k < kend; k++) {
                if (!info.inputs.append(*phi->getOperand(k)))
                    return false;
            }
        }

        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;
            InstructionInfo& info = instructions[ins->id()];

            for (size_t k = 0; k < ins->numTemps(); k++) {
                if (!ins->getTemp(k)->isBogusTemp()) {
                    uint32_t vreg = ins->getTemp(k)->virtualRegister();
                    virtualRegisters[vreg] = ins->getTemp(k);
                }
                if (!info.temps.append(*ins->getTemp(k)))
                    return false;
            }
            for (size_t k = 0; k < ins->numDefs(); k++) {
                if (!ins->getDef(k)->isBogusTemp()) {
                    uint32_t vreg = ins->getDef(k)->virtualRegister();
                    virtualRegisters[vreg] = ins->getDef(k);
                }
                if (!info.outputs.append(*ins->getDef(k)))
                    return false;
            }
            for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
                if (!info.inputs.append(**alloc))
                    return false;
            }
        }
    }

    return seen.init();
}

bool
AllocationIntegrityState::check(bool populateSafepoints)
{
    MOZ_ASSERT(!instructions.empty());

#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_RegAlloc))
        dump();
#endif
#ifdef DEBUG
    for (size_t blockIndex = 0; blockIndex < graph.numBlocks(); blockIndex++) {
        LBlock* block = graph.getBlock(blockIndex);

        // Check that all instruction inputs and outputs have been assigned an allocation.
        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;

            for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next())
                MOZ_ASSERT(!alloc->isUse());

            for (size_t i = 0; i < ins->numDefs(); i++) {
                LDefinition* def = ins->getDef(i);
                MOZ_ASSERT(!def->output()->isUse());

                LDefinition oldDef = instructions[ins->id()].outputs[i];
                MOZ_ASSERT_IF(oldDef.policy() == LDefinition::MUST_REUSE_INPUT,
                              *def->output() == *ins->getOperand(oldDef.getReusedInput()));
            }

            for (size_t i = 0; i < ins->numTemps(); i++) {
                LDefinition* temp = ins->getTemp(i);
                MOZ_ASSERT_IF(!temp->isBogusTemp(), temp->output()->isRegister());

                LDefinition oldTemp = instructions[ins->id()].temps[i];
                MOZ_ASSERT_IF(oldTemp.policy() == LDefinition::MUST_REUSE_INPUT,
                              *temp->output() == *ins->getOperand(oldTemp.getReusedInput()));
            }
        }
    }
#endif

    // Check that the register assignment and move groups preserve the original
    // semantics of the virtual registers. Each virtual register has a single
    // write (owing to the SSA representation), but the allocation may move the
    // written value around between registers and memory locations along
    // different paths through the script.
    //
    // For each use of an allocation, follow the physical value which is read
    // backward through the script, along all paths to the value's virtual
    // register's definition.
    for (size_t blockIndex = 0; blockIndex < graph.numBlocks(); blockIndex++) {
        LBlock* block = graph.getBlock(blockIndex);
        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;
            const InstructionInfo& info = instructions[ins->id()];

            LSafepoint* safepoint = ins->safepoint();
            if (safepoint) {
                for (size_t i = 0; i < ins->numTemps(); i++) {
                    if (ins->getTemp(i)->isBogusTemp())
                        continue;
                    uint32_t vreg = info.temps[i].virtualRegister();
                    LAllocation* alloc = ins->getTemp(i)->output();
                    if (!checkSafepointAllocation(ins, vreg, *alloc, populateSafepoints))
                        return false;
                }
                MOZ_ASSERT_IF(ins->isCall() && !populateSafepoints,
                              safepoint->liveRegs().emptyFloat() &&
                              safepoint->liveRegs().emptyGeneral());
            }

            size_t inputIndex = 0;
            for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
                LAllocation oldInput = info.inputs[inputIndex++];
                if (!oldInput.isUse())
                    continue;

                uint32_t vreg = oldInput.toUse()->virtualRegister();

                if (safepoint && !oldInput.toUse()->usedAtStart()) {
                    if (!checkSafepointAllocation(ins, vreg, **alloc, populateSafepoints))
                        return false;
                }

                // Start checking at the previous instruction, in case this
                // instruction reuses its input register for an output.
                LInstructionReverseIterator riter = block->rbegin(ins);
                riter++;
                if (!checkIntegrity(block, *riter, vreg, **alloc, populateSafepoints))
                    return false;

                while (!worklist.empty()) {
                    IntegrityItem item = worklist.popCopy();
                    if (!checkIntegrity(item.block, *item.block->rbegin(), item.vreg, item.alloc,
                                        populateSafepoints)) {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

bool
AllocationIntegrityState::checkIntegrity(LBlock* block, LInstruction* ins,
                                         uint32_t vreg, LAllocation alloc, bool populateSafepoints)
{
    for (LInstructionReverseIterator iter(block->rbegin(ins)); iter != block->rend(); iter++) {
        ins = *iter;

        // Follow values through assignments in move groups. All assignments in
        // a move group are considered to happen simultaneously, so stop after
        // the first matching move is found.
        if (ins->isMoveGroup()) {
            LMoveGroup* group = ins->toMoveGroup();
            for (int i = group->numMoves() - 1; i >= 0; i--) {
                if (group->getMove(i).to() == alloc) {
                    alloc = group->getMove(i).from();
                    break;
                }
            }
        }

        const InstructionInfo& info = instructions[ins->id()];

        // Make sure the physical location being tracked is not clobbered by
        // another instruction, and that if the originating vreg definition is
        // found that it is writing to the tracked location.

        for (size_t i = 0; i < ins->numDefs(); i++) {
            LDefinition* def = ins->getDef(i);
            if (def->isBogusTemp())
                continue;
            if (info.outputs[i].virtualRegister() == vreg) {
                MOZ_ASSERT(*def->output() == alloc);

                // Found the original definition, done scanning.
                return true;
            } else {
                MOZ_ASSERT(*def->output() != alloc);
            }
        }

        for (size_t i = 0; i < ins->numTemps(); i++) {
            LDefinition* temp = ins->getTemp(i);
            if (!temp->isBogusTemp())
                MOZ_ASSERT(*temp->output() != alloc);
        }

        if (ins->safepoint()) {
            if (!checkSafepointAllocation(ins, vreg, alloc, populateSafepoints))
                return false;
        }
    }

    // Phis are effectless, but change the vreg we are tracking. Check if there
    // is one which produced this vreg. We need to follow back through the phi
    // inputs as it is not guaranteed the register allocator filled in physical
    // allocations for the inputs and outputs of the phis.
    for (size_t i = 0; i < block->numPhis(); i++) {
        const InstructionInfo& info = blocks[block->mir()->id()].phis[i];
        LPhi* phi = block->getPhi(i);
        if (info.outputs[0].virtualRegister() == vreg) {
            for (size_t j = 0, jend = phi->numOperands(); j < jend; j++) {
                uint32_t newvreg = info.inputs[j].toUse()->virtualRegister();
                LBlock* predecessor = block->mir()->getPredecessor(j)->lir();
                if (!addPredecessor(predecessor, newvreg, alloc))
                    return false;
            }
            return true;
        }
    }

    // No phi which defined the vreg we are tracking, follow back through all
    // predecessors with the existing vreg.
    for (size_t i = 0, iend = block->mir()->numPredecessors(); i < iend; i++) {
        LBlock* predecessor = block->mir()->getPredecessor(i)->lir();
        if (!addPredecessor(predecessor, vreg, alloc))
            return false;
    }

    return true;
}

bool
AllocationIntegrityState::checkSafepointAllocation(LInstruction* ins,
                                                   uint32_t vreg, LAllocation alloc,
                                                   bool populateSafepoints)
{
    LSafepoint* safepoint = ins->safepoint();
    MOZ_ASSERT(safepoint);

    if (ins->isCall() && alloc.isRegister())
        return true;

    if (alloc.isRegister()) {
        AnyRegister reg = alloc.toRegister();
        if (populateSafepoints)
            safepoint->addLiveRegister(reg);

        MOZ_ASSERT(safepoint->liveRegs().has(reg));
    }

    // The |this| argument slot is implicitly included in all safepoints.
    if (alloc.isArgument() && alloc.toArgument()->index() < THIS_FRAME_ARGSLOT + sizeof(Value))
        return true;

    LDefinition::Type type = virtualRegisters[vreg]
                             ? virtualRegisters[vreg]->type()
                             : LDefinition::GENERAL;

    switch (type) {
      case LDefinition::OBJECT:
        if (populateSafepoints) {
            JitSpew(JitSpew_RegAlloc, "Safepoint object v%u i%u %s",
                    vreg, ins->id(), alloc.toString().get());
            if (!safepoint->addGcPointer(alloc))
                return false;
        }
        MOZ_ASSERT(safepoint->hasGcPointer(alloc));
        break;
      case LDefinition::SLOTS:
        if (populateSafepoints) {
            JitSpew(JitSpew_RegAlloc, "Safepoint slots v%u i%u %s",
                    vreg, ins->id(), alloc.toString().get());
            if (!safepoint->addSlotsOrElementsPointer(alloc))
                return false;
        }
        MOZ_ASSERT(safepoint->hasSlotsOrElementsPointer(alloc));
        break;
#ifdef JS_NUNBOX32
      // Do not assert that safepoint information for nunbox types is complete,
      // as if a vreg for a value's components are copied in multiple places
      // then the safepoint information may not reflect all copies. All copies
      // of payloads must be reflected, however, for generational GC.
      case LDefinition::TYPE:
        if (populateSafepoints) {
            JitSpew(JitSpew_RegAlloc, "Safepoint type v%u i%u %s",
                    vreg, ins->id(), alloc.toString().get());
            if (!safepoint->addNunboxType(vreg, alloc))
                return false;
        }
        break;
      case LDefinition::PAYLOAD:
        if (populateSafepoints) {
            JitSpew(JitSpew_RegAlloc, "Safepoint payload v%u i%u %s",
                    vreg, ins->id(), alloc.toString().get());
            if (!safepoint->addNunboxPayload(vreg, alloc))
                return false;
        }
        MOZ_ASSERT(safepoint->hasNunboxPayload(alloc));
        break;
#else
      case LDefinition::BOX:
        if (populateSafepoints) {
            JitSpew(JitSpew_RegAlloc, "Safepoint boxed value v%u i%u %s",
                    vreg, ins->id(), alloc.toString().get());
            if (!safepoint->addBoxedValue(alloc))
                return false;
        }
        MOZ_ASSERT(safepoint->hasBoxedValue(alloc));
        break;
#endif
      default:
        break;
    }

    return true;
}

bool
AllocationIntegrityState::addPredecessor(LBlock* block, uint32_t vreg, LAllocation alloc)
{
    // There is no need to reanalyze if we have already seen this predecessor.
    // We share the seen allocations across analysis of each use, as there will
    // likely be common ground between different uses of the same vreg.
    IntegrityItem item;
    item.block = block;
    item.vreg = vreg;
    item.alloc = alloc;
    item.index = seen.count();

    IntegrityItemSet::AddPtr p = seen.lookupForAdd(item);
    if (p)
        return true;
    if (!seen.add(p, item))
        return false;

    return worklist.append(item);
}

void
AllocationIntegrityState::dump()
{
#ifdef DEBUG
    fprintf(stderr, "Register Allocation Integrity State:\n");

    for (size_t blockIndex = 0; blockIndex < graph.numBlocks(); blockIndex++) {
        LBlock* block = graph.getBlock(blockIndex);
        MBasicBlock* mir = block->mir();

        fprintf(stderr, "\nBlock %lu", static_cast<unsigned long>(blockIndex));
        for (size_t i = 0; i < mir->numSuccessors(); i++)
            fprintf(stderr, " [successor %u]", mir->getSuccessor(i)->id());
        fprintf(stderr, "\n");

        for (size_t i = 0; i < block->numPhis(); i++) {
            const InstructionInfo& info = blocks[blockIndex].phis[i];
            LPhi* phi = block->getPhi(i);
            CodePosition input(block->getPhi(0)->id(), CodePosition::INPUT);
            CodePosition output(block->getPhi(block->numPhis() - 1)->id(), CodePosition::OUTPUT);

            fprintf(stderr, "[%u,%u Phi] [def %s] ",
                    input.bits(),
                    output.bits(),
                    phi->getDef(0)->toString().get());
            for (size_t j = 0; j < phi->numOperands(); j++)
                fprintf(stderr, " [use %s]", info.inputs[j].toString().get());
            fprintf(stderr, "\n");
        }

        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;
            const InstructionInfo& info = instructions[ins->id()];

            CodePosition input(ins->id(), CodePosition::INPUT);
            CodePosition output(ins->id(), CodePosition::OUTPUT);

            fprintf(stderr, "[");
            if (input != CodePosition::MIN)
                fprintf(stderr, "%u,%u ", input.bits(), output.bits());
            fprintf(stderr, "%s]", ins->opName());

            if (ins->isMoveGroup()) {
                LMoveGroup* group = ins->toMoveGroup();
                for (int i = group->numMoves() - 1; i >= 0; i--) {
                    fprintf(stderr, " [%s -> %s]",
                            group->getMove(i).from().toString().get(),
                            group->getMove(i).to().toString().get());
                }
                fprintf(stderr, "\n");
                continue;
            }

            for (size_t i = 0; i < ins->numDefs(); i++)
                fprintf(stderr, " [def %s]", ins->getDef(i)->toString().get());

            for (size_t i = 0; i < ins->numTemps(); i++) {
                LDefinition* temp = ins->getTemp(i);
                if (!temp->isBogusTemp())
                    fprintf(stderr, " [temp v%u %s]", info.temps[i].virtualRegister(),
                            temp->toString().get());
            }

            size_t index = 0;
            for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
                fprintf(stderr, " [use %s", info.inputs[index++].toString().get());
                if (!alloc->isConstant())
                    fprintf(stderr, " %s", alloc->toString().get());
                fprintf(stderr, "]");
            }

            fprintf(stderr, "\n");
        }
    }

    // Print discovered allocations at the ends of blocks, in the order they
    // were discovered.

    Vector<IntegrityItem, 20, SystemAllocPolicy> seenOrdered;
    if (!seenOrdered.appendN(IntegrityItem(), seen.count())) {
        fprintf(stderr, "OOM while dumping allocations\n");
        return;
    }

    for (IntegrityItemSet::Enum iter(seen); !iter.empty(); iter.popFront()) {
        IntegrityItem item = iter.front();
        seenOrdered[item.index] = item;
    }

    if (!seenOrdered.empty()) {
        fprintf(stderr, "Intermediate Allocations:\n");

        for (size_t i = 0; i < seenOrdered.length(); i++) {
            IntegrityItem item = seenOrdered[i];
            fprintf(stderr, "  block %u reg v%u alloc %s\n",
                    item.block->mir()->id(), item.vreg, item.alloc.toString().get());
        }
    }

    fprintf(stderr, "\n");
#endif
}

const CodePosition CodePosition::MAX(UINT_MAX);
const CodePosition CodePosition::MIN(0);

bool
RegisterAllocator::init()
{
    if (!insData.init(mir, graph.numInstructions()))
        return false;

    if (!entryPositions.reserve(graph.numBlocks()) || !exitPositions.reserve(graph.numBlocks()))
        return false;

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        LBlock* block = graph.getBlock(i);
        for (LInstructionIterator ins = block->begin(); ins != block->end(); ins++)
            insData[ins->id()] = *ins;
        for (size_t j = 0; j < block->numPhis(); j++) {
            LPhi* phi = block->getPhi(j);
            insData[phi->id()] = phi;
        }

        CodePosition entry = block->numPhis() != 0
                             ? CodePosition(block->getPhi(0)->id(), CodePosition::INPUT)
                             : inputOf(block->firstInstructionWithId());
        CodePosition exit = outputOf(block->lastInstructionWithId());

        MOZ_ASSERT(block->mir()->id() == i);
        entryPositions.infallibleAppend(entry);
        exitPositions.infallibleAppend(exit);
    }

    return true;
}

LMoveGroup*
RegisterAllocator::getInputMoveGroup(LInstruction* ins)
{
    MOZ_ASSERT(!ins->fixReuseMoves());
    if (ins->inputMoves())
        return ins->inputMoves();

    LMoveGroup* moves = LMoveGroup::New(alloc());
    ins->setInputMoves(moves);
    ins->block()->insertBefore(ins, moves);
    return moves;
}

LMoveGroup*
RegisterAllocator::getFixReuseMoveGroup(LInstruction* ins)
{
    if (ins->fixReuseMoves())
        return ins->fixReuseMoves();

    LMoveGroup* moves = LMoveGroup::New(alloc());
    ins->setFixReuseMoves(moves);
    ins->block()->insertBefore(ins, moves);
    return moves;
}

LMoveGroup*
RegisterAllocator::getMoveGroupAfter(LInstruction* ins)
{
    if (ins->movesAfter())
        return ins->movesAfter();

    LMoveGroup* moves = LMoveGroup::New(alloc());
    ins->setMovesAfter(moves);

    ins->block()->insertAfter(ins, moves);
    return moves;
}

void
RegisterAllocator::dumpInstructions()
{
#ifdef JS_JITSPEW
    fprintf(stderr, "Instructions:\n");

    for (size_t blockIndex = 0; blockIndex < graph.numBlocks(); blockIndex++) {
        LBlock* block = graph.getBlock(blockIndex);
        MBasicBlock* mir = block->mir();

        fprintf(stderr, "\nBlock %lu", static_cast<unsigned long>(blockIndex));
        for (size_t i = 0; i < mir->numSuccessors(); i++)
            fprintf(stderr, " [successor %u]", mir->getSuccessor(i)->id());
        fprintf(stderr, "\n");

        for (size_t i = 0; i < block->numPhis(); i++) {
            LPhi* phi = block->getPhi(i);

            fprintf(stderr, "[%u,%u Phi] [def %s]",
                    inputOf(phi).bits(),
                    outputOf(phi).bits(),
                    phi->getDef(0)->toString().get());
            for (size_t j = 0; j < phi->numOperands(); j++)
                fprintf(stderr, " [use %s]", phi->getOperand(j)->toString().get());
            fprintf(stderr, "\n");
        }

        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;

            fprintf(stderr, "[");
            if (ins->id() != 0)
                fprintf(stderr, "%u,%u ", inputOf(ins).bits(), outputOf(ins).bits());
            fprintf(stderr, "%s]", ins->opName());

            if (ins->isMoveGroup()) {
                LMoveGroup* group = ins->toMoveGroup();
                for (int i = group->numMoves() - 1; i >= 0; i--) {
                    // Use two printfs, as LAllocation::toString is not reentant.
                    fprintf(stderr, " [%s", group->getMove(i).from().toString().get());
                    fprintf(stderr, " -> %s]", group->getMove(i).to().toString().get());
                }
                fprintf(stderr, "\n");
                continue;
            }

            for (size_t i = 0; i < ins->numDefs(); i++)
                fprintf(stderr, " [def %s]", ins->getDef(i)->toString().get());

            for (size_t i = 0; i < ins->numTemps(); i++) {
                LDefinition* temp = ins->getTemp(i);
                if (!temp->isBogusTemp())
                    fprintf(stderr, " [temp %s]", temp->toString().get());
            }

            for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
                if (!alloc->isBogus())
                    fprintf(stderr, " [use %s]", alloc->toString().get());
            }

            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "\n");
#endif // JS_JITSPEW
}

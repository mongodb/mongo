/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/StupidAllocator.h"

#include "jstypes.h"

using namespace js;
using namespace js::jit;

static inline uint32_t
DefaultStackSlot(uint32_t vreg)
{
    // On x86/x64, we have to keep the stack aligned on 16 bytes for spilling
    // SIMD registers.  To avoid complexity in this stupid allocator, we just
    // allocate 16 bytes stack slot for all vreg.
    return vreg * 2 * sizeof(Value);
}

LAllocation*
StupidAllocator::stackLocation(uint32_t vreg)
{
    LDefinition* def = virtualRegisters[vreg];
    if (def->policy() == LDefinition::FIXED && def->output()->isArgument())
        return def->output();

    return new(alloc()) LStackSlot(DefaultStackSlot(vreg));
}

StupidAllocator::RegisterIndex
StupidAllocator::registerIndex(AnyRegister reg)
{
    for (size_t i = 0; i < registerCount; i++) {
        if (reg == registers[i].reg)
            return i;
    }
    MOZ_CRASH("Bad register");
}

bool
StupidAllocator::init()
{
    if (!RegisterAllocator::init())
        return false;

    if (!virtualRegisters.appendN((LDefinition*)nullptr, graph.numVirtualRegisters()))
        return false;

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        LBlock* block = graph.getBlock(i);
        for (LInstructionIterator ins = block->begin(); ins != block->end(); ins++) {
            for (size_t j = 0; j < ins->numDefs(); j++) {
                LDefinition* def = ins->getDef(j);
                virtualRegisters[def->virtualRegister()] = def;
            }

            for (size_t j = 0; j < ins->numTemps(); j++) {
                LDefinition* def = ins->getTemp(j);
                if (def->isBogusTemp())
                    continue;
                virtualRegisters[def->virtualRegister()] = def;
            }
        }
        for (size_t j = 0; j < block->numPhis(); j++) {
            LPhi* phi = block->getPhi(j);
            LDefinition* def = phi->getDef(0);
            uint32_t vreg = def->virtualRegister();

            virtualRegisters[vreg] = def;
        }
    }

    // Assign physical registers to the tracked allocation.
    {
        registerCount = 0;
        LiveRegisterSet remainingRegisters(allRegisters_.asLiveSet());
        while (!remainingRegisters.emptyGeneral())
            registers[registerCount++].reg = AnyRegister(remainingRegisters.takeAnyGeneral());

        while (!remainingRegisters.emptyFloat())
            registers[registerCount++].reg =
                AnyRegister(remainingRegisters.takeAnyFloat<RegTypeName::Any>());

        MOZ_ASSERT(registerCount <= MAX_REGISTERS);
    }

    return true;
}

bool
StupidAllocator::allocationRequiresRegister(const LAllocation* alloc, AnyRegister reg)
{
    if (alloc->isRegister() && alloc->toRegister() == reg)
        return true;
    if (alloc->isUse()) {
        const LUse* use = alloc->toUse();
        if (use->policy() == LUse::FIXED) {
            AnyRegister usedReg = GetFixedRegister(virtualRegisters[use->virtualRegister()], use);
            if (usedReg.aliases(reg))
                return true;
        }
    }
    return false;
}

bool
StupidAllocator::registerIsReserved(LInstruction* ins, AnyRegister reg)
{
    // Whether reg is already reserved for an input or output of ins.
    for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
        if (allocationRequiresRegister(*alloc, reg))
            return true;
    }
    for (size_t i = 0; i < ins->numTemps(); i++) {
        if (allocationRequiresRegister(ins->getTemp(i)->output(), reg))
            return true;
    }
    for (size_t i = 0; i < ins->numDefs(); i++) {
        if (allocationRequiresRegister(ins->getDef(i)->output(), reg))
            return true;
    }
    return false;
}

AnyRegister
StupidAllocator::ensureHasRegister(LInstruction* ins, uint32_t vreg)
{
    // Ensure that vreg is held in a register before ins.

    // Check if the virtual register is already held in a physical register.
    RegisterIndex existing = findExistingRegister(vreg);
    if (existing != UINT32_MAX) {
        if (registerIsReserved(ins, registers[existing].reg)) {
            evictAliasedRegister(ins, existing);
        } else {
            registers[existing].age = ins->id();
            return registers[existing].reg;
        }
    }

    RegisterIndex best = allocateRegister(ins, vreg);
    loadRegister(ins, vreg, best, virtualRegisters[vreg]->type());

    return registers[best].reg;
}

StupidAllocator::RegisterIndex
StupidAllocator::allocateRegister(LInstruction* ins, uint32_t vreg)
{
    // Pick a register for vreg, evicting an existing register if necessary.
    // Spill code will be placed before ins, and no existing allocated input
    // for ins will be touched.
    MOZ_ASSERT(ins);

    LDefinition* def = virtualRegisters[vreg];
    MOZ_ASSERT(def);

    RegisterIndex best = UINT32_MAX;

    for (size_t i = 0; i < registerCount; i++) {
        AnyRegister reg = registers[i].reg;

        if (!def->isCompatibleReg(reg))
            continue;

        // Skip the register if it is in use for an allocated input or output.
        if (registerIsReserved(ins, reg))
            continue;

        if (registers[i].vreg == MISSING_ALLOCATION ||
            best == UINT32_MAX ||
            registers[best].age > registers[i].age)
        {
            best = i;
        }
    }

    evictAliasedRegister(ins, best);
    return best;
}

void
StupidAllocator::syncRegister(LInstruction* ins, RegisterIndex index)
{
    if (registers[index].dirty) {
        LMoveGroup* input = getInputMoveGroup(ins);
        LAllocation source(registers[index].reg);

        uint32_t existing = registers[index].vreg;
        LAllocation* dest = stackLocation(existing);
        input->addAfter(source, *dest, registers[index].type);

        registers[index].dirty = false;
    }
}

void
StupidAllocator::evictRegister(LInstruction* ins, RegisterIndex index)
{
    syncRegister(ins, index);
    registers[index].set(MISSING_ALLOCATION);
}

void
StupidAllocator::evictAliasedRegister(LInstruction* ins, RegisterIndex index)
{
    for (size_t i = 0; i < registers[index].reg.numAliased(); i++) {
        uint32_t aindex = registerIndex(registers[index].reg.aliased(i));
        syncRegister(ins, aindex);
        registers[aindex].set(MISSING_ALLOCATION);
    }
}

void
StupidAllocator::loadRegister(LInstruction* ins, uint32_t vreg, RegisterIndex index, LDefinition::Type type)
{
    // Load a vreg from its stack location to a register.
    LMoveGroup* input = getInputMoveGroup(ins);
    LAllocation* source = stackLocation(vreg);
    LAllocation dest(registers[index].reg);
    input->addAfter(*source, dest, type);
    registers[index].set(vreg, ins);
    registers[index].type = type;
}

StupidAllocator::RegisterIndex
StupidAllocator::findExistingRegister(uint32_t vreg)
{
    for (size_t i = 0; i < registerCount; i++) {
        if (registers[i].vreg == vreg)
            return i;
    }
    return UINT32_MAX;
}

bool
StupidAllocator::go()
{
    // This register allocator is intended to be as simple as possible, while
    // still being complicated enough to share properties with more complicated
    // allocators. Namely, physical registers may be used to carry virtual
    // registers across LIR instructions, but not across basic blocks.
    //
    // This algorithm does not pay any attention to liveness. It is performed
    // as a single forward pass through the basic blocks in the program. As
    // virtual registers and temporaries are defined they are assigned physical
    // registers, evicting existing allocations in an LRU fashion.

    // For virtual registers not carried in a register, a canonical spill
    // location is used. Each vreg has a different spill location; since we do
    // not track liveness we cannot determine that two vregs have disjoint
    // lifetimes. Thus, the maximum stack height is the number of vregs (scaled
    // by two on 32 bit platforms to allow storing double values).
    graph.setLocalSlotCount(DefaultStackSlot(graph.numVirtualRegisters()));

    if (!init())
        return false;

    for (size_t blockIndex = 0; blockIndex < graph.numBlocks(); blockIndex++) {
        LBlock* block = graph.getBlock(blockIndex);
        MOZ_ASSERT(block->mir()->id() == blockIndex);

        for (size_t i = 0; i < registerCount; i++)
            registers[i].set(MISSING_ALLOCATION);

        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction* ins = *iter;

            if (ins == *block->rbegin())
                syncForBlockEnd(block, ins);

            allocateForInstruction(ins);
        }
    }

    return true;
}

void
StupidAllocator::syncForBlockEnd(LBlock* block, LInstruction* ins)
{
    // Sync any dirty registers, and update the synced state for phi nodes at
    // each successor of a block. We cannot conflate the storage for phis with
    // that of their inputs, as we cannot prove the live ranges of the phi and
    // its input do not overlap. The values for the two may additionally be
    // different, as the phi could be for the value of the input in a previous
    // loop iteration.

    for (size_t i = 0; i < registerCount; i++)
        syncRegister(ins, i);

    LMoveGroup* group = nullptr;

    MBasicBlock* successor = block->mir()->successorWithPhis();
    if (successor) {
        uint32_t position = block->mir()->positionInPhiSuccessor();
        LBlock* lirsuccessor = successor->lir();
        for (size_t i = 0; i < lirsuccessor->numPhis(); i++) {
            LPhi* phi = lirsuccessor->getPhi(i);

            uint32_t sourcevreg = phi->getOperand(position)->toUse()->virtualRegister();
            uint32_t destvreg = phi->getDef(0)->virtualRegister();

            if (sourcevreg == destvreg)
                continue;

            LAllocation* source = stackLocation(sourcevreg);
            LAllocation* dest = stackLocation(destvreg);

            if (!group) {
                // The moves we insert here need to happen simultaneously with
                // each other, yet after any existing moves before the instruction.
                LMoveGroup* input = getInputMoveGroup(ins);
                if (input->numMoves() == 0) {
                    group = input;
                } else {
                    group = LMoveGroup::New(alloc());
                    block->insertAfter(input, group);
                }
            }

            group->add(*source, *dest, phi->getDef(0)->type());
        }
    }
}

void
StupidAllocator::allocateForInstruction(LInstruction* ins)
{
    // Sync all registers before making a call.
    if (ins->isCall()) {
        for (size_t i = 0; i < registerCount; i++)
            syncRegister(ins, i);
    }

    // Allocate for inputs which are required to be in registers.
    for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
        if (!alloc->isUse())
            continue;
        LUse* use = alloc->toUse();
        uint32_t vreg = use->virtualRegister();
        if (use->policy() == LUse::REGISTER) {
            AnyRegister reg = ensureHasRegister(ins, vreg);
            alloc.replace(LAllocation(reg));
        } else if (use->policy() == LUse::FIXED) {
            AnyRegister reg = GetFixedRegister(virtualRegisters[vreg], use);
            RegisterIndex index = registerIndex(reg);
            if (registers[index].vreg != vreg) {
                // Need to evict multiple registers
                evictAliasedRegister(ins, registerIndex(reg));
                // If this vreg is already assigned to an incorrect register
                RegisterIndex existing = findExistingRegister(vreg);
                if (existing != UINT32_MAX)
                    evictRegister(ins, existing);
                loadRegister(ins, vreg, index, virtualRegisters[vreg]->type());
            }
            alloc.replace(LAllocation(reg));
        } else {
            // Inputs which are not required to be in a register are not
            // allocated until after temps/definitions, as the latter may need
            // to evict registers which hold these inputs.
        }
    }

    // Find registers to hold all temporaries and outputs of the instruction.
    for (size_t i = 0; i < ins->numTemps(); i++) {
        LDefinition* def = ins->getTemp(i);
        if (!def->isBogusTemp())
            allocateForDefinition(ins, def);
    }
    for (size_t i = 0; i < ins->numDefs(); i++) {
        LDefinition* def = ins->getDef(i);
        allocateForDefinition(ins, def);
    }

    // Allocate for remaining inputs which do not need to be in registers.
    for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
        if (!alloc->isUse())
            continue;
        LUse* use = alloc->toUse();
        uint32_t vreg = use->virtualRegister();
        MOZ_ASSERT(use->policy() != LUse::REGISTER && use->policy() != LUse::FIXED);

        RegisterIndex index = findExistingRegister(vreg);
        if (index == UINT32_MAX) {
            LAllocation* stack = stackLocation(use->virtualRegister());
            alloc.replace(*stack);
        } else {
            registers[index].age = ins->id();
            alloc.replace(LAllocation(registers[index].reg));
        }
    }

    // If this is a call, evict all registers except for those holding outputs.
    if (ins->isCall()) {
        for (size_t i = 0; i < registerCount; i++) {
            if (!registers[i].dirty)
                registers[i].set(MISSING_ALLOCATION);
        }
    }
}

void
StupidAllocator::allocateForDefinition(LInstruction* ins, LDefinition* def)
{
    uint32_t vreg = def->virtualRegister();

    if ((def->output()->isRegister() && def->policy() == LDefinition::FIXED) ||
        def->policy() == LDefinition::MUST_REUSE_INPUT)
    {
        // Result will be in a specific register, spill any vreg held in
        // that register before the instruction.
        RegisterIndex index =
            registerIndex(def->policy() == LDefinition::FIXED
                          ? def->output()->toRegister()
                          : ins->getOperand(def->getReusedInput())->toRegister());
        evictRegister(ins, index);
        registers[index].set(vreg, ins, true);
        registers[index].type = virtualRegisters[vreg]->type();
        def->setOutput(LAllocation(registers[index].reg));
    } else if (def->policy() == LDefinition::FIXED) {
        // The result must be a stack location.
        def->setOutput(*stackLocation(vreg));
    } else {
        // Find a register to hold the result of the instruction.
        RegisterIndex best = allocateRegister(ins, vreg);
        registers[best].set(vreg, ins, true);
        registers[best].type = virtualRegisters[vreg]->type();
        def->setOutput(LAllocation(registers[best].reg));
    }
}

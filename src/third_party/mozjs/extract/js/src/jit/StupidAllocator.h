/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_StupidAllocator_h
#define jit_StupidAllocator_h

#include "jit/RegisterAllocator.h"

// Simple register allocator that only carries registers within basic blocks.

namespace js {
namespace jit {

class StupidAllocator : public RegisterAllocator
{
    static const uint32_t MAX_REGISTERS = AnyRegister::Total;
    static const uint32_t MISSING_ALLOCATION = UINT32_MAX;

    struct AllocatedRegister {
        AnyRegister reg;

        // The type of the value in the register.
        LDefinition::Type type;

        // Virtual register this physical reg backs, or MISSING_ALLOCATION.
        uint32_t vreg;

        // id of the instruction which most recently used this register.
        uint32_t age;

        // Whether the physical register is not synced with the backing stack slot.
        bool dirty;

        void set(uint32_t vreg, LInstruction* ins = nullptr, bool dirty = false) {
            this->vreg = vreg;
            this->age = ins ? ins->id() : 0;
            this->dirty = dirty;
        }
    };

    // Active allocation for the current code position.
    mozilla::Array<AllocatedRegister, MAX_REGISTERS> registers;
    uint32_t registerCount;

    // Type indicating an index into registers.
    typedef uint32_t RegisterIndex;

    // Information about each virtual register.
    Vector<LDefinition*, 0, SystemAllocPolicy> virtualRegisters;

  public:
    StupidAllocator(MIRGenerator* mir, LIRGenerator* lir, LIRGraph& graph)
      : RegisterAllocator(mir, lir, graph)
    {
    }

    MOZ_MUST_USE bool go();

  private:
    MOZ_MUST_USE bool init();

    void syncForBlockEnd(LBlock* block, LInstruction* ins);
    void allocateForInstruction(LInstruction* ins);
    void allocateForDefinition(LInstruction* ins, LDefinition* def);

    LAllocation* stackLocation(uint32_t vreg);

    RegisterIndex registerIndex(AnyRegister reg);

    AnyRegister ensureHasRegister(LInstruction* ins, uint32_t vreg);
    RegisterIndex allocateRegister(LInstruction* ins, uint32_t vreg);

    void syncRegister(LInstruction* ins, RegisterIndex index);
    void evictRegister(LInstruction* ins, RegisterIndex index);
    void evictAliasedRegister(LInstruction* ins, RegisterIndex index);
    void loadRegister(LInstruction* ins, uint32_t vreg, RegisterIndex index, LDefinition::Type type);

    RegisterIndex findExistingRegister(uint32_t vreg);

    bool allocationRequiresRegister(const LAllocation* alloc, AnyRegister reg);
    bool registerIsReserved(LInstruction* ins, AnyRegister reg);
};

} // namespace jit
} // namespace js

#endif /* jit_StupidAllocator_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_Lowering_mips32_h
#define jit_mips32_Lowering_mips32_h

#include "jit/mips-shared/Lowering-mips-shared.h"

namespace js {
namespace jit {

class LIRGeneratorMIPS : public LIRGeneratorMIPSShared
{
  protected:
    LIRGeneratorMIPS(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorMIPSShared(gen, graph, lirGraph)
    { }

  protected:
    // Adds a box input to an instruction, setting operand |n| to the type and
    // |n+1| to the payload.
    void useBoxFixed(LInstruction* lir, size_t n, MDefinition* mir, Register reg1, Register reg2,
                     bool useAtStart = false);

    inline LDefinition tempToUnbox() {
        return LDefinition::BogusTemp();
    }

    void lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex);
    void defineUntypedPhi(MPhi* phi, size_t lirIndex);

    void lowerTruncateDToInt32(MTruncateToInt32* ins);
    void lowerTruncateFToInt32(MTruncateToInt32* ins);

  public:
    void visitBox(MBox* box);
    void visitUnbox(MUnbox* unbox);
    void visitReturn(MReturn* ret);
    void visitRandom(MRandom* ins);
};

typedef LIRGeneratorMIPS LIRGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_mips32_Lowering_mips32_h */

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
    // Returns a box allocation with type set to reg1 and payload set to reg2.
    LBoxAllocation useBoxFixed(MDefinition* mir, Register reg1, Register reg2,
                               bool useAtStart = false);

    inline LDefinition tempToUnbox() {
        return LDefinition::BogusTemp();
    }

    void lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex);
    void defineUntypedPhi(MPhi* phi, size_t lirIndex);

    void lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex);
    void defineInt64Phi(MPhi* phi, size_t lirIndex);

    void lowerTruncateDToInt32(MTruncateToInt32* ins);
    void lowerTruncateFToInt32(MTruncateToInt32* ins);

    void lowerDivI64(MDiv* div);
    void lowerModI64(MMod* mod);
    void lowerUDivI64(MDiv* div);
    void lowerUModI64(MMod* mod);

  public:
    void visitBox(MBox* box);
    void visitUnbox(MUnbox* unbox);
    void visitReturn(MReturn* ret);
    void visitRandom(MRandom* ins);
    void visitWasmTruncateToInt64(MWasmTruncateToInt64* ins);
    void visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins);
};

typedef LIRGeneratorMIPS LIRGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_mips32_Lowering_mips32_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_Lowering_arm_h
#define jit_arm_Lowering_arm_h

#include "jit/shared/Lowering-shared.h"

namespace js {
namespace jit {

class LIRGeneratorARM : public LIRGeneratorShared
{
  public:
    LIRGeneratorARM(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorShared(gen, graph, lirGraph)
    { }

  protected:
    // Adds a box input to an instruction, setting operand |n| to the type and
    // |n+1| to the payload.
    void useBoxFixed(LInstruction* lir, size_t n, MDefinition* mir, Register reg1, Register reg2,
                     bool useAtStart = false);

    // x86 has constraints on what registers can be formatted for 1-byte
    // stores and loads; on ARM all registers are okay.
    LAllocation useByteOpRegister(MDefinition* mir);
    LAllocation useByteOpRegisterOrNonDoubleConstant(MDefinition* mir);
    LDefinition tempByteOpRegister();

    inline LDefinition tempToUnbox() {
        return LDefinition::BogusTemp();
    }

    bool needTempForPostBarrier() { return false; }

    void lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex);
    void defineUntypedPhi(MPhi* phi, size_t lirIndex);
    void lowerForShift(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir, MDefinition* lhs,
                       MDefinition* rhs);
    void lowerUrshD(MUrsh* mir);

    void lowerForALU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                     MDefinition* input);
    void lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                     MDefinition* lhs, MDefinition* rhs);

    void lowerForFPU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                     MDefinition* src);
    template<size_t Temps>
    void lowerForFPU(LInstructionHelper<1, 2, Temps>* ins, MDefinition* mir,
                     MDefinition* lhs, MDefinition* rhs);

    void lowerForCompIx4(LSimdBinaryCompIx4* ins, MSimdBinaryComp* mir,
                         MDefinition* lhs, MDefinition* rhs)
    {
        return lowerForFPU(ins, mir, lhs, rhs);
    }
    void lowerForCompFx4(LSimdBinaryCompFx4* ins, MSimdBinaryComp* mir,
                         MDefinition* lhs, MDefinition* rhs)
    {
        return lowerForFPU(ins, mir, lhs, rhs);
    }

    void lowerForBitAndAndBranch(LBitAndAndBranch* baab, MInstruction* mir,
                                 MDefinition* lhs, MDefinition* rhs);
    void lowerTruncateDToInt32(MTruncateToInt32* ins);
    void lowerTruncateFToInt32(MTruncateToInt32* ins);
    void lowerDivI(MDiv* div);
    void lowerModI(MMod* mod);
    void lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs);
    void lowerUDiv(MDiv* div);
    void lowerUMod(MMod* mod);
    void visitPowHalf(MPowHalf* ins);
    void visitAsmJSNeg(MAsmJSNeg* ins);

    LTableSwitch* newLTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
                                  MTableSwitch* ins);
    LTableSwitchV* newLTableSwitchV(MTableSwitch* ins);

  public:
    void visitBox(MBox* box);
    void visitUnbox(MUnbox* unbox);
    void visitReturn(MReturn* ret);
    void lowerPhi(MPhi* phi);
    void visitGuardShape(MGuardShape* ins);
    void visitGuardObjectGroup(MGuardObjectGroup* ins);
    void visitAsmJSUnsignedToDouble(MAsmJSUnsignedToDouble* ins);
    void visitAsmJSUnsignedToFloat32(MAsmJSUnsignedToFloat32* ins);
    void visitAsmJSLoadHeap(MAsmJSLoadHeap* ins);
    void visitAsmJSStoreHeap(MAsmJSStoreHeap* ins);
    void visitAsmJSLoadFuncPtr(MAsmJSLoadFuncPtr* ins);
    void visitAsmJSCompareExchangeHeap(MAsmJSCompareExchangeHeap* ins);
    void visitAsmJSAtomicExchangeHeap(MAsmJSAtomicExchangeHeap* ins);
    void visitAsmJSAtomicBinopHeap(MAsmJSAtomicBinopHeap* ins);
    void visitStoreTypedArrayElementStatic(MStoreTypedArrayElementStatic* ins);
    void visitSimdBinaryArith(MSimdBinaryArith* ins);
    void visitSimdSelect(MSimdSelect* ins);
    void visitSimdSplatX4(MSimdSplatX4* ins);
    void visitSimdValueX4(MSimdValueX4* ins);
    void visitCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins);
    void visitAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins);
    void visitAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins);
    void visitSubstr(MSubstr* ins);
    void visitRandom(MRandom* ins);
};

typedef LIRGeneratorARM LIRGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_arm_Lowering_arm_h */

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
    // Returns a box allocation with type set to reg1 and payload set to reg2.
    LBoxAllocation useBoxFixed(MDefinition* mir, Register reg1, Register reg2,
                               bool useAtStart = false);

    // x86 has constraints on what registers can be formatted for 1-byte
    // stores and loads; on ARM all registers are okay.
    LAllocation useByteOpRegister(MDefinition* mir);
    LAllocation useByteOpRegisterAtStart(MDefinition* mir);
    LAllocation useByteOpRegisterOrNonDoubleConstant(MDefinition* mir);
    LDefinition tempByteOpRegister();

    inline LDefinition tempToUnbox() {
        return LDefinition::BogusTemp();
    }

    bool needTempForPostBarrier() { return false; }

    void lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex);
    void defineUntypedPhi(MPhi* phi, size_t lirIndex);
    void lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex);
    void defineInt64Phi(MPhi* phi, size_t lirIndex);

    void lowerForShift(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir, MDefinition* lhs,
                       MDefinition* rhs);
    void lowerUrshD(MUrsh* mir);

    void lowerForALU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                     MDefinition* input);
    void lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                     MDefinition* lhs, MDefinition* rhs);

    void lowerForALUInt64(LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
                          MDefinition* mir, MDefinition* lhs, MDefinition* rhs);
    void lowerForMulInt64(LMulI64* ins, MMul* mir, MDefinition* lhs, MDefinition* rhs);
    template<size_t Temps>
    void lowerForShiftInt64(LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, Temps>* ins,
                            MDefinition* mir, MDefinition* lhs, MDefinition* rhs);

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
    void lowerDivI64(MDiv* div);
    void lowerModI64(MMod* mod);
    void lowerUDivI64(MDiv* div);
    void lowerUModI64(MMod* mod);
    void lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs);
    void lowerUDiv(MDiv* div);
    void lowerUMod(MMod* mod);
    void visitPowHalf(MPowHalf* ins) override;
    void visitWasmNeg(MWasmNeg* ins) override;

    LTableSwitch* newLTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
                                  MTableSwitch* ins);
    LTableSwitchV* newLTableSwitchV(MTableSwitch* ins);

  public:
    void visitBox(MBox* box) override;
    void visitUnbox(MUnbox* unbox) override;
    void visitReturn(MReturn* ret) override;
    void lowerPhi(MPhi* phi);
    void visitWasmSelect(MWasmSelect* ins) override;
    void visitWasmUnsignedToDouble(MWasmUnsignedToDouble* ins) override;
    void visitWasmUnsignedToFloat32(MWasmUnsignedToFloat32* ins) override;
    void visitWasmLoad(MWasmLoad* ins) override;
    void visitWasmStore(MWasmStore* ins) override;
    void visitAsmJSLoadHeap(MAsmJSLoadHeap* ins) override;
    void visitAsmJSStoreHeap(MAsmJSStoreHeap* ins) override;
    void visitWasmCompareExchangeHeap(MWasmCompareExchangeHeap* ins) override;
    void visitWasmAtomicExchangeHeap(MWasmAtomicExchangeHeap* ins) override;
    void visitWasmAtomicBinopHeap(MWasmAtomicBinopHeap* ins) override;
    void visitCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins) override;
    void visitAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins) override;
    void visitAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins) override;
    void visitSubstr(MSubstr* ins) override;
    void visitRandom(MRandom* ins) override;
    void visitWasmTruncateToInt64(MWasmTruncateToInt64* ins) override;
    void visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins) override;
    void visitCopySign(MCopySign* ins) override;
    void visitExtendInt32ToInt64(MExtendInt32ToInt64* ins) override;
    void visitSignExtendInt64(MSignExtendInt64* ins) override;
};

typedef LIRGeneratorARM LIRGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_arm_Lowering_arm_h */

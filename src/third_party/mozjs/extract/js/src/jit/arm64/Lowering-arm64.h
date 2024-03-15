/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_Lowering_arm64_h
#define jit_arm64_Lowering_arm64_h

#include "jit/shared/Lowering-shared.h"

namespace js {
namespace jit {

class LIRGeneratorARM64 : public LIRGeneratorShared {
 protected:
  LIRGeneratorARM64(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorShared(gen, graph, lirGraph) {}

  // Returns a box allocation. reg2 is ignored on 64-bit platforms.
  LBoxAllocation useBoxFixed(MDefinition* mir, Register reg1, Register reg2,
                             bool useAtStart = false);

  LAllocation useByteOpRegister(MDefinition* mir);
  LAllocation useByteOpRegisterAtStart(MDefinition* mir);
  LAllocation useByteOpRegisterOrNonDoubleConstant(MDefinition* mir);
  LDefinition tempByteOpRegister();

  LDefinition tempToUnbox();

  bool needTempForPostBarrier() { return true; }

  // ARM64 has a scratch register, so no need for another temp for dispatch ICs.
  LDefinition tempForDispatchCache(MIRType outputType = MIRType::None) {
    return LDefinition::BogusTemp();
  }

  void lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block,
                            size_t lirIndex);
  void lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block,
                          size_t lirIndex) {
    lowerTypedPhiInput(phi, inputPosition, block, lirIndex);
  }
  void defineInt64Phi(MPhi* phi, size_t lirIndex) {
    defineTypedPhi(phi, lirIndex);
  }
  void lowerForShift(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                     MDefinition* lhs, MDefinition* rhs);
  void lowerUrshD(MUrsh* mir);

  void lowerPowOfTwoI(MPow* mir);

  void lowerForALU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                   MDefinition* input);
  void lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                   MDefinition* lhs, MDefinition* rhs);

  void lowerForALUInt64(LInstructionHelper<INT64_PIECES, INT64_PIECES, 0>* ins,
                        MDefinition* mir, MDefinition* input);
  void lowerForALUInt64(
      LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
      MDefinition* mir, MDefinition* lhs, MDefinition* rhs);
  void lowerForMulInt64(LMulI64* ins, MMul* mir, MDefinition* lhs,
                        MDefinition* rhs);
  template <size_t Temps>
  void lowerForShiftInt64(
      LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, Temps>* ins,
      MDefinition* mir, MDefinition* lhs, MDefinition* rhs);

  void lowerForCompareI64AndBranch(MTest* mir, MCompare* comp, JSOp op,
                                   MDefinition* left, MDefinition* right,
                                   MBasicBlock* ifTrue, MBasicBlock* ifFalse);

  void lowerForFPU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                   MDefinition* input);

  template <size_t Temps>
  void lowerForFPU(LInstructionHelper<1, 2, Temps>* ins, MDefinition* mir,
                   MDefinition* lhs, MDefinition* rhs);

  void lowerBuiltinInt64ToFloatingPoint(MBuiltinInt64ToFloatingPoint* ins);
  void lowerWasmBuiltinTruncateToInt64(MWasmBuiltinTruncateToInt64* ins);
  void lowerForBitAndAndBranch(LBitAndAndBranch* baab, MInstruction* mir,
                               MDefinition* lhs, MDefinition* rhs);
  void lowerWasmBuiltinTruncateToInt32(MWasmBuiltinTruncateToInt32* ins);
  void lowerTruncateDToInt32(MTruncateToInt32* ins);
  void lowerTruncateFToInt32(MTruncateToInt32* ins);
  void lowerDivI(MDiv* div);
  void lowerModI(MMod* mod);
  void lowerDivI64(MDiv* div);
  void lowerWasmBuiltinDivI64(MWasmBuiltinDivI64* div);
  void lowerModI64(MMod* mod);
  void lowerWasmBuiltinModI64(MWasmBuiltinModI64* mod);
  void lowerUDivI64(MDiv* div);
  void lowerUModI64(MMod* mod);
  void lowerNegI(MInstruction* ins, MDefinition* input);
  void lowerNegI64(MInstruction* ins, MDefinition* input);
  void lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs);
  void lowerUDiv(MDiv* div);
  void lowerUMod(MMod* mod);
  void lowerWasmSelectI(MWasmSelect* select);
  void lowerWasmSelectI64(MWasmSelect* select);
  bool canSpecializeWasmCompareAndSelect(MCompare::CompareType compTy,
                                         MIRType insTy);
  void lowerWasmCompareAndSelect(MWasmSelect* ins, MDefinition* lhs,
                                 MDefinition* rhs, MCompare::CompareType compTy,
                                 JSOp jsop);

  void lowerBigIntLsh(MBigIntLsh* ins);
  void lowerBigIntRsh(MBigIntRsh* ins);
  void lowerBigIntDiv(MBigIntDiv* ins);
  void lowerBigIntMod(MBigIntMod* ins);

  void lowerAtomicLoad64(MLoadUnboxedScalar* ins);
  void lowerAtomicStore64(MStoreUnboxedScalar* ins);

#ifdef ENABLE_WASM_SIMD
  bool canFoldReduceSimd128AndBranch(wasm::SimdOp op);
  bool canEmitWasmReduceSimd128AtUses(MWasmReduceSimd128* ins);
#endif

  LTableSwitchV* newLTableSwitchV(MTableSwitch* ins);
  LTableSwitch* newLTableSwitch(const LAllocation& in,
                                const LDefinition& inputCopy,
                                MTableSwitch* ins);

  void lowerPhi(MPhi* phi);
};

typedef LIRGeneratorARM64 LIRGeneratorSpecific;

}  // namespace jit
}  // namespace js

#endif /* jit_arm64_Lowering_arm64_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_Lowering_x86_shared_h
#define jit_x86_shared_Lowering_x86_shared_h

#include "jit/shared/Lowering-shared.h"

namespace js {
namespace jit {

class LIRGeneratorX86Shared : public LIRGeneratorShared {
 protected:
  LIRGeneratorX86Shared(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorShared(gen, graph, lirGraph) {}

  LTableSwitch* newLTableSwitch(const LAllocation& in,
                                const LDefinition& inputCopy,
                                MTableSwitch* ins);
  LTableSwitchV* newLTableSwitchV(MTableSwitch* ins);

  void lowerForShift(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                     MDefinition* lhs, MDefinition* rhs);
  void lowerForALU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                   MDefinition* input);
  void lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                   MDefinition* lhs, MDefinition* rhs);

  template <size_t Temps>
  void lowerForShiftInt64(
      LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, Temps>* ins,
      MDefinition* mir, MDefinition* lhs, MDefinition* rhs);

  void lowerForCompareI64AndBranch(MTest* mir, MCompare* comp, JSOp op,
                                   MDefinition* left, MDefinition* right,
                                   MBasicBlock* ifTrue, MBasicBlock* ifFalse);
  template <size_t Temps>
  void lowerForFPU(LInstructionHelper<1, 2, Temps>* ins, MDefinition* mir,
                   MDefinition* lhs, MDefinition* rhs);
  void lowerForBitAndAndBranch(LBitAndAndBranch* baab, MInstruction* mir,
                               MDefinition* lhs, MDefinition* rhs);
  void lowerNegI(MInstruction* ins, MDefinition* input);
  void lowerNegI64(MInstruction* ins, MDefinition* input);
  void lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs);
  void lowerDivI(MDiv* div);
  void lowerModI(MMod* mod);
  void lowerUDiv(MDiv* div);
  void lowerUMod(MMod* mod);
  void lowerUrshD(MUrsh* mir);
  void lowerPowOfTwoI(MPow* mir);
  void lowerWasmSelectI(MWasmSelect* select);
  void lowerWasmSelectI64(MWasmSelect* select);
  void lowerBigIntLsh(MBigIntLsh* ins);
  void lowerBigIntRsh(MBigIntRsh* ins);
  void lowerWasmBuiltinTruncateToInt32(MWasmBuiltinTruncateToInt32* ins);
  void lowerTruncateDToInt32(MTruncateToInt32* ins);
  void lowerTruncateFToInt32(MTruncateToInt32* ins);
  void lowerCompareExchangeTypedArrayElement(
      MCompareExchangeTypedArrayElement* ins, bool useI386ByteRegisters);
  void lowerAtomicExchangeTypedArrayElement(
      MAtomicExchangeTypedArrayElement* ins, bool useI386ByteRegisters);
  void lowerAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins,
                                         bool useI386ByteRegisters);

#ifdef ENABLE_WASM_SIMD
  bool isThreeOpAllowed() { return Assembler::HasAVX(); }
  bool canFoldReduceSimd128AndBranch(wasm::SimdOp op);
  bool canEmitWasmReduceSimd128AtUses(MWasmReduceSimd128* ins);
#endif
};

}  // namespace jit
}  // namespace js

#endif /* jit_x86_shared_Lowering_x86_shared_h */

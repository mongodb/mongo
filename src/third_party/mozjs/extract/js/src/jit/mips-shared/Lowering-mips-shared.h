/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_Lowering_mips_shared_h
#define jit_mips_shared_Lowering_mips_shared_h

#include "jit/shared/Lowering-shared.h"

namespace js {
namespace jit {

class LIRGeneratorMIPSShared : public LIRGeneratorShared {
 protected:
  LIRGeneratorMIPSShared(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorShared(gen, graph, lirGraph) {}

  // x86 has constraints on what registers can be formatted for 1-byte
  // stores and loads; on MIPS all registers are okay.
  LAllocation useByteOpRegister(MDefinition* mir);
  LAllocation useByteOpRegisterAtStart(MDefinition* mir);
  LAllocation useByteOpRegisterOrNonDoubleConstant(MDefinition* mir);
  LDefinition tempByteOpRegister();

  bool needTempForPostBarrier() { return false; }

  void lowerForShift(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                     MDefinition* lhs, MDefinition* rhs);
  void lowerUrshD(MUrsh* mir);

  void lowerPowOfTwoI(MPow* mir);

  void lowerForALU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                   MDefinition* input);
  void lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                   MDefinition* lhs, MDefinition* rhs);

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
                   MDefinition* src);
  template <size_t Temps>
  void lowerForFPU(LInstructionHelper<1, 2, Temps>* ins, MDefinition* mir,
                   MDefinition* lhs, MDefinition* rhs);

  void lowerForBitAndAndBranch(LBitAndAndBranch* baab, MInstruction* mir,
                               MDefinition* lhs, MDefinition* rhs);
  void lowerWasmBuiltinTruncateToInt32(MWasmBuiltinTruncateToInt32* ins);
  void lowerDivI(MDiv* div);
  void lowerModI(MMod* mod);
  void lowerNegI(MInstruction* ins, MDefinition* input);
  void lowerNegI64(MInstruction* ins, MDefinition* input);
  void lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs);
  void lowerUDiv(MDiv* div);
  void lowerUMod(MMod* mod);
  void lowerWasmSelectI(MWasmSelect* select);
  void lowerWasmSelectI64(MWasmSelect* select);

  void lowerBigIntLsh(MBigIntLsh* ins);
  void lowerBigIntRsh(MBigIntRsh* ins);

  LTableSwitch* newLTableSwitch(const LAllocation& in,
                                const LDefinition& inputCopy,
                                MTableSwitch* ins);
  LTableSwitchV* newLTableSwitchV(MTableSwitch* ins);

  void lowerPhi(MPhi* phi);
};

}  // namespace jit
}  // namespace js

#endif /* jit_mips_shared_Lowering_mips_shared_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_wasm32_Lowering_wasm32_h
#define jit_wasm32_Lowering_wasm32_h

#include "jit/shared/Lowering-shared.h"

namespace js::jit {

class LIRGeneratorWasm32 : public LIRGeneratorShared {
 protected:
  LIRGeneratorWasm32(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorShared(gen, graph, lirGraph) {
    MOZ_CRASH();
  }

  LBoxAllocation useBoxFixed(MDefinition*, Register, Register,
                             bool useAtStart = false) {
    MOZ_CRASH();
  }

  LAllocation useByteOpRegister(MDefinition*) { MOZ_CRASH(); }
  LAllocation useByteOpRegisterAtStart(MDefinition*) { MOZ_CRASH(); }
  LAllocation useByteOpRegisterOrNonDoubleConstant(MDefinition*) {
    MOZ_CRASH();
  }
  LDefinition tempByteOpRegister() { MOZ_CRASH(); }
  LDefinition tempToUnbox() { MOZ_CRASH(); }
  bool needTempForPostBarrier() { MOZ_CRASH(); }
  void lowerUntypedPhiInput(MPhi*, uint32_t, LBlock*, size_t) { MOZ_CRASH(); }
  void lowerInt64PhiInput(MPhi*, uint32_t, LBlock*, size_t) { MOZ_CRASH(); }
  void defineInt64Phi(MPhi*, size_t) { MOZ_CRASH(); }
  void lowerForShift(LInstructionHelper<1, 2, 0>*, MDefinition*, MDefinition*,
                     MDefinition*) {
    MOZ_CRASH();
  }
  void lowerUrshD(MUrsh*) { MOZ_CRASH(); }
  void lowerPowOfTwoI(MPow*) { MOZ_CRASH(); }
  template <typename T>
  void lowerForALU(T, MDefinition*, MDefinition*, MDefinition* v = nullptr) {
    MOZ_CRASH();
  }
  template <typename T>
  void lowerForFPU(T, MDefinition*, MDefinition*, MDefinition* v = nullptr) {
    MOZ_CRASH();
  }
  template <typename T>
  void lowerForALUInt64(T, MDefinition*, MDefinition*,
                        MDefinition* v = nullptr) {
    MOZ_CRASH();
  }
  void lowerForMulInt64(LMulI64*, MMul*, MDefinition*,
                        MDefinition* v = nullptr) {
    MOZ_CRASH();
  }
  template <typename T>
  void lowerForShiftInt64(T, MDefinition*, MDefinition*,
                          MDefinition* v = nullptr) {
    MOZ_CRASH();
  }
  void lowerForBitAndAndBranch(LBitAndAndBranch*, MInstruction*, MDefinition*,
                               MDefinition*) {
    MOZ_CRASH();
  }
  void lowerForCompareI64AndBranch(MTest*, MCompare*, JSOp, MDefinition*,
                                   MDefinition*, MBasicBlock*, MBasicBlock*) {
    MOZ_CRASH();
  }

  void lowerConstantDouble(double, MInstruction*) { MOZ_CRASH(); }
  void lowerConstantFloat32(float, MInstruction*) { MOZ_CRASH(); }
  void lowerTruncateDToInt32(MTruncateToInt32*) { MOZ_CRASH(); }
  void lowerTruncateFToInt32(MTruncateToInt32*) { MOZ_CRASH(); }
  void lowerBuiltinInt64ToFloatingPoint(MBuiltinInt64ToFloatingPoint* ins) {
    MOZ_CRASH();
  }
  void lowerWasmBuiltinTruncateToInt64(MWasmBuiltinTruncateToInt64* ins) {
    MOZ_CRASH();
  }
  void lowerWasmBuiltinTruncateToInt32(MWasmBuiltinTruncateToInt32* ins) {
    MOZ_CRASH();
  }
  void lowerDivI(MDiv*) { MOZ_CRASH(); }
  void lowerModI(MMod*) { MOZ_CRASH(); }
  void lowerDivI64(MDiv*) { MOZ_CRASH(); }
  void lowerWasmBuiltinDivI64(MWasmBuiltinDivI64* div) { MOZ_CRASH(); }
  void lowerModI64(MMod*) { MOZ_CRASH(); }
  void lowerWasmBuiltinModI64(MWasmBuiltinModI64* mod) { MOZ_CRASH(); }
  void lowerNegI(MInstruction*, MDefinition*) { MOZ_CRASH(); }
  void lowerNegI64(MInstruction*, MDefinition*) { MOZ_CRASH(); }
  void lowerMulI(MMul*, MDefinition*, MDefinition*) { MOZ_CRASH(); }
  void lowerUDiv(MDiv*) { MOZ_CRASH(); }
  void lowerUMod(MMod*) { MOZ_CRASH(); }
  void lowerWasmSelectI(MWasmSelect* select) { MOZ_CRASH(); }
  void lowerWasmSelectI64(MWasmSelect* select) { MOZ_CRASH(); }
  void lowerWasmCompareAndSelect(MWasmSelect* ins, MDefinition* lhs,
                                 MDefinition* rhs, MCompare::CompareType compTy,
                                 JSOp jsop) {
    MOZ_CRASH();
  }
  bool canSpecializeWasmCompareAndSelect(MCompare::CompareType compTy,
                                         MIRType insTy) {
    MOZ_CRASH();
  }

  void lowerBigIntLsh(MBigIntLsh*) { MOZ_CRASH(); }
  void lowerBigIntRsh(MBigIntRsh*) { MOZ_CRASH(); }
  void lowerBigIntDiv(MBigIntDiv*) { MOZ_CRASH(); }
  void lowerBigIntMod(MBigIntMod*) { MOZ_CRASH(); }

  void lowerAtomicLoad64(MLoadUnboxedScalar*) { MOZ_CRASH(); }
  void lowerAtomicStore64(MStoreUnboxedScalar*) { MOZ_CRASH(); }

  LTableSwitch* newLTableSwitch(LAllocation, LDefinition, MTableSwitch*) {
    MOZ_CRASH();
  }
  LTableSwitchV* newLTableSwitchV(MTableSwitch*) { MOZ_CRASH(); }
};

typedef LIRGeneratorWasm32 LIRGeneratorSpecific;

}  // namespace js::jit

#endif /* jit_wasm32_Lowering_wasm32_h */
